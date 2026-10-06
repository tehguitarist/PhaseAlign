#pragma once

#include "dsp/AllpassCascade.h"
#include "dsp/ConstantRotator.h"
#include "dsp/DelayStage.h"
#include "dsp/Ramp.h"

#include <algorithm>

// The processing chain: polarity flip -> phase -> delay (IMPLEMENTATION_PLAN 1, PLAN 4.1). Plain C++ that runs
// on any float buffers with its own state, so the v0.8 analyser can reuse it offline (PLAN 4.9).
//
// Real-time use: prepare() allocates; reset(), setSettings() and process() don't allocate, lock or log.
namespace pa::dsp
{
enum class PhaseMode
{
    high,
    low,
    constant
};

struct ChainSettings
{
    int delayTenths = 0; // the delay knob in tenths of a sample at the current rate, already rounded; may be negative
    bool delayOn = false;
    bool polarityInverted = false;
    bool phaseOn = true;
    double phaseDegrees = 0.0;
    PhaseMode phaseMode = PhaseMode::high;

    // Every stage out of the signal path, latency 0.
    static ChainSettings neutral()
    {
        ChainSettings s;
        s.delayOn = false;
        s.phaseOn = false;
        return s;
    }

    // What host bypass fades to: every stage out of the signal path, but the latency kept (so the delay stays on at a
    // knob value of 0, and the mode stays), so bypass delays the input by the reported latency (plan 1.3).
    ChainSettings bypassed() const
    {
        auto s = *this;
        s.delayTenths = 0;
        s.polarityInverted = false;
        s.phaseOn = false;
        return s;
    }
};

// Polarity: a gain ramp from +1 to -1 through zero (2.2). Settled at +1 it does nothing; at -1 it negates.
class PolarityStage
{
  public:
    void prepare(double sampleRate) { gain.prepare(crossfadeSamples(sampleRate), 2.0f); }

    void reset(bool inverted) { gain.reset(inverted ? -1.0f : 1.0f); }

    void setInverted(bool inverted) { gain.setTarget(inverted ? -1.0f : 1.0f); }

    bool isSettled() const { return gain.isSettled(); }

    void process(float* const* io, int numChannels, int n)
    {
        if (gain.isSettled())
        {
            if (gain.getCurrent() < 0.0f)
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < n; ++i)
                        io[ch][i] = -io[ch][i];
            return;
        }

        float g[DelayStage::maxBlock];
        gain.fill(g, n);
        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < n; ++i)
                io[ch][i] *= g[i];
    }

  private:
    LinearRamp gain;
};

// Phase stage: Hi/Lo (AllpassCascade, no latency) or Constant (ConstantRotator, latency L). The on/off crossfade mixes
// the wet path with the dry one: the input in Hi/Lo, the input delayed by L in Constant, so phase on/off never changes
// the latency. In Hi/Lo the cascade runs while the stage is off (warm, R2), and the rotator keeps only its history, so
// it can start at once. Which of the two is active (Constant or not) changes only through setConstant(), which the
// chain calls while its output is silent (a latency change). Settled off in Hi/Lo, the output is the input, bit-exact.
class PhaseStage
{
  public:
    static constexpr int maxChannels = 8;

    void prepare(double sampleRate, int numChannels)
    {
        cascade.prepare(sampleRate);
        rotator.prepare(sampleRate, numChannels);
        wetGain.prepare(crossfadeSamples(sampleRate));
    }

    void reset(const ChainSettings& s)
    {
        constant = s.phaseMode == PhaseMode::constant;
        thetaTarget = s.phaseDegrees;
        hiLoMode = s.phaseMode == PhaseMode::low ? AllpassCascade::Mode::lo : AllpassCascade::Mode::hi;
        cascade.reset(hiLoMode, s.phaseDegrees);
        rotator.reset(s.phaseDegrees);
        wetGain.reset(s.phaseOn ? 1.0f : 0.0f);
    }

    void setTargets(const ChainSettings& s)
    {
        wetGain.setTarget(s.phaseOn ? 1.0f : 0.0f);
        thetaTarget = s.phaseDegrees;
        cascade.setTarget(s.phaseDegrees);
        rotator.setTarget(s.phaseDegrees);
        if (s.phaseMode != PhaseMode::constant)
        {
            hiLoMode = s.phaseMode == PhaseMode::low ? AllpassCascade::Mode::lo : AllpassCascade::Mode::hi;
            cascade.setMode(hiLoMode); // glides (R3); unheard while Constant is active
        }
    }

    // Only while the output is silent. Back to Hi/Lo, the cascade (which doesn't run in Constant) starts from a clear
    // state at the current mode and angle.
    void setConstant(bool on)
    {
        if (on == constant)
            return;
        constant = on;
        if (! constant)
            cascade.reset(hiLoMode, thetaTarget);
    }

