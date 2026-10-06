#pragma once

#include "dsp/PhaseMapping.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

// Hi/Lo phase modes (IMPLEMENTATION_PLAN 2.3): two first-order all-pass sections in series, double state, the
// knob angle smoothed linearly over smoothMs and mapped to the sections' k (PhaseMapping.h). Switching Hi <-> Lo glides
// log k from the old mode's mapping to the new one's over glideMs, so the signal stays all-pass throughout (R3);
// switching back during a glide reverses it from where it is. Reference: prototype/hilo.py (HiLo), golden-tested.
//
// Coefficients are computed on a fixed grid of updateInterval samples counted from reset(), so the output doesn't
// depend on the host's block sizes: at the start of each cell the angle and glide move on by one cell, and each
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
    static constexpr int updateInterval = 32;
    static constexpr double smoothMs = 30.0, glideMs = 30.0;

    enum class Mode
    {
        hi,
        lo
    };

    void prepare(double sampleRateIn)
    {
        sampleRate = sampleRateIn;
        smoothSamples = std::max(1.0, std::round(smoothMs * 1.0e-3 * sampleRate));
        glideSamples = std::max(1.0, std::round(glideMs * 1.0e-3 * sampleRate));
        reset(Mode::hi, 0.0);
    }

    // Jumps to the mode and angle, clears the state.
    void reset(Mode m, double thetaDegrees)
    {
        mode = previousMode = m;
        theta = target = thetaDegrees;
        step = 0.0;
        glide = 1.0;
        untilUpdate = 0;
        for (auto& channel : state)
            channel.fill(0.0);
        stale = true;
    }

    // Jumps to the mode with no glide, keeping the state: for a switch made while the output is silent.
    void jumpToMode(Mode m)
    {
        if (m != mode || glide < 1.0)
            stale = true;
        mode = previousMode = m;
        glide = 1.0;
    }

    void setTarget(double thetaDegrees)
    {
        if (! std::equal_to<double>()(thetaDegrees, target)) // exact: any new value restarts the ramp
        {
            target = thetaDegrees;
            step = (target - theta) / smoothSamples; // a linear ramp of fixed length
        }
    }

    void setMode(Mode m)
    {
        if (m == mode)
            return;
        if (glide < 1.0 && m == previousMode) // back again mid-glide: reverse from where it is
        {
            std::swap(mode, previousMode);
            glide = 1.0 - glide;
        }
        else
        {
            previousMode = mode;
            mode = m;
            glide = 0.0;
        }
        stale = true;
    }

    bool isSettled() const { return std::equal_to<double>()(theta, target) && glide >= 1.0; }

    double getTheta() const { return theta; }

    void process(float* const* io, int numChannels, int n)
    {
        for (int start = 0; start < n;)
        {
            if (untilUpdate == 0)
                startCell();
            const auto m = std::min(n - start, untilUpdate);
            const auto done = updateInterval - untilUpdate;
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
        for (int i = 0; i < m; ++i)
        {
            auto ga = g1[0], gb = g1[1];
            if (interpolating)
            {
                const auto t = (double)(done + i + 1) / updateInterval;
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

    static const mapping::ModeShape& shape(Mode m) { return m == Mode::hi ? mapping::hi : mapping::lo; }

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
        untilUpdate = updateInterval;
    }

    void computeG(double* g) const
    {
        auto [k1, k2] = mapping::kPair(shape(mode), theta, sampleRate);
        if (glide < 1.0)
        {
            const auto [o1, o2] = mapping::kPair(shape(previousMode), theta, sampleRate);
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

    // The angle ramp and the glide move on by one cell.
    void advance()
    {
        if (! std::equal_to<double>()(theta, target))
        {
            theta += step * updateInterval;
            if ((step > 0.0) == (theta >= target))
                theta = target;
        }
        if (glide < 1.0)
            glide = std::min(1.0, glide + updateInterval / glideSamples);
    }

    double sampleRate = 48000.0, smoothSamples = 1.0, glideSamples = 1.0;
    Mode mode = Mode::hi, previousMode = Mode::hi;
    double theta = 0.0, target = 0.0, step = 0.0, glide = 1.0;
    int untilUpdate = 0;
    bool stale = true, interpolating = false;
    double g0[2] = {1.0, 1.0}, g1[2] = {1.0, 1.0};
    std::array<std::array<double, 3>, maxChannels> state{}; // per channel: x1, section 1's y1, section 2's y1
};
} // namespace pa::dsp
