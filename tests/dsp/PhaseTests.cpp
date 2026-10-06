#include "AllocationCounter.h"
#include "dsp/Chain.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <random>
#include <vector>

// The phase stage (IMPLEMENTATION_PLAN 2.3, 2.4, M2): Hi/Lo is flat and its readout is the knob angle; Constant is a
// true rotation, exact at 0 and 180 degrees, with latency L.
using namespace pa::dsp;
using Catch::Approx;

namespace
{
ChainSettings phaseOnly(PhaseMode mode, double degrees)
{
    auto s = ChainSettings::neutral();
    s.phaseOn = true;
    s.phaseMode = mode;
    s.phaseDegrees = degrees;
    return s;
}

// The chain's impulse response at settled settings (mono).
std::vector<float> impulseResponse(double fs, const ChainSettings& s, int length)
{
    Chain chain;
    chain.prepare(fs, 1, 400);
    chain.reset(s);
    std::vector<float> x((size_t)length, 0.0f);
    x[0] = 1.0f;
    for (int pos = 0; pos < length; pos += 512)
    {
        float* p = x.data() + pos;
        chain.process(&p, 1, std::min(512, length - pos));
    }
    return x;
}

// H(f) of an impulse response, with a pure delay of `delay` samples removed.
std::complex<double> response(const std::vector<float>& h, double f, double fs, int delay = 0)
{
    std::complex<double> sum = 0.0;
    const auto w = 2.0 * M_PI * f / fs;
    for (size_t n = 0; n < h.size(); ++n)
        sum += (double)h[n] * std::polar(1.0, -w * ((double)n - delay));
    return sum;
}

double lagDegrees(std::complex<double> h)
{
    auto lag = -std::arg(h) * 180.0 / M_PI;
    while (lag < -1.0)
        lag += 360.0;
    return lag;
}

std::vector<double> testFrequencies(double fs)
{
    std::vector<double> f;
    for (auto x = 20.0; x <= std::min(20000.0, 0.45 * fs); x *= 1.25)
        f.push_back(x);
    return f;
}
} // namespace

TEST_CASE("Hi/Lo is flat within 0.001 dB and its readout is the knob angle within 0.1 degree", "[dsp][phase]")
{
    for (const auto fs : {44100.0, 48000.0, 96000.0, 192000.0})
        for (const auto mode : {PhaseMode::high, PhaseMode::low})
            for (const auto theta : {0.0, 10.0, 45.0, 90.0, 100.0, 135.0, 180.0})
            {
                INFO("fs " << fs << ", " << (mode == PhaseMode::high ? "Hi" : "Lo") << " " << theta);
                const auto h = impulseResponse(fs, phaseOnly(mode, theta), (int)(0.4 * fs));
                for (const auto f : testFrequencies(fs))
                    REQUIRE(std::abs(20.0 * std::log10(std::abs(response(h, f, fs)))) < 0.001);

                // The readout (plan 2.3): Hi, the lag at 150.1 Hz; Lo, the lag at 75.1 Hz up to 90 degrees, then 90
                // plus section 2's lag at 1502 Hz (section 1, fixed at 90 there, is taken off analytically).
                if (mode == PhaseMode::high)
                    CHECK(lagDegrees(response(h, 150.1, fs)) == Approx(theta).margin(0.1));
                else if (theta <= 90.0)
                    CHECK(lagDegrees(response(h, 75.1, fs)) == Approx(theta).margin(0.1));
                else
                {
                    const auto k1 = std::tan(M_PI / 4.0) / std::tan(M_PI * 75.1 / fs);
                    const auto section1 = 2.0 * std::atan(k1 * std::tan(M_PI * 1502.0 / fs)) * 180.0 / M_PI;
                    CHECK(lagDegrees(response(h, 1502.0, fs)) - section1 == Approx(theta - 90.0).margin(0.1));
                }
            }
}

TEST_CASE("Hi/Lo at 0 degrees is within 0.25 degree of the input up to 20 kHz", "[dsp][phase]")
{
    for (const auto fs : {44100.0, 96000.0})
        for (const auto mode : {PhaseMode::high, PhaseMode::low})
        {
            const auto h = impulseResponse(fs, phaseOnly(mode, 0.0), 8192);
            for (const auto f : testFrequencies(fs))
                REQUIRE(std::abs(lagDegrees(response(h, f, fs))) < 0.25);
        }
}

TEST_CASE("Constant: exact at 0 and 180 degrees, 90 degrees within 0.5 from 20 Hz to 20 kHz, latency L", "[dsp][phase]")
{
    for (const auto fs : {44100.0, 48000.0, 96000.0})
    {
        INFO("fs " << fs);
        const auto latency = ConstantRotator::latencyFor(fs);
        CHECK(latency == (HilbertFir::tapsFor(fs) - 1) / 2 - 1);
        CHECK(Chain::latencyFor(phaseOnly(PhaseMode::constant, 0.0), fs, 400) == latency);
        const auto length = 2 * latency + 4096;

        // 0 degrees: the input delayed by L, bit-exact; 180: its exact negation.
        for (const auto theta : {0.0, 180.0})
        {
            const auto h = impulseResponse(fs, phaseOnly(PhaseMode::constant, theta), length);
            for (int i = 0; i < length; ++i)
                REQUIRE(h[(size_t)i] == (i == latency ? (theta > 0.0 ? -1.0f : 1.0f) : 0.0f));
        }

        // 90 and 45: the angle relative to the delayed input, and the level (plan 2.4: 4097 taps at 48 kHz).
        for (const auto theta : {90.0, 45.0, 135.0})
        {
            INFO(theta << " degrees");
            const auto h = impulseResponse(fs, phaseOnly(PhaseMode::constant, theta), length);
            for (const auto f : testFrequencies(fs))
            {
                const auto r = response(h, f, fs, latency);
                REQUIRE(lagDegrees(r) == Approx(theta).margin(0.5));
                REQUIRE(std::abs(20.0 * std::log10(std::abs(r))) < (f < 40.0 ? 0.15 : 0.01));
            }
        }
    }
}

