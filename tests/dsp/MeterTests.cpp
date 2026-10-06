#include "AllocationCounter.h"
#include "meter/CorrelationAnalyser.h"
#include "meter/MeterCapture.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <vector>

using Catch::Approx;
using pa::meter::CorrelationAnalyser;
using pa::meter::MeterCapture;

namespace
{
std::vector<float> noise(int length, unsigned seed, float amplitude = 0.3f)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(0.0f, amplitude);
    std::vector<float> x((size_t)length);
    for (auto& v : x)
        v = dist(rng);
    return x;
}

std::vector<float> delayed(const std::vector<float>& x, int d, float gain = 1.0f)
{
    std::vector<float> y(x.size(), 0.0f);
    for (size_t i = (size_t)d; i < x.size(); ++i)
        y[i] = gain * x[i - (size_t)d];
    return y;
}

// Feeds the streams in 1000-sample chunks, as the screen's 30 Hz pulls would.
void feed(CorrelationAnalyser& a, const std::vector<float>& in, const std::vector<float>& out,
          const std::vector<float>& sc)
{
    for (size_t pos = 0; pos < in.size(); pos += 1000)
    {
        const auto n = (int)std::min<size_t>(1000, in.size() - pos);
        a.process(in.data() + pos, out.data() + pos, sc.data() + pos, n);
    }
}

double minMeasured(const std::vector<float>& v)
{
    double m = 2.0;
    for (const auto x : v)
        if (! std::isnan(x))
            m = std::min(m, (double)x);
    return m;
}
} // namespace

TEST_CASE("the FFT size scales with the rate: 8192 at 44.1/48 kHz", "[meter]")
{
    CHECK(CorrelationAnalyser::fftSizeFor(44100.0) == 8192);
    CHECK(CorrelationAnalyser::fftSizeFor(48000.0) == 8192);
    CHECK(CorrelationAnalyser::fftSizeFor(88200.0) == 16384);
    CHECK(CorrelationAnalyser::fftSizeFor(96000.0) == 16384);
    CHECK(CorrelationAnalyser::fftSizeFor(192000.0) == 32768);
}

TEST_CASE("a delayed copy reads +1 once aligned, and the time view finds the delay", "[meter]")
{
    for (const auto fs : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        INFO("fs " << fs);
        const auto d = (int)std::lround(0.0013 * fs); // 1.3 ms
        const auto length = (int)(3.0 * fs);
        const auto source = noise(length, 1);
        const auto sidechain = delayed(source, d, 0.5f); // later and quieter: level doesn't matter
        const auto aligned = delayed(source, d);

        CorrelationAnalyser a;
        a.prepare(fs);
        feed(a, source, aligned, sidechain);

        CHECK(a.overallProcessed() > 0.999f);
        CHECK(minMeasured(a.curveProcessed()) > 0.99);

        // Unaligned: r(f) follows cos(2 pi f d) where the 1/6-octave window is narrow against the comb.
        const auto& hz = a.curveFrequencies();
        for (size_t i = 0; i < hz.size(); ++i)
            if (hz[i] > 40.0f && hz[i] < 150.0f)
            {
                INFO(hz[i] << " Hz");
                CHECK(a.curveUnprocessed()[i] == Approx(std::cos(2.0 * M_PI * hz[i] * d / fs)).margin(0.08));
            }
        CHECK(std::abs(a.overallUnprocessed()) < 0.3f);

        a.computeLag();
        CHECK(a.lagPeakUnprocessed().clear);
        CHECK(a.lagPeakUnprocessed().lagMs == Approx(1000.0 * d / fs).margin(0.02));
        CHECK(a.lagPeakUnprocessed().value > 0.9f);
        CHECK(a.lagPeakProcessed().lagMs == Approx(0.0).margin(0.02));
        CHECK(a.lagPeakProcessed().value > 0.9f);
    }
}

TEST_CASE("an inverted sidechain reads -1 and peaks at 0 ms, negative", "[meter]")
{
    const auto fs = 48000.0;
    const auto source = noise((int)(2.0 * fs), 2);
    auto inverted = source;
    for (auto& v : inverted)
        v = -v;

    CorrelationAnalyser a;
    a.prepare(fs);
    feed(a, source, source, inverted);
    CHECK(a.overallProcessed() < -0.999f);
    a.computeLag();
    CHECK(a.lagPeakProcessed().lagMs == Approx(0.0).margin(0.01));
    CHECK(a.lagPeakProcessed().value < -0.9f);
}

