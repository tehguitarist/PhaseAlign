#include "AllocationCounter.h"
#include "analyse/Search.h"
#include "dsp/PhaseResponse.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ANALYSE's search against its spec (prototype/analyse.py): prototype/analyse_golden.py runs suggest_with_shift on
// fixed pairs and writes what it found to tests/golden/analyse_expected.txt; the C++ must find the same, to rounding.
using namespace pa::analyse;

namespace
{
std::vector<float> loadI16(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f.good());
    f.seekg(0, std::ios::end);
    const auto bytes = (size_t)f.tellg();
    f.seekg(0);
    std::vector<int16_t> q(bytes / 2);
    f.read(reinterpret_cast<char*>(q.data()), (std::streamsize)bytes);
    std::vector<float> v(q.size());
    for (size_t i = 0; i < q.size(); ++i)
        v[i] = (float)q[i] / 32768.0f;
    return v;
}

std::vector<float> loadF32(const std::string& path)
{
    std::vector<float> v;
    if (auto* f = std::fopen(path.c_str(), "rb"))
    {
        std::fseek(f, 0, SEEK_END);
        v.resize((size_t)std::ftell(f) / sizeof(float));
        std::rewind(f);
        REQUIRE(std::fread(v.data(), sizeof(float), v.size(), f) == v.size());
        std::fclose(f);
    }
    return v;
}

Mode modeFrom(const std::string& s)
{
    return s == "lo" ? Mode::lo : s == "hi" ? Mode::hi : s == "constant" ? Mode::constant : Mode::none;
}

const char* verdictName(Verdict v)
{
    switch (v)
    {
    case Verdict::suggest:
        return "suggest";
    case Verdict::close:
        return "close";
    case Verdict::delayOnly:
        return "delayOnly";
    case Verdict::nothing:
        return "nothing";
    case Verdict::noSignal:
        return "noSignal";
    }
    return "?";
}

struct ExpectedOption
{
    Candidate c;
    double gain, lowEnd;
    bool fromAttacks, delayOnly;
};

struct ExpectedScore
{
    Candidate c;
    double value;
};

struct Case
{
    std::string name, x, y;
    Scope scope;
    double fs = 48000.0;
    double baseline = 0.0;
    AttackReading attack;
    std::string verdict, message;
    bool lessLowEnd = false, chanceLevel = false, weakMatch = false;
    bool hasChanceGain = false;
    double chanceGain = 0.0;
    int shift = 0;
    std::vector<ExpectedOption> options;
    std::vector<ExpectedScore> scores;
};

Candidate readCandidate(std::istringstream& in)
{
    std::string mode;
    int wide, flip;
    Candidate c;
    in >> mode >> wide >> c.theta >> c.delayMs >> flip;
    c.mode = modeFrom(mode);
    c.wide = wide != 0;
    c.flip = flip != 0;
    return c;
}

std::vector<Case> readCases(const std::string& path)
{
    std::ifstream f(path);
    std::vector<Case> cases;
    std::string line;
    while (std::getline(f, line))
    {
        std::istringstream in(line);
        std::string key;
        in >> key;
        if (key == "case")
        {
            Case c;
            int d, p;
            in >> c.name >> c.x >> c.y >> d >> p >> c.fs;
            c.scope = {d != 0, p != 0};
            cases.push_back(c);
            continue;
        }
        if (cases.empty())
            continue;
        auto& c = cases.back();
        if (key == "baseline")
            in >> c.baseline;
        else if (key == "attack")
        {
            int clear;
            in >> c.attack.lagMs >> clear >> c.attack.peak >> c.attack.runnerUp;
            c.attack.clear = clear != 0;
        }
        else if (key == "verdict")
            in >> c.verdict;
        else if (key == "lessLowEnd")
        {
            int v;
            in >> v;
            c.lessLowEnd = v != 0;
        }
        else if (key == "shift")
            in >> c.shift;
        else if (key == "flags")
        {
            int chance, weak;
            in >> chance >> weak;
            c.chanceLevel = chance != 0;
            c.weakMatch = weak != 0;
        }
        else if (key == "chanceGain")
        {
            in >> c.chanceGain;
            c.hasChanceGain = true;
        }
        else if (key == "message")
            c.message = line.size() > 8 ? line.substr(8) : std::string();
        else if (key == "option")
        {
            ExpectedOption o;
            o.c = readCandidate(in);
            int fromAttacks, delayOnly;
            in >> o.c.score >> o.gain >> o.lowEnd >> fromAttacks >> delayOnly;
            o.fromAttacks = fromAttacks != 0;
            o.delayOnly = delayOnly != 0;
            c.options.push_back(o);
        }
        else if (key == "score")
        {
            ExpectedScore s;
            s.c = readCandidate(in);
            in >> s.value;
            c.scores.push_back(s);
        }
    }
    return cases;
}

