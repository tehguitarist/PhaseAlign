#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "meter/CorrelationAnalyser.h"
#include "params/Parameters.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <random>

using namespace pa::params;
using Catch::Approx;

namespace
{
void setParam(PhaseAlignProcessor& p, const char* paramId, float value)
{
    auto* param = p.getValueTreeState().getParameter(paramId);
    param->setValueNotifyingHost(param->convertTo0to1(value));
}

// Main stereo in/out plus a stereo sidechain: 4 channels in the process buffer, main first.
juce::AudioBuffer<float> noiseBuffer(int numSamples, unsigned seed)
{
    juce::AudioBuffer<float> b(4, numSamples);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    for (int ch = 0; ch < 4; ++ch)
        for (int i = 0; i < numSamples; ++i)
            b.setSample(ch, i, dist(rng));
    return b;
}

// The latency while the delay is on: the reach rounded up to whole samples plus the interpolation's lookahead.
int delayLatency(double fs)
{
    return pa::dsp::Chain::delayLatencyFor(fs, maxDelayTenths(fs));
}

// Hi/Lo's latency (its oversampling, plan 2.3), there whenever Constant isn't selected, phase on or off.
int hiLoLatency(double fs)
{
    return pa::dsp::HiLoStage::latencyFor(fs);
}

// Processes `buffer` in place in blocks from `sizes` (cycled), through processBlock or the bypassed path.
void process(PhaseAlignProcessor& p, juce::AudioBuffer<float>& buffer, const std::vector<int>& sizes,
             bool bypassed = false)
{
    juce::MidiBuffer midi;
    size_t k = 0;
    for (int pos = 0; pos < buffer.getNumSamples();)
    {
        const auto n = std::min(sizes[k++ % sizes.size()], buffer.getNumSamples() - pos);
        juce::AudioBuffer<float> block(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), pos, n);
        if (bypassed)
            p.processBlockBypassed(block, midi);
        else
            p.processBlock(block, midi);
        pos += n;
    }
}
} // namespace

TEST_CASE("the reported latency matches the measured impulse offset, with the delay on and off", "[processor]")
{
    for (const auto fs : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0})
        for (const auto on : {false, true})
            for (const auto samples : {-maxDelaySamples(fs) + 1, -103, -1, 0, 1, 48, maxDelaySamples(fs) - 1})
            {
                INFO("fs " << fs << ", delay " << (on ? "on" : "off") << ", " << samples << " samples");
                PhaseAlignProcessor proc;
                setParam(proc, id::delayMs, (float)(samples * 1000.0 / fs)); // whole samples: an exact impulse out
                setParam(proc, id::delayOn, on ? 1.0f : 0.0f);
                setParam(proc, id::phaseOn, 0.0f); // Hi/Lo at 0 degrees is not an identity (its k floor)
                proc.prepareToPlay(fs, 512);

                const auto latency = proc.getLatencySamples();
                CHECK(latency == (on ? delayLatency(fs) : 0) + hiLoLatency(fs));

                juce::AudioBuffer<float> buffer(4, 2048);
                buffer.clear();
                buffer.setSample(0, 10, 1.0f);
                buffer.setSample(1, 10, 0.5f);
                process(proc, buffer, {512});

                // Net of the reported latency, the shift is the knob's value (or nothing while off).
                const auto at = 10 + latency + (on ? samples : 0);
                for (int i = 0; i < 2048; ++i)
                {
                    REQUIRE(buffer.getSample(0, i) == (i == at ? 1.0f : 0.0f));
                    REQUIRE(buffer.getSample(1, i) == (i == at ? 0.5f : 0.0f));
                }
            }
}

