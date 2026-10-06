#include "AllocationCounter.h"
#include "dsp/Chain.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <random>
#include <string>
#include <vector>

using namespace pa::dsp;
using Catch::Approx;

namespace
{
constexpr double rates[] = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};

// The delay knob's reach either way in tenths of a sample, its whole-sample part, and the latency while the delay is on
// (plan 2.1a).
int reachTenths(double fs)
{
    return (int)std::lround(4.0 * fs * 10.0 / 1000.0);
}

int maxDelayAt(double fs)
{
    return reachTenths(fs) / 10;
}

int latencyAt(double fs)
{
    return Chain::delayLatencyFor(fs, reachTenths(fs));
}

// Hi/Lo's latency (its oversampling, plan 2.3), which the chain has whenever Constant isn't active, phase on or off.
int hiLoAt(double fs)
{
    return HiLoStage::latencyFor(fs);
}

using Signal = std::vector<std::vector<float>>; // [channel][sample]

Signal makeSignal(int numChannels, int length, float value = 0.0f)
{
    return Signal((size_t)numChannels, std::vector<float>((size_t)length, value));
}

Signal noise(int numChannels, int length, unsigned seed, float amplitude = 0.5f)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-amplitude, amplitude);
    auto s = makeSignal(numChannels, length);
    for (auto& ch : s)
        for (auto& x : ch)
            x = dist(rng);
    return s;
}

Signal sine(int numChannels, int length, double freq, double fs, float amplitude)
{
    auto s = makeSignal(numChannels, length);
    for (size_t ch = 0; ch < s.size(); ++ch)
        for (int i = 0; i < length; ++i)
            s[ch][(size_t)i] = amplitude * (float)std::sin(2.0 * M_PI * freq * i / fs + 0.3 * (double)ch);
    return s;
}

struct Change
{
    int at; // sample index
    ChainSettings settings;
};

// Runs `input` through a chain prepared at fs, starting settled at `initial`, applying each change at its
// sample, and splitting the rest into blocks whose sizes come from `nextBlockSize`.
template <typename BlockSizeFn>
Signal run(double fs, const Signal& input, const ChainSettings& initial, const std::vector<Change>& changes,
           BlockSizeFn&& nextBlockSize)
{
    Chain chain;
    chain.prepare(fs, (int)input.size(), reachTenths(fs));
    chain.reset(initial);

    auto out = input;
    const auto length = (int)out[0].size();
    std::vector<float*> ptrs(out.size());
    size_t nextChange = 0;
    for (int pos = 0; pos < length;)
    {
        while (nextChange < changes.size() && changes[nextChange].at <= pos)
            chain.setSettings(changes[nextChange++].settings);

        auto n = std::min(nextBlockSize(), length - pos);
        if (nextChange < changes.size())
            n = std::min(n, changes[nextChange].at - pos);

        for (size_t ch = 0; ch < out.size(); ++ch)
            ptrs[ch] = out[ch].data() + pos;
        chain.process(ptrs.data(), (int)out.size(), n);
        pos += n;
    }
    return out;
}

Signal run(double fs, const Signal& input, const ChainSettings& initial, const std::vector<Change>& changes = {},
           int blockSize = 512)
{
    return run(fs, input, initial, changes, [blockSize] { return blockSize; });
}

// The delay on at `samples` (the knob, whole samples): the chain delays by its latency + samples. The phase stage is
// off: Hi/Lo at 0 degrees is not an identity (its k floor), and these tests are about the delay. With the stage off the
// output is still delayed by Hi/Lo's latency (hiLoAt).
ChainSettings withDelay(int samples)
{
    auto s = ChainSettings::neutral();
    s.delayOn = true;
    s.delayTenths = 10 * samples;
    return s;
}

ChainSettings withDelayTenths(int tenths)
{
    auto s = withDelay(0);
    s.delayTenths = tenths;
    return s;
}

// x delayed by k samples (zeros before).
Signal shifted(const Signal& x, int k)
{
    auto y = makeSignal((int)x.size(), (int)x[0].size());
    for (size_t ch = 0; ch < x.size(); ++ch)
        for (size_t i = (size_t)k; i < x[ch].size(); ++i)
            y[ch][i] = x[ch][i - (size_t)k];
    return y;
}

