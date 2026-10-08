#pragma once

#include "dsp/Chain.h"
#include "dsp/PhaseMapping.h"

#include <cmath>
#include <complex>

// The phase stage's frequency response at a static setting, in closed form: what
// the meter applies to its held cross-spectrum so the phase knob previews on a frozen picture. Unit magnitude;
// the phase is a lag (negative), as the panel angle is.
//
//   - Hi/Lo: two first-order all-passes in series, H(z) = (-p + z^-1) / (1 - p z^-1), p = (k - 1) / (k + 1), with k
//     from the mapping for the (mode, range) shape at the knob angle (PhaseMapping.h), at the rate the cascade runs at
//     (the session rate times the oversampling factor; the halfbands in between are linear phase, and the reported
//     latency compensates them).
//   - Constant: cos(theta) x + sin(theta) hilbert(x), whose response at a positive frequency is e^(-j theta).
//
// Reference for both: the stage's own DSP (AllpassCascade, ConstantRotator), checked against it in MeterTests.
namespace pa::dsp
{
// The response at one static setting, its coefficients worked out once (the ANALYSE search evaluates thousands of bins
// per setting).
class PhaseStageResponse
{
  public:
    PhaseStageResponse(const ChainSettings& settings, double sampleRate)
    {
        constexpr auto pi = mapping::pi;
        if (! settings.phaseOn)
            return;
        if (settings.phaseMode == PhaseMode::constant)
        {
            const auto theta = std::clamp(settings.phaseDegrees, 0.0, settings.phaseWide ? 180.0 : 90.0) * pi / 180.0;
            rotation = std::polar(1.0, -theta);
            return;
        }

        const mapping::Shape shape{settings.phaseMode == PhaseMode::high ? mapping::Mode::hi : mapping::Mode::lo,
                                   settings.phaseWide};
        const auto phi = mapping::knobAngle(settings.phaseDegrees, settings.phaseWide);
        const auto [theta1, theta2] = mapping::angles(shape, phi);
        sessionRate = sampleRate * Oversampler::factorFor(sampleRate);
        const auto [k1, k2] = mapping::kAngles(shape, theta1, theta2, sessionRate);
        p[0] = (k1 - 1.0) / (k1 + 1.0);
        p[1] = (k2 - 1.0) / (k2 + 1.0);
        allpass = true;
    }

    std::complex<double> operator()(double hz) const
    {
        if (! allpass)
            return rotation;
        const auto zInverse = std::polar(1.0, -2.0 * mapping::pi * hz / sessionRate);
        std::complex<double> h = 1.0;
        for (const auto pole : p)
            h *= (-pole + zInverse) / (1.0 - pole * zInverse);
        return h;
    }

  private:
    bool allpass = false;
    std::complex<double> rotation = 1.0; // phase off (1) or Constant
    double sessionRate = 48000.0, p[2] = {0.0, 0.0};
};

inline std::complex<double> phaseStageResponse(const ChainSettings& settings, double sampleRate, double hz)
{
    return PhaseStageResponse(settings, sampleRate)(hz);
}
} // namespace pa::dsp
