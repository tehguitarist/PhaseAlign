#pragma once

#include "dsp/PhaseMapping.h"
#include "dsp/Simd.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

// Hi/Lo phase modes (IMPLEMENTATION_PLAN 2.3): two first-order all-pass sections in series, double state. The knob's
// panel angle and the range (90 or 180) set the target of phi, section 1's angle, which is smoothed linearly over
// smoothMs and mapped to the sections' k by the current shape, (mode, range) (PhaseMapping.h). Changing the shape (mode
// or range) glides log k from the old shape's mapping to the new one's over glideMs, so the signal stays all-pass
// throughout (R3); switching back during a glide reverses it from where it is. Reference: prototype/hilo.py (HiLo),
// golden-tested.
//
// Coefficients are computed on a fixed grid of `cell` samples counted from reset(), so the output doesn't
// depend on the host's block sizes: at the start of each cell phi and the glide move on by one cell, and each
// section's G is interpolated linearly per sample from its value at the old angle to its value at the new one (every G
// in (0, 1] is an all-pass, so the path stays all-pass). Without the interpolation, a section whose corner sweeps down
// from near Nyquist (just past 0 and 90 degrees) would step and click. While the angle is static and no glide is
// running, no tan() runs and G is constant.
//
// Each section is the first-order all-pass of a TPT one-pole with G = 1 / (1 + k), H(z) = (-p + z^-1) / (1 - p z^-1)
// with p = 1 - 2G, run in direct form I: y = -p x + x1 + p y1, where x1 and y1 are its last input and output. (It was
// run as the TPT structure itself until 2026-10-06. Near identity, k = kMin, the pole is just inside z = -1 and the TPT
// state holds a near-lossless resonance at Nyquist driven by the input's top end, hidden from the output while k stays
// there; when the knob left 0 or 90 degrees it came out as a burst, up to 12 times the input on white noise. Direct
// form I keeps only past samples as state, so there is nothing hidden to come out. The transfer function is the same,
// so a static setting sounds the same; plan 2.3.)
namespace pa::dsp
{
class AllpassCascade
{
  public:
    static constexpr int maxChannels = 8;
    static constexpr int defaultCell = 32;
    static constexpr double smoothMs = 30.0, glideMs = 30.0;

    using Mode = mapping::Mode;

    // cellLength: the coefficient grid in samples (32; 32 M when run at M times a session rate, so the grid keeps its
    // length in time).
    void prepare(double sampleRateIn, int cellLength = defaultCell)
    {
        sampleRate = sampleRateIn;
        cell = std::max(1, cellLength);
        smoothSamples = std::max(1.0, std::round(smoothMs * 1.0e-3 * sampleRate));
        glideSamples = std::max(1.0, std::round(glideMs * 1.0e-3 * sampleRate));
        reset(Mode::hi, false, 0.0);
    }

    // Jumps to the shape and angle, clears the state.
    void reset(Mode m, bool wide, double thetaDegrees)
    {
        shape = previousShape = {m, wide};
        phi = target = mapping::knobAngle(thetaDegrees, wide);
        step = 0.0;
        glide = 1.0;
        untilUpdate = 0;
        for (auto& channel : state)
            channel.fill(0.0);
        stale = true;
    }

    // The panel angle (0 to 90, or 0 to 180 when wide) and the shape. A new target for phi restarts its linear ramp
    // (a RANGE toggle at the same knob position keeps phi, so it only glides the shape); a new shape starts the glide,
    // or reverses it when it is the one just left.
    void set(double thetaDegrees, Mode m, bool wide)
    {
        const auto newPhi = mapping::knobAngle(thetaDegrees, wide);
        if (! std::equal_to<double>()(newPhi, target)) // exact: any new value restarts the ramp
        {
            target = newPhi;
            step = (target - phi) / smoothSamples; // a linear ramp of fixed length
        }
        const mapping::Shape next{m, wide};
        if (next == shape)
            return;
        if (glide < 1.0 && next == previousShape) // back again mid-glide: reverse from where it is
        {
            std::swap(shape, previousShape);
            glide = 1.0 - glide;
        }
        else
        {
            previousShape = shape;
            shape = next;
            glide = 0.0;
        }
        stale = true;
    }