TEST_CASE("in Constant mode the reported latency matches the measured offset, and follows the mode", "[processor]")
{
    for (const auto fs : {44100.0, 48000.0, 96000.0})
        for (const auto on : {false, true})
        {
            INFO("fs " << fs << ", delay " << (on ? "on" : "off"));
            PhaseAlignProcessor proc;
            setParam(proc, id::delayMs, (float)(-37 * 1000.0 / fs));
            setParam(proc, id::delayOn, on ? 1.0f : 0.0f);
            setParam(proc, id::phaseMode, 2.0f); // Constant at 0 degrees: the input delayed by L, bit-exact
            proc.prepareToPlay(fs, 512);

            const auto latency = proc.getLatencySamples();
            CHECK(latency == (on ? delayLatency(fs) : 0) + pa::dsp::ConstantRotator::latencyFor(fs));

            juce::AudioBuffer<float> buffer(4, 2 * latency + 1024);
            buffer.clear();
            buffer.setSample(0, 10, 1.0f);
            process(proc, buffer, {512});
            const auto at = 10 + latency + (on ? -37 : 0);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                REQUIRE(buffer.getSample(0, i) == (i == at ? 1.0f : 0.0f));

            // Leaving and entering Constant reports the new latency at once (from the message thread).
            setParam(proc, id::phaseMode, 1.0f);
            CHECK(proc.getLatencySamples() == (on ? delayLatency(fs) : 0) + hiLoLatency(fs));
            setParam(proc, id::phaseMode, 2.0f);
            CHECK(proc.getLatencySamples() == latency);
        }
}

TEST_CASE("net of the reported latency, a fractional setting shifts by the knob's value to 0.1 sample", "[processor]")
{
    for (const auto fs : {44100.0, 48000.0, 96000.0})
        for (const auto ms : {-4.0f, -2.345f, -0.002f, 1.0f, 3.21f})
        {
            INFO("fs " << fs << ", " << ms << " ms");
            PhaseAlignProcessor proc;
            setParam(proc, id::delayMs, ms);
            setParam(proc, id::delayOn, 1.0f);
            setParam(proc, id::phaseOn, 0.0f); // the delay alone (Hi/Lo's halfbands have their own small ripple)
            proc.prepareToPlay(fs, 512);
            const auto shift = proc.getLatencySamples() + delayInSamples(ms, fs); // samples, one decimal

            const auto w = 2.0 * M_PI * 1000.0 / fs;
            juce::AudioBuffer<float> buffer(4, 8192);
            for (int ch = 0; ch < 4; ++ch)
                for (int i = 0; i < 8192; ++i)
                    buffer.setSample(ch, i, (float)(0.5 * std::sin(w * i)));
            process(proc, buffer, {512});
            for (int i = 2048; i < 8192; ++i)
                REQUIRE(buffer.getSample(0, i) == Approx(0.5 * std::sin(w * (i - shift))).margin(2e-4));
        }
}

