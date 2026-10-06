#pragma once

#include "dsp/AllpassCascade.h"
#include "dsp/Oversampler.h"

#include <algorithm>

// Hi/Lo as built since the de-cramping (IMPLEMENTATION_PLAN 2.3; reference prototype/hilo.py, HiLoOversampled): the
// cascade at M times the session rate between the oversampler's halfbands, so its sections are within 2.5 degrees of
// analog ones to 20 kHz at every rate. That can't be done at zero latency (Foster's reactance theorem, plan 2.3), so
// it costs getLatency() samples: 32 at 44.1 kHz, 18 at 48, 5 at 88.2 and 96, 0 from 176.4 kHz up. The coefficient grid
// stays 32 session samples long, so the angle and glide move exactly as before.
namespace pa::dsp
{
class HiLoStage
{
  public:
    static constexpr int maxChannels = Oversampler::maxChannels;
    static constexpr int maxBlock = Oversampler::maxBlock;
    using Mode = AllpassCascade::Mode;

    static int latencyFor(double sampleRate) { return Oversampler::latencyFor(sampleRate); }

    void prepare(double sampleRate)
    {
        oversampler.prepare(sampleRate);
        const auto m = oversampler.getFactor();
        cascade.prepare(sampleRate * m, AllpassCascade::defaultCell * m);
    }

    // Jumps to the shape (mode and range) and panel angle, clears the state.
    void reset(Mode m, bool wide, double thetaDegrees)
    {
        oversampler.reset();
        cascade.reset(m, wide, thetaDegrees);
    }

    // The panel angle (0 to 90, or 0 to 180 when wide) and the shape: a change glides (AllpassCascade::set).
    void set(double thetaDegrees, Mode m, bool wide) { cascade.set(thetaDegrees, m, wide); }
    bool isSettled() const { return cascade.isSettled(); }
    double getPhi() const { return cascade.getPhi(); }
    int getLatency() const { return oversampler.getLatency(); }

    // In place, n <= maxBlock.
    void process(float* const* io, int numChannels, int n)
    {
        const auto m = oversampler.getFactor();
        if (m == 1)
        {
            cascade.process(io, numChannels, n);
            return;
        }
        float os[maxChannels][Oversampler::maxFactor * maxBlock];
        float* osPtr[maxChannels];
        for (int ch = 0; ch < numChannels; ++ch)
            osPtr[ch] = os[ch];
        oversampler.up(io, osPtr, numChannels, n);
        cascade.process(osPtr, numChannels, m * n);
        oversampler.down(osPtr, io, numChannels, n);
    }

    // Keeps the stage warm while its output isn't heard (R2): the same state afterwards as process(), for less (the
    // outer down-filter only keeps its history). The input is left as it is.
    void processUnheard(const float* const* in, int numChannels, int n)
    {
        const auto m = oversampler.getFactor();
        float os[maxChannels][Oversampler::maxFactor * maxBlock];
        float* osPtr[maxChannels];
        for (int ch = 0; ch < numChannels; ++ch)
            osPtr[ch] = os[ch];
        if (m == 1)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                std::copy(in[ch], in[ch] + n, os[ch]);
            cascade.process(osPtr, numChannels, n);
            return;
        }
        oversampler.up(in, osPtr, numChannels, n);
        cascade.process(osPtr, numChannels, m * n);
        oversampler.down(osPtr, nullptr, numChannels, n, false);
    }

  private:
    Oversampler oversampler;
    AllpassCascade cascade;
};
} // namespace pa::dsp