// Two cascaded second-order Butterworth low-passes at 20 kHz (RBJ), in place.
void lowPass20k(std::vector<float>& x, double fs)
{
    const auto w = 2.0 * M_PI * 20000.0 / fs, alpha = std::sin(w) / (2.0 * std::sqrt(0.5)), c = std::cos(w);
    const auto a0 = 1.0 + alpha;
    const double b0 = (1.0 - c) / 2.0 / a0, b1 = (1.0 - c) / a0, b2 = b0, a1 = -2.0 * c / a0, a2 = (1.0 - alpha) / a0;
    for (int pass = 0; pass < 2; ++pass)
    {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto& v : x)
        {
            const double y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1, x1 = v, y2 = y1, y1 = y;
            v = (float)y;
        }
    }
}

bool bitEqual(const Signal& a, const Signal& b)
{
    return a == b; // element-wise float ==
}

// A busy automation script: delay jumps faster than the fade (so "latest wins" is exercised), polarity flips, delay
// and phase on/off, the phase angle moving and the mode switching (Hi <-> Lo glides; Constant is a latency change), all
// overlapping.
std::vector<Change> automationScript(double fs, int length, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> delayDist(-reachTenths(fs), reachTenths(fs)); // fractions included
    std::uniform_int_distribution<int> gapDist(1, (int)(0.03 * fs));
    std::uniform_real_distribution<double> angleDist(0.0, 180.0);
    std::uniform_int_distribution<int> modeDist(0, 2);
    std::bernoulli_distribution coin(0.3), rare(0.1), wideCoin(0.2);

    std::vector<Change> changes;
    ChainSettings s;
    for (int at = gapDist(rng); at < length; at += gapDist(rng))
    {
        s.delayTenths = delayDist(rng);
        s.phaseDegrees = angleDist(rng);
        if (coin(rng))
            s.polarityInverted = ! s.polarityInverted;
        if (coin(rng))
            s.delayOn = ! s.delayOn;
        if (coin(rng))
            s.phaseOn = ! s.phaseOn;
        if (rare(rng))
            s.phaseMode = (PhaseMode)modeDist(rng);
        if (wideCoin(rng))
            s.phaseWide = ! s.phaseWide;
        changes.push_back({at, s});
    }
    return changes;
}
} // namespace

//==============================================================================
TEST_CASE("ramps move at a fixed rate, land exactly and reverse without a jump", "[dsp]")
{
    LinearRamp r;
    r.prepare(10);
    r.reset(0.0f);
    r.setTarget(1.0f);
    for (int i = 1; i < 10; ++i)
        CHECK(r.next() == Approx((float)i / 10.0f));
    CHECK(r.next() == 1.0f);
    CHECK(r.isSettled());

    r.setTarget(0.0f);
    for (int i = 0; i < 4; ++i)
        r.next();
    r.setTarget(1.0f); // reverse mid-ramp: continues from 0.6
    CHECK(r.next() == Approx(0.7f));

    float w[3] = {0.0f, 0.5f, 1.0f}, io[3] = {0.3f, 0.3f, 0.3f}, wet[3] = {-0.7f, -0.7f, -0.7f};
    crossfadeInto(io, wet, w, 3);
    CHECK(io[0] == 0.3f);
    CHECK(io[1] == Approx(-0.2f));
    CHECK(io[2] == -0.7f);
    CHECK(crossfadeSamples(48000.0) == 2400);
}

TEST_CASE("an impulse is delayed by exactly the latency plus the set number of samples at every rate", "[dsp]")
{
    for (const auto fs : rates)
    {
        const auto lmax = maxDelayAt(fs), latency = latencyAt(fs) + hiLoAt(fs);
        for (const auto d : {-lmax, -37, -2, -1, 0, 1, 2, 37, lmax / 2, lmax})
        {
            INFO("fs " << fs << ", delay " << d);
            auto input = makeSignal(2, 2000);
            input[0][100] = 1.0f;
            input[1][100] = -0.25f;
            const auto out = run(fs, input, withDelay(d));

            auto expected = makeSignal(2, 2000);
            expected[0][(size_t)(100 + latency + d)] = 1.0f;
            expected[1][(size_t)(100 + latency + d)] = -0.25f;
            CHECK(bitEqual(out, expected));
        }

        // Off: no delay and only Hi/Lo's latency, whatever the knob says.
        auto off = withDelay(-37);
        off.delayOn = false;
        const auto input = noise(2, 2000, 41);
        CHECK(bitEqual(run(fs, input, off), shifted(input, hiLoAt(fs))));
    }
}

