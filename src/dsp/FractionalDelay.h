#pragma once

#include "dsp/FractionalKernelTables.h"

#include <algorithm>
#include <array>
#include <cmath>

// Interpolation kernels for the delay's 0.1-sample steps (study in
// reference/subsample.py). For a fraction f (1 to 9 tenths), a kernel of `taps` weights reads that many consecutive
// samples, the newest `lookahead` samples ahead of the tap: the latency it adds. Each is within 0.02 dB and 0.01
// samples (phase delay) of a pure delay from 20 Hz to 20 kHz at the rate. Two designs:
//
// - kaiser: a sinc centred at lookahead + f/10 under a Kaiser window of the same centre, normalised to unity at DC;
//   linear phase. 48 taps at 44.1 kHz, 24 at 48 kHz, 8 from 88.2 kHz up; lookahead taps/2 - 1 (23 / 11 / 3 samples).
// - lowDelay: minimax kernels designed off centre by linear programming (subsample.py part 4, tables generated into
//   FractionalKernelTables.h); not linear phase, so the group delay ripples near 20 kHz (up to 1.2 samples at
//   44.1 kHz, 0.5 at 48 kHz) though the phase delay meets the same spec. Lookahead 6 / 4 / 1 / 0 samples at
//   44.1 / 48 / 88.2 / 176.4 kHz and up, with 28 / 16 / 6 / 4 taps.
namespace pa::dsp
{
class FractionalKernels
{
  public:
    enum class Design
    {
        kaiser,
        lowDelay
    };
    static constexpr Design design = Design::kaiser; // the chosen design

    static constexpr int maxTaps = 48;
    static constexpr int steps = 10; // tenths of a sample

    static int tapsFor(double sampleRate)
    {
        if (design == Design::lowDelay)
            return tableFor(sampleRate).taps;
        return sampleRate < 46000.0 ? 48 : sampleRate < 80000.0 ? 24 : 8;
    }

    // Samples the kernels read ahead of the tap: the latency they add.
    static int lookaheadFor(double sampleRate)
    {
        if (design == Design::lowDelay)
            return tableFor(sampleRate).lookahead;
        return tapsFor(sampleRate) / 2 - 1;
    }

    void prepare(double sampleRate)
    {
        taps = tapsFor(sampleRate);
        if (design == Design::lowDelay)
        {
            const auto& table = tableFor(sampleRate);
            for (int f = 1; f < steps; ++f)
                std::copy(table.kernels + (f - 1) * taps, table.kernels + f * taps, kernels[(size_t)f].begin());
            return;
        }

        const auto beta = betaFor(sampleRate);
        const auto half = 0.5 * taps;
        for (int f = 1; f < steps; ++f)
        {
            const auto centre = lookaheadFor(sampleRate) + f / (double)steps;
            std::array<double, maxTaps> w{};
            double sum = 0.0;
            for (int k = 0; k < taps; ++k)
            {
                const auto x = k - centre;
                const auto r = x / half;
                const auto window = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(beta);
                w[(size_t)k] = sinc(x) * window;
                sum += w[(size_t)k];
            }
            // Stored reversed: reversed[j] weights the j-th of `taps` consecutive samples, oldest first.
            for (int k = 0; k < taps; ++k)
                kernels[(size_t)f][(size_t)(taps - 1 - k)] = (float)(w[(size_t)k] / sum);
        }
    }

    int getTaps() const { return taps; }

    // The kernel for a fraction of f tenths (1 to 9), oldest sample first.
    const float* reversed(int f) const { return kernels[(size_t)f].data(); }

  private:
    static double betaFor(double sampleRate)
    {
        return sampleRate < 46000.0 ? 7.0 : sampleRate < 80000.0 ? 6.0 : sampleRate < 120000.0 ? 7.0 : 8.0;
    }

    // The table designed for the highest rate at or below this one (the 44.1 kHz one below 44.1 kHz too, where no
    // design meets the spec).
    static const fractional_tables::Table& tableFor(double sampleRate)
    {
        const auto* best = &fractional_tables::tables[0];
        for (const auto& t : fractional_tables::tables)
            if (sampleRate >= t.minRate)
                best = &t;
        return *best;
    }

    static double sinc(double x)
    {
        constexpr double pi = 3.14159265358979323846;
        return std::abs(x) < 1.0e-12 ? 1.0 : std::sin(pi * x) / (pi * x);
    }

    // Modified Bessel function of the first kind, order 0 (the Kaiser window), by its power series.
    static double besselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 50; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < 1.0e-17 * sum)
                break;
        }
        return sum;
    }

    int taps = 8;
    std::array<std::array<float, maxTaps>, steps> kernels{};
};
} // namespace pa::dsp