    bool isConstant() const { return constant; }
    int latency() const { return constant ? rotator.getLatency() : 0; }
    static int latencyFor(const ChainSettings& s, double sampleRate)
    {
        return s.phaseMode == PhaseMode::constant ? ConstantRotator::latencyFor(sampleRate) : 0;
    }

    bool isSettled() const { return wetGain.isSettled() && (constant ? rotator.isSettled() : cascade.isSettled()); }

    void process(float* const* io, int numChannels, int n)
    {
        float wet[maxChannels][DelayStage::maxBlock], dry[maxChannels][DelayStage::maxBlock];
        float *wetPtr[maxChannels], *dryPtr[maxChannels];
        for (int ch = 0; ch < numChannels; ++ch)
        {
            wetPtr[ch] = wet[ch];
            dryPtr[ch] = dry[ch];
        }
        const auto settledOff = wetGain.isSettled() && wetGain.getCurrent() <= 0.0f;

        if (constant)
        {
            rotator.process(io, wetPtr, dryPtr, numChannels, n, ! settledOff);
        }
        else
        {
            rotator.process(io, nullptr, nullptr, numChannels, n, false); // history only
            for (int ch = 0; ch < numChannels; ++ch)
            {
                std::copy(io[ch], io[ch] + n, wet[ch]);
                std::copy(io[ch], io[ch] + n, dry[ch]);
            }
            cascade.process(wetPtr, numChannels, n); // warm even while off (R2)
        }

        if (wetGain.isSettled())
        {
            const auto* from = settledOff ? dry : wet;
            for (int ch = 0; ch < numChannels; ++ch)
                std::copy(from[ch], from[ch] + n, io[ch]);
            return;
        }

        float w[DelayStage::maxBlock];
        wetGain.fill(w, n);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            std::copy(dry[ch], dry[ch] + n, io[ch]);
            crossfadeInto(io[ch], wet[ch], w, n);
        }
    }

  private:
    AllpassCascade cascade;
    ConstantRotator rotator;
    AllpassCascade::Mode hiLoMode = AllpassCascade::Mode::hi;
    double thetaTarget = 0.0;
    bool constant = false;
    LinearRamp wetGain;
};

// The chain's latency depends on the delay being on (plan 2.1a) and on Constant mode (2.4). The delay knob
// reaches d = +-maxDelayTenths, in tenths of a sample. With the delay on, the chain's latency is Lmax + H, where Lmax
// is the knob's reach rounded up to whole samples and H the interpolation kernels' lookahead, and the delay stage
// applies Lmax + H + d, so the net shift is d. With it off, the stage applies 0 and the latency is 0.
//
// A change of latency (the delay on or off, entering or leaving Constant) fades the output out (latencyFadeMs),
// switches on the sample where it lands on silence, and fades back in. Changing back during the fade-out reverses it
// without switching. When the phase path changes (entering or leaving Constant), the delay line is cleared and the new
// path is faded in before the delay, so whatever tap reads it later (the delay on now, or switched on or moved soon
// after) only ever finds silence followed by a fade-in, never the old path's audio.
class Chain
{
  public:
    static constexpr int subBlockSize = DelayStage::maxBlock; // control changes are handled per sub-block (1.3)
    static constexpr int maxChannels = PhaseStage::maxChannels;

    // Allocates. maxDelayTenths is the delay knob's reach either way at this rate, in tenths of a sample.
    void prepare(double sampleRate, int numChannelsIn, int maxDelayTenthsIn)
    {
        numChannels = std::clamp(numChannelsIn, 1, maxChannels);
        maxDelayTenths = std::max(0, maxDelayTenthsIn);
        delayLatency = delayLatencyFor(sampleRate, maxDelayTenths);
        polarity.prepare(sampleRate);
        phase.prepare(sampleRate, numChannels);
        delay.prepare(sampleRate, numChannels, DelayStage::tenths * delayLatency + maxDelayTenths);
        outputGain.prepare(latencyFadeSamples(sampleRate));
        pathGain.prepare(latencyFadeSamples(sampleRate));
        reset({});
    }

    // Jumps to `s` with no transitions and clears the history.
    void reset(const ChainSettings& s)
    {
        polarity.reset(s.polarityInverted);
        phase.reset(s);
        activeDelayOn = wantedDelayOn = s.delayOn;
        wantedConstant = s.phaseMode == PhaseMode::constant;
        delayTenths = s.delayTenths;
        delay.reset(tap());
        outputGain.reset(1.0f);
        pathGain.reset(1.0f);
    }

    // New targets; each stage glides to them from wherever it is. A latency change waits for the output to fade out.
    void setSettings(const ChainSettings& s)
    {
        polarity.setInverted(s.polarityInverted);
        phase.setTargets(s);
        wantedDelayOn = s.delayOn;
        wantedConstant = s.phaseMode == PhaseMode::constant;
        delayTenths = s.delayTenths;
        if (wantedDelayOn == activeDelayOn)
            delay.setTarget(tap());
        outputGain.setTarget(latencyChangePending() ? 0.0f : 1.0f);
    }