TEST_CASE("the delay adds the reach plus the kernels' lookahead to the latency while on, and nothing while off",
          "[dsp]")
{
    // Reach rounded up to whole samples, plus the lookahead of the rate's interpolation kernels (FractionalDelay.h).
    const auto kaiser = FractionalKernels::design == FractionalKernels::Design::kaiser;
    CHECK(latencyAt(44100.0) == 177 + (kaiser ? 23 : 6)); // 176.4 samples; 48 taps (low delay: 28)
    CHECK(latencyAt(48000.0) == 192 + (kaiser ? 11 : 4)); // 24 taps (16)
    CHECK(latencyAt(88200.0) == 353 + (kaiser ? 3 : 1));  // 352.8 samples; 8 taps (6)
    CHECK(latencyAt(96000.0) == 384 + (kaiser ? 3 : 1));
    CHECK(latencyAt(192000.0) == 768 + (kaiser ? 3 : 0)); // (4)
    for (const auto fs : rates)
    {
        INFO("fs " << fs);
        const auto reach = reachTenths(fs), latency = latencyAt(fs), hiLo = hiLoAt(fs);
        CHECK(Chain::latencyFor(withDelay(-3), fs, reach) == latency + hiLo);
        CHECK(Chain::latencyFor(ChainSettings::neutral(), fs, reach) == hiLo);
        CHECK(Chain::latencyFor(withDelay(0).bypassed(), fs, reach) == latency + hiLo); // bypass keeps the latency
        CHECK(1000.0 * (latency - maxDelayAt(fs)) / fs < 0.6); // the interpolation adds under 0.6 ms

        Chain chain;
        chain.prepare(fs, 2, reach);
        CHECK(chain.latency() == hiLo);
        chain.reset(withDelay(5));
        CHECK(chain.latency() == latency + hiLo);
    }
}

TEST_CASE("fractional delays are within 0.02 dB and 0.01 samples of exact up to 20 kHz", "[dsp]")
{
    for (const auto fs : rates)
        for (const auto tenths : {-reachTenths(fs), -1234, -5, 1, 3, 5, 7, 9, 487, reachTenths(fs)})
        {
            const auto latency = latencyAt(fs) + hiLoAt(fs);
            const auto expectedDelay = latency + tenths / 10.0; // samples
            for (const auto freq : {50.0, 1000.0, 10000.0, std::min(20000.0, 0.45 * fs)})
            {
                INFO("fs " << fs << ", delay " << tenths / 10.0 << " samples, " << freq << " Hz");
                const auto w = 2.0 * M_PI * freq / fs;
                const auto length = 4096 + 2 * latency;
                auto input = makeSignal(1, length);
                for (int i = 0; i < length; ++i)
                    input[0][(size_t)i] = (float)(0.5 * std::sin(w * i));
                const auto out = run(fs, input, withDelayTenths(tenths), {}, 37);

                // Least-squares fit of a sine at this frequency over the settled part: its level and phase delay.
                double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
                for (int i = 2 * latency + 100; i < length; ++i)
                {
                    const auto si = std::sin(w * i), ci = std::cos(w * i), y = (double)out[0][(size_t)i];
                    ss += si * si, sc += si * ci, cc += ci * ci, ys += y * si, yc += y * ci;
                }
                const auto det = ss * cc - sc * sc;
                const auto a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det; // y = a sin + b cos
                const auto level = 20.0 * std::log10(std::hypot(a, b) / 0.5);
                // 0.5 sin(w (i - D)) = 0.5 cos(wD) sin(wi) - 0.5 sin(wD) cos(wi), so D = atan2(-b, a) / w (mod period).
                auto delay = std::atan2(-b, a) / w;
                const auto period = 2.0 * M_PI / w;
                delay += period * std::round((expectedDelay - delay) / period);
                CHECK(std::abs(level) < 0.02);
                CHECK(delay == Approx(expectedDelay).margin(0.01));
            }
        }
}