TEST_CASE("switching the delay on and off is click-free and updates the reported latency", "[processor]")
{
    for (const auto fs : {44100.0, 96000.0})
    {
        INFO("fs " << fs);
        const auto block = 256;
        const auto lmax = delayLatency(fs), h = hiLoLatency(fs);
        const auto d = -44; // whole samples, so the end can be checked exactly
        PhaseAlignProcessor proc;
        setParam(proc, id::delayMs, (float)(d * 1000.0 / fs));
        setParam(proc, id::phaseOn, 0.0f); // so the end can be checked exactly
        proc.prepareToPlay(fs, block);
        CHECK(proc.getLatencySamples() == h);

        // A low sine: its own sample-to-sample step is small, so any hard switch would stand out.
        const auto freq = 100.0, amplitude = 0.5;
        const auto length = (int)(1.5 * fs) / block * block;
        juce::AudioBuffer<float> input(4, length);
        for (int ch = 0; ch < 4; ++ch)
            for (int i = 0; i < length; ++i)
                input.setSample(ch, i, (float)(amplitude * std::sin(2.0 * M_PI * freq * i / fs)));
        auto out = input;

        // Toggled every 0.2 s from the message thread, as the editor or host would; the last off is reversed one block
        // later, part way through its fade-out.
        std::vector<std::pair<int, bool>> toggles;
        for (int k = 1; k <= 6; ++k)
            toggles.push_back({(int)(0.2 * k * fs) / block * block, k % 2 == 1});
        REQUIRE(block < pa::dsp::latencyFadeSamples(fs));
        toggles.push_back({toggles.back().first + block, true});

        juce::MidiBuffer midi;
        size_t next = 0;
        for (int pos = 0; pos < length; pos += block)
        {
            while (next < toggles.size() && toggles[next].first <= pos)
            {
                setParam(proc, id::delayOn, toggles[next].second ? 1.0f : 0.0f);
                CHECK(proc.getLatencySamples() == (toggles[next].second ? lmax : 0) + h);
                ++next;
            }
            juce::AudioBuffer<float> b(out.getArrayOfWritePointers(), 4, pos, block);
            proc.processBlock(b, midi);
        }

        const auto naturalStep = 2.0 * M_PI * freq / fs * amplitude;
        const auto limit = 1.05 * naturalStep + amplitude / pa::dsp::latencyFadeSamples(fs);
        double worst = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 1; i < length; ++i)
                worst = std::max(worst, (double)std::abs(out.getSample(ch, i) - out.getSample(ch, i - 1)));
        CHECK(worst <= limit);

        // Ends on: the input delayed by Lmax + d (and Hi/Lo's latency), exactly.
        for (int i = length - block; i < length; ++i)
            REQUIRE(out.getSample(0, i) == input.getSample(0, i - lmax - d - h));
    }
}

TEST_CASE("host bypass fades out to a bit-exact pass-through, delayed by the latency, and fades back in", "[processor]")
{
    const auto fs = 48000.0;
    const auto fadeSamples = (int)(pa::dsp::crossfadeMs * fs / 1000.0);
    for (const auto on : {false, true})
    {
        INFO("delay " << (on ? "on" : "off"));
        PhaseAlignProcessor proc;
        setParam(proc, id::delayMs, 1.0f);
        setParam(proc, id::delayOn, on ? 1.0f : 0.0f);
        setParam(proc, id::polarity, 1.0f);
        setParam(proc, id::phaseOn, 0.0f); // so the DC check is exact
        proc.prepareToPlay(fs, 256);
        const auto latency = proc.getLatencySamples();
        CHECK(latency == (on ? delayLatency(fs) : 0) + hiLoLatency(fs));

        auto active = noiseBuffer(4800, 1);
        process(proc, active, {256});

        // Bypassed for longer than the fade: after it (and the latency, since the polarity fades ahead of the delay),
        // the main channels are the input delayed by the latency, exactly.
        const auto input = noiseBuffer(3 * fadeSamples, 2);
        auto bypassed = input;
        process(proc, bypassed, {256}, true);
        CHECK(proc.getLatencySamples() == latency);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = fadeSamples + latency; i < bypassed.getNumSamples(); ++i)
                REQUIRE(bypassed.getSample(ch, i) == input.getSample(ch, i - latency));

        // Leaving bypass ramps the polarity back in rather than flipping: DC passes through 0 on its way to -1 (the
        // delay fades in at the same time, so the zero crossing isn't exactly half way). With the delay on, the first
        // samples out are still the bypassed noise, until the taps have passed it.
        const auto first = latency + (on ? (int)delayInSamples(1.0, fs) : 0); // and Hi/Lo's latency has passed
        juce::AudioBuffer<float> dc(4, 2 * fadeSamples);
        for (int ch = 0; ch < 4; ++ch)
            juce::FloatVectorOperations::fill(dc.getWritePointer(ch), 1.0f, dc.getNumSamples());
        process(proc, dc, {256});
        CHECK(dc.getSample(0, first) > 0.95f);
        float smallest = 1.0f;
        for (int i = first; i < fadeSamples + first; ++i)
            smallest = std::min(smallest, std::abs(dc.getSample(0, i)));
        CHECK(smallest < 0.001f);
        CHECK(dc.getSample(0, 2 * fadeSamples - 1) == -1.0f);
    }
}

