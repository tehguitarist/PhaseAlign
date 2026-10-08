#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

// Hi/Lo knob mapping (see reference/hilo.py).
//
// Each (mode, range) combination is a "shape" with its own section reference frequencies: LOW 90 is one section at
// 75.1 Hz, LOW 180 two stacked sections at 150.1 Hz, HIGH 90 one section at 150.1 Hz, HIGH 180 sections at 75.1 Hz and
// 20 x 75.1 Hz. The knob's panel angle theta (0 to 90 in RANGE out, 0 to 180 in RANGE in) gives phi, the angle
// of section 1 (its lag at f1): phi = theta in RANGE out, theta / 2 in RANGE in. Section 2 carries phi in the
// RANGE in and nothing in RANGE out. Each section's k = max(tan(theta_i / 2) / tan(pi f_i / fs), kMin). The mapping
// is in Hz, so it is the same at every rate.
namespace pa::dsp::mapping
{
inline constexpr double pi = 3.14159265358979323846;

enum class Mode
{
    hi,
    lo
};

struct Shape
{
    Mode mode = Mode::hi;
    bool wide = false; // RANGE in

    friend bool operator==(const Shape& a, const Shape& b) { return a.mode == b.mode && a.wide == b.wide; }
    friend bool operator!=(const Shape& a, const Shape& b) { return ! (a == b); }
};

// The four shapes in a fixed order, matching SHAPE_ORDER in reference/hilo.py: low 90, low 180, high 90, high 180.
inline constexpr int shapeCount = 4;
inline constexpr int shapeIndex(const Shape& s)
{
    return (s.mode == Mode::hi ? 2 : 0) + (s.wide ? 1 : 0);
}
inline constexpr Shape shapeAt(int i)
{
    return {i >= 2 ? Mode::hi : Mode::lo, (i & 1) != 0};
}

struct References
{
    double f1, f2; // section reference frequencies (Hz)
};

inline constexpr References references(const Shape& s)
{
    if (s.mode == Mode::lo)
        return s.wide ? References{150.1, 150.1} : References{75.1, 75.1};
    return s.wide ? References{75.1, 20.0 * 75.1} : References{150.1, 150.1};
}

// k never goes below this: lag at 20 kHz under 0.25 degrees at 44.1 kHz. At k = 0 the pole would sit on z = -1.
inline const double kMin = std::tan(0.125 * pi / 180.0) / std::tan(pi * 20000.0 / 44100.0);

// Panel angle (degrees) -> phi, section 1's angle.
inline double knobAngle(double theta, bool wide)
{
    theta = std::clamp(theta, 0.0, wide ? 180.0 : 90.0);
    return wide ? theta / 2.0 : theta;
}

// The shape's section angles at phi: (theta1, theta2).
inline std::pair<double, double> angles(const Shape& s, double phi)
{
    return {phi, s.wide ? phi : 0.0};
}

// Section angles (degrees) -> (k1, k2) at the sample rate.
inline std::pair<double, double> kAngles(const Shape& s, double theta1, double theta2, double sampleRate)
{
    const auto r = references(s);
    const auto k = [&](double t, double f)
    { return std::max(std::tan(t * pi / 360.0) / std::tan(pi * f / sampleRate), kMin); };
    return {k(theta1, r.f1), k(theta2, r.f2)};
}
} // namespace pa::dsp::mapping