TEST_CASE("signals below the gate are not measured; sidechain silence is timed", "[meter]")
{
    const auto fs = 48000.0;
    const auto length = (int)(2.0 * fs);
    const auto loud = noise(length, 3);
    const auto quiet = noise(length, 4, 3.0e-5f); // about -90 dBFS
    const std::vector<float> silence((size_t)length, 0.0f);

    CorrelationAnalyser a;
    a.prepare(fs);
    feed(a, loud, loud, quiet);
    CHECK(std::isnan(a.overallProcessed()));
    CHECK(std::isnan(a.curveProcessed()[100]));
    CHECK(a.sidechainSilentSeconds() > 1.5);

    feed(a, loud, loud, loud);
    CHECK(a.overallProcessed() > 0.99f);
    CHECK(a.sidechainSilentSeconds() == 0.0);

    feed(a, loud, loud, silence);
    CHECK(a.sidechainSilentSeconds() > 1.5);
}

TEST_CASE("averaging settles within about a second", "[meter]")
{
    // P4: tau = max(0.3 s, 8 cycles) takes r from +1 to below -0.8 in about 0.75 s after a polarity step.
    const auto fs = 48000.0;
    const auto source = noise((int)(4.0 * fs), 5);
    auto flipped = source;
    for (size_t i = (size_t)(2.0 * fs); i < flipped.size(); ++i)
        flipped[i] = -flipped[i];

    CorrelationAnalyser a;
    a.prepare(fs);
    feed(a, std::vector<float>(source.begin(), source.begin() + (long)(2.0 * fs)),
         std::vector<float>(source.begin(), source.begin() + (long)(2.0 * fs)),
         std::vector<float>(flipped.begin(), flipped.begin() + (long)(2.0 * fs)));
    CHECK(a.overallProcessed() > 0.99f);

    const auto rest = [&](double from, double to)
    { return std::vector<float>(source.begin() + (long)(from * fs), source.begin() + (long)(to * fs)); };
    const auto restFlipped = [&](double from, double to)
    { return std::vector<float>(flipped.begin() + (long)(from * fs), flipped.begin() + (long)(to * fs)); };
    feed(a, rest(2.0, 2.4), rest(2.0, 2.4), restFlipped(2.0, 2.4));
    CHECK(a.overallProcessed() > -0.8f); // not yet
    feed(a, rest(2.4, 3.2), rest(2.4, 3.2), restFlipped(2.4, 3.2));
    CHECK(a.overallProcessed() < -0.8f);
}

//==============================================================================
TEST_CASE("capture: nothing is queued while inactive; streams stay aligned when full", "[meter]")
{
    MeterCapture c;
    float in[32], out[32], sc[32];
    for (int i = 0; i < 32; ++i)
    {
        in[i] = (float)i;
        out[i] = (float)(100 + i);
        sc[i] = (float)(200 + i);
    }

    c.push(in, out, sc, 32);
    CHECK(c.getNumReady() == 0);

    c.setActive(true);
    const pa::test::AllocationProbe probe;
    for (int k = 0; k < 3000; ++k) // 96000 samples: more than the FIFO holds
        c.push(in, out, sc, 32);
    CHECK(probe.allocations() == 0);
    CHECK(c.getNumReady() == MeterCapture::capacity - 1);

    std::vector<float> a(64), b(64), s(64);
    float* dest[] = {a.data(), b.data(), s.data()};
    REQUIRE(c.pull(dest, 64) == 64);
    for (int i = 0; i < 64; ++i)
    {
        CHECK(b[(size_t)i] == a[(size_t)i] + 100.0f);
        CHECK(s[(size_t)i] == a[(size_t)i] + 200.0f);
    }

    // Stopping and starting again discards what was left.
    c.setActive(false);
    c.setActive(true);
    CHECK(c.getNumReady() == 0);
}