TEST_CASE("any host block size works, including larger than prepared and variable", "[processor]")
{
    const auto fs = 44100.0;
    const auto input = noiseBuffer(30000, 3);

    const auto render = [&](const std::vector<int>& sizes)
    {
        PhaseAlignProcessor proc;
        setParam(proc, id::delayMs, 0.5f);
        proc.prepareToPlay(fs, 64);
        auto b = input;
        process(proc, b, sizes);
        return b;
    };

    const auto reference = render({64});
    for (const auto& sizes : std::vector<std::vector<int>>{{1}, {4096}, {1, 4096, 7, 333, 64, 2048}})
    {
        const auto out = render(sizes);
        for (int ch = 0; ch < 4; ++ch) // the sidechain channels are left alone too
            for (int i = 0; i < out.getNumSamples(); ++i)
                REQUIRE(out.getSample(ch, i) == reference.getSample(ch, i));
    }
}

//==============================================================================
TEST_CASE("M4: a delayed copy as the sidechain reads +1 once the delay knob aligns it, in all three modes",
          "[processor][meter]")
{
    const auto fs = 48000.0;
    const auto block = 512;
    // +50: the sidechain is 50 samples (1.04 ms) later than this track; -50: it is earlier, so the knob goes negative.
    // The phase stage is on at 0 degrees; Constant adds its latency, which the meter aligns too.
    struct Case
    {
        int d;
        float mode;
    };
    for (const auto& c : {Case{50, 0.0f}, Case{-50, 0.0f}, Case{50, 1.0f}, Case{-50, 2.0f}})
    {
        const auto d = c.d;
        const auto mode = c.mode;
        INFO("offset " << d << ", mode " << mode);
        PhaseAlignProcessor proc;
        setParam(proc, id::delayOn, 1.0f); // its 4 ms latency: the meter aligns its input and sidechain to the output
        setParam(proc, id::phaseMode, mode);
        proc.prepareToPlay(fs, block);
        auto& capture = proc.getMeterCapture();
        capture.setActive(true); // what the meter screen does while it is showing

        pa::meter::CorrelationAnalyser analyser;
        analyser.prepare(fs);
        std::vector<float> streams[pa::meter::MeterCapture::numStreams];
        for (auto& s : streams)
            s.resize((size_t)pa::meter::MeterCapture::capacity);
        float* dest[] = {streams[0].data(), streams[1].data(), streams[2].data(), streams[3].data(), streams[4].data()};

        std::mt19937 rng(9);
        std::normal_distribution<float> dist(0.0f, 0.2f);
        std::vector<float> history((size_t)std::abs(d), 0.0f); // the source's last |d| samples
        size_t h = 0;

        const auto run = [&](double seconds)
        {
            juce::MidiBuffer midi;
            for (int done = 0; done < (int)(seconds * fs); done += block)
            {
                juce::AudioBuffer<float> b(4, block);
                for (int i = 0; i < block; ++i)
                {
                    const auto x = dist(rng);
                    const auto late = history[h];
                    history[h] = x;
                    h = (h + 1) % history.size();
                    const auto track = d > 0 ? x : late, sidechain = d > 0 ? late : x;
                    b.setSample(0, i, track);
                    b.setSample(1, i, track);
                    b.setSample(2, i, sidechain);
                    b.setSample(3, i, sidechain);
                }
                proc.processBlock(b, midi);
                const auto n = capture.pull(dest, pa::meter::MeterCapture::capacity);
                analyser.process(dest[0], dest[1], dest[2], n);
            }
        };

        run(2.0);
        CHECK(analyser.overallProcessed() < 0.5f); // not aligned yet
        // Knob at 0: no net shift, so the output reads what the input does (the capture lines them up).
        CHECK(analyser.overallProcessed() == Approx(analyser.overallUnprocessed()).margin(0.02));

        setParam(proc, id::delayMs, (float)(1000.0 * d / fs)); // the delay knob, set to the peak's reading
        run(8.0); // the slow average (R23) has to forget the 2 s that weren't aligned
        CHECK(analyser.overallProcessed() > 0.999f);
        for (const auto r : analyser.bandsProcessed())
            if (! std::isnan(r))
                REQUIRE(r > 0.99f);
        CHECK(analyser.overallUnprocessed() < 0.5f); // the input as it was
    }
}