// The whole result, field by field. Scores agree to rounding (double FFTs against numpy's); the attack reading's
// features are kept in single precision, so its numbers agree to a little less.
void checkCase(const Case& c, const std::vector<float>& x, const std::vector<float>& y)
{
    INFO("case " << c.name);
    const auto start = std::chrono::steady_clock::now();
    const auto r = suggestWithShift(x.data(), y.data(), (int)std::min(x.size(), y.size()), c.fs, c.scope);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("analyse %-20s %5.2f s of audio in %.3f s: %s%s%s, shift %d", c.name.c_str(), (double)x.size() / c.fs,
                seconds, verdictName(r.verdict), r.chanceLevel ? " (chance level)" : "",
                r.weakMatch ? " (weak match)" : "", r.shiftSamples);
    for (const auto& o : r.options)
        std::printf("; %s (%.4f)", describe(o.candidate).c_str(), o.candidate.score);
    std::printf("\n");

    CHECK(! r.cancelled);
    CHECK(r.baseline == Catch::Approx(c.baseline).margin(1e-9));
    CHECK(r.attack.lagMs == Catch::Approx(c.attack.lagMs).margin(1e-3));
    CHECK(r.attack.clear == c.attack.clear);
    CHECK(r.attack.peak == Catch::Approx(c.attack.peak).margin(1e-5));
    CHECK(r.attack.runnerUp == Catch::Approx(c.attack.runnerUp).margin(1e-4));
    CHECK(verdictName(r.verdict) == c.verdict);
    CHECK(r.lessLowEnd == c.lessLowEnd);
    CHECK(r.chanceLevel == c.chanceLevel);
    CHECK(r.weakMatch == c.weakMatch);
    if (c.hasChanceGain)
        CHECK(r.chanceGain == Catch::Approx(c.chanceGain).margin(1e-9));
    CHECK(r.shiftSamples == c.shift);
    CHECK(r.message == c.message);
    REQUIRE(r.options.size() == c.options.size());
    for (size_t i = 0; i < c.options.size(); ++i)
    {
        INFO("option " << i + 1 << ": " << describe(r.options[i].candidate) << " vs " << describe(c.options[i].c));
        const auto& got = r.options[i];
        const auto& want = c.options[i];
        CHECK(got.candidate.mode == want.c.mode);
        CHECK(got.candidate.wide == want.c.wide);
        CHECK(got.candidate.flip == want.c.flip);
        CHECK(got.candidate.theta == Catch::Approx(want.c.theta).margin(1e-9));
        CHECK(got.candidate.delayMs == Catch::Approx(want.c.delayMs).margin(want.delayOnly ? 1e-3 : 1e-9));
        CHECK(got.candidate.score == Catch::Approx(want.c.score).margin(1e-9));
        CHECK(got.gain == Catch::Approx(want.gain).margin(1e-9));
        CHECK(got.lowEndChange == Catch::Approx(want.lowEnd).margin(1e-9));
        CHECK(got.fromAttacks == want.fromAttacks);
        CHECK(got.delayOnly == want.delayOnly);
    }
    if (! c.scores.empty())
    {
        const auto sp = Spectra::compute(x.data(), y.data(), (int)std::min(x.size(), y.size()), c.fs);
        for (const auto& s : c.scores)
        {
            INFO("score of " << describe(s.c));
            CHECK(scoreAt(sp, s.c) == Catch::Approx(s.value).margin(1e-9));
        }
    }
}
} // namespace

TEST_CASE("ANALYSE's search matches prototype/analyse.py on the golden pairs", "[analyse]")
{
    const std::string dir = PA_GOLDEN_DIR;
    const auto cases = readCases(dir + "/analyse_expected.txt");
    REQUIRE(cases.size() == 6);
    std::map<std::string, std::vector<float>> signals;
    for (const auto& c : cases)
    {
        for (const auto& name : {c.x, c.y})
            if (signals.count(name) == 0)
                signals[name] = loadI16(dir + "/analyse_" + name + ".i16");
        checkCase(c, signals[c.x], signals[c.y]);
    }
}

