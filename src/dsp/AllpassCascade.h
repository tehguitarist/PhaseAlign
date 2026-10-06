#pragma once

#include "dsp/PhaseMapping.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

// Hi/Lo phase modes (IMPLEMENTATION_PLAN 2.3): two first-order TPT all-pass sections in series, double state, the
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
// TPT section: v = (x - s) G, lp = v + s, s' = lp + v, y = 2 lp - x, with G = 1 / (1 + k).
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
        for (auto& section : state)
            section.fill(0.0);
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
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto& s = state[(size_t)ch];
                auto* x = io[ch] + start;
                for (int i = 0; i < m; ++i)
                {
                    const auto t = interpolating ? (double)(done + i + 1) / updateInterval : 1.0;
                    double y = x[i];
                    for (int k = 0; k < 2; ++k)
                    {
                        const auto g = g0[k] + (g1[k] - g0[k]) * t;
                        const auto v = (y - s[(size_t)k]) * g;
                        const auto lp = v + s[(size_t)k];
                        s[(size_t)k] = lp + v;
                        y = 2.0 * lp - y;
                    }
                    x[i] = (float)y;
                }
            }
            untilUpdate -= m;
            start += m;
        }
    }

  private:
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
        for (auto& section : state)
            for (auto& v : section)
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
    std::array<std::array<double, 2>, maxChannels> state{};
};
} // namespace pa::dsp
