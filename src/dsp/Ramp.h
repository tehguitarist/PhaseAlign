#pragma once

#include <algorithm>
#include <cmath>
#include <functional>

// Linear ramps for click-free transitions. Plain C++: no JUCE, no allocation.
namespace pa::dsp
{
// One length for every crossfade and gain ramp: delay changes (dual-tap crossfade), delay on/off, polarity
// and stage on/off. 50 ms linear is click-free, with smooth delay sweeps.
inline constexpr double crossfadeMs = 50.0;

inline int crossfadeSamples(double sampleRate)
{
    return std::max(1, (int)std::lround(crossfadeMs * sampleRate / 1000.0));
}

// A latency change (the delay switched on or off; later, entering or leaving Constant) fades the output out over
// this, switches while silent, and fades back in over it (about 10 + 10 ms).
inline constexpr double latencyFadeMs = 10.0;

inline int latencyFadeSamples(double sampleRate)
{
    return std::max(1, (int)std::lround(latencyFadeMs * sampleRate / 1000.0));
}

// A value that moves towards its target at a fixed rate per sample and lands on it exactly, on the sample
// the rate says (the position is kept in double, so rounding never adds or loses a sample). Retargeting
// mid-ramp continues from the current value, so a reversal never jumps.
class LinearRamp
{
  public:
    // `samplesForFullScale`: how long a move of `fullScale` takes.
    void prepare(int samplesForFullScale, float fullScale = 1.0f)
    {
        step = (double)fullScale / (double)std::max(1, samplesForFullScale);
    }

    void reset(float value) { current = target = value; }

    void setTarget(float newTarget) { target = newTarget; }

    float getTarget() const { return (float)target; }

    float getCurrent() const { return (float)current; }

    bool isSettled() const
    {
        return std::equal_to<double>()(current, target); // exact: next() lands on the target
    }

    // How many next() calls until it lands on the target (0 if settled). Runs the same arithmetic on a copy, so it is
    // exact; it costs one step per sample of the remaining ramp.
    int samplesToTarget() const
    {
        auto copy = *this;
        int n = 0;
        for (; ! copy.isSettled(); ++n)
            copy.next();
        return n;
    }

    float next()
    {
        const auto remaining = target - current;
        if (std::abs(remaining) < 1.5 * step)
            current = target;
        else
            current += remaining > 0.0 ? step : -step;
        return (float)current;
    }

    // Fills `out` with the next n values.
    void fill(float* out, int n)
    {
        for (int i = 0; i < n; ++i)
            out[i] = next();
    }

  private:
    double current = 0.0, target = 0.0, step = 1.0;
};

// io = (1 - w) * io + w * wet, per sample. Exact at the ends: w = 0 gives io, w = 1 gives wet.
inline void crossfadeInto(float* io, const float* wet, const float* w, int n)
{
    for (int i = 0; i < n; ++i)
        io[i] = (1.0f - w[i]) * io[i] + w[i] * wet[i];
}
} // namespace pa::dsp