    bool isSettled() const { return std::equal_to<double>()(phi, target) && glide >= 1.0; }

    // Section 1's angle now (the ramped phi).
    double getPhi() const { return phi; }

    void process(float* const* io, int numChannels, int n)
    {
        for (int start = 0; start < n;)
        {
            if (untilUpdate == 0)
                startCell();
            const auto m = std::min(n - start, untilUpdate);
            const auto done = cell - untilUpdate;
            // Channels in pairs: their recursions are independent, so running two at once hides each one's latency.
            int ch = 0;
            for (; ch + 2 <= numChannels; ch += 2)
                runCell<2>(io + ch, state.data() + ch, start, m, done);
            if (ch < numChannels)
                runCell<1>(io + ch, state.data() + ch, start, m, done);
            untilUpdate -= m;
            start += m;
        }
    }

  private:
    // m samples of C channels from `start`, `done` samples into the cell. While not interpolating g0 == g1. State per
    // channel: the cascade's last input, section 1's last output (section 2's last input) and section 2's last output.
    template <int C>
    void runCell(float* const* io, std::array<double, 3>* s, int start, int m, int done)
    {
        double x1[C], y1[C], y2[C];
        float* x[C];
        for (int c = 0; c < C; ++c)
        {
            x1[c] = s[c][0];
            y1[c] = s[c][1];
            y2[c] = s[c][2];
            x[c] = io[c] + start;
        }
        int i = 0;
        if (! interpolating) // two samples per step: see staticPairs()
            i = staticPairs<C>(x, x1, y1, y2, m);
        for (; i < m; ++i)
        {
            auto ga = g1[0], gb = g1[1];
            if (interpolating)
            {
                const auto t = (double)(done + i + 1) / cell;
                ga = g0[0] + (g1[0] - g0[0]) * t;
                gb = g0[1] + (g1[1] - g0[1]) * t;
            }
            const auto pa = 1.0 - 2.0 * ga, pb = 1.0 - 2.0 * gb;
            for (int c = 0; c < C; ++c)
            {
                const double in = x[c][i];
                const auto a = x1[c] - pa * in + pa * y1[c];
                const auto b = y1[c] - pb * a + pb * y2[c];
                x1[c] = in;
                y1[c] = a;
                y2[c] = b;
                x[c][i] = (float)b;
            }
        }
        for (int c = 0; c < C; ++c)
            s[c] = {x1[c], y1[c], y2[c]};
    }