TEST_CASE("capture delays the input and sidechain by the latency, so they line up with the output", "[meter]")
{
    MeterCapture c;
    c.prepareAlignment(200, 32);
    c.setActive(true);

    // A ramp source; the output is it `latency` samples later, as the chain makes it.
    const auto latency = 192;
    std::vector<float> src(4000);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = (float)(i + 1);
    const auto out = [&](int i) { return i >= latency ? src[(size_t)(i - latency)] : 0.0f; };

    const pa::test::AllocationProbe probe;
    float o[32];
    for (int pos = 0; pos < 4000; pos += 32)
    {
        for (int i = 0; i < 32; ++i)
            o[i] = out(pos + i);
        c.push(src.data() + pos, o, src.data() + pos, 32, latency);
    }
    CHECK(probe.allocations() == 0);

    std::vector<float> a(4000), b(4000), s(4000);
    float* dest[] = {a.data(), b.data(), s.data()};
    REQUIRE(c.pull(dest, 4000) == 4000);
    for (size_t i = 0; i < 4000; ++i)
    {
        REQUIRE(a[i] == b[i]);
        REQUIRE(s[i] == b[i]);
    }

    // Restarting capture clears the alignment history: no stale samples from the last run.
    c.setActive(false);
    c.push(src.data(), o, src.data(), 32, latency); // inactive: ignored
    c.setActive(true);
    c.push(src.data(), o, src.data(), 32, latency);
    REQUIRE(c.pull(dest, 32) == 32);
    for (size_t i = 0; i < 32; ++i)
        REQUIRE(a[i] == 0.0f);
}