TEST_CASE("settled stages are bit-exact: off is the delayed input, polarity is an exact negation", "[dsp]")
{
    for (const auto fs : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        INFO("fs " << fs);
        const auto input = noise(2, 20000, 1);
        const auto hiLo = hiLoAt(fs);

        // Every stage off: the input delayed by Hi/Lo's latency, exactly.
        CHECK(bitEqual(run(fs, input, ChainSettings::neutral()), shifted(input, hiLo)));
        // The delay on at 0 is the input delayed by the latency, exactly; so is host bypass with the delay on.
        CHECK(bitEqual(run(fs, input, withDelay(0)), shifted(input, latencyAt(fs) + hiLo)));
        CHECK(bitEqual(run(fs, input, withDelay(0).bypassed()), shifted(input, latencyAt(fs) + hiLo)));

        auto inverted = ChainSettings::neutral();
        inverted.polarityInverted = true;
        auto negated = shifted(input, hiLo);
        for (auto& ch : negated)
            for (auto& x : ch)
                x = -x;
        CHECK(bitEqual(run(fs, input, inverted), negated));

        // After fading every stage out (the delay off is a latency change, faster than the other fades), the output is
        // the delayed input again, bit-exact.
        const auto fade = crossfadeSamples(fs);
        auto active = withDelay(100);
        active.polarityInverted = true;
        const auto out = run(fs, input, active, {{1000, ChainSettings::neutral()}});
        for (size_t ch = 0; ch < 2; ++ch)
            for (int i = 1000 + fade + hiLo; i < 20000; ++i) // the polarity fade reaches the output hiLo later
                REQUIRE(out[ch][(size_t)i] == input[ch][(size_t)(i - hiLo)]);
    }
}

TEST_CASE("a delay change crossfades between the two taps; the latest target wins", "[dsp]")
{
    const auto fs = 48000.0;
    const auto fade = crossfadeSamples(fs);

    // DC on channel 0 is unchanged by any delay, so use a ramp signal: x[n] = n, read back as the tap.
    auto input = makeSignal(1, 4 * fade + 1000);
    for (size_t i = 0; i < input[0].size(); ++i)
        input[0][i] = (float)i;

    const auto out = run(fs, input, withDelay(10),
                         {{500, withDelay(20)}, {500 + fade / 2, withDelay(40)}, {500 + fade / 2 + 1, withDelay(30)}});

    // The effective delay, less the latency: what the knob says.
    const auto latency = latencyAt(fs) + hiLoAt(fs);
    const auto tapAt = [&](int i) { return (double)i - (double)out[0][(size_t)i] - latency; };
    CHECK(tapAt(499) == Approx(10.0));
    CHECK(tapAt(500 + fade / 2) == Approx(15.0).margin(0.01));        // half way from 10 to 20
    CHECK(tapAt(500 + fade - 1) == Approx(20.0));                     // the first fade completes as planned...
    CHECK(tapAt(500 + fade + fade / 2) == Approx(25.0).margin(0.01)); // ...then 20 -> 30: 40 was skipped
    CHECK(tapAt(500 + 2 * fade) == Approx(30.0));
    CHECK(tapAt(4 * fade + 999) == Approx(30.0));

    // Crossing zero is an ordinary change: 10 -> -25 fades between the two taps.
    const auto across = run(fs, input, withDelay(10), {{500, withDelay(-25)}});
    const auto acrossAt = [&](int i) { return (double)i - (double)across[0][(size_t)i] - latency; };
    CHECK(acrossAt(500 + fade / 2) == Approx(-7.5).margin(0.05));
    CHECK(acrossAt(500 + fade) == Approx(-25.0));
}

TEST_CASE("polarity ramps linearly through zero over the crossfade length", "[dsp]")
{
    const auto fs = 44100.0;
    const auto fade = crossfadeSamples(fs);
    auto inverted = ChainSettings::neutral();
    inverted.polarityInverted = true;
    const auto out = run(fs, makeSignal(1, 3 * fade, 1.0f), ChainSettings::neutral(), {{100, inverted}});
    const auto h = (size_t)hiLoAt(fs); // the output is delayed by Hi/Lo's latency

    CHECK(out[0][99 + h] == 1.0f);
    CHECK(out[0][(size_t)(100 + fade / 2) + h] == Approx(0.0f).margin(2.0f / (float)fade));
    CHECK(out[0][(size_t)(100 + fade - 1) + h] == -1.0f);
    CHECK(out[0][(size_t)(3 * fade - 1)] == -1.0f);
}

