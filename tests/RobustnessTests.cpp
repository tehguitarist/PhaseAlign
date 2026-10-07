#include "PluginProcessor.h"
#include "ThreadAllocations.h"
#include "meter/MeterCapture.h"
#include "params/Parameters.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <vector>

// Robustness through the plugin's real entry points (IMPLEMENTATION_PLAN M5): mono buses, sample-rate and block-size
// changes, offline rendering, host bypass during a latency switch, a mode change during a delay crossfade, state
// recall into a running instance, and no allocation in the audio callbacks, in every mode.
using namespace pa::params;

namespace
{
constexpr float hi = 0.0f, lo = 1.0f, constant = 2.0f; // phaseMode values

void setParam(PhaseAlignProcessor& p, const char* paramId, float value)
{
    auto* param = p.getValueTreeState().getParameter(paramId);
    param->setValueNotifyingHost(param->convertTo0to1(value));
}

// Delay on at `delayMs`, the phase stage on at 60 degrees in `mode`, polarity inverted: every stage in the path.
void setBusy(PhaseAlignProcessor& p, float mode, float delayMs = 0.37f)
{
    setParam(p, id::delayOn, 1.0f);
    setParam(p, id::delayMs, delayMs);
    setParam(p, id::phaseMode, mode);
    setParam(p, id::phase, 60.0f / 90.0f);
    setParam(p, id::polarity, 1.0f);
}

juce::AudioBuffer<float> noise(int numChannels, int numSamples, unsigned seed)
{
    juce::AudioBuffer<float> b(numChannels, numSamples);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            b.setSample(ch, i, dist(rng));
    return b;
}

// A 100 Hz sine at 0.5 on every channel, faded in over 10 ms (a path with latency replays the onset later).
juce::AudioBuffer<float> lowSine(int numChannels, int numSamples, double fs)
{
    juce::AudioBuffer<float> b(numChannels, numSamples);
    const auto fadeIn = (int)(0.01 * fs);
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            b.setSample(
                ch, i, (float)(0.5 * std::sin(2.0 * M_PI * 100.0 * i / fs)) * std::min(1.0f, (float)i / (float)fadeIn));
    return b;
}

// The largest sample-to-sample step the chain may make on lowSine (as in tests/dsp/ChainTests.cpp): the sine's own
// step plus every fade's slope across the largest jump it can bridge.
double stepLimit(double fs)
{
    const auto amplitude = 0.5;
    const auto naturalStep = 2.0 * M_PI * 100.0 / fs * amplitude;
    const auto fade = (double)pa::dsp::crossfadeSamples(fs), latencyFade = (double)pa::dsp::latencyFadeSamples(fs);
    const auto phaseRamp = amplitude * M_PI / (pa::dsp::ConstantRotator::smoothMs * 1.0e-3 * fs);
    return 1.05 * naturalStep + 3.0 * (2.0 * amplitude) / fade + amplitude / latencyFade + phaseRamp;
}

double worstStep(const juce::AudioBuffer<float>& b, int numChannels, int from = 1)
{
    double worst = 0.0;
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = std::max(1, from); i < b.getNumSamples(); ++i)
            worst = std::max(worst, (double)std::abs(b.getSample(ch, i) - b.getSample(ch, i - 1)));
    return worst;
}

// Processes [start, start + n) of `buffer` in place, in blocks of `block`, through processBlock or the bypassed path.
void process(PhaseAlignProcessor& p, juce::AudioBuffer<float>& buffer, int start, int n, int block,
             bool bypassed = false)
{
    juce::MidiBuffer midi;
    for (int pos = start; pos < start + n;)
    {
        const auto m = std::min(block, start + n - pos);
        juce::AudioBuffer<float> sub(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), pos, m);
        if (bypassed)
            p.processBlockBypassed(sub, midi);
        else
            p.processBlock(sub, midi);
        pos += m;
    }
}

void process(PhaseAlignProcessor& p, juce::AudioBuffer<float>& buffer, int block)
{
    process(p, buffer, 0, buffer.getNumSamples(), block);
}

// Empties the meter's FIFO, as the meter screen does, so it never fills.
void drain(PhaseAlignProcessor& p)
{
    static std::vector<float> streams[pa::meter::MeterCapture::numStreams];
    for (auto& s : streams)
        s.resize((size_t)pa::meter::MeterCapture::capacity);
    float* dest[] = {streams[0].data(), streams[1].data(), streams[2].data(), streams[3].data(), streams[4].data()};
    p.getMeterCapture().pull(dest, pa::meter::MeterCapture::capacity);
}

bool allFinite(const juce::AudioBuffer<float>& b)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite(b.getSample(ch, i)))
                return false;
    return true;
}
} // namespace

