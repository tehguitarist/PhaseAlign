#pragma once

#include "dsp/DelayLine.h"
#include "dsp/FractionalDelay.h"
#include "dsp/Ramp.h"

// Delay in steps of 0.1 sample (IMPLEMENTATION_PLAN 2.1, 2.1a) with a dual-tap crossfade on every change. Taps are in
// tenths of a sample: a whole number of samples is a plain read (bit-exact), anything else goes through an
// interpolation kernel (FractionalDelay.h), which reads FractionalKernels::lookaheadFor() samples ahead of the tap, so
// a fractional tap must be at least that many samples. The buffer is always written, so a tap that comes back into use
// never reads stale memory. A new target that arrives during a fade is latched and starts its own fade when the
// current one ends ("latest wins"). Off is a fade to tap 0, after which the output is the input, bit-exact.
namespace pa::dsp
{
class DelayStage
{
  public:
    static constexpr int maxBlock = 32; // process() takes at most this many samples
    static constexpr int tenths = FractionalKernels::steps;

    // Allocates. maxTapTenths: the longest tap, in tenths of a sample.
    void prepare(double sampleRate, int numChannels, int maxTapTenths)
    {
        kernels.prepare(sampleRate);
        lookahead = FractionalKernels::lookaheadFor(sampleRate);
        maxTap = std::max(0, maxTapTenths);
        line.prepare(numChannels, maxTap / tenths + 1 + kernels.getTaps(), maxBlock + kernels.getTaps());
        fade.prepare(crossfadeSamples(sampleRate));
        reset(0);
    }

    // Jumps straight to the tap with no fade, and clears the history.
    void reset(int tapTenths)
    {
        line.clear();
        jumpTo(tapTenths);
    }

    // Jumps straight to the tap with no fade, keeping the history: for a switch made while the output is muted.
    void jumpTo(int tapTenths)
    {
        current = pending = clampTap(tapTenths);
        fading = false;
    }

    void setTarget(int tapTenths) { pending = clampTap(tapTenths); }

    // Silence in the history, taps unchanged: for a switch of what feeds the delay, made while the output is muted.
    void clearHistory() { line.clear(); }

    int getCurrentTap() const { return current; } // tenths

    bool isSettled() const { return ! fading && pending == current; }

    void process(float* const* io, int numChannels, int n)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            line.write(ch, io[ch], n);

        if (isSettled())
        {
            if (current != 0)
                for (int ch = 0; ch < numChannels; ++ch)
                    readTap(ch, current, 0, n, io[ch]);
            line.advance(n);
            return;
        }

        // Per-sample taps and fade gains for this block: a fade can end, and the latched target's fade
        // start, part way through.
        int from[maxBlock], to[maxBlock];
        float gain[maxBlock];
        for (int i = 0; i < n; ++i)
        {
            if (! fading && pending != current)
            {
                next = pending;
                fade.reset(0.0f);
                fade.setTarget(1.0f);
                fading = true;
            }
            from[i] = current;
            if (fading)
            {
                to[i] = next;
                gain[i] = fade.next();
                if (fade.isSettled())
                {
                    current = next;
                    fading = false;
                }
            }
            else
            {
                to[i] = current;
                gain[i] = 0.0f;
            }
        }

        // Runs of samples with the same pair of taps, each tap read as a block.
        float a[maxBlock], b[maxBlock];
        for (int start = 0; start < n;)
        {
            auto end = start + 1;
            while (end < n && from[end] == from[start] && to[end] == to[start])
                ++end;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                readTap(ch, from[start], start, end, a);
                readTap(ch, to[start], start, end, b);
                for (int i = start; i < end; ++i)
                    io[ch][i] = (1.0f - gain[i]) * a[i - start] + gain[i] * b[i - start];
            }
            start = end;
        }
        line.advance(n);
    }

  private:
    int clampTap(int t) const { return std::clamp(t, 0, maxTap); }

    // Samples [start, end) of the current block delayed by `tap` tenths, into out[0 .. end - start).
    void readTap(int ch, int tap, int start, int end, float* out) const
    {
        const auto whole = tap / tenths, fraction = tap % tenths;
        const auto n = end - start;
        if (fraction == 0)
        {
            const auto* x = line.span(ch, start + n - 1, whole, n);
            std::copy(x, x + n, out);
            return;
        }

        // y[i] = sum over k of kernel[k] * x[oldest + i + k], each sum in tap order. Outputs in register-sized chunks
        // with the taps inside, so the sums stay in registers; the loop over a chunk's outputs vectorises.
        const auto taps = kernels.getTaps();
        const auto* kernel = kernels.reversed(fraction);
        const auto* x = line.span(ch, start + n - 1, whole - lookahead, taps + n - 1);
        constexpr int chunk = 16;
        int i0 = 0;
        for (; i0 + chunk <= n; i0 += chunk)
        {
            float acc[chunk] = {};
            for (int k = 0; k < taps; ++k)
            {
                const auto w = kernel[k];
                const auto* xs = x + i0 + k;
                for (int i = 0; i < chunk; ++i)
                    acc[i] += w * xs[i];
            }
            std::copy(acc, acc + chunk, out + i0);
        }
        std::fill(out + i0, out + n, 0.0f);
        for (int k = 0; k < taps; ++k)
        {
            const auto w = kernel[k];
            for (int i = i0; i < n; ++i)
                out[i] += w * x[i + k];
        }
    }

    DelayLine line;
    FractionalKernels kernels;
    LinearRamp fade;
    int maxTap = 0, lookahead = 0;
    int current = 0, next = 0, pending = 0;
    bool fading = false;
};
} // namespace pa::dsp
