#include "dsp/Chain.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

// P3 groundwork (IMPLEMENTATION_PLAN M1 P3): worst-case automation rendered through the C++ chain, for listening and
// for tuning the fade lengths by ear. Hidden; writes raw stereo float renders plus an event list per scenario, which
// prototype/p3_report.py turns into WAVs and a report of the worst sample steps and where they happen:
//
//   build/PhaseAlignDspTests_artefacts/<cfg>/PhaseAlignDspTests "[p3]"   # → prototype/out/p3/raw/ (or $PA_P3_DIR)
//   .venv/bin/python prototype/p3_report.py
//
// Changes are applied at 64-sample callback boundaries, as a host would. The fade lengths are the plugin's own.
using namespace pa::dsp;

namespace
{
constexpr double fs = 48000.0;
constexpr int callback = 64;

using Stereo = std::vector<float>; // interleaved L R

int reach()
{
    return (int)std::lround(4.0 * fs * 10.0 / 1000.0);
}

int tenthsFromMs(double ms)
{
    return std::clamp((int)std::lround(ms * fs / 100.0), -reach(), reach());
}

// Sources, seconds long, stereo (slightly different per channel, as two mics would be).
Stereo sine100(double seconds)
{
    const auto n = (int)(seconds * fs);
    Stereo x((size_t)(2 * n));
    for (int i = 0; i < n; ++i)
        for (int ch = 0; ch < 2; ++ch)
            x[(size_t)(2 * i + ch)] = (float)(0.5 * std::sin(2.0 * M_PI * 100.0 * i / fs + 0.3 * ch));
    return x;
}

Stereo pink(double seconds)
{
    // Paul Kellet's economy pink filter on white noise, about -12 dBFS RMS.
    const auto n = (int)(seconds * fs);
    Stereo x((size_t)(2 * n));
    std::mt19937 rng(71);
    std::normal_distribution<double> white(0.0, 1.0);
    for (int ch = 0; ch < 2; ++ch)
    {
        double b0 = 0, b1 = 0, b2 = 0;
        for (int i = 0; i < n; ++i)
        {
            const auto w = white(rng);
            b0 = 0.99765 * b0 + w * 0.0990460;
            b1 = 0.96300 * b1 + w * 0.2965164;
            b2 = 0.57000 * b2 + w * 1.0526913;
            x[(size_t)(2 * i + ch)] = (float)(0.07 * (b0 + b1 + b2 + w * 0.1848));
        }
    }
    return x;
}

// A synthetic loop at 120 bpm: a kick (pitch-dropping sine), a hat (short noise bursts) and a bass note.
Stereo drums(double seconds)
{
    const auto n = (int)(seconds * fs);
    Stereo x((size_t)(2 * n), 0.0f);
    std::mt19937 rng(73);
    std::uniform_real_distribution<double> noise(-1.0, 1.0);
    const auto beat = (int)(0.5 * fs);
    for (int i = 0; i < n; ++i)
    {
        const auto t = (double)(i % beat) / fs; // since the beat
        const auto kickPhase = 2.0 * M_PI * (45.0 * t + 60.0 * (1.0 - std::exp(-t * 30.0)) / 30.0);
        const auto kick = 0.6 * std::sin(kickPhase) * std::exp(-t * 9.0);
        const auto th = (double)(i % (beat / 2)) / fs;
        const auto hat = 0.15 * noise(rng) * std::exp(-th * 80.0);
        const auto bass = 0.2 * std::sin(2.0 * M_PI * 55.0 * i / fs) * (0.6 + 0.4 * std::cos(2.0 * M_PI * i / beat));
        for (int ch = 0; ch < 2; ++ch)
            x[(size_t)(2 * i + ch)] = (float)(kick + bass + (ch == 0 ? hat : 0.8 * hat));
    }
    return x;
}

// One of the user's stem pairs (captures/<tag>_a.f32 and _b.f32, from prototype/export_pairs.py; gitignored) as a
// stereo source, the two mics left and right, from the loudest stretch of the length asked for. Empty where the files
// are missing.
std::function<Stereo(double)> stemPair(const std::string& tag)
{
    const auto load = [](const std::string& path)
    {
        std::vector<float> v;
        if (auto* f = std::fopen(path.c_str(), "rb"))
        {
            std::fseek(f, 0, SEEK_END);
            v.resize((size_t)std::ftell(f) / sizeof(float));
            std::rewind(f);
            if (std::fread(v.data(), sizeof(float), v.size(), f) != v.size())
                v.clear();
            std::fclose(f);
        }
        return v;
    };
    const auto base = std::string(PA_GOLDEN_DIR) + "/../../captures/" + tag;
    auto a = load(base + "_a.f32"), b = load(base + "_b.f32");
    if (a.empty() || a.size() != b.size())
        return {};
    return [a = std::move(a), b = std::move(b)](double seconds)
    {
        const auto n = (size_t)(seconds * fs);
        Stereo x(2 * n, 0.0f);
        if (a.size() <= n)
        {
            for (size_t i = 0; i < a.size(); ++i)
                x[2 * i] = a[i], x[2 * i + 1] = b[i];
            return x;
        }
        // The loudest window, on a 0.25 s grid.
        size_t best = 0;
        double bestEnergy = -1.0;
        for (size_t start = 0; start + n <= a.size(); start += (size_t)(0.25 * fs))
        {
            double e = 0.0;
            for (size_t i = start; i < start + n; ++i)
                e += (double)a[i] * a[i] + (double)b[i] * b[i];
            if (e > bestEnergy)
                bestEnergy = e, best = start;
        }
        for (size_t i = 0; i < n; ++i)
            x[2 * i] = a[best + i], x[2 * i + 1] = b[best + i];
        return x;
    };
}

// Every source fades in over 10 ms (a path with latency replays the onset later, and an abrupt start is a step).
Stereo fadedIn(Stereo x)
{
    const auto fade = (int)(0.01 * fs);
    for (int i = 0; i < fade && 2 * i + 1 < (int)x.size(); ++i)
        for (int ch = 0; ch < 2; ++ch)
            x[(size_t)(2 * i + ch)] *= (float)i / (float)fade;
    return x;
}

struct Event
{
    int at; // sample
    std::string what;
    std::function<void(ChainSettings&)> apply;
};

struct Scenario
{
    std::string name;
    ChainSettings initial;
    std::vector<Event> events;
    double seconds;
};

void write(const std::string& path, const std::vector<float>& data)
{
    auto* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fwrite(data.data(), sizeof(float), data.size(), f);
    std::fclose(f);
}

Stereo render(const Scenario& s, const Stereo& input)
{
    const auto n = (int)(input.size() / 2);
    std::vector<float> l((size_t)n), r((size_t)n);
    for (int i = 0; i < n; ++i)
        l[(size_t)i] = input[(size_t)(2 * i)], r[(size_t)i] = input[(size_t)(2 * i + 1)];

    Chain chain;
    chain.prepare(fs, 2, reach());
    chain.reset(s.initial);
    auto settings = s.initial;
    size_t next = 0;
    for (int pos = 0; pos < n; pos += callback)
    {
        // Parameters as the host delivers them: whatever has changed by the start of this callback.
        auto changed = false;
        while (next < s.events.size() && s.events[next].at <= pos)
        {
            s.events[next++].apply(settings);
            changed = true;
        }
        if (changed)
            chain.setSettings(settings);
        float* p[] = {l.data() + pos, r.data() + pos};
        chain.process(p, 2, std::min(callback, n - pos));
    }
    Stereo out((size_t)(2 * n));
    for (int i = 0; i < n; ++i)
        out[(size_t)(2 * i)] = l[(size_t)i], out[(size_t)(2 * i + 1)] = r[(size_t)i];
    return out;
}

int at(double seconds)
{
    return (int)std::lround(seconds * fs);
}

std::vector<Scenario> scenarios()
{
    std::vector<Scenario> list;
    auto base = ChainSettings::neutral(); // delay off, phase off: each scenario turns on what it needs

    { // Delay jumps: across the whole range, fractional values, and faster than the 50 ms fade (latest wins).
        Scenario s{"delay_jumps", base, {}, 6.0};
        s.initial.delayOn = true;
        const double values[] = {4.0, -4.0, 0.0, 2.345, -1.019, 3.999, -3.999, 0.01};
        double t = 0.5;
        for (const auto v : values)
        {
            s.events.push_back(
                {at(t), "delay " + std::to_string(v) + " ms", [v](auto& c) { c.delayTenths = tenthsFromMs(v); }});
            t += 0.35;
        }
        for (int k = 0; k < 30; ++k, t += 0.01) // a burst every 10 ms
        {
            const auto v = (k % 2 == 0 ? 1.0 : -1.0) * (0.5 + 0.1 * k);
            s.events.push_back(
                {at(t), "burst delay " + std::to_string(v) + " ms", [v](auto& c) { c.delayTenths = tenthsFromMs(v); }});
        }
        list.push_back(s);
    }
    { // Delay sweep: the knob dragged from -4 to +4 ms over a second, then back fast, updated every callback.
        Scenario s{"delay_sweep", base, {}, 4.0};
        s.initial.delayOn = true;
        s.initial.delayTenths = tenthsFromMs(-4.0);
        for (int pos = at(0.5); pos < at(1.5); pos += callback)
        {
            const auto v = -4.0 + 8.0 * (pos - at(0.5)) / (double)at(1.0);
            s.events.push_back({pos, "sweep", [v](auto& c) { c.delayTenths = tenthsFromMs(v); }});
        }
        for (int pos = at(2.0); pos < at(2.2); pos += callback)
        {
            const auto v = 4.0 - 8.0 * (pos - at(2.0)) / (double)at(0.2);
            s.events.push_back({pos, "fast sweep", [v](auto& c) { c.delayTenths = tenthsFromMs(v); }});
        }
        list.push_back(s);
    }
    { // Delay on/off: a latency change each time (fade out, switch, fade in), slow then fast.
        Scenario s{"delay_onoff", base, {}, 5.0};
        s.initial.delayTenths = tenthsFromMs(1.7);
        auto on = false;
        for (double t = 0.5; t < 3.0; t += 0.4)
            s.events.push_back({at(t), (on = ! on) ? "delay on" : "delay off", [v = on](auto& c) { c.delayOn = v; }});
        for (double t = 3.0; t < 4.5; t += 0.015) // faster than the fade: reversals
            s.events.push_back(
                {at(t), (on = ! on) ? "delay on (fast)" : "delay off (fast)", [v = on](auto& c) { c.delayOn = v; }});
        list.push_back(s);
    }
    { // Polarity flips: slow, then faster than the 50 ms ramp.
        Scenario s{"polarity", base, {}, 4.0};
        auto inv = false;
        for (double t = 0.5; t < 2.5; t += 0.25)
            s.events.push_back(
                {at(t), (inv = ! inv) ? "invert" : "normal", [v = inv](auto& c) { c.polarityInverted = v; }});
        for (double t = 2.5; t < 3.5; t += 0.02)
            s.events.push_back({at(t), (inv = ! inv) ? "invert (fast)" : "normal (fast)",
                                [v = inv](auto& c) { c.polarityInverted = v; }});
        list.push_back(s);
    }
    { // Hi <-> Lo glides at 120 and 180 degrees, and a reversal 15 ms into a glide.
        Scenario s{"hilo_glides", base, {}, 5.0};
        s.initial.phaseOn = true;
        s.initial.phaseDegrees = 120.0;
        s.initial.phaseWide = true;
        auto lo = false;
        for (double t = 0.5; t < 2.0; t += 0.3)
            s.events.push_back({at(t), (lo = ! lo) ? "Lo" : "Hi",
                                [v = lo](auto& c) { c.phaseMode = v ? PhaseMode::low : PhaseMode::high; }});
        s.events.push_back({at(2.2), "180 degrees", [](auto& c) { c.phaseDegrees = 180.0; }});
        for (double t = 2.5; t < 4.0; t += 0.3)
        {
            s.events.push_back({at(t), "Lo", [](auto& c) { c.phaseMode = PhaseMode::low; }});
            s.events.push_back({at(t + 0.015), "Hi (reversal)", [](auto& c) { c.phaseMode = PhaseMode::high; }});
        }
        list.push_back(s);
    }
    { // The phase knob swept 0 -> 180 -> 0 in Hi, then Lo, then Constant, updated every callback.
        Scenario s{"phase_sweeps", base, {}, 7.0};
        s.initial.phaseOn = true;
        s.initial.phaseWide = true;
        const std::pair<double, PhaseMode> runs[] = {
            {0.5, PhaseMode::high}, {2.5, PhaseMode::low}, {4.5, PhaseMode::constant}};
        for (const auto& [start, mode] : runs)
        {
            s.events.push_back({at(start - 0.3),
                                mode == PhaseMode::high  ? "Hi"
                                : mode == PhaseMode::low ? "Lo"
                                                         : "Constant",
                                [m = mode](auto& c) { c.phaseMode = m; }});
            for (int pos = at(start); pos < at(start + 1.0); pos += callback)
            {
                const auto x = (pos - at(start)) / (double)at(1.0);
                const auto deg = 180.0 * (x < 0.5 ? 2.0 * x : 2.0 - 2.0 * x);
                s.events.push_back({pos, "sweep", [deg](auto& c) { c.phaseDegrees = deg; }});
            }
        }
        list.push_back(s);
    }
    { // Phase on/off at 90 degrees in Hi and in Constant.
        Scenario s{"phase_onoff", base, {}, 5.0};
        s.initial.phaseDegrees = 90.0;
        auto on = false;
        for (double t = 0.5; t < 2.0; t += 0.25)
            s.events.push_back(
                {at(t), (on = ! on) ? "phase on (Hi)" : "phase off (Hi)", [v = on](auto& c) { c.phaseOn = v; }});
        s.events.push_back({at(2.1), "Constant", [](auto& c) { c.phaseMode = PhaseMode::constant; }});
        for (double t = 2.5; t < 4.5; t += 0.25)
            s.events.push_back({at(t), (on = ! on) ? "phase on (Constant)" : "phase off (Constant)",
                                [v = on](auto& c) { c.phaseOn = v; }});
        list.push_back(s);
    }
    { // Entering and leaving Constant (a latency change), at 60 degrees with the delay on, then fast.
        Scenario s{"constant_entry_exit", base, {}, 6.0};
        s.initial.phaseOn = true;
        s.initial.phaseDegrees = 60.0;
        s.initial.delayOn = true;
        s.initial.delayTenths = tenthsFromMs(-1.3);
        auto constant = false;
        for (double t = 0.5; t < 3.5; t += 0.5)
            s.events.push_back({at(t), (constant = ! constant) ? "enter Constant" : "leave Constant (Hi)",
                                [v = constant](auto& c) { c.phaseMode = v ? PhaseMode::constant : PhaseMode::high; }});
        for (double t = 3.5; t < 5.0; t += 0.012) // reversals during the 10 ms fades
            s.events.push_back({at(t), (constant = ! constant) ? "enter Constant (fast)" : "leave Constant (fast)",
                                [v = constant](auto& c) { c.phaseMode = v ? PhaseMode::constant : PhaseMode::low; }});
        list.push_back(s);
    }
    { // Everything at once: random changes of every control every 1 to 30 ms (the tests' automation script).
        Scenario s{"all_at_once", base, {}, 8.0};
        std::mt19937 rng(79);
        std::uniform_int_distribution<int> delayDist(-reach(), reach());
        std::uniform_int_distribution<int> gapDist(1, (int)(0.03 * fs));
        std::uniform_real_distribution<double> angleDist(0.0, 180.0);
        std::uniform_int_distribution<int> modeDist(0, 2);
        std::bernoulli_distribution coin(0.3), rare(0.1), wideCoin(0.2);
        for (int t = at(0.5); t < at(7.5); t += gapDist(rng))
        {
            const auto d = delayDist(rng);
            const auto a = angleDist(rng);
            const auto flip = coin(rng), delay = coin(rng), phase = coin(rng);
            const auto mode = rare(rng) ? modeDist(rng) : -1;
            const auto wide = wideCoin(rng);
            s.events.push_back({t, "random", [=](auto& c)
                                {
                                    c.delayTenths = d;
                                    c.phaseDegrees = a;
                                    c.polarityInverted ^= flip;
                                    c.delayOn ^= delay;
                                    c.phaseOn ^= phase;
                                    c.phaseWide ^= wide;
                                    if (mode >= 0)
                                        c.phaseMode = (PhaseMode)mode;
                                }});
        }
        list.push_back(s);
    }
    return list;
}
} // namespace

