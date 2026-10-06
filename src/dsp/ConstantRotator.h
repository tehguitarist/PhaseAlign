#pragma once

#include "dsp/DelayLine.h"
#include "dsp/HilbertFir.h"
#include "dsp/PartitionedConvolver.h"

#include <algorithm>
#include <cmath>
#include <functional>

// Constant mode (IMPLEMENTATION_PLAN 2.4): y[n] = cos(theta) x[n - L] + sin(theta) (h * x)[n - (L - D)], a true
// constant rotation with h the FIR Hilbert (HilbertFir.h). L is the latency: the Hilbert's centre D less one, because
// the zero first tap is dropped and the convolver has no latency of its own (L = D - 1). At 0 degrees the output is the
// delayed input, bit-exact; at 180 its exact negation.
//
// The angle is smoothed linearly over smoothMs on a fixed grid of updateInterval samples (as AllpassCascade does), and
// cos/sin are interpolated per sample across each grid cell, so a moving angle doesn't step. The convolver's output
// is computed only while it is used: not while sin(theta) is settled at 0, nor while the stage is off; its spectra are
// kept current meanwhile, so it resumes on any sample. While another mode is active it keeps only its input history,
// and rebuilds from it on entering Constant (a one-off cost, in the silence of the switch) (2.6).
// Reference: prototype/p2_constant.py (ConstantRotator), golden-tested.
namespace pa::dsp
{
class ConstantRotator
{
  public:
    static constexpr int maxChannels = 8;
    static constexpr int updateInterval = 32;
    static constexpr double smoothMs = 20.0;

    // The convolver's blocks: the head applied directly, then each FFT level's block (PartitionedConvolver.h). Measured
    // fastest (plan 2.6): uniform 128 at 44.1/48 kHz. Where the kernel is twice as long or more, a second level of
    // larger blocks for its tail costs less than more 128-sample partitions: 2048 from 88.2 kHz. The same with vDSP and
    // with PFFFT (measured on the M1; to check against CI's x86 runners).
    static std::vector<int> blockSizesFor(double sampleRate)
    {
        return sampleRate >= 80000.0 ? std::vector<int>{128, 2048} : std::vector<int>{128};
    }

    static int latencyFor(double sampleRate) { return (HilbertFir::tapsFor(sampleRate) - 1) / 2 - 1; }

    // Allocates.
    void prepare(double sampleRate, int numChannelsIn)
    {
        numChannels = std::clamp(numChannelsIn, 1, maxChannels);
        latency = latencyFor(sampleRate);
        const auto h = HilbertFir::design(HilbertFir::tapsFor(sampleRate));
        std::vector<float> kernel(h.begin() + 1, h.end() - 1); // taps 0 and N - 1 are zero
        convolver.prepare(kernel, blockSizesFor(sampleRate), numChannels);
        dry.prepare(numChannels, latency, updateInterval);
        smoothSamples = std::max(1.0, std::round(smoothMs * 1.0e-3 * sampleRate));
        reset(0.0);
    }

    void reset(double thetaDegrees)
    {
        convolver.reset();
        dry.clear();
        theta = target = thetaDegrees;
        step = 0.0;
        untilUpdate = 0;
        trig(theta, c1, s1);
    }

    void setTarget(double thetaDegrees)
    {
        if (! std::equal_to<double>()(thetaDegrees, target))
        {
            target = thetaDegrees;
            step = (target - theta) / smoothSamples;
        }
    }

    bool isSettled() const { return std::equal_to<double>()(theta, target); }

    int getLatency() const { return latency; }

    // n samples of `in`: the delayed input x[n - L] into dryOut and, if wetNeeded, the rotation into wetOut (otherwise
    // wetOut is left alone). With dryOut null as well, only the history is kept (another mode is active): cheap writes.
    // Either output may be the input (each cell's input is taken in before any output is written), not both.
    void process(const float* const* in, float* const* wetOut, float* const* dryOut, int numChannelsIn, int n,
                 bool wetNeeded)
    {
        const auto nch = std::min(numChannelsIn, numChannels);
        float q[maxChannels][updateInterval];
        float* qp[maxChannels];
        const float* x[maxChannels];
        for (int start = 0; start < n;)
        {
            if (untilUpdate == 0)
            {
                // This cell runs from the angle now (where the last one ended) to the angle after it, linearly per
                // sample. No trig while the angle is still.
                c0 = c1, s0 = s1;
                if (! isSettled())
                {
                    advanceTheta();
                    trig(theta, c1, s1);
                }
                untilUpdate = updateInterval;
            }
            const auto m = std::min(n - start, untilUpdate);
            const auto done = updateInterval - untilUpdate; // samples of the cell already played
            const auto still = std::equal_to<double>()(s0, 0.0) && std::equal_to<double>()(s1, 0.0); // at 0 or 180
            const auto rotating = wetNeeded && ! still;

            for (int ch = 0; ch < nch; ++ch)
            {
                x[ch] = in[ch] + start;
                qp[ch] = q[ch];
                dry.write(ch, x[ch], m);
            }
            // In Constant mode (dryOut given) the convolver keeps its spectra current even while its output isn't used,
            // so it can resume on any sample without a spike; in another mode it keeps only its history.
            const auto work = rotating            ? PartitionedConvolver::Work::full
                              : dryOut != nullptr ? PartitionedConvolver::Work::spectra
                                                  : PartitionedConvolver::Work::history;
            convolver.process(x, rotating ? qp : nullptr, m, work);

            for (int ch = 0; ch < nch && dryOut != nullptr; ++ch)
            {
                auto* d = dryOut[ch] + start;
                dry.read(ch, m, latency, d);
                if (! wetNeeded)
                    continue;
                auto* w = wetOut[ch] + start;
                if (! rotating) // settled at 0 or 180 degrees: exact
                {
                    const auto sign = c0 < 0.0 ? -1.0f : 1.0f;
                    for (int i = 0; i < m; ++i)
                        w[i] = sign * d[i];
                    continue;
                }
                for (int i = 0; i < m; ++i)
                {
                    const auto t = (double)(done + i + 1) / updateInterval;
                    const auto c = c0 + (c1 - c0) * t, sn = s0 + (s1 - s0) * t;
                    w[i] = (float)(c * d[i] + sn * q[ch][i]);
                }
            }
            dry.advance(m);
            untilUpdate -= m;
            start += m;
        }
    }

  private:
    // Exact at the angles where the rotation must be exact.
    static void trig(double degrees, double& c, double& s)
    {
        if (std::equal_to<double>()(degrees, 0.0))
            c = 1.0, s = 0.0;
        else if (std::equal_to<double>()(degrees, 180.0))
            c = -1.0, s = 0.0;
        else if (std::equal_to<double>()(degrees, 90.0))
            c = 0.0, s = 1.0;
        else
        {
            const auto r = degrees * 3.14159265358979323846 / 180.0;
            c = std::cos(r), s = std::sin(r);
        }
    }

    void advanceTheta()
    {
        if (isSettled())
            return;
        theta += step * updateInterval;
        if ((step > 0.0) == (theta >= target))
            theta = target;
    }

    int numChannels = 1, latency = 0;
    PartitionedConvolver convolver;
    DelayLine dry;
    double smoothSamples = 1.0, theta = 0.0, target = 0.0, step = 0.0;
    double c0 = 1.0, s0 = 0.0, c1 = 1.0, s1 = 0.0;
    int untilUpdate = 0;
};
} // namespace pa::dsp