TEST_CASE("the time view reads a fractional offset to within 0.01 sample", "[meter]")
{
    // A long Kaiser-windowed sinc gives the sidechain an accurate fractional delay, independent of the plugin's
    // kernels.
    const auto i0 = [](double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 60; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    const auto fractionalDelay = [&](const std::vector<float>& x, double delay)
    {
        constexpr int half = 255;
        std::vector<double> h(2 * half + 1);
        const auto whole = (int)std::floor(delay);
        const auto frac = delay - whole;
        for (int k = -half; k <= half; ++k)
        {
            const auto t = k - frac;
            const auto r = t / (half + 1.0);
            const auto sinc = std::abs(t) < 1e-12 ? 1.0 : std::sin(M_PI * t) / (M_PI * t);
            h[(size_t)(k + half)] = sinc * i0(10.0 * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0(10.0);
        }
        std::vector<float> y(x.size(), 0.0f);
        for (size_t n = 0; n < x.size(); ++n)
        {
            double acc = 0.0;
            for (int k = -half; k <= half; ++k)
            {
                const auto idx = (long)n - whole - k;
                if (idx >= 0 && idx < (long)x.size())
                    acc += h[(size_t)(k + half)] * x[(size_t)idx];
            }
            y[n] = (float)acc;
        }
        return y;
    };

    for (const auto fs : {44100.0, 96000.0})
        for (const auto offset : {12.0, 12.3, -7.5, 30.8})
        {
            INFO("fs " << fs << ", offset " << offset << " samples");
            const auto source = noise((int)(2.5 * fs), 21);
            const auto track = fractionalDelay(source, 300.0);
            const auto sidechain = fractionalDelay(source, 300.0 + offset);
            CorrelationAnalyser a;
            a.prepare(fs);
            feed(a, track, track, sidechain);
            a.computeLag();
            const auto peak = a.lagPeakUnprocessed();
            CHECK(peak.clear);
            CHECK(peak.lagMs * fs / 1000.0 == Approx(offset).margin(0.01));
        }
}

TEST_CASE("offsets beyond the time view are found coarsely and read as out of the delay's reach", "[meter]")
{
    // Positive: the sidechain is later; negative: the track is (it needs a negative delay).
    for (const auto fs : {44100.0, 48000.0, 96000.0})
        for (const auto ms : {3.9, -3.9, 4.5, -4.5, 7.0, -12.0, 25.0, -38.0})
        {
            INFO("fs " << fs << ", " << ms << " ms");
            const auto d = (int)std::lround(std::abs(ms) * fs / 1000.0);
            const auto length = (int)(3.0 * fs);
            const auto source = noise(length, 7);
            const auto late = delayed(source, d);
            const auto& track = ms > 0.0 ? source : late;
            const auto& sidechain = ms > 0.0 ? late : source;

            CorrelationAnalyser a;
            a.prepare(fs);
            feed(a, track, track, sidechain);
            a.computeLag();
            const auto peak = a.inputPeak();
            const auto expected = (ms > 0.0 ? 1.0 : -1.0) * 1000.0 * d / fs;
            CHECK(peak.clear);
            CHECK(peak.coarse == (std::abs(ms) > CorrelationAnalyser::lagRangeMs));
            CHECK(peak.lagMs == Approx(expected).margin(peak.coarse ? 1000.0 / fs : 0.02));
            CHECK(a.inputOutOfReach(4.0) == (std::abs(ms) > 4.0));
        }

    // Nothing correlated (independent noise): no clear peak anywhere, so nothing is said to be out of reach.
    const auto fs = 48000.0;
    CorrelationAnalyser a;
    a.prepare(fs);
    const auto x = noise((int)(3.0 * fs), 8), y = noise((int)(3.0 * fs), 9);
    feed(a, x, x, y);
    a.computeLag();
    CHECK_FALSE(a.inputPeak().clear);
    CHECK_FALSE(a.inputOutOfReach(4.0));
}

TEST_CASE("fast averaging follows a change sooner than slow", "[meter]")
{
    const auto fs = 48000.0;
    const auto source = noise((int)(4.0 * fs), 5);
    auto inverted = source;
    for (auto& v : inverted)
        v = -v;

    CorrelationAnalyser slow, fast;
    slow.prepare(fs);
    fast.prepare(fs);
    fast.setSpeed(CorrelationAnalyser::Speed::fast);
    CHECK(fast.getSpeed() == CorrelationAnalyser::Speed::fast);

    // 3 s of a perfect match, then 0.3 s of the opposite.
    feed(slow, source, source, source);
    feed(fast, source, source, source);
    const std::vector<float> head(source.begin(), source.begin() + (int)(0.3 * fs));
    const std::vector<float> flipped(inverted.begin(), inverted.begin() + (int)(0.3 * fs));
    feed(slow, head, head, flipped);
    feed(fast, head, head, flipped);
    CHECK(slow.overallProcessed() > fast.overallProcessed());
    CHECK(fast.overallProcessed() < 0.0f); // fast has turned over, slow hasn't
    CHECK(slow.overallProcessed() > 0.0f);
}

TEST_CASE("preview: the input as it would read with the delay knob set; clearing restores the output", "[meter]")
{
    for (const auto fs : {44100.0, 48000.0, 96000.0})
    {
        INFO("fs " << fs);
        const auto d = (int)std::lround(0.0013 * fs);
        const auto source = noise((int)(3.0 * fs), 6);
        const auto sidechain = delayed(source, d);

        CorrelationAnalyser a;
        a.prepare(fs);
        feed(a, source, source, sidechain); // the plugin is doing nothing
        const auto before = a.overallProcessed();
        const auto beforeInput = a.overallUnprocessed();
        CHECK(std::abs(before) < 0.3f);

        a.setPreviewDelayMs(1000.0 * d / fs);
        CHECK(a.isPreviewing());
        CHECK(a.overallProcessed() > 0.999f);
        CHECK(minMeasured(a.curveProcessed()) > 0.99);
        CHECK(a.overallUnprocessed() == beforeInput); // the input trace never moves

        // Half a millisecond off is worse than right.
        a.setPreviewDelayMs(1000.0 * d / fs + 0.5);
        CHECK(a.overallProcessed() < 0.9f);

        a.clearPreview();
        CHECK_FALSE(a.isPreviewing());
        CHECK(a.overallProcessed() == before);
    }
}

TEST_CASE("phase view: a delay is a slope, a polarity flip is 180, noise is not drawn", "[meter]")
{
    const auto fs = 48000.0;
    const auto d = 62; // 1.29 ms
    const auto source = noise((int)(3.0 * fs), 7);
    const auto sidechain = delayed(source, d);

    CorrelationAnalyser a;
    a.prepare(fs);
    feed(a, source, source, sidechain);
    const auto& hz = a.curveFrequencies();
    int checked = 0;
    for (size_t i = 0; i < hz.size(); ++i)
        if (hz[i] > 40.0f && hz[i] < 300.0f)
        {
            INFO(hz[i] << " Hz");
            REQUIRE_FALSE(std::isnan(a.phaseProcessed()[i]));
            auto expected = std::fmod(360.0 * hz[i] * d / fs + 180.0, 360.0) - 180.0;
            CHECK(a.phaseProcessed()[i] == Approx(expected).margin(8.0));
            CHECK(a.phaseProcessedCoherence()[i] > 0.9f);
            ++checked;
        }
    CHECK(checked > 20);

    // Previewing the right delay flattens it to 0 right across the spectrum, high frequencies included.
    a.setPreviewDelayMs(1000.0 * d / fs);
    for (size_t i = 0; i < hz.size(); ++i)
        if (! std::isnan(a.phaseProcessed()[i]))
            CHECK(std::abs(a.phaseProcessed()[i]) < 5.0f);
    CHECK_FALSE(std::isnan(a.phaseProcessed()[hz.size() - 20])); // near 15 kHz: only the preview keeps it coherent
    a.clearPreview();

    auto inverted = source;
    for (auto& v : inverted)
        v = -v;
    CorrelationAnalyser b;
    b.prepare(fs);
    feed(b, source, source, inverted);
    for (size_t i = 20; i < hz.size() - 20; i += 10)
        CHECK(std::abs(b.phaseProcessed()[i]) > 170.0f);

    CorrelationAnalyser c;
    c.prepare(fs);
    feed(c, source, source, noise((int)(3.0 * fs), 8));
    int drawn = 0;
    for (const auto v : c.phaseProcessed())
        drawn += std::isnan(v) ? 0 : 1;
    CHECK(drawn < (int)hz.size() / 4);
}

TEST_CASE("bands view: six bands, +1 when aligned, the delayed input comb-averages, preview follows", "[meter]")
{
    const auto fs = 48000.0;
    const auto d = 62;
    const auto source = noise((int)(3.0 * fs), 9);
    const auto sidechain = delayed(source, d);

    CorrelationAnalyser a;
    a.prepare(fs);
    feed(a, source, delayed(source, d), sidechain); // output aligned, input not
    REQUIRE((int)a.bandsProcessed().size() == CorrelationAnalyser::numBands);
    for (const auto v : a.bandsProcessed())
        CHECK(v > 0.99f);
    // The unaligned input: the lowest band (20-100 Hz, d = 1.3 ms) is still nearly in phase, the top ones comb away.
    CHECK(a.bandsUnprocessed()[0] > 0.8f);
    CHECK(std::abs(a.bandsUnprocessed()[5]) < 0.3f);

    CorrelationAnalyser b;
    b.prepare(fs);
    feed(b, source, source, sidechain);
    CHECK(b.bandsProcessed()[5] < 0.3f);
    b.setPreviewDelayMs(1000.0 * d / fs);
    for (const auto v : b.bandsProcessed())
        CHECK(v > 0.99f);
}

namespace
{
// A kick: a decaying sine sweeping down from `startHz` to `endHz`, a short click at the front, every `period` samples.
std::vector<float> kickTrain(int length, double fs, double startHz, double endHz, int period, unsigned seed)
{
    std::mt19937 rng(100); // the beater click is the same in both kicks; the body (pitch, sweep) is what differs
    (void)seed;
    std::normal_distribution<float> dist(0.0f, 1.0f);
    std::vector<float> x((size_t)length, 0.0f);
    const auto len = (int)(0.25 * fs);
    for (int start = 0; start + len < length; start += period)
    {
        double phase = 0.0;
        for (int n = 0; n < len; ++n)
        {
            const auto t = n / fs;
            const auto hz = endHz + (startHz - endHz) * std::exp(-t / 0.03);
            phase += 2.0 * M_PI * hz / fs;
            const auto click = n < 40 ? 0.3f * dist(rng) * (1.0f - n / 40.0f) : 0.0f;
            x[(size_t)(start + n)] += (float)(0.8 * std::exp(-t / 0.08) * std::sin(phase)) + click;
        }
    }
    return x;
}
} // namespace

TEST_CASE("a kick against a different kick sample: the time view finds the offset", "[meter]")
{
    for (const auto speed : {CorrelationAnalyser::Speed::slow, CorrelationAnalyser::Speed::fast})
    {
        const auto fs = 48000.0;
        const auto d = 58; // 1.2 ms
        const auto length = (int)(6.0 * fs);
        const auto period = (int)(0.5 * fs);
        const auto mic = kickTrain(length, fs, 120.0, 50.0, period, 1);
        auto sample = kickTrain(length, fs, 90.0, 45.0, period, 2); // another kick: different click, pitch, tail
        auto sidechain = delayed(sample, d);
        const auto floorNoise = noise(length, 3, 0.003f);
        for (size_t i = 0; i < sidechain.size(); ++i)
            sidechain[i] += floorNoise[i];

        CorrelationAnalyser a;
        a.prepare(fs);
        a.setSpeed(speed);
        feed(a, mic, mic, sidechain);
        a.computeLag();
        const auto peak = a.inputPeak();
        INFO("fast " << (speed == CorrelationAnalyser::Speed::fast) << " peak " << peak.lagMs << " ms value "
                     << peak.value << " clear " << peak.clear);
        if (speed == CorrelationAnalyser::Speed::slow)
        {
            CHECK(peak.clear);
            CHECK(peak.lagMs == Approx(1000.0 * d / fs).margin(0.15));
        }
        else if (peak.clear) // fast has about two frames to go on: it may decline, but not claim a wrong offset
            CHECK(peak.lagMs == Approx(1000.0 * d / fs).margin(0.15));

        // The attacks, though, are what a kick is about: found by both speeds, with different bodies.
        const auto attack = a.attackInput();
        INFO("attack " << attack.lagMs << " ms value " << attack.value);
        CHECK(attack.clear);
        CHECK(attack.lagMs == Approx(1000.0 * d / fs).margin(0.1));
    }
}

TEST_CASE("fast: a delay on steady material is found, and the meter follows a change within about 0.3 s", "[meter]")
{
    for (const auto fs : {44100.0, 48000.0, 96000.0})
    {
        INFO("fs " << fs);
        const auto d = (int)std::lround(0.0013 * fs);
        const auto source = noise((int)(2.0 * fs), 11);
        CorrelationAnalyser a;
        a.prepare(fs);
        a.setSpeed(CorrelationAnalyser::Speed::fast);
        CHECK(a.getFftSize() == CorrelationAnalyser::fftSizeFor(fs, CorrelationAnalyser::Speed::fast));
        feed(a, source, source, delayed(source, d));
        a.computeLag();
        CHECK(a.inputPeak().clear);
        CHECK(a.inputPeak().lagMs == Approx(1000.0 * d / fs).margin(0.02));

        // From matched to inverted: reads negative within 0.3 s.
        auto inverted = source;
        for (auto& v : inverted)
            v = -v;
        const auto n = (int)(0.3 * fs);
        const std::vector<float> head(source.begin(), source.begin() + n), flipped(inverted.begin(), inverted.begin() + n);
        feed(a, head, head, head);
        feed(a, head, head, flipped);
        CHECK(a.overallProcessed() < 0.0f);
    }
}

// Hidden: the user's own kick pair (captures/, gitignored; raw float32 mono at 48 kHz written by a script). The
// attacks are 1.23 ms apart (onsets read off the waveforms: 1483.35 ms and 1484.58 ms); the sub bodies disagree.
TEST_CASE("the user's kick pair: the attack lag finds the onset offset", "[.kickfiles]")
{
    const auto load = [](const char* path)
    {
        std::vector<float> v;
        if (auto* f = std::fopen(path, "rb"))
        {
            std::fseek(f, 0, SEEK_END);
            v.resize((size_t)std::ftell(f) / sizeof(float));
            std::rewind(f);
            REQUIRE(std::fread(v.data(), sizeof(float), v.size(), f) == v.size());
            std::fclose(f);
        }
        return v;
    };
    const auto mic = load("captures/kick_mic.f32"), sample = load("captures/kick_smp.f32");
    REQUIRE_FALSE(mic.empty());
    const auto fs = 48000.0;
    for (const auto speed : {CorrelationAnalyser::Speed::slow, CorrelationAnalyser::Speed::fast})
    {
        CorrelationAnalyser a;
        a.prepare(fs);
        a.setSpeed(speed);
        const auto n = std::min(mic.size(), sample.size());
        int readings = 0, clear = 0;
        for (size_t pos = 0; pos < n; pos += 1600) // 30 Hz pulls
        {
            const auto m = (int)std::min<size_t>(1600, n - pos);
            a.process(mic.data() + pos, mic.data() + pos, sample.data() + pos, m);
            if (pos % (size_t)(fs * 3) < 1600 && pos > (size_t)(fs * 4))
            {
                a.computeLag();
                const auto w = a.inputPeak(), t = a.attackInput();
                ++readings;
                if (t.clear)
                {
                    ++clear;
                    CHECK(t.lagMs == Approx(1.23).margin(0.15)); // never a wrong one called clear
                }
                std::printf("%s t=%5.1fs  waveform %+7.2f ms (%.2f%s)  attack %+6.2f ms (%.2f%s)\n",
                            speed == CorrelationAnalyser::Speed::fast ? "fast" : "slow", pos / fs, w.lagMs, w.value,
                            w.clear ? " clear" : "", t.lagMs, t.value, t.clear ? " clear" : "");
            }
        }
        CHECK(clear >= readings - 2);
    }
}
