#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

// Hi/Lo knob mapping (IMPLEMENTATION_PLAN 2.3, P5; reference prototype/hilo.py).
// The knob angle theta (0 to 180 degrees) is split into theta1 (section 1's lag at f1) and theta2 (section 2's lag at
// f2), and each section's k = max(tan(theta_i / 2) / tan(pi f_i / fs), kMin). The mapping is in Hz, so it is the same
// at every rate. Don't change the mapping without asking the user (CLAUDE.md).
namespace pa::dsp::mapping
{
inline constexpr double pi = 3.14159265358979323846;

struct ModeShape
{
    double f1, f2;   // section reference frequencies (Hz)
    double blendEnd; // Hi only: the theta where the split reaches the ganged pair; 0 = none (Lo)
};

inline constexpr ModeShape hi{150.1, 150.1, 125.0};
inline constexpr ModeShape lo{75.1, 20.0 * 75.1, 0.0};

// k never goes below this: lag at 20 kHz under 0.25 degrees at 44.1 kHz. At k = 0 the TPT state would sit on a pole.
inline const double kMin = std::tan(0.125 * pi / 180.0) / std::tan(pi * 20000.0 / 44100.0);

// theta (degrees) -> (theta1, theta2).
inline std::pair<double, double> split(const ModeShape& m, double theta)
{
    theta = std::clamp(theta, 0.0, 180.0);
    if (theta <= 90.0)
        return {theta, 0.0};
    if (m.blendEnd <= 0.0)
        return {90.0, theta - 90.0};
    auto s = std::min((theta - 90.0) / (m.blendEnd - 90.0), 1.0);
    s = s * s * (3.0 - 2.0 * s);
    const auto t1 = 90.0 - s * (90.0 - theta / 2.0);
    return {t1, theta - t1};
}

// theta (degrees) -> (k1, k2) at the sample rate.
inline std::pair<double, double> kPair(const ModeShape& m, double theta, double sampleRate)
{
    const auto [t1, t2] = split(m, theta);
    const auto k = [&](double t, double f)
    { return std::max(std::tan(t * pi / 360.0) / std::tan(pi * f / sampleRate), kMin); };
    return {k(t1, m.f1), k(t2, m.f2)};
}
} // namespace pa::dsp::mapping