TEST_CASE("nothing is captured while the editor is closed, hidden, or the meter is off", "[processor][meter]")
{
    PhaseAlignProcessor proc;
    proc.prepareToPlay(48000.0, 512);
    auto& capture = proc.getMeterCapture();
    const auto processSome = [&]
    {
        auto b = noiseBuffer(2048, 4);
        process(proc, b, {512});
    };

    processSome();
    CHECK_FALSE(capture.isActive());
    CHECK(capture.getNumReady() == 0);

    {
        // Open but not on screen (meter on): still nothing.
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        processSome();
        CHECK_FALSE(capture.isActive());
        CHECK(capture.getNumReady() == 0);
    }
    CHECK_FALSE(capture.isActive());
}

//==============================================================================
TEST_CASE("a sidechain that is a copy of the track's own input counts as no sidechain (Logic with no source chosen)",
          "[processor][meter]")
{
    const auto fs = 48000.0;
    const auto block = 512;
    PhaseAlignProcessor proc;
    proc.prepareToPlay(fs, block);
    auto& capture = proc.getMeterCapture();
    capture.setActive(true); // what the meter screen does while it is showing

    std::mt19937 rng(21);
    std::normal_distribution<float> dist(0.0f, 0.2f);
    juce::MidiBuffer midi;
    const auto run = [&](double seconds, const std::function<void(juce::AudioBuffer<float>&)>& fillSidechain)
    {
        for (int done = 0; done < (int)(seconds * fs); done += block)
        {
            juce::AudioBuffer<float> b(4, block);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < block; ++i)
                    b.setSample(ch, i, dist(rng));
            fillSidechain(b);
            proc.processBlock(b, midi);
        }
    };
    const auto copyOfInput = [](juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < 2; ++ch)
            b.copyFrom(2 + ch, 0, b, ch, 0, b.getNumSamples());
    };
    const auto otherSignal = [&](juce::AudioBuffer<float>& b)
    {
        for (int ch = 2; ch < 4; ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample(ch, i, dist(rng));
    };

    // A real, different sidechain: present.
    run(0.5, otherSignal);
    CHECK(capture.hasSidechain());

    // The host's copy of the track itself: not a sidechain, once it has been like that for a quarter of a second.
    run(0.1, copyOfInput);
    CHECK(capture.hasSidechain()); // not yet decided
    run(0.5, copyOfInput);
    CHECK_FALSE(capture.hasSidechain());

    // A different signal again: present at once.
    run(0.05, otherSignal);
    CHECK(capture.hasSidechain());

    // Silence in both is trivially "identical" but proves nothing: it doesn't start the count ...
    run(0.5, copyOfInput);
    CHECK_FALSE(capture.hasSidechain());
    run(0.5, otherSignal);
    CHECK(capture.hasSidechain());
    for (int done = 0; done < (int)(1.0 * fs); done += block)
    {
        juce::AudioBuffer<float> silence(4, block);
        silence.clear();
        proc.processBlock(silence, midi);
    }
    CHECK(capture.hasSidechain()); // ... so a silent pair is left as it was (the screen's own silence timer handles it)

    // Not capturing: nothing is tracked, and it starts from scratch.
    capture.setActive(false);
    run(1.0, copyOfInput);
    CHECK(capture.hasSidechain()); // (capture off: the answer isn't used, and nothing was counted)
}