TEST_CASE("P3 renders: worst-case automation through the chain", "[.][p3]")
{
    const auto* env = std::getenv("PA_P3_DIR");
    const auto dir = env != nullptr ? std::string(env) : std::string(PA_GOLDEN_DIR) + "/../../prototype/out/p3/raw";
    REQUIRE(juce::File(dir).createDirectory());

    std::vector<std::pair<std::string, std::function<Stereo(double)>>> sources = {
        {"sine100", sine100}, {"pink", pink}, {"drums", drums}};
    for (const auto* tag : {"kick", "snare", "bass", "guitar", "hats"})
        if (auto stem = stemPair(tag))
            sources.push_back({std::string("stem_") + tag, std::move(stem)});
    for (const auto& s : scenarios())
    {
        // The event list, once per scenario (the latency each setting needs, for the report's alignment).
        auto* f = std::fopen((dir + "/" + s.name + ".events.txt").c_str(), "w");
        REQUIRE(f != nullptr);
        std::fprintf(f, "# sample rate %.0f, callback %d; sample\tlatency after\tevent\n", fs, callback);
        auto settings = s.initial;
        std::fprintf(f, "0\t%d\tinitial\n", Chain::latencyFor(settings, fs, reach()));
        for (const auto& e : s.events)
        {
            e.apply(settings);
            if (e.what != "sweep" && e.what != "fast sweep") // one line per drag would bury the rest
                std::fprintf(f, "%d\t%d\t%s\n", e.at, Chain::latencyFor(settings, fs, reach()), e.what.c_str());
        }
        std::fclose(f);

        for (const auto& [sourceName, make] : sources)
        {
            const auto input = fadedIn(make(s.seconds));
            write(dir + "/" + s.name + "_" + sourceName + "_in.f32", input);
            write(dir + "/" + s.name + "_" + sourceName + "_out.f32", render(s, input));
        }
    }
    WARN("wrote " << dir);
}
