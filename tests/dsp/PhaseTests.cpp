#include "AllocationCounter.h"
#include "dsp/Chain.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

// The phase stage (IMPLEMENTATION_PLAN 2.3, 2.4, M2): Hi/Lo is flat, its readout is the knob angle and its sections are
// within 2.5 degrees of analog ones to 20 kHz (oversampled, latency Lh); Constant is a true rotation, exact at 0 and
// 180 degrees, with latency L.
using namespace pa::dsp;
using Catch::Approx;

namespace
{
ChainSettings phaseOnly(PhaseMode mode, double degrees, bool wide = false)
{
    auto s = ChainSettings::neutral();
    s.phaseOn = true;
    s.phaseMode = mode;
    s.phaseDegrees = degrees;
    s.phaseWide = wide;
    return s;
}

// Hi/Lo's four shapes, each with the knob positions worth checking (panel angles; RANGE out ends at 90).
struct ShapeCase
{
    PhaseMode mode;
    bool wide;
    std::vector<double> thetas;
    mapping::Shape shape() const { return {mode == PhaseMode::low ? mapping::Mode::lo : mapping::Mode::hi, wide}; }
    std::string name() const { return std::string(mode == PhaseMode::low ? "LOW " : "HIGH ") + (wide ? "180" : "90"); }
};

std::vector<ShapeCase> shapeCases(bool edges = false)
{
    std::vector<double> narrow = {0.0, 10.0, 45.0, 90.0}, wide = {0.0, 10.0, 45.0, 90.0, 100.0, 135.0, 180.0};
    if (edges) // just past the ends of each range: a corner far up
    {
        narrow = {0.0, 0.5, 2.0, 10.0, 45.0, 90.0};
        wide = {0.0, 0.5, 2.0, 10.0, 45.0, 90.0, 90.5, 92.0, 120.0, 180.0};
    }
    std::vector<ShapeCase> list;
    for (const auto mode : {PhaseMode::low, PhaseMode::high})
    {
        list.push_back({mode, false, narrow});
        list.push_back({mode, true, wide});
    }
    return list;
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

// Analog first-order sections with the mapping's lag at each reference frequency (plan 2.3): what Hi/Lo approaches.
static double analogLagDegrees(const mapping::Shape& shape, double theta, double f)
{
    const auto r = mapping::references(shape);
    const auto [t1, t2] = mapping::angles(shape, mapping::knobAngle(theta, shape.wide));
    return 2.0 *
           (std::atan(std::tan(t1 * M_PI / 360.0) * f / r.f1) + std::atan(std::tan(t2 * M_PI / 360.0) * f / r.f2)) *
           180.0 / M_PI;
}

TEST_CASE("Hi/Lo is flat within 0.1 dB to 20 kHz, its readout is the knob angle within 0.1 degree, latency Lh",
          "[dsp][phase]")
{
    for (const auto fs : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        const auto latency = HiLoStage::latencyFor(fs);
        CHECK(latency == (fs < 47000.0 ? 32 : fs < 85000.0 ? 18 : fs < 170000.0 ? 5 : 0));
        CHECK(Chain::latencyFor(phaseOnly(PhaseMode::high, 0.0), fs, 400) == latency);
        CHECK(Chain::latencyFor(ChainSettings::neutral(), fs, 400) == latency); // Hi/Lo's even while off
        for (const auto& c : shapeCases())
            for (const auto theta : c.thetas)
            {
                INFO("fs " << fs << ", " << c.name() << " " << theta);
                const auto h = impulseResponse(fs, phaseOnly(c.mode, theta, c.wide), (int)(0.4 * fs));
                // The halfbands' passband ripple (prototype/oversampling.py: within 0.09 dB end to end).
                for (const auto f : testFrequencies(fs))
                    REQUIRE(std::abs(20.0 * std::log10(std::abs(response(h, f, fs, latency)))) < 0.1);

                // The readout (plan 2.3): the lag at the first section's reference frequency is the panel angle in
                // LOW 90, HIGH 90 and LOW 180. HIGH 180 reads the first section's angle (theta / 2) instead, as the
                // closed form says (section 2's small lag at 75.1 Hz adds to it).
                const auto r = mapping::references(c.shape());
                const auto lag = lagDegrees(response(h, r.f1, fs, latency));
                if (c.mode == PhaseMode::high && c.wide)
                    CHECK(lag == Approx(analogLagDegrees(c.shape(), theta, r.f1)).margin(0.1));
                else
                    CHECK(lag == Approx(theta).margin(0.1));
            }
    }
}

TEST_CASE("Hi/Lo is within 2.5 degrees of analog sections to 20 kHz at every rate (de-cramped)", "[dsp][phase]")
{
    // Settings just past 0 and 90 degrees are the worst: a corner far up, which a zero-latency section cramps by up to
    // 80 degrees at 16-20 kHz at 44.1 kHz (plan 2.3, prototype/out/hf/report.md).
    for (const auto fs : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0})
        for (const auto& c : shapeCases(true))
            for (const auto theta : c.thetas)
            {
                INFO("fs " << fs << ", " << c.name() << " " << theta);
                const auto latency = HiLoStage::latencyFor(fs);
                // LOW 180 stacks two sections at one corner, so their de-cramping errors add (hilo.py: up to 4.9).
                const auto stack = c.mode == PhaseMode::low && c.wide ? 2.0 : 1.0;
                const auto h = impulseResponse(fs, phaseOnly(c.mode, theta, c.wide), (int)(0.4 * fs));
                for (const auto f : testFrequencies(fs))
                {
                    INFO(f << " Hz");
                    auto lag = lagDegrees(response(h, f, fs, latency));
                    const auto analog = analogLagDegrees(c.shape(), theta, f);
                    lag += 360.0 * std::round((analog - lag) / 360.0);
                    REQUIRE(lag == Approx(analog).margin(stack * (f <= 16000.0 ? 2.1 : 2.6)));
                }
            }
}

TEST_CASE("the plain cascade's static response is the mapping's closed form, in all four shapes", "[dsp][phase]")
{
    // AllpassCascade alone (no oversampling) at 48 kHz: its impulse response against k = tan(theta_i / 2) / tan(pi f_i
    // / fs) per section, within 0.01 degree from 20 Hz to 20 kHz; the lag at the first reference is theta for LOW 90,
    // HIGH 90 and LOW 180, and the closed form's for HIGH 180.
    const auto fs = 48000.0;
    const auto length = (int)(0.5 * fs);
    for (const auto& c : shapeCases(true))
        for (const auto theta : c.thetas)
        {
            INFO(c.name() << " " << theta);
            const auto mode = c.mode == PhaseMode::low ? AllpassCascade::Mode::lo : AllpassCascade::Mode::hi;
            AllpassCascade cascade;
            cascade.prepare(fs);
            cascade.reset(mode, c.wide, theta);
            std::vector<float> h((size_t)length, 0.0f);
            h[0] = 1.0f;
            float* p = h.data();
            cascade.process(&p, 1, length);

            const auto phi = mapping::knobAngle(theta, c.wide);
            const auto [t1, t2] = mapping::angles(c.shape(), phi);
            const auto [k1, k2] = mapping::kAngles(c.shape(), t1, t2, fs);
            const auto section = [&](double k, double f)
            { return 2.0 * std::atan(k * std::tan(M_PI * f / fs)) * 180.0 / M_PI; };
            for (const auto f : testFrequencies(fs))
            {
                INFO(f << " Hz");
                const auto expected = section(k1, f) + section(k2, f);
                auto lag = lagDegrees(response(h, f, fs));
                lag += 360.0 * std::round((expected - lag) / 360.0);
                REQUIRE(lag == Approx(expected).margin(0.01));
                REQUIRE(std::abs(20.0 * std::log10(std::abs(response(h, f, fs)))) < 0.01);
            }
            const auto r = mapping::references(c.shape());
            const auto atRef = lagDegrees(response(h, r.f1, fs));
            if (c.mode == PhaseMode::high && c.wide)
                CHECK(atRef == Approx(section(k1, r.f1) + section(k2, r.f1)).margin(0.01));
            else if (theta >= 1.0 && theta < 180.0) // the k floor and the 0/180 wrap aside
                CHECK(atRef == Approx(theta).margin(0.01));
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
        chain.reset(phaseOnly(PhaseMode::high, 120.0, true));
        const auto glideAt = (int)(0.2 * fs), reverseAt = glideAt + (int)(0.015 * fs);
        for (int pos = 0; pos < length; pos += 64)
        {
            if (pos == glideAt / 64 * 64)
                chain.setSettings(phaseOnly(PhaseMode::low, 120.0, true));
            if (pos == reverseAt / 64 * 64)
                chain.setSettings(phaseOnly(PhaseMode::high, 120.0, true));
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
    CHECK(chain.latency() == HiLoStage::latencyFor(fs));

    std::vector<float> x(4800, 0.25f);
    float* p = x.data();
    chain.setSettings(phaseOnly(PhaseMode::constant, 0.0));
    chain.process(&p, 1, 4800); // more than the 10 ms fade-out
    CHECK(chain.latency() == ConstantRotator::latencyFor(fs));
    chain.setSettings(phaseOnly(PhaseMode::low, 0.0));
    chain.process(&p, 1, 4800);
    CHECK(chain.latency() == HiLoStage::latencyFor(fs));
}

TEST_CASE("leaving 0 or 90 degrees on broadband input releases no burst", "[dsp][phase]")
{
    // At identity a section's pole is just inside z = -1. Run as a TPT structure, its state held a near-lossless
    // resonance at Nyquist driven by the input's top end, which came out when the knob moved: white noise at +-0.5
    // peaked at over 4 (plan 2.3). Rotated and band-limited noise is nearly Gaussian, with the input's sigma (0.29), so
    // it legitimately peaks at about 4 sigma over this length (1.15; 1.02 here while a corner sweeps down from far
    // above 20 kHz just past 0 degrees), so the peak limit is 1.25. A burst also adds energy, which a moving all-pass
    // doesn't: every 1024-sample window's RMS stays within 1.5 dB of the input's over the same window (the estimate's
    // own spread is about 0.2 dB).
    std::mt19937 rng(67);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    struct Case
    {
        PhaseMode mode;
        double from, to;
        bool wideFrom, wideTo; // RANGE in before and after the move
        bool viaConstant;      // settled in Constant first, then Hi/Lo at `from`, then the move
    };
    for (const auto fs : {44100.0, 48000.0, 96000.0, 192000.0})
        for (const auto& c : {Case{PhaseMode::high, 0.0, 60.0, false, false, false},
                              Case{PhaseMode::high, 90.0, 150.0, true, true, false},
                              Case{PhaseMode::low, 0.0, 60.0, false, false, false},
                              Case{PhaseMode::low, 90.0, 150.0, true, true, false},
                              Case{PhaseMode::high, 0.0, 60.0, false, false, true},
                              // RANGE toggled from 90 to 180 with the second section at identity (and back)
                              Case{PhaseMode::high, 60.0, 60.0, false, true, false},
                              Case{PhaseMode::low, 60.0, 60.0, false, true, false},
                              Case{PhaseMode::low, 120.0, 50.0, true, false, false}})
        {
            INFO("fs " << fs << ", mode " << (int)c.mode << ", " << c.from << " to " << c.to << ", range "
                       << (c.wideFrom ? 180 : 90) << " to " << (c.wideTo ? 180 : 90)
                       << (c.viaConstant ? " after Constant" : ""));
            const auto length = (int)(0.6 * fs);
            std::vector<float> x((size_t)length);
            for (auto& v : x)
                v = dist(rng);
            const auto input = x;
            Chain chain;
            chain.prepare(fs, 1, 400);
            chain.reset(phaseOnly(c.viaConstant ? PhaseMode::constant : c.mode, c.from, c.wideFrom));
            float worst = 0.0f;
            for (int pos = 0; pos < length; pos += 64)
            {
                if (c.viaConstant && pos == (int)(0.1 * fs) / 64 * 64)
                    chain.setSettings(phaseOnly(c.mode, c.from, c.wideFrom)); // leaves Constant (a latency switch)
                if (pos == (int)(0.3 * fs) / 64 * 64)
                    chain.setSettings(phaseOnly(c.mode, c.to, c.wideTo));
                float* p = x.data() + pos;
                chain.process(&p, 1, std::min(64, length - pos));
                for (int i = pos; i < std::min(pos + 64, length); ++i)
                    worst = std::max(worst, std::abs(x[(size_t)i]));
            }
            CHECK(worst < 1.25f);

            // Windows of the output against the same span of the input (shifted by the latency of the path in use).
            const auto latency = HiLoStage::latencyFor(fs);
            for (int start = (int)(0.2 * fs); start + 1024 <= length; start += 512)
            {
                double out = 0.0, in = 0.0;
                for (int i = start; i < start + 1024; ++i)
                {
                    out += (double)x[(size_t)i] * x[(size_t)i];
                    in += (double)input[(size_t)(i - latency)] * input[(size_t)(i - latency)];
                }
                INFO("window at " << start);
                CHECK(std::abs(10.0 * std::log10(out / in)) < 1.5);
            }
        }
}

TEST_CASE("a RANGE toggle or a mode switch mid-signal keeps the energy and makes no step", "[dsp][phase]")
{
    // The shape glides (R3) like a mode switch always did: the sections stay all-pass, so white noise keeps its energy
    // and the output never steps more than the plain input does (like the checks in prototype/hilo.py).
    struct Case
    {
        PhaseMode mode;
        double from, to;
        bool wideFrom, wideTo;
        PhaseMode modeTo;
    };
    const auto fs = 48000.0;
    const auto length = (int)fs;
    const auto toggleAt = length / 2 / 64 * 64;
    for (const auto& c : {Case{PhaseMode::low, 160.0, 80.0, true, false, PhaseMode::low},
                          Case{PhaseMode::high, 80.0, 160.0, false, true, PhaseMode::high},
                          Case{PhaseMode::high, 170.0, 85.0, true, false, PhaseMode::high},
                          Case{PhaseMode::low, 80.0, 160.0, false, true, PhaseMode::low},
                          Case{PhaseMode::high, 120.0, 120.0, true, true, PhaseMode::low},
                          Case{PhaseMode::low, 45.0, 45.0, false, false, PhaseMode::high},
                          Case{PhaseMode::high, 60.0, 120.0, false, true, PhaseMode::low}})
    {
        INFO((int)c.mode << " " << c.from << (c.wideFrom ? " (180)" : " (90)") << " to " << (int)c.modeTo << " " << c.to
                         << (c.wideTo ? " (180)" : " (90)"));
        std::mt19937 rng(71);
        std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
        std::vector<float> x((size_t)length);
        for (auto& v : x)
            v = dist(rng);
        const auto input = x;

        Chain chain;
        chain.prepare(fs, 1, 400);
        chain.reset(phaseOnly(c.mode, c.from, c.wideFrom));
        const pa::test::AllocationProbe probe;
        for (int pos = 0; pos < length; pos += 64)
        {
            if (pos == toggleAt)
                chain.setSettings(phaseOnly(c.modeTo, c.to, c.wideTo));
            float* p = x.data() + pos;
            chain.process(&p, 1, std::min(64, length - pos));
        }
        CHECK(probe.allocations() == 0);

        const auto latency = HiLoStage::latencyFor(fs);
        double maxStep = 0.0, maxInStep = 0.0;
        for (int i = toggleAt - latency; i < length; ++i)
        {
            maxStep = std::max(maxStep, (double)std::abs(x[(size_t)i] - x[(size_t)i - 1]));
            maxInStep =
                std::max(maxInStep, (double)std::abs(input[(size_t)(i - latency)] - input[(size_t)(i - latency - 1)]));
        }
        CHECK(maxStep <= 1.5 * maxInStep); // white noise: both are about the noise's own peak-to-peak steps

        // Energy over windows spanning the glide against the input's (shifted by the latency).
        for (int start = toggleAt - 4096; start + 1024 <= length; start += 512)
        {
            double out = 0.0, in = 0.0;
            for (int i = start; i < start + 1024; ++i)
            {
                out += (double)x[(size_t)i] * x[(size_t)i];
                in += (double)input[(size_t)(i - latency)] * input[(size_t)(i - latency)];
            }
            INFO("window at " << start);
            CHECK(std::abs(10.0 * std::log10(out / in)) < 1.5);
        }
    }
}

namespace
{
std::vector<float> hilbertKernel(double fs)
{
    const auto h = HilbertFir::design(HilbertFir::tapsFor(fs));
    return {h.begin() + 1, h.end() - 1}; // as ConstantRotator uses it
}
} // namespace

TEST_CASE("the partitioned convolver equals direct convolution for any block layout and work changes", "[dsp][phase]")
{
    const auto kernel = hilbertKernel(48000.0);
    const auto length = 20000;
    std::mt19937 rng(53);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    std::vector<float> x((size_t)length);
    for (auto& v : x)
        v = dist(rng);

    // Direct convolution in double.
    std::vector<double> expected((size_t)length, 0.0);
    for (int n = 0; n < length; ++n)
        for (int k = 0; k < (int)kernel.size() && k <= n; k += 1)
            expected[(size_t)n] += (double)kernel[(size_t)k] * x[(size_t)(n - k)];

    for (const auto& blocks : std::vector<std::vector<int>>{{128}, {256}, {64, 512}, {128, 1024}, {128, 512, 2048}})
    {
        INFO("blocks " << blocks.front() << " .. " << blocks.back() << " (" << blocks.size() << " levels)");
        PartitionedConvolver conv;
        conv.prepare(kernel, blocks, 1);
        std::vector<float> y((size_t)length, 0.0f);
        // Odd step sizes, and stretches of history-only and spectra-only work: the output is checked only where it
        // was computed, and must be exact there however the work changed before.
        std::mt19937 steps(59);
        std::uniform_int_distribution<int> stepDist(1, 700);
        double worst = 0.0;
        for (int pos = 0; pos < length;)
        {
            const auto m = std::min(stepDist(steps), length - pos);
            const auto phase = (pos / 3000) % 4;
            const auto mode = phase == 1   ? PartitionedConvolver::Work::history
                              : phase == 3 ? PartitionedConvolver::Work::spectra
                                           : PartitionedConvolver::Work::full;
            const float* in = x.data() + pos;
            float* out = y.data() + pos;
            conv.process(&in, &out, m, mode);
            if (mode == PartitionedConvolver::Work::full)
                for (int i = pos; i < pos + m; ++i)
                    worst = std::max(worst, std::abs((double)y[(size_t)i] - expected[(size_t)i]));
            pos += m;
        }
        CHECK(worst < 2.0e-6);
    }
}

// Hidden benchmark: the convolver alone, stereo, in the rotator's 32-sample steps, for candidate block layouts.
TEST_CASE("convolver cost per stereo frame by block layout", "[.][bench]")
{
    for (const auto fs : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        const auto kernel = hilbertKernel(fs);
        const auto* seconds = std::getenv("PA_BENCH_SECONDS");
        const auto length = (int)((seconds != nullptr ? std::atof(seconds) : 5.0) * fs);
        std::mt19937 rng(61);
        std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
        std::vector<float> l((size_t)length), r((size_t)length);
        for (int i = 0; i < length; ++i)
            l[(size_t)i] = dist(rng), r[(size_t)i] = dist(rng);

        for (const auto& blocks : std::vector<std::vector<int>>{{64},
                                                                {128},
                                                                {256},
                                                                {64, 512},
                                                                {128, 512},
                                                                {128, 1024},
                                                                {128, 2048},
                                                                {256, 1024},
                                                                {256, 2048},
                                                                {64, 256, 1024},
                                                                {64, 512, 2048},
                                                                {128, 512, 2048},
                                                                {128, 1024, 4096},
                                                                {256, 1024, 4096}})
        {
            std::string name;
            for (const auto b : blocks)
                name += (name.empty() ? "" : "/") + std::to_string(b);
            const auto* filter = std::getenv("PA_BENCH"); // e.g. "48 kHz, blocks 128:"
            if (filter != nullptr &&
                (std::to_string((int)(fs / 1000)) + " kHz, blocks " + name + ":").find(filter) == std::string::npos)
                continue;
            PartitionedConvolver conv;
            conv.prepare(kernel, blocks, 2);
            auto best = 1.0e30;
            for (int repeat = 0; repeat < 3; ++repeat)
            {
                conv.reset();
                const auto start = std::chrono::steady_clock::now();
                for (int pos = 0; pos < length; pos += 32)
                {
                    const float* in[] = {l.data() + pos, r.data() + pos};
                    float out0[32], out1[32];
                    float* out[] = {out0, out1};
                    conv.process(in, out, std::min(32, length - pos), PartitionedConvolver::Work::full);
                }
                best = std::min(
                    best, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() /
                              length);
            }
            WARN(fs / 1000.0 << " kHz, blocks " << name << ": " << best << " ns per stereo frame");
        }
    }
}