TEST_CASE("ANALYSE's search: the known answers of the synthetic pairs", "[analyse]")
{
    // What the pairs were built as (analyse_golden.py), independent of the Python's output.
    const std::string dir = PA_GOLDEN_DIR;
    const auto ax = loadI16(dir + "/analyse_a_x.i16"), ay = loadI16(dir + "/analyse_a_y.i16"),
               bx = loadI16(dir + "/analyse_b_x.i16"), cx = loadI16(dir + "/analyse_c_x.i16"),
               cy = loadI16(dir + "/analyse_c_y.i16");
    const auto n = (int)ax.size();
    const auto fs = 48000.0;

    // A: the input is the sidechain 1.25 ms early and turned 120 degrees.
    auto r = suggestWithShift(ax.data(), ay.data(), n, fs, {});
    REQUIRE(! r.options.empty());
    CHECK(r.options[0].candidate.mode == Mode::constant);
    CHECK(r.options[0].candidate.theta == Catch::Approx(120.0).margin(angleStep));
    CHECK(r.options[0].candidate.delayMs == Catch::Approx(1.25).margin(0.01));

    // B: 6.5 ms late (312 samples) is beyond the knob: shift by hand, then turn 60 degrees.
    r = suggestWithShift(bx.data(), ay.data(), n, fs, {});
    CHECK(std::abs(r.shiftSamples + 312) <= 1);
    CHECK(r.message.find("consider shifting") != std::string::npos);
    REQUIRE(! r.options.empty());
    CHECK(r.options[0].candidate.theta == Catch::Approx(60.0).margin(angleStep));
    CHECK(std::abs(r.options[0].candidate.delayMs * 1e-3 * fs + r.shiftSamples + 312.0) < 1.0);

    // C: steady noise 2 ms late, turned 70 degrees.
    r = suggestWithShift(cx.data(), cy.data(), n, fs, {});
    REQUIRE(! r.options.empty());
    CHECK(r.options[0].candidate.mode == Mode::constant);
    CHECK(r.options[0].candidate.theta == Catch::Approx(70.0).margin(angleStep));
    CHECK(r.options[0].candidate.delayMs == Catch::Approx(-2.0).margin(0.01));

    // DELAY off: no delay in any option; PHASE off: no phase.
    r = suggestWithShift(ax.data(), ay.data(), n, fs, {false, true});
    for (const auto& o : r.options)
        CHECK(o.candidate.delayMs == 0.0);
    r = suggestWithShift(ax.data(), ay.data(), n, fs, {true, false});
    for (const auto& o : r.options)
        CHECK(o.candidate.mode == Mode::none);
}

TEST_CASE("ANALYSE's search: silence, too short, and cancelling", "[analyse]")
{
    const auto fs = 48000.0;
    std::vector<float> silence(48000, 0.0f), noise(48000);
    uint32_t seed = 1;
    for (auto& v : noise)
    {
        seed = seed * 1664525u + 1013904223u;
        v = (float)((double)seed / 4294967296.0 - 0.5);
    }
    CHECK(suggestWithShift(silence.data(), noise.data(), 48000, fs, {}).verdict == Verdict::noSignal);
    CHECK(suggestWithShift(noise.data(), silence.data(), 48000, fs, {}).verdict == Verdict::noSignal);
    CHECK(suggestWithShift(noise.data(), noise.data(), 4000, fs, {}).verdict == Verdict::noSignal); // under one frame

    std::atomic<bool> cancel{true};
    const auto r = suggestWithShift(noise.data(), noise.data(), 48000, fs, {}, &cancel);
    CHECK(r.cancelled);
    CHECK(r.options.empty());
}

TEST_CASE("ANALYSE's search: a pair that is the same signal needs nothing", "[analyse]")
{
    const std::string dir = PA_GOLDEN_DIR;
    const auto ay = loadI16(dir + "/analyse_a_y.i16");
    const auto r = suggestWithShift(ay.data(), ay.data(), (int)ay.size(), 48000.0, {});
    CHECK(r.baseline == Catch::Approx(1.0).margin(1e-9));
    CHECK(r.verdict == Verdict::nothing);
    CHECK(r.shiftSamples == 0);
}

TEST_CASE("ANALYSE's search at other sample rates finds the same answer", "[analyse]")
{
    // A Constant rotation and a delay built at each rate: the frame size and band layout scale with the rate.
    for (const auto fs : {44100.0, 96000.0, 192000.0})
    {
        INFO("fs " << fs);
        const auto n = (int)(3.0 * fs);
        std::vector<double> y((size_t)n);
        uint32_t seed = 7;
        double lp = 0.0;
        for (int i = 0; i < n; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            lp += 0.2 * ((double)seed / 4294967296.0 - 0.5 - lp);
            y[(size_t)i] = lp;
        }
        // x = y delayed by 1 ms (an integer number of samples at each rate here, from 44.1 kHz's 44.1: use 2 ms there)
        const auto delay = (int)std::lround((fs == 44100.0 ? 2.0 : 1.0) * 1e-3 * fs);
        std::vector<float> xf((size_t)n, 0.0f), yf((size_t)n);
        for (int i = 0; i < n; ++i)
        {
            yf[(size_t)i] = (float)y[(size_t)i];
            xf[(size_t)i] = i >= delay ? (float)y[(size_t)(i - delay)] : 0.0f;
        }
        const auto r = suggestWithShift(xf.data(), yf.data(), n, fs, {true, false});
        REQUIRE(! r.options.empty());
        CHECK(r.options[0].candidate.delayMs * 1e-3 * fs == Catch::Approx(-delay).margin(0.05));
        CHECK(! r.options[0].candidate.flip);
    }
}