TEST_CASE("switching the delay on or off fades out, changes the latency in silence and fades back in", "[dsp]")
{
    for (const auto fs : {44100.0, 48000.0, 192000.0})
        for (const auto blockSize : {1, 7, 32, 512})
        {
            INFO("fs " << fs << ", block " << blockSize);
            const auto h = hiLoAt(fs);       // the latency while off: Hi/Lo's
            const auto lmax = latencyAt(fs); // the delay's, added while on
            const auto f = latencyFadeSamples(fs);
            const auto d = -10;

            // x[n] = n + 1: every sample says where it came from.
            const auto length = 2000 + 6 * f;
            auto x = makeSignal(1, length);
            for (int i = 0; i < length; ++i)
                x[0][(size_t)i] = (float)(i + 1);

            Chain chain;
            chain.prepare(fs, 1, reachTenths(fs));
            auto off = withDelay(d);
            off.delayOn = false;
            chain.reset(off);

            auto out = x;
            std::vector<int> latencyOut((size_t)length);
            const auto on = 100, backOff = 100 + 3 * f, reverse = backOff + f / 2;
            for (int pos = 0; pos < length;)
            {
                if (pos == on)
                    chain.setSettings(withDelay(d));
                if (pos == backOff)
                    chain.setSettings(off);
                if (pos == reverse) // changed back half way through the fade-out: no switch
                    chain.setSettings(withDelay(d));
                auto n = std::min(blockSize, length - pos);
                for (const auto at : {on, backOff, reverse})
                    if (at > pos)
                        n = std::min(n, at - pos);
                float* p = out[0].data() + pos;
                chain.process(&p, 1, n);
                for (int i = pos; i < pos + n; ++i)
                    latencyOut[(size_t)i] = chain.latency();
                pos += n;
            }

            const auto in = [&](int i) { return (double)x[0][(size_t)i]; };
            const auto at = [&](int i) { return (double)out[0][(size_t)i]; };

            // Before: the input delayed by Hi/Lo's latency. (latencyOut is per sample only with 1-sample blocks.)
            CHECK(at(on - 1) == in(on - 1 - h));
            CHECK(latencyOut[(size_t)(on - 1)] == h);
            const auto perSample = blockSize == 1;

            // Fade-out: a linear gain down to silence over f samples, still without the delay.
            for (int k = 0; k < f; ++k)
                REQUIRE(at(on + k) == Approx(in(on + k - h) * (1.0 - (k + 1.0) / f)).epsilon(1e-5).margin(1e-3));
            CHECK(at(on + f - 1) == 0.0);

            // Switched on the next sample; fade-in of the input delayed by Lmax + d (and h), then exact.
            if (perSample)
            {
                CHECK(latencyOut[(size_t)(on + f - 1)] == h);
                CHECK(latencyOut[(size_t)(on + f)] == lmax + h);
            }
            for (int k = 0; k < f; ++k)
            {
                const auto i = on + f + k;
                REQUIRE(at(i) == Approx(in(i - lmax - d - h) * (k + 1.0) / f).epsilon(1e-5).margin(1e-3));
            }
            for (int i = on + 2 * f; i < backOff; ++i)
                REQUIRE(at(i) == in(i - lmax - d - h));

            // Switching off, then back on half way through the fade-out: the gain turns round from where it is, the
            // latency never changes, and it ends where it started.
            const auto half = f / 2;
            CHECK(at(reverse - 1) == Approx(in(reverse - 1 - lmax - d - h) * (1.0 - (double)half / f)).epsilon(1e-5));
            for (int i = backOff; i < length; ++i)
                REQUIRE(latencyOut[(size_t)i] == lmax + h);
            for (int i = reverse + half + 1; i < length; ++i)
                REQUIRE(at(i) == in(i - lmax - d - h));
        }
}