TEST_CASE("Constant restarted after a pause equals one that never stopped", "[dsp][phase]")
{
    // The convolver doesn't run while its output is multiplied by 0 (stage off, or settled at 0 or 180 degrees) and
    // rebuilds itself from its history when needed again; the result must be the same as if it had run throughout.
    const auto fs = 48000.0;
    const auto length = (int)(0.5 * fs);
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    std::vector<float> input((size_t)length);
    for (auto& v : input)
        v = dist(rng);

    const auto render = [&](ChainSettings first, int at, ChainSettings second, int block)
    {
        Chain chain;
        chain.prepare(fs, 1, 400);
        chain.reset(first);
        auto x = input;
        for (int pos = 0; pos < length;)
        {
            if (pos == at)
                chain.setSettings(second);
            auto n = std::min(block, length - pos);
            if (pos < at)
                n = std::min(n, at - pos);
            float* p = x.data() + pos;
            chain.process(&p, 1, n);
            pos += n;
        }
        return x;
    };

    const auto on90 = phaseOnly(PhaseMode::constant, 90.0);
    auto off90 = on90;
    off90.phaseOn = false;
    const auto at0 = phaseOnly(PhaseMode::constant, 0.0);
    const auto reference = render(on90, 0, on90, 512);

    // Off, then on mid-block: after the 50 ms crossfade, identical.
    const auto restarted = render(off90, 9001, on90, 300);
    const auto settledAt = 9001 + crossfadeSamples(fs) + 64;
    for (int i = settledAt; i < length; ++i)
        REQUIRE(restarted[(size_t)i] == Approx(reference[(size_t)i]).margin(1.0e-6));

    // Settled at 0 degrees (no convolution), then moving to 90: once there, identical.
    const auto moved = render(at0, 7000, on90, 128);
    const auto arrived = 7000 + (int)(ConstantRotator::smoothMs * 1.0e-3 * fs) + 64;
    for (int i = arrived; i < length; ++i)
        REQUIRE(moved[(size_t)i] == Approx(reference[(size_t)i]).margin(1.0e-6));
}

TEST_CASE("a Hi <-> Lo glide stays all-pass", "[dsp][phase]")
{
    // A steady sine keeps its level through the glide (and its reversal) within 0.25 dB. A crossfade between the two
    // modes' outputs instead would dip by cos(dphi / 2): 1.25 dB where they are 60 degrees apart, deeper beyond. (The
    // phase moving during a window shifts its RMS by about 0.1 dB, so this is no tighter.)
    const auto fs = 48000.0;
    for (const auto freq : {60.0, 400.0, 3000.0})
    {
        INFO(freq << " Hz");
        const auto length = (int)(0.6 * fs);
        std::vector<float> x((size_t)length);
        for (int i = 0; i < length; ++i)
            x[(size_t)i] = (float)(0.5 * std::sin(2.0 * M_PI * freq * i / fs));

        Chain chain;
        chain.prepare(fs, 1, 400);
        chain.reset(phaseOnly(PhaseMode::high, 120.0));
        const auto glideAt = (int)(0.2 * fs), reverseAt = glideAt + (int)(0.015 * fs);
        for (int pos = 0; pos < length; pos += 64)
        {
            if (pos == glideAt / 64 * 64)
                chain.setSettings(phaseOnly(PhaseMode::low, 120.0));
            if (pos == reverseAt / 64 * 64)
                chain.setSettings(phaseOnly(PhaseMode::high, 120.0));
            float* p = x.data() + pos;
            chain.process(&p, 1, std::min(64, length - pos));
        }
        // RMS over windows of whole periods (at least 10 ms) across the glide, against the sine's 0.5 / sqrt 2.
        const auto period = (int)std::lround(fs / freq); // exact at these frequencies
        const auto window = period * (int)std::ceil(0.01 * freq);
        for (int start = glideAt - window; start + window < glideAt + (int)(0.06 * fs); start += window / 2)
        {
            double sum = 0.0;
            for (int i = start; i < start + window; ++i)
                sum += (double)x[(size_t)i] * x[(size_t)i];
            REQUIRE(std::abs(10.0 * std::log10(sum / window / 0.125)) < 0.25);
        }
    }
}

TEST_CASE("entering and leaving Constant changes the latency between fades", "[dsp][phase]")
{
    const auto fs = 48000.0;
    Chain chain;
    chain.prepare(fs, 1, 400);
    chain.reset(phaseOnly(PhaseMode::high, 0.0));
    CHECK(chain.latency() == 0);

    std::vector<float> x(4800, 0.25f);
    float* p = x.data();
    chain.setSettings(phaseOnly(PhaseMode::constant, 0.0));
    chain.process(&p, 1, 4800); // more than the 10 ms fade-out
    CHECK(chain.latency() == ConstantRotator::latencyFor(fs));
    chain.setSettings(phaseOnly(PhaseMode::low, 0.0));
    chain.process(&p, 1, 4800);
    CHECK(chain.latency() == 0);
}