TEST_CASE("PhaseStageResponse is the response phaseStageResponse gives", "[analyse]")
{
    // The precomputed form the search uses, against the one-call form the meter's preview uses.
    for (const auto mode : {pa::dsp::PhaseMode::high, pa::dsp::PhaseMode::low, pa::dsp::PhaseMode::constant})
        for (const auto wide : {false, true})
        {
            pa::dsp::ChainSettings s;
            s.phaseOn = true;
            s.phaseMode = mode;
            s.phaseWide = wide;
            s.phaseDegrees = wide ? 133.0 : 61.0;
            const pa::dsp::PhaseStageResponse response(s, 44100.0);
            for (const auto hz : {20.0, 150.0, 1000.0, 15000.0})
                CHECK(std::abs(response(hz) - pa::dsp::phaseStageResponse(s, 44100.0, hz)) < 1e-15);
        }
}

// Hidden: the user's own pairs (captures/, gitignored), against the Python on the same files
// (prototype/analyse_golden.py writes captures/analyse_user_expected.txt and the second set's raw copies).
TEST_CASE("ANALYSE's search matches the Python on the user's pairs", "[.useranalyse]")
{
    const auto cases = readCases("captures/analyse_user_expected.txt");
    REQUIRE(! cases.empty());
    for (const auto& c : cases)
    {
        const auto x = loadF32(c.x + ".f32"), y = loadF32(c.y + ".f32");
        REQUIRE(! x.empty());
        checkCase(c, x, y);
    }
}

// Hidden, Release: what each part of the search costs on the user's longest pair (the progress bar's weights).
TEST_CASE("ANALYSE's search: the cost of its parts", "[.analysecost]")
{
    const auto x = loadF32("captures/kick_a.f32"), y = loadF32("captures/kick_b.f32");
    REQUIRE(! x.empty());
    const auto n = (int)std::min(x.size(), y.size());
    const auto time = [](auto&& f)
    {
        const auto start = std::chrono::steady_clock::now();
        f();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    Spectra sp;
    const auto spectra = time([&] { sp = Spectra::compute(x.data(), y.data(), n, 48000.0); });
    const auto attack = time([&] { attackLag(x.data(), y.data(), n, 48000.0); });
    SearchOptions o;
    const auto full = time([&] { search(sp, o); });
    o.phaseOn = false;
    const auto noPhase = time([&] { search(sp, o); });
    std::printf("%.1f s of audio: spectra %.3f s, attack %.3f s, search %.3f s (phase off %.3f s), whole %.3f s\n",
                n / 48000.0, spectra, attack, full, noPhase,
                time([&] { suggestWithShift(x.data(), y.data(), n, 48000.0, {}); }));
}

// Hidden, Release: how the progress fraction follows the clock on the user's pairs (the bar should move steadily).
TEST_CASE("ANALYSE's search: progress against time", "[.analysecost]")
{
    for (const auto* name : {"captures/kick", "captures/bass", "captures/stems/kick_oh", "captures/stems/snare_sample"})
    {
        const auto x = loadF32(std::string(name) + "_a.f32"), y = loadF32(std::string(name) + "_b.f32");
        if (x.empty())
            continue;
        const auto n = (int)std::min(x.size(), y.size());
        std::atomic<float> fraction{0.0f};
        std::vector<std::pair<double, float>> samples;
        const auto start = std::chrono::steady_clock::now();
        std::thread worker([&] { suggestWithShift(x.data(), y.data(), n, 48000.0, {}, nullptr, &fraction); });
        std::atomic<bool> running{true};
        std::thread watcher(
            [&]
            {
                while (running)
                {
                    samples.push_back({std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                                       fraction.load()});
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            });
        worker.join();
        running = false;
        watcher.join();
        const auto total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("%-28s %.2f s:", name, total);
        for (const auto q : {0.25, 0.5, 0.75, 0.95})
        {
            float f = 0.0f;
            for (const auto& [t, v] : samples)
                if (t <= q * total)
                    f = v;
            std::printf("  at %2.0f%% of the time %3.0f%%", q * 100.0, f * 100.0);
        }
        std::printf("\n");
    }
}