TEST_CASE("no sample step above the threshold under scripted automation", "[dsp]")
{
    // A low sine has small sample-to-sample steps; any switch without a ramp would step by up to 2x its
    // amplitude. Each transition may add at most its ramp's slope times the largest jump it bridges.
    const auto freq = 100.0;
    const auto amplitude = 0.5f;
    for (const auto fs : rates)
    {
        INFO("fs " << fs);
        const auto length = (int)(3.0 * fs);
        // A 10 ms fade-in: a path with latency (the delay, Constant) replays the signal's onset later, and an abrupt
        // start would be a step there.
        auto input = sine(2, length, freq, fs, amplitude);
        const auto fadeIn = (int)(0.01 * fs);
        for (auto& ch : input)
            for (int i = 0; i < fadeIn; ++i)
                ch[(size_t)i] *= (float)i / (float)fadeIn;
        const auto out = run(fs, input, ChainSettings{}, automationScript(fs, length, 7));

        const auto fade = (double)crossfadeSamples(fs);
        const auto latencyFade = (double)latencyFadeSamples(fs);
        const auto naturalStep = 2.0 * M_PI * freq / fs * amplitude;
        // Three crossfades at once, plus the output fade around a latency change (the delay on or off), plus a moving
        // phase: up to 180 degrees over Constant's 20 ms angle smoothing (Hi/Lo's is 30 ms).
        const auto phaseRamp = amplitude * M_PI / (ConstantRotator::smoothMs * 1.0e-3 * fs);
        const auto limit = 1.05 * naturalStep + 3.0 * (2.0 * amplitude) / fade + amplitude / latencyFade + phaseRamp;
        // In the audible band: at 88.2 kHz and up, a corner sweeping down from near Nyquist (a phase knob passing 0 or
        // 90 degrees) leaves a brief wobble far above 20 kHz that a raw sample-to-sample step would count.
        auto measured = out;
        if (fs >= 88200.0)
            for (auto& ch : measured)
                lowPass20k(ch, fs);
        double worst = 0.0;
        for (const auto& ch : measured)
            for (size_t i = 1; i < ch.size(); ++i)
                worst = std::max(worst, (double)std::abs(ch[i] - ch[i - 1]));
        CHECK(worst <= limit);
        CHECK(worst < 0.05); // and nowhere near a click (a hard switch steps by up to 1.0 here)
    }
}

TEST_CASE("no NaN, infinity or denormal under fast automation", "[dsp]")
{
    // Without flush-to-zero (offline use). Several scripts: the random sequences differ between standard libraries, so
    // one seed covers different automation on each platform (CI found a subnormal on Linux and Windows that macOS's
    // sequence never reached).
    for (const auto fs : {96000.0, 48000.0})
        for (unsigned seed = 1; seed <= 20; ++seed)
        {
            INFO("fs " << fs << ", seed " << seed);
            const auto length = (int)(1.0 * fs);
            auto input = noise(2, length, seed, 1.0f);
            for (auto& ch : input) // then a long silence
                std::fill(ch.begin() + length / 2, ch.end(), 0.0f);
            input[0][(size_t)(length / 2 + 10)] = 1.0e-30f; // a tiny value the fades scale down

            const auto out = run(fs, input, withDelay(0), automationScript(fs, length, 7 * seed + 4));
            for (const auto& ch : out)
                for (const auto x : ch)
                {
                    REQUIRE(std::isfinite(x));
                    REQUIRE(std::fpclassify(x) != FP_SUBNORMAL);
                }
        }
}

TEST_CASE("block sizes 1 to 4096, fixed or variable, give identical output", "[dsp]")
{
    const auto fs = 44100.0;
    const auto length = (int)(1.5 * fs);
    const auto input = noise(2, length, 5);
    const auto script = automationScript(fs, length, 13);
    const auto reference = run(fs, input, withDelay(0), script, 32);

    for (const auto blockSize : {1, 2, 7, 31, 33, 64, 480, 512, 1024, 4096})
    {
        INFO("block size " << blockSize);
        CHECK(bitEqual(run(fs, input, withDelay(0), script, blockSize), reference));
    }

    std::mt19937 rng(17);
    std::uniform_int_distribution<int> sizes(1, 4096);
    CHECK(bitEqual(run(fs, input, withDelay(0), script, [&] { return sizes(rng); }), reference));
}

