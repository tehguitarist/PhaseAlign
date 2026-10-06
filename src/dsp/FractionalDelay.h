#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// Interpolation kernels for the delay's 0.1-sample steps (IMPLEMENTATION_PLAN 2.1a; user, 2026-10-06; study in
// prototype/subsample.py). Each is a sinc centred at lookahead + f/10 samples under a Kaiser window of the same centre,
// normalised to unity at DC. The tap count is the shortest that stays within 0.02 dB and 0.01 samples of a pure delay
// from 20 Hz to 20 kHz at the rate (48 at 44.1 kHz, 24 at 48 kHz, 8 from 88.2 kHz up), so the added latency,
// `lookahead` samples, is as small as it can be.
namespace pa::dsp
{
class FractionalKernels
{
  public:
    static constexpr int maxTaps = 48;
    static constexpr int steps = 10; // tenths of a sample

    static int tapsFor(double sampleRate) { return sampleRate < 46000.0 ? 48 : sampleRate < 80000.0 ? 24 : 8; }

    static double betaFor(double sampleRate)
    {
        return sampleRate < 46000.0 ? 7.0 : sampleRate < 80000.0 ? 6.0 : sampleRate < 120000.0 ? 7.0 : 8.0;
    }

    // Samples the kernels read ahead of the tap: the latency they add.
    static int lookaheadFor(double sampleRate) { return tapsFor(sampleRate) / 2 - 1; }

    void prepare(double sampleRate)
    {
        taps = tapsFor(sampleRate);
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
