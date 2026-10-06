#pragma once

#include "dsp/HalfbandTables.h"
#include "dsp/Simd.h"

#include <algorithm>
#include <array>
#include <iterator>

// Hi/Lo's oversampling (IMPLEMENTATION_PLAN 2.3; reference prototype/oversampling.py, Oversampler). Up by 4 below
// 85 kHz, by 2 below 170 kHz, not at all from there up, through linear-phase halfbands, which add a pure delay (the
// latency, a whole number of samples) and no phase. Plain C++, no allocation: blocks of up to maxBlock samples at the
// session rate.
namespace pa::dsp
{
// One 2x halfband stage: length 4K - 1, centre tap 0.5 at 2K - 1, the other odd taps 0, so up is a dense 2K-tap FIR on
// the even outputs and a plain delay on the odd ones, and down a dense 2K-tap FIR on one input phase plus half of one
// sample of the other. The dense taps are symmetric, so each pair of samples is added before its multiply, and the
// filters run in blocks of 16 outputs whose sums stay in registers (simd::F4, so every compiler vectorises along the
// outputs; each sum is still in tap order).
class HalfbandStage
{
  public:
    static constexpr int maxChannels = 8;
    static constexpr int maxTaps = 30;  // dense taps, 2K
    static constexpr int maxInput = 64; // input samples per call, at the stage's lower rate (up) or per output (down)

    // evenTaps: h[0], h[2], ..., h[4K - 2], symmetric. delayedDown: down() delays its input by one sample
    // at the higher rate (the outer stage of 4x, so the latency is a whole number of session samples).
    void prepare(const float* evenTaps, int numEvenTaps, bool delayedDownIn)
    {
        dense = std::min(numEvenTaps, maxTaps);
        k = dense / 2;
        delayedDown = delayedDownIn;
        for (int j = 0; j < k; ++j)
        {
            taps[(size_t)j] = evenTaps[j];
            upTaps[(size_t)j] = 2.0f * evenTaps[j];
        }
        reset();
    }

    void reset()
    {
        for (auto* b : {&upHistory, &evenHistory, &oddHistory})
            for (auto& c : *b)
                c.fill(0.0f);
    }

    // n input samples per channel -> 2n output samples. n <= maxInput; in and out may not overlap.
    void up(const float* const* in, float* const* out, int numChannels, int n)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& h = upHistory[(size_t)ch];
            std::copy(in[ch], in[ch] + n, h.data() + history);
            const float* x = h.data() + history; // x[i - j] for j < dense is inside the history
            float acc[maxInput + block];
            symmetricFir(x, upTaps.data(), acc, n);
            float* y = out[ch];
            for (int i = 0; i < n; ++i)
            {
                y[2 * i] = acc[i];
                y[2 * i + 1] = x[i - k + 1]; // the centre tap: 2 * 0.5
            }
            std::copy(h.data() + n, h.data() + n + history, h.data());
        }
    }

    // 2n input samples per channel -> n output samples. n <= maxInput; in and out may be the same buffer. With
    // compute false, only the history moves on (the output isn't needed; out is untouched), so a later call carries on
    // exactly as if every output had been computed.
    void down(const float* const* in, float* const* out, int numChannels, int n, bool compute = true)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& eh = evenHistory[(size_t)ch];
            auto& oh = oddHistory[(size_t)ch];
            float* e = eh.data() + history;
            float* o = oh.data() + history;
            for (int i = 0; i < n; ++i)
            {
                e[i] = in[ch][2 * i];
                o[i] = in[ch][2 * i + 1];
            }
            if (compute)
            {
                // Plain: y[i] = sum_j h[2j] E[i - j] + 0.5 O[i - K]. Delayed by one input sample:
                // y[i] = sum_j h[2j] O[i - 1 - j] + 0.5 E[i - K].
                const float* d = delayedDown ? o - 1 : e;
                const float* c = delayedDown ? e : o;
                float acc[maxInput + block];
                symmetricFir(d, taps.data(), acc, n);
                for (int i = 0; i < n; ++i)
                    out[ch][i] = acc[i] + 0.5f * c[i - k];
            }
            std::copy(eh.data() + n, eh.data() + n + history, eh.data());
            std::copy(oh.data() + n, oh.data() + n + history, oh.data());
        }
    }

  private:
    static constexpr int history = maxTaps + 1; // enough for the dense taps one sample back
    static constexpr int block = 16;            // outputs per register block

    // y[i] = sum_{j < k} c[j] (x[i - j] + x[i - (dense - 1) + j]) for i < n, rounded up to whole blocks (y has room;
    // x is readable that far, its tail holding stale samples whose outputs are discarded). Four registers of sums per
    // block, the taps inside.
    void symmetricFir(const float* x, const float* c, float* y, int n) const
    {
        using namespace simd;
        static_assert(block == 16);
        for (int i0 = 0; i0 < n; i0 += block)
        {
            auto a0 = zero(), a1 = zero(), a2 = zero(), a3 = zero();
            const float* xi = x + i0;
            for (int j = 0; j < k; ++j)
            {
                const auto cj = splat(c[j]);
                const float* p = xi - j;
                const float* q = xi - (dense - 1) + j;
                a0 = madd(a0, cj, add(load(p), load(q)));
                a1 = madd(a1, cj, add(load(p + 4), load(q + 4)));
                a2 = madd(a2, cj, add(load(p + 8), load(q + 8)));
                a3 = madd(a3, cj, add(load(p + 12), load(q + 12)));
            }
            store(y + i0, a0);
            store(y + i0 + 4, a1);
            store(y + i0 + 8, a2);
            store(y + i0 + 12, a3);
        }
    }

    int dense = 0, k = 0;
    bool delayedDown = false;
    std::array<float, maxTaps / 2> taps{}, upTaps{};                        // the first half; the rest mirror them
    using Buffer = std::array<float, (size_t)(history + maxInput + block)>; // history, then the block and read slack
    std::array<Buffer, maxChannels> upHistory{}, evenHistory{}, oddHistory{};
};