TEST_CASE("mono buses work in every mode and match a stereo instance's left channel", "[robustness]")
{
    const auto fs = 48000.0;
    for (const auto mode : {hi, lo, constant})
        for (const auto& sidechain : {juce::AudioChannelSet::mono(), juce::AudioChannelSet::disabled()})
        {
            INFO("mode " << mode << ", sidechain " << sidechain.getDescription());
            PhaseAlignProcessor stereo, mono;
            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add(juce::AudioChannelSet::mono());
            layout.inputBuses.add(sidechain);
            layout.outputBuses.add(juce::AudioChannelSet::mono());
            REQUIRE(mono.setBusesLayout(layout));
            for (auto* p : {&stereo, &mono})
            {
                setBusy(*p, mode);
                p->prepareToPlay(fs, 256);
                p->getMeterCapture().setActive(true); // the meter's path too
            }
            CHECK(mono.getLatencySamples() == stereo.getLatencySamples());
            CHECK(mono.getTotalNumInputChannels() == (sidechain.isDisabled() ? 1 : 2));

            const auto source = noise(4, (int)fs, 3);
            auto s = source;
            juce::AudioBuffer<float> m(mono.getTotalNumInputChannels(), source.getNumSamples());
            for (int ch = 0; ch < m.getNumChannels(); ++ch)
                m.copyFrom(ch, 0, source, ch == 0 ? 0 : 2, 0, source.getNumSamples()); // main L, sidechain L
            process(stereo, s, 256);
            process(mono, m, 256);

            for (int i = 0; i < m.getNumSamples(); ++i)
                REQUIRE(m.getSample(0, i) == s.getSample(0, i));
            CHECK(mono.getMeterCapture().getNumReady() > 0);
        }
}

TEST_CASE("a sample-rate change while running re-prepares at the new rate", "[robustness]")
{
    // Constant at 0 degrees with the delay on at 37 samples: an impulse comes out exactly at the latency plus 37.
    PhaseAlignProcessor proc;
    setParam(proc, id::delayOn, 1.0f);
    setParam(proc, id::phaseMode, constant);
    setParam(proc, id::phase, 0.0f);

    for (const auto fs : {44100.0, 96000.0, 48000.0, 192000.0, 88200.0})
    {
        INFO("fs " << fs);
        setParam(proc, id::delayMs, (float)(1000.0 * 37.0 / fs));
        proc.prepareToPlay(fs, 512); // no releaseResources in between: some hosts don't call it
        CHECK(proc.getLatencySamples() == proc.requiredLatency());
        CHECK(proc.getDelaySampleRate() == fs);

        auto busy = noise(4, 4096, 5); // some audio first, at the new rate
        process(proc, busy, 512);
        CHECK(allFinite(busy));

        // Silence long enough to flush that, then an impulse.
        const auto latency = proc.getLatencySamples();
        REQUIRE(delayInSamples(1000.0 * 37.0 / fs, fs) == 37.0);
        const auto shift = latency + 37;
        juce::AudioBuffer<float> b(4, 2 * latency + shift + 1024);
        b.clear();
        const auto at = latency + 512;
        b.setSample(0, at, 1.0f);
        process(proc, b, 512);
        CHECK(b.getSample(0, at + shift) == 1.0f);
        CHECK(b.getSample(0, at + shift - 1) == 0.0f);
        CHECK(b.getSample(0, at + shift + 1) == 0.0f);
    }
}

TEST_CASE("any host block size gives identical output in every mode, with the meter on or off", "[robustness]")
{
    const auto fs = 44100.0;
    const auto input = noise(4, 30000, 7);
    for (const auto mode : {hi, lo, constant})
    {
        INFO("mode " << mode);
        const auto render = [&](const std::vector<int>& sizes, bool meter)
        {
            PhaseAlignProcessor proc;
            setBusy(proc, mode);
            proc.prepareToPlay(fs, 64); // smaller than most of the blocks below
            proc.getMeterCapture().setActive(meter);
            auto b = input;
            juce::MidiBuffer midi;
            size_t k = 0;
            for (int pos = 0; pos < b.getNumSamples();)
            {
                const auto n = std::min(sizes[k++ % sizes.size()], b.getNumSamples() - pos);
                juce::AudioBuffer<float> sub(b.getArrayOfWritePointers(), b.getNumChannels(), pos, n);
                proc.processBlock(sub, midi);
                pos += n;
                drain(proc);
            }
            return b;
        };

        const auto reference = render({64}, false);
        for (const auto& sizes : std::vector<std::vector<int>>{{1}, {4096}, {1, 4096, 7, 333, 64, 2048}, {30000}})
            for (const auto meter : {false, true})
            {
                INFO("first block " << sizes.front() << ", meter " << meter);
                const auto out = render(sizes, meter);
                for (int ch = 0; ch < 4; ++ch) // the sidechain channels are left alone too
                    for (int i = 0; i < out.getNumSamples(); ++i)
                        REQUIRE(out.getSample(ch, i) == reference.getSample(ch, i));
            }
    }
}