    // Static coefficients, whole pairs of samples; returns how many samples it did. y[n] = u[n] + p y[n - 1] with
    // u[n] = x[n - 1] - p x[n], so y[n + 1] = (u[n + 1] + p u[n]) + p^2 y[n - 1]: each output of a pair waits on the
    // previous pair's last output through one multiply-add, which halves the recursions' critical path; two channels
    // run side by side in one register. The same filter, rounded differently in the last bits.
    template <int C>
    int staticPairs(float* const* x, double* x1, double* y1, double* y2, int m) const
    {
        using namespace simd;
        const auto pa = 1.0 - 2.0 * g1[0], pb = 1.0 - 2.0 * g1[1];
        int i = 0;
        if constexpr (C == 2)
        {
            const auto vpa = splat2(pa), vpb = splat2(pb), vpa2 = splat2(pa * pa), vpb2 = splat2(pb * pb);
            auto vx1 = pair(x1[0], x1[1]), vy1 = pair(y1[0], y1[1]), vy2 = pair(y2[0], y2[1]);
            for (; i + 2 <= m; i += 2)
            {
                const auto in0 = pair(x[0][i], x[1][i]), in1 = pair(x[0][i + 1], x[1][i + 1]);
                const auto ua0 = msub(vx1, vpa, in0), ua1 = msub(in0, vpa, in1);
                const auto a0 = madd(ua0, vpa, vy1);
                const auto a1 = madd(madd(ua1, vpa, ua0), vpa2, vy1);
                const auto ub0 = msub(vy1, vpb, a0), ub1 = msub(a0, vpb, a1);
                const auto b0 = madd(ub0, vpb, vy2);
                const auto b1 = madd(madd(ub1, vpb, ub0), vpb2, vy2);
                vx1 = in1;
                vy1 = a1;
                vy2 = b1;
                x[0][i] = (float)lane0(b0);
                x[1][i] = (float)lane1(b0);
                x[0][i + 1] = (float)lane0(b1);
                x[1][i + 1] = (float)lane1(b1);
            }
            x1[0] = lane0(vx1), x1[1] = lane1(vx1);
            y1[0] = lane0(vy1), y1[1] = lane1(vy1);
            y2[0] = lane0(vy2), y2[1] = lane1(vy2);
        }
        else
        {
            const auto pa2 = pa * pa, pb2 = pb * pb;
            for (; i + 2 <= m; i += 2)
                for (int c = 0; c < C; ++c)
                {
                    const double in0 = x[c][i], in1 = x[c][i + 1];
                    const auto ua0 = x1[c] - pa * in0, ua1 = in0 - pa * in1;
                    const auto a0 = ua0 + pa * y1[c];
                    const auto a1 = (ua1 + pa * ua0) + pa2 * y1[c];
                    const auto ub0 = y1[c] - pb * a0, ub1 = a0 - pb * a1;
                    const auto b0 = ub0 + pb * y2[c];
                    const auto b1 = (ub1 + pb * ub0) + pb2 * y2[c];
                    x1[c] = in1;
                    y1[c] = a1;
                    y2[c] = b1;
                    x[c][i] = (float)b0;
                    x[c][i + 1] = (float)b1;
                }
        }
        return i;
    }

    // A new cell: from the coefficients now (g0) to those after one cell's move of the angle and glide (g1).
    void startCell()
    {
        if (stale)
        {
            computeG(g1);
            stale = false;
        }
        g0[0] = g1[0];
        g0[1] = g1[1];
        if (! isSettled())
        {
            advance();
            computeG(g1);
        }
        interpolating = ! std::equal_to<double>()(g0[0], g1[0]) || ! std::equal_to<double>()(g0[1], g1[1]);
        flushDenormals();
        untilUpdate = cell;
    }

    void computeG(double* g) const
    {
        const auto [a1, a2] = mapping::angles(shape, phi);
        auto [k1, k2] = mapping::kAngles(shape, a1, a2, sampleRate);
        if (glide < 1.0)
        {
            const auto [p1, p2] = mapping::angles(previousShape, phi);
            const auto [o1, o2] = mapping::kAngles(previousShape, p1, p2, sampleRate);
            k1 = std::exp((1.0 - glide) * std::log(o1) + glide * std::log(k1));
            k2 = std::exp((1.0 - glide) * std::log(o2) + glide * std::log(k2));
        }
        g[0] = 1.0 / (1.0 + k1);
        g[1] = 1.0 / (1.0 + k2);
    }

    // A decaying state would otherwise reach denormals in silence when the caller hasn't set flush-to-zero (offline
    // use).
    void flushDenormals()
    {
        for (auto& channel : state)
            for (auto& v : channel)
                if (std::abs(v) < 1.0e-20)
                    v = 0.0;
    }

    // The ramp of phi and the glide move on by one cell.
    void advance()
    {
        if (! std::equal_to<double>()(phi, target))
        {
            phi += step * cell;
            if ((step > 0.0) == (phi >= target))
                phi = target;
        }
        if (glide < 1.0)
            glide = std::min(1.0, glide + cell / glideSamples);
    }

    double sampleRate = 48000.0, smoothSamples = 1.0, glideSamples = 1.0;
    mapping::Shape shape, previousShape;
    double phi = 0.0, target = 0.0, step = 0.0, glide = 1.0;
    int cell = defaultCell, untilUpdate = 0;
    bool stale = true, interpolating = false;
    double g0[2] = {1.0, 1.0}, g1[2] = {1.0, 1.0};
    std::array<std::array<double, 3>, maxChannels> state{}; // per channel: x1, section 1's y1, section 2's y1
};
} // namespace pa::dsp