    // Any length. Channels beyond those prepared are left untouched.
    void process(float* const* channels, int numChannelsIn, int numSamples)
    {
        const auto nch = std::min(numChannelsIn, numChannels);
        float* sub[maxChannels];
        for (int start = 0; start < numSamples;)
        {
            if (latencyChangePending() && outputGain.isSettled()) // faded out: switch while silent
            {
                const auto phasePathChanges = wantedConstant != phase.isConstant();
                activeDelayOn = wantedDelayOn;
                delay.jumpTo(tap());
                phase.setConstant(wantedConstant);
                if (phasePathChanges)
                {
                    delay.clearHistory();
                    pathGain.reset(0.0f);
                    pathGain.setTarget(1.0f);
                }
                outputGain.setTarget(1.0f);
            }

            // Up to a sub-block, ending where a fade-out lands, so the switch happens on that exact sample whatever the
            // host's block size.
            auto n = std::min(subBlockSize, numSamples - start);
            if (latencyChangePending())
                n = std::min(n, std::max(1, outputGain.samplesToTarget()));

            for (int ch = 0; ch < nch; ++ch)
                sub[ch] = channels[ch] + start;
            polarity.process(sub, nch, n);
            phase.process(sub, nch, n);
            applyPathGain(sub, nch, n);
            delay.process(sub, nch, n);
            applyOutputGain(sub, nch, n);
            start += n;
        }
    }

    // The delay's latency while on: the knob's reach in whole samples (Lmax) plus the kernels' lookahead.
    static int delayLatencyFor(double sampleRate, int maxDelayTenths)
    {
        const auto lmax = (std::max(0, maxDelayTenths) + DelayStage::tenths - 1) / DelayStage::tenths;
        return lmax + FractionalKernels::lookaheadFor(sampleRate);
    }

    // The latency `s` needs, in samples: the delay's while it is on, plus Constant's.
    static int latencyFor(const ChainSettings& s, double sampleRate, int maxDelayTenths)
    {
        return (s.delayOn ? delayLatencyFor(sampleRate, maxDelayTenths) : 0) + PhaseStage::latencyFor(s, sampleRate);
    }

    // The largest latency at this rate (for sizing the meter's alignment).
    static int maxLatencyFor(double sampleRate, int maxDelayTenths)
    {
        return delayLatencyFor(sampleRate, maxDelayTenths) + ConstantRotator::latencyFor(sampleRate);
    }

    // The latency of what is coming out now. It changes when a latency switch happens, in the silence between the
    // fades; the meter's alignment follows it.
    int latency() const { return (activeDelayOn ? delayLatency : 0) + phase.latency(); }

    bool isSettled() const
    {
        return polarity.isSettled() && phase.isSettled() && delay.isSettled() && outputGain.isSettled() &&
               ! latencyChangePending() && pathGain.isSettled();
    }

    int getCurrentDelayTenths() const { return delay.getCurrentTap(); }

  private:
    bool latencyChangePending() const { return wantedDelayOn != activeDelayOn || wantedConstant != phase.isConstant(); }

    // The delay stage's tap, in tenths, for the active latency: the latency plus d while on, 0 while off.
    int tap() const
    {
        return activeDelayOn
                   ? DelayStage::tenths * delayLatency + std::clamp(delayTenths, -maxDelayTenths, maxDelayTenths)
                   : 0;
    }

    void applyPathGain(float* const* io, int nch, int n)
    {
        if (pathGain.isSettled())
            return;
        float g[subBlockSize];
        pathGain.fill(g, n);
        for (int ch = 0; ch < nch; ++ch)
            for (int i = 0; i < n; ++i)
                io[ch][i] *= g[i];
    }

    void applyOutputGain(float* const* io, int nch, int n)
    {
        if (outputGain.isSettled())
        {
            if (outputGain.getCurrent() < 1.0f) // silent, waiting to switch
                for (int ch = 0; ch < nch; ++ch)
                    std::fill(io[ch], io[ch] + n, 0.0f);
            return;
        }

        float g[subBlockSize];
        outputGain.fill(g, n);
        for (int ch = 0; ch < nch; ++ch)
            for (int i = 0; i < n; ++i)
                io[ch][i] *= g[i];
    }

    int numChannels = 0, maxDelayTenths = 0, delayLatency = 0;
    int delayTenths = 0;
    bool activeDelayOn = false, wantedDelayOn = false, wantedConstant = false;
    PolarityStage polarity;
    PhaseStage phase;
    DelayStage delay;
    LinearRamp outputGain; // 1, or fading around a latency change
    LinearRamp pathGain;   // 1, or fading in a new phase path ahead of the delay
};
} // namespace pa::dsp