TEST_CASE("offline (non-realtime) rendering in one large block matches realtime", "[robustness]")
{
    const auto fs = 48000.0;
    const auto input = noise(4, 96000, 9);
    for (const auto mode : {hi, constant})
    {
        INFO("mode " << mode);
        PhaseAlignProcessor realtime, offline;
        for (auto* p : {&realtime, &offline})
            setBusy(*p, mode);
        offline.setNonRealtime(true);
        realtime.prepareToPlay(fs, 512);
        offline.prepareToPlay(fs, 96000);
        auto a = input, b = input;
        process(realtime, a, 512);
        process(offline, b, 96000);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
                REQUIRE(b.getSample(ch, i) == a.getSample(ch, i));
    }
}

TEST_CASE("host bypass toggled during a latency switch stays click-free and ends delayed by the latency",
          "[robustness]")
{
    const auto fs = 48000.0;
    const auto ms = [fs](double t) { return (int)(t * fs / 1000.0); };
    struct Case
    {
        const char* name;
        const char* param;
        float to;
    };
    for (const auto& c : {Case{"delay on", id::delayOn, 1.0f}, Case{"into Constant", id::phaseMode, constant}})
    {
        INFO(c.name);
        PhaseAlignProcessor proc;
        setParam(proc, id::delayMs, 1.5f);
        setParam(proc, id::phase, 0.5f);
        proc.prepareToPlay(fs, 64);
        auto b = lowSine(4, ms(800), fs);
        const auto input = b;

        process(proc, b, 0, ms(200), 64);
        setParam(proc, c.param, c.to); // the latency switch starts: a 10 ms fade-out
        CHECK(proc.getLatencySamples() == proc.requiredLatency());
        process(proc, b, ms(200), ms(5), 64);         // half way down
        process(proc, b, ms(205), ms(300), 64, true); // host bypass: the switch completes inside it
        CHECK(proc.getLatencySamples() == proc.requiredLatency());
        process(proc, b, ms(505), b.getNumSamples() - ms(505), 64); // and back

        CHECK(worstStep(b, 2) <= stepLimit(fs));
        CHECK(worstStep(b, 2) < 0.05); // and nowhere near a click (a hard switch steps by up to 1.0 here)
        // Late in the bypassed stretch, every fade is over: the input delayed by the (new) latency, exactly.
        const auto latency = proc.getLatencySamples();
        for (int i = ms(400); i < ms(505); ++i)
            REQUIRE(b.getSample(0, i) == input.getSample(0, i - latency));
    }
}

TEST_CASE("a mode change during a delay crossfade is click-free and lands on the right delay", "[robustness]")
{
    const auto fs = 48000.0;
    const auto ms = [fs](double t) { return (int)(t * fs / 1000.0); };
    PhaseAlignProcessor proc;
    setParam(proc, id::delayOn, 1.0f);
    setParam(proc, id::delayMs, 1.0f);
    setParam(proc, id::phase, 0.0f);
    proc.prepareToPlay(fs, 128);
    auto b = lowSine(2 + 2, ms(1000), fs);

    process(proc, b, 0, ms(200), 128);
    setParam(proc, id::delayMs, 3.0f); // a 50 ms crossfade starts
    process(proc, b, ms(200), ms(10), 128);
    setParam(proc, id::phaseMode, constant); // a latency switch inside it
    CHECK(proc.getLatencySamples() == proc.requiredLatency());
    process(proc, b, ms(210), ms(15), 128);
    setParam(proc, id::delayMs, -2.0f); // and a new delay target during the fade-in
    process(proc, b, ms(225), ms(20), 128);
    setParam(proc, id::phaseMode, hi); // back out while that crossfade runs
    process(proc, b, ms(245), ms(20), 128);
    setParam(proc, id::phaseMode, constant);
    process(proc, b, ms(265), b.getNumSamples() - ms(265), 128);
    CHECK(worstStep(b, 2) <= stepLimit(fs));
    CHECK(worstStep(b, 2) < 0.05); // and nowhere near a click (a hard switch steps by up to 1.0 here)
    CHECK(proc.getLatencySamples() == proc.requiredLatency());

    // Settled in Constant at 0 degrees: an impulse arrives at the latency plus the delay (-2 ms), exactly.
    const auto latency = proc.getLatencySamples();
    const auto shift = latency + (int)std::lround(delayInSamples(-2.0, fs));
    juce::AudioBuffer<float> imp(4, 2 * latency + 2048);
    imp.clear();
    const auto at = latency + 256;
    imp.setSample(0, at, 1.0f);
    process(proc, imp, 128);
    CHECK(imp.getSample(0, at + shift) == 1.0f);
    CHECK(imp.getSample(0, at + shift + 1) == 0.0f);
}