TEST_CASE("processing never allocates", "[dsp]")
{
    const auto fs = 48000.0;
    const auto length = (int)fs;
    auto signal = noise(2, length, 19);
    const auto script = automationScript(fs, length, 23);

    Chain chain;
    chain.prepare(fs, 2, reachTenths(fs));
    float* ptrs[2];
    size_t next = 0;

    const pa::test::AllocationProbe probe;
    for (int pos = 0; pos < length; pos += 256)
    {
        while (next < script.size() && script[next].at <= pos)
            chain.setSettings(script[next++].settings);
        ptrs[0] = signal[0].data() + pos;
        ptrs[1] = signal[1].data() + pos;
        chain.process(ptrs, 2, std::min(256, length - pos));
    }
    chain.reset(ChainSettings::neutral());
    CHECK(probe.allocations() == 0);
}

// Hidden benchmark (plan 2.6 budgets, full chain per stereo frame: Hi/Lo under 50 ns, Constant under 150 ns). Build in
// Release for meaningful numbers. Each case is the best of three runs of 5 s of audio in 512-sample blocks. Set
// PA_BENCH to a substring of a case's name to run only the cases that match, and PA_BENCH_SECONDS for longer runs (for
// a profiler).
TEST_CASE("chain cost per stereo frame", "[.][bench]")
{
    struct Case
    {
        const char* name;
        double fs;
        int tenths; // the delay knob
        bool automated;
        PhaseMode mode;
        bool phase; // the phase stage on at 60 degrees (Constant: on at 0 when false)
        bool delayOn = true;
    };
    const auto* filter = std::getenv("PA_BENCH");
    for (const auto& c :
         {Case{"idle: delay off, Hi with phase off, 48 kHz", 48000.0, 0, false, PhaseMode::high, false, false},
          Case{"delay only, whole samples, 48 kHz", 48000.0, 770, false, PhaseMode::high, false},
          Case{"delay only, fractional, 48 kHz (24 taps)", 48000.0, 773, false, PhaseMode::high, false},
          Case{"delay only, fractional, 44.1 kHz (48 taps)", 44100.0, 773, false, PhaseMode::high, false},
          Case{"delay only, fractional, 96 kHz (8 taps)", 96000.0, 773, false, PhaseMode::high, false},
          Case{"Hi at 60, delay off, 48 kHz", 48000.0, 0, false, PhaseMode::high, true, false},
          Case{"Hi at 60, delay off, 44.1 kHz", 44100.0, 0, false, PhaseMode::high, true, false},
          Case{"Hi at 60, delay off, 96 kHz", 96000.0, 0, false, PhaseMode::high, true, false},
          Case{"Hi at 60, delay off, 192 kHz", 192000.0, 0, false, PhaseMode::high, true, false},
          Case{"Hi at 60, fractional delay, 44.1 kHz", 44100.0, 773, false, PhaseMode::high, true},
          Case{"Hi at 60, fractional delay, 48 kHz", 48000.0, 773, false, PhaseMode::high, true},
          Case{"Constant at 60, delay off, 48 kHz", 48000.0, 0, false, PhaseMode::constant, true, false},
          Case{"Constant at 60, fractional delay, 44.1 kHz", 44100.0, 773, false, PhaseMode::constant, true},
          Case{"Constant at 60, fractional delay, 48 kHz", 48000.0, 773, false, PhaseMode::constant, true},
          Case{"Constant at 60, fractional delay, 96 kHz", 96000.0, 773, false, PhaseMode::constant, true},
          Case{"Constant at 60, fractional delay, 192 kHz", 192000.0, 773, false, PhaseMode::constant, true},
          Case{"Constant at 0 (no convolution), 48 kHz", 48000.0, 773, false, PhaseMode::constant, false},
          Case{"constant automation (all modes), 48 kHz", 48000.0, 773, true, PhaseMode::high, true},
          Case{"constant automation (all modes), 44.1 kHz", 44100.0, 773, true, PhaseMode::high, true}})
    {
        if (filter != nullptr && std::string(c.name).find(filter) == std::string::npos)
            continue;
        const auto* seconds = std::getenv("PA_BENCH_SECONDS");
        const auto length = (int)((seconds != nullptr ? std::atof(seconds) : 5.0) * c.fs);
        const auto script = automationScript(c.fs, length, 31);
        auto best = 1.0e30;
        for (int repeat = 0; repeat < 3; ++repeat)
        {
            auto signal = noise(2, length, 29);
            Chain chain;
            chain.prepare(c.fs, 2, reachTenths(c.fs));
            auto settings = withDelayTenths(c.tenths);
            settings.delayOn = c.delayOn;
            settings.phaseMode = c.mode;
            settings.phaseOn = c.mode == PhaseMode::constant || c.phase;
            settings.phaseDegrees = c.phase ? 60.0 : 0.0;
            chain.reset(settings);
            float* ptrs[2];
            size_t next = 0;
            const auto start = std::chrono::steady_clock::now();
            for (int pos = 0; pos < length; pos += 512)
            {
                while (c.automated && next < script.size() && script[next].at <= pos)
                    chain.setSettings(script[next++].settings);
                ptrs[0] = signal[0].data() + pos;
                ptrs[1] = signal[1].data() + pos;
                chain.process(ptrs, 2, std::min(512, length - pos));
            }
            const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
            best = std::min(best, ns / length);
        }
        WARN(c.name << ": " << best << " ns per stereo frame");
    }
    if (filter != nullptr)
        return;

    // The worst single callback (64 samples) in Constant, where the convolver's larger blocks complete: a spike, not
    // an average.
    for (const auto fs : {48000.0, 96000.0, 192000.0})
    {
        Chain chain;
        chain.prepare(fs, 2, reachTenths(fs));
        auto s = withDelayTenths(773);
        s.phaseMode = PhaseMode::constant;
        s.phaseOn = true;
        s.phaseDegrees = 60.0;
        chain.reset(s);
        const auto length = (int)(2.0 * fs);
        auto signal = noise(2, length, 47);
        double worst = 0.0, total = 0.0;
        for (int pos = 0; pos + 64 <= length; pos += 64)
        {
            float* ptrs[] = {signal[0].data() + pos, signal[1].data() + pos};
            const auto start = std::chrono::steady_clock::now();
            chain.process(ptrs, 2, 64);
            const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            worst = std::max(worst, us);
            total += us;
        }
        // Short enough not to wrap: of a callback's 1333 / 667 / 333 us at 48 / 96 / 192 kHz.
        WARN("Constant 64-sample callback, " << fs / 1000.0 << " kHz: worst " << worst << " us, mean "
                                             << total / (length / 64) << " us");
    }

    // The one-off cost of the convolver rebuilding itself from its history (Constant switched back on): one callback.
    for (const auto fs : {48000.0, 192000.0})
    {
        Chain chain;
        chain.prepare(fs, 2, reachTenths(fs));
        auto s = ChainSettings::neutral();
        s.phaseMode = PhaseMode::constant;
        s.phaseDegrees = 60.0;
        chain.reset(s);
        auto signal = noise(2, 65536, 37);
        float* ptrs[] = {signal[0].data(), signal[1].data()};
        chain.process(ptrs, 2, 32768);
        s.phaseOn = true;
        chain.setSettings(s);
        ptrs[0] += 32768;
        ptrs[1] += 32768;
        const auto start = std::chrono::steady_clock::now();
        chain.process(ptrs, 2, 64);
        const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        WARN("rebuild in one 64-sample callback, " << fs / 1000.0 << " kHz: " << us << " us");
    }
}

// Hidden: renders the automation script (every stage and mode, at every rate, stereo and mono) to raw float files in
// $PA_DUMP_DIR, so two builds can be compared byte for byte (an optimisation that should be bit-exact).
TEST_CASE("dump renders for comparing builds", "[.][dump]")
{
    const auto* dir = std::getenv("PA_DUMP_DIR");
    REQUIRE(dir != nullptr);
    for (const auto fs : rates)
        for (const auto numChannels : {2, 1})
        {
            const auto length = (int)(2.0 * fs);
            const auto input = noise(numChannels, length, 41);
            const auto out = run(fs, input, withDelay(0), automationScript(fs, length, 43), 512);
            const auto name =
                std::string(dir) + "/chain_" + std::to_string((int)fs) + "_" + std::to_string(numChannels) + ".f32";
            auto* f = std::fopen(name.c_str(), "wb");
            REQUIRE(f != nullptr);
            for (const auto& ch : out)
                std::fwrite(ch.data(), sizeof(float), ch.size(), f);
            std::fclose(f);
        }
}
