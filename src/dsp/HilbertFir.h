#pragma once

#include <cmath>
#include <vector>

// Constant mode's FIR Hilbert transformer (IMPLEMENTATION_PLAN 2.4; P2, prototype/p2_constant.py). Odd length N, type
// III, Kaiser-windowed (beta 6) ideal Hilbert centred at tap D = (N - 1) / 2: h[D + m] = 2 / (pi m) for odd m, 0 for
// even m, so its phase is exactly 90 degrees at every frequency. 4097 taps at 48 kHz (user, 2026-10-06), scaled with
// the rate to keep the same duration (and so the same low end), rounded to N = 1 (mod 4): D is then even, so the first
// and last taps are zero as well.
namespace pa::dsp
{
struct HilbertFir
{
    static constexpr int tapsAt48k = 4097;
    static constexpr double beta = 6.0;

    static int tapsFor(double sampleRate)
    {
        const auto scaled = tapsAt48k * sampleRate / 48000.0;
        return 4 * (int)std::lround((scaled - 1.0) / 4.0) + 1;
    }

    static std::vector<double> design(int taps)
    {
        constexpr double pi = 3.14159265358979323846;
        const auto d = (taps - 1) / 2;
        std::vector<double> h((size_t)taps, 0.0);
        for (int k = 0; k < taps; ++k)
        {
            const auto m = k - d;
            if (m % 2 == 0)
                continue;
            const auto r = 2.0 * k / (taps - 1) - 1.0; // -1 to 1 across the window
            const auto window = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(beta);
            h[(size_t)k] = 2.0 / (pi * m) * window;
        }
        return h;
    }

    static double besselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 60; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < 1.0e-17 * sum)
                break;
        }
        return sum;
    }
};
} // namespace pa::dsp