TEST_CASE("state recalled into a running instance switches to Constant cleanly and then matches a fresh one",
          "[robustness]")
{
    const auto fs = 48000.0;
    const auto ms = [fs](double t) { return (int)(t * fs / 1000.0); };

    // The state to recall: Constant at 45 degrees, delay on at 2.5 ms, inverted.
    PhaseAlignProcessor source;
    setParam(source, id::phaseMode, constant);
    setParam(source, id::phase, 0.5f);
    setParam(source, id::delayOn, 1.0f);
    setParam(source, id::delayMs, 2.5f);
    setParam(source, id::polarity, 1.0f);
    juce::MemoryBlock state;
    source.getStateInformation(state);

    // `running` plays in Hi with the delay off, then the state arrives mid-stream; `fresh` had it from the start.
    PhaseAlignProcessor running, fresh;
    fresh.setStateInformation(state.getData(), (int)state.getSize());
    running.prepareToPlay(fs, 256);
    fresh.prepareToPlay(fs, 256);
    CHECK(running.getLatencySamples() == pa::dsp::HiLoStage::latencyFor(fs)); // Hi/Lo's

    auto a = lowSine(4, ms(2000), fs);
    auto b = a;
    process(running, a, 0, ms(500), 256);
    running.setStateInformation(state.getData(), (int)state.getSize()); // the message thread, between callbacks
    CHECK(running.getLatencySamples() == fresh.getLatencySamples());
    CHECK(running.getLatencySamples() == running.requiredLatency());
    process(running, a, ms(500), a.getNumSamples() - ms(500), 256);
    process(fresh, b, 256);

    CHECK(worstStep(a, 2) <= stepLimit(fs));
    CHECK(worstStep(a, 2) < 0.05); // and nowhere near a click (a hard switch steps by up to 1.0 here)
    // Once every fade and the angle have settled, both are the same function of the same input history.
    for (int ch = 0; ch < 2; ++ch)
        for (int i = ms(1000); i < a.getNumSamples(); ++i)
            REQUIRE(std::abs(a.getSample(ch, i) - b.getSample(ch, i)) < 1.0e-6f);
}

TEST_CASE("the audio callbacks never allocate, in every mode, with automation, bypass and the meter on", "[robustness]")
{
    for (const auto fs : {44100.0, 96000.0})
    {
        INFO("fs " << fs);
        PhaseAlignProcessor proc;
        proc.prepareToPlay(fs, 512);
        proc.getMeterCapture().setActive(true);
        auto b = noise(4, 512, 11);
        juce::MidiBuffer midi;
        std::mt19937 rng(13);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);

        drain(proc); // sizes its buffers, outside the measurement
        long allocations = 0;
        for (int callback = 0; callback < 1500; ++callback)
        {
            // Parameter changes come from the host between callbacks (on this thread here, so a latency change is
            // reported synchronously, outside the measured calls).
            setParam(proc, id::delayMs, 8.0f * unit(rng) - 4.0f);
            setParam(proc, id::phase, unit(rng));
            if (callback % 7 == 0)
                setParam(proc, id::polarity, unit(rng) < 0.5f ? 0.0f : 1.0f);
            if (callback % 11 == 0)
                setParam(proc, id::phaseOn, unit(rng) < 0.5f ? 0.0f : 1.0f);
            if (callback % 50 == 0)
                setParam(proc, id::delayOn, unit(rng) < 0.5f ? 0.0f : 1.0f);
            if (callback % 100 == 0)
                setParam(proc, id::phaseMode, (float)(callback / 100 % 3)); // Hi, Lo, Constant in turn
            if (callback % 37 == 0)
                setParam(proc, id::phaseRange, unit(rng) < 0.5f ? 0.0f : 1.0f);

            const auto n = 1 + (callback * 97) % 512;
            juce::AudioBuffer<float> sub(b.getArrayOfWritePointers(), 4, 0, n);
            const auto before = pa::test::threadAllocations();
            if (callback % 200 < 20)
                proc.processBlockBypassed(sub, midi);
            else
                proc.processBlock(sub, midi);
            allocations += pa::test::threadAllocations() - before;
            drain(proc);
        }
        CHECK(allocations == 0);
    }
}