class Oversampler
{
  public:
    static constexpr int maxChannels = HalfbandStage::maxChannels;
    static constexpr int maxBlock = 32; // session-rate samples per call
    static constexpr int maxFactor = 4;

    static int factorFor(double sampleRate) { return sampleRate >= 170000.0 ? 1 : sampleRate >= 85000.0 ? 2 : 4; }

    // In session samples: (N - 1) / 2 for the outer stage (N = 2 * dense - 1), plus K of the inner one at 4x
    // (prototype/oversampling.py).
    static int latencyFor(double sampleRate)
    {
        const auto f = factorFor(sampleRate);
        if (f == 1)
            return 0;
        if (f == 2)
            return wideDense - 1;
        return outerDense(sampleRate) - 1 + wideDense / 2;
    }

    void prepare(double sampleRate)
    {
        factor = factorFor(sampleRate);
        latency = latencyFor(sampleRate);
        if (factor == 2)
            outer.prepare(halfband_tables::wide, (int)std::size(halfband_tables::wide), false);
        else if (factor == 4)
        {
            if (sampleRate < 47000.0)
                outer.prepare(halfband_tables::narrow44, (int)std::size(halfband_tables::narrow44), true);
            else
                outer.prepare(halfband_tables::narrow48, (int)std::size(halfband_tables::narrow48), true);
            inner.prepare(halfband_tables::wide, (int)std::size(halfband_tables::wide), false);
        }
    }

    void reset()
    {
        outer.reset();
        inner.reset();
    }

    int getFactor() const { return factor; }
    int getLatency() const { return latency; }

    // n <= maxBlock session samples per channel -> factor * n into out (not overlapping in).
    void up(const float* const* in, float* const* out, int numChannels, int n)
    {
        if (factor == 2)
        {
            outer.up(in, out, numChannels, n);
            return;
        }
        float mid[maxChannels][2 * maxBlock];
        float* midPtr[maxChannels];
        for (int ch = 0; ch < numChannels; ++ch)
            midPtr[ch] = mid[ch];
        outer.up(in, midPtr, numChannels, n);
        inner.up(midPtr, out, numChannels, 2 * n);
    }

    // factor * n samples per channel in `in` (overwritten) -> n session samples into out. With compute false the
    // output isn't needed: the outer stage only keeps its history (the costly filter), out is untouched.
    void down(float* const* in, float* const* out, int numChannels, int n, bool compute = true)
    {
        if (factor == 4)
            inner.down(in, in, numChannels, 2 * n); // in place: the first 2n samples of each channel
        outer.down(in, out, numChannels, n, compute);
    }

  private:
    static constexpr int wideDense = (int)std::size(halfband_tables::wide);

    static int outerDense(double sampleRate)
    {
        return sampleRate < 47000.0 ? (int)std::size(halfband_tables::narrow44)
                                    : (int)std::size(halfband_tables::narrow48);
    }

    int factor = 1, latency = 0;
    HalfbandStage outer, inner;
};
} // namespace pa::dsp
