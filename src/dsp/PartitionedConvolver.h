#pragma once

#include "dsp/DelayLine.h"
#include "dsp/RealFft.h"
#include "dsp/Simd.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

// Zero-latency non-uniform partitioned convolution (IMPLEMENTATION_PLAN 2.4, 2.6). Our own rather than
// juce::dsp::Convolution for exact control of latency and of when it computes:
//
// - The kernel's first b0 taps (the head) are applied directly, sample by sample (only its nonzero taps).
// - The rest is split into levels of uniform partitioned FFT convolution (overlap-save), each with its own block size
//   b_k, ascending, each a multiple of the one before: level k covers taps [b_k, b_k+1) (the last level, to the end of
//   the kernel) in partitions of b_k taps. Each block of b_k inputs is transformed with the previous block (FFT size
//   2 b_k) into the level's frequency-domain delay line (FDL), and the level's contribution to its next output block is
//   the sum over its partitions of H_q X_{j+1-q}, inverse-transformed. Every partition of level k starts at least b_k
//   taps in, so that is ready before it is needed, and output sample n is the convolution at n. Small blocks near the
//   start keep the latency at zero; large blocks for the long tail cost fewer multiply-adds per sample (one level,
//   block b0, is plain uniform partitioning).
// - process() takes how much work to do (Work). `full`: the output. `spectra`: the FDLs are kept current (forward FFTs
//   only) but no output is computed, so full output can resume at any sample for the cost of one output block per
//   level. `history`: only the time-domain input history is kept, no FFTs; going back to the others rebuilds the FDLs
//   from that history at once (a one-off spike). So a caller can stop the output whenever it is multiplied by 0, at no
//   cost to correctness.
//
// Not real-time: prepare() allocates. process() doesn't allocate, lock or log.
//
// The two hot loops (the head taps and the per-bin multiply-adds) are plain register-blocked loops that GCC and clang
// vectorise. MSVC doesn't, and Windows ran the convolver at 3 to 4 times Linux's cost (CI run 37420074764, plan 2.6 L),
// so under MSVC they use dsp/Simd.h. Nowhere else changes. PA_CONVOLVER_SIMD forces the Simd.h loops on any platform,
// to test them where there is no MSVC (the arithmetic is the same, so results agree to rounding).
#if defined(_MSC_VER) || defined(PA_CONVOLVER_SIMD)
#define PA_CONVOLVER_USE_SIMD 1
#endif

namespace pa::dsp
{
class PartitionedConvolver
{
  public:
    // blockSizes: b0 (the head's length, and the first level's block), then each further level's block.
    void prepare(const std::vector<float>& kernel, const std::vector<int>& blockSizes, int numChannelsIn)
    {
        numChannels = numChannelsIn;
        headLength = blockSizes.front();
        const auto kernelLength = (int)kernel.size();

        head.clear();
        for (int k = 0; k < headLength && k < kernelLength; ++k)
            if (! std::equal_to<float>()(kernel[(size_t)k], 0.0f))
                head.push_back({k, kernel[(size_t)k]});

        levels.clear();
        for (size_t i = 0; i < blockSizes.size(); ++i)
        {
            const auto b = blockSizes[i];
            const auto end = i + 1 < blockSizes.size() ? std::min(blockSizes[i + 1], kernelLength) : kernelLength;
            if (b >= end)
                break;
            levels.push_back(std::make_unique<Level>());
            levels.back()->prepare(kernel, b, end, numChannels);
        }
        headLine.prepare(numChannels, headLength, headLength);
        reset();
    }

    enum class Work
    {
        history,
        spectra,
        full
    };

    void reset()
    {
        for (auto& level : levels)
            level->reset();
        headLine.clear();
    }

    int getLatency() const { return 0; }

    // n samples of each channel in `in`; with Work::full, the convolution into `out` (which may alias `in`); otherwise
    // `out` gets zeros, or is untouched if null.
    void process(const float* const* in, float* const* out, int numSamples, Work mode)
    {
        for (int start = 0; start < numSamples;)
        {
            auto m = std::min({numSamples - start, maxHeadChunk, headLength});
            for (auto& level : levels)
            {
                level->prepareFor(mode);
                m = std::min(m, level->untilBlockEnd());
            }

            for (int ch = 0; ch < numChannels; ++ch)
            {
                for (auto& level : levels)
                    level->write(ch, in[ch] + start, m);
                headLine.write(ch, in[ch] + start, m);
                if (mode == Work::full)
                {
                    // The levels, plus the head applied directly: x[i - k] for each nonzero head tap k, in tap order.
                    float acc[maxHeadChunk];
                    std::fill(acc, acc + m, 0.0f);
                    for (auto& level : levels)
                        level->addOutput(ch, acc, m);
                    const auto* x = headLine.span(ch, m - 1, 0, headLength + m - 1) + (headLength - 1);
                    applyHead(x, acc, m);
                    std::copy(acc, acc + m, out[ch] + start);
                }
                else if (out != nullptr)
                    std::fill(out[ch] + start, out[ch] + start + m, 0.0f);
            }
            headLine.advance(m);
            for (auto& level : levels)
                level->advance(m, mode);
            start += m;
        }
    }

  private:
    // acc[i] += sum over head taps of weight * x[i - offset], i < m. Outputs in register-sized chunks with the taps
    // inside, so the accumulators stay in registers instead of going through memory once per tap.
    void applyHead(const float* x, float* acc, int m) const
    {
        constexpr int chunk = 16;
        int i0 = 0;
#if PA_CONVOLVER_USE_SIMD
        for (; i0 + chunk <= m; i0 += chunk)
        {
            auto a0 = simd::load(acc + i0), a1 = simd::load(acc + i0 + 4), a2 = simd::load(acc + i0 + 8),
                 a3 = simd::load(acc + i0 + 12);
            for (const auto& t : head)
            {
                const auto w = simd::splat(t.weight);
                const auto* xs = x + i0 - t.offset;
                a0 = simd::madd(a0, w, simd::load(xs));
                a1 = simd::madd(a1, w, simd::load(xs + 4));
                a2 = simd::madd(a2, w, simd::load(xs + 8));
                a3 = simd::madd(a3, w, simd::load(xs + 12));
            }
            simd::store(acc + i0, a0);
            simd::store(acc + i0 + 4, a1);
            simd::store(acc + i0 + 8, a2);
            simd::store(acc + i0 + 12, a3);
        }
#else
        for (; i0 + chunk <= m; i0 += chunk)
        {
            float a[chunk];
            std::copy(acc + i0, acc + i0 + chunk, a);
            for (const auto& t : head)
            {
                const auto* xs = x + i0 - t.offset;
                for (int i = 0; i < chunk; ++i)
                    a[i] += t.weight * xs[i];
            }
            std::copy(a, a + chunk, acc + i0);
        }
#endif
        for (const auto& t : head)
            for (int i = i0; i < m; ++i)
                acc[i] += t.weight * x[i - t.offset];
    }

    // One level of uniform partitions: block b, taps [b, end) of the kernel.
    class Level
    {
      public:
        void prepare(const std::vector<float>& kernel, int blockSize, int end, int numChannelsIn)
        {
            b = blockSize;
            int order = 0;
            while ((1 << order) < 2 * b)
                ++order;
            n = 1 << order;
            bins = n / 2;
            fft.prepare(order);
            parts = (end - b + b - 1) / b;
            work.assign((size_t)n, 0.0f);

            // Partition q (1 to parts) holds taps [q b, (q + 1) b), as packed split spectra (RealFft.h; the engine's
            // scale folded in) so the multiply-adds vectorise. Stored at index q - 1.
            hRe.assign((size_t)(parts * bins), 0.0f);
            hIm.assign((size_t)(parts * bins), 0.0f);
            for (int q = 1; q <= parts; ++q)
            {
                std::fill(work.begin(), work.end(), 0.0f);
                for (int k = 0; k < b && q * b + k < end; ++k)
                    work[(size_t)k] = kernel[(size_t)(q * b + k)] * fft.convolutionScale();
                fft.forward(work.data(), hRe.data() + (q - 1) * bins, hIm.data() + (q - 1) * bins);
            }

            historyBlocks = parts + 2; // parts + 1 complete blocks plus the one filling
            channels.assign((size_t)numChannelsIn, {});
            for (auto& c : channels)
            {
                c.history.assign((size_t)(historyBlocks * b), 0.0f);
                c.xRe.assign((size_t)(parts * bins), 0.0f);
                c.xIm.assign((size_t)(parts * bins), 0.0f);
                c.out.assign((size_t)b, 0.0f);
            }
            yRe.assign((size_t)(numChannelsIn * bins), 0.0f);
            yIm.assign((size_t)(numChannelsIn * bins), 0.0f);
            slots.assign((size_t)parts, 0);
        }

        void reset()
        {
            for (auto& c : channels)
            {
                std::fill(c.history.begin(), c.history.end(), 0.0f);
                std::fill(c.xRe.begin(), c.xRe.end(), 0.0f);
                std::fill(c.xIm.begin(), c.xIm.end(), 0.0f);
                std::fill(c.out.begin(), c.out.end(), 0.0f);
            }
            block = 0;
            pos = 0;
            spectraValid = outputValid = true; // all-zero history: the zero FDL and output are right
        }

        int untilBlockEnd() const { return b - pos; }

        // Before samples are taken in with `mode`: catch up on what the mode needs.
        void prepareFor(Work mode)
        {
            if (mode != Work::history && ! spectraValid)
            {
                for (auto& c : channels) // the FDL from the last `parts` complete blocks
                    for (auto j = block - parts; j < block; ++j)
                        computeSpectrum(c, j);
                spectraValid = true;
                outputValid = false;
            }
            if (mode == Work::full && ! outputValid)
            {
                computeOutputs(block); // this block's output, from the FDL up to block - 1
                outputValid = true;
            }
        }

        void write(int ch, const float* x, int m)
        {
            auto& c = channels[(size_t)ch];
            std::copy(x, x + m, c.history.data() + wrap(block, historyBlocks) * b + pos);
        }

        void addOutput(int ch, float* acc, int m) const
        {
            const auto* o = channels[(size_t)ch].out.data() + pos;
            for (int i = 0; i < m; ++i)
                acc[i] += o[i];
        }

        void advance(int m, Work mode)
        {
            pos += m;
            if (pos < b)
                return;
            if (mode != Work::history) // a block is complete
                for (auto& c : channels)
                    computeSpectrum(c, block);
            if (mode == Work::full)
                computeOutputs(block + 1);
            spectraValid = mode != Work::history;
            outputValid = mode == Work::full;
            ++block;
            pos = 0;
        }

      private:
        struct Channel
        {
            std::vector<float> history;  // historyBlocks blocks, by block number modulo historyBlocks
            std::vector<float> xRe, xIm; // FDL, circular: the spectrum of input block j is in slot j mod parts
            std::vector<float> out;      // the output block being played
        };

        static int wrap(long long j, int count) { return (int)(((j % count) + count) % count); }

        // FFT of input blocks j - 1 and j into the FDL as the newest entry (it replaces block j - parts).
        void computeSpectrum(Channel& c, long long j)
        {
            const auto prev = wrap(j - 1, historyBlocks) * b;
            const auto cur = wrap(j, historyBlocks) * b;
            std::copy(c.history.data() + prev, c.history.data() + prev + b, work.data());
            std::copy(c.history.data() + cur, c.history.data() + cur + b, work.data() + b);
            const auto newest = (size_t)(wrap(j, parts) * bins);
            fft.forward(work.data(), c.xRe.data() + newest, c.xIm.data() + newest);
        }

        // This level's part of output block j (played while block j fills), for every channel: the sum over q of
        // H_q X_{j-q}, from the FDL up to block j - 1, inverse-transformed.
        void computeOutputs(long long j)
        {
            for (int q = 1; q <= parts; ++q)
                slots[(size_t)(q - 1)] = wrap(j - q, parts) * bins; // input block j - q meets partition q
            const auto nch = (int)channels.size();
            int ch = 0;
            for (; ch + 2 <= nch; ch += 2)
                accumulate<2>(ch);
            if (ch < nch)
                accumulate<1>(ch);

            for (ch = 0; ch < nch; ++ch)
            {
                fft.inverse(yRe.data() + ch * bins, yIm.data() + ch * bins, work.data());
                // Overlap-save: the last b samples of the 2b are the valid part.
                std::copy(work.data() + b, work.data() + 2 * b, channels[(size_t)ch].out.data());
            }
        }

        // The spectra of C channels from `first` into yRe/yIm. Bins in register-sized chunks with the partitions
        // inside, so the sums stay in registers, and each partition's H is loaded once for all C channels. Each bin
        // sums the partitions in order, as a plain loop would.
        template <int C>
        void accumulate(int first)
        {
            constexpr int chunk = 8;
            const Channel* cs[C];
            for (int c = 0; c < C; ++c)
                cs[c] = &channels[(size_t)(first + c)];
            int k0 = 0;
#if PA_CONVOLVER_USE_SIMD
            // Four bins to a vector, two vectors per chunk; the same sums in the same order as the plain loop below.
            for (; k0 + chunk <= bins; k0 += chunk)
            {
                simd::F4 ar[C][2], ai[C][2];
                for (int c = 0; c < C; ++c)
                    ar[c][0] = ar[c][1] = ai[c][0] = ai[c][1] = simd::zero();
                for (int q = 1; q <= parts; ++q)
                {
                    const auto* hr = hRe.data() + (q - 1) * bins + k0;
                    const auto* hi = hIm.data() + (q - 1) * bins + k0;
                    const auto slot = slots[(size_t)(q - 1)] + k0;
                    const auto hr0 = simd::load(hr), hr1 = simd::load(hr + 4);
                    const auto hi0 = simd::load(hi), hi1 = simd::load(hi + 4);
                    for (int c = 0; c < C; ++c)
                    {
                        const auto* xr = cs[c]->xRe.data() + slot;
                        const auto* xi = cs[c]->xIm.data() + slot;
                        const auto xr0 = simd::load(xr), xr1 = simd::load(xr + 4);
                        const auto xi0 = simd::load(xi), xi1 = simd::load(xi + 4);
                        ar[c][0] = simd::add(ar[c][0], simd::sub(simd::mul(hr0, xr0), simd::mul(hi0, xi0)));
                        ar[c][1] = simd::add(ar[c][1], simd::sub(simd::mul(hr1, xr1), simd::mul(hi1, xi1)));
                        ai[c][0] = simd::add(ai[c][0], simd::add(simd::mul(hr0, xi0), simd::mul(hi0, xr0)));
                        ai[c][1] = simd::add(ai[c][1], simd::add(simd::mul(hr1, xi1), simd::mul(hi1, xr1)));
                    }
                }
                for (int c = 0; c < C; ++c)
                {
                    simd::store(yRe.data() + (first + c) * bins + k0, ar[c][0]);
                    simd::store(yRe.data() + (first + c) * bins + k0 + 4, ar[c][1]);
                    simd::store(yIm.data() + (first + c) * bins + k0, ai[c][0]);
                    simd::store(yIm.data() + (first + c) * bins + k0 + 4, ai[c][1]);
                }
            }
#else
            for (; k0 + chunk <= bins; k0 += chunk)
            {
                float ar[C][chunk] = {}, ai[C][chunk] = {};
                for (int q = 1; q <= parts; ++q)
                {
                    const auto* hr = hRe.data() + (q - 1) * bins + k0;
                    const auto* hi = hIm.data() + (q - 1) * bins + k0;
                    const auto slot = slots[(size_t)(q - 1)] + k0;
                    for (int c = 0; c < C; ++c)
                    {
                        const auto* xr = cs[c]->xRe.data() + slot;
                        const auto* xi = cs[c]->xIm.data() + slot;
                        for (int k = 0; k < chunk; ++k)
                        {
                            ar[c][k] += hr[k] * xr[k] - hi[k] * xi[k];
                            ai[c][k] += hr[k] * xi[k] + hi[k] * xr[k];
                        }
                    }
                }
                for (int c = 0; c < C; ++c)
                {
                    std::copy(ar[c], ar[c] + chunk, yRe.data() + (first + c) * bins + k0);
                    std::copy(ai[c], ai[c] + chunk, yIm.data() + (first + c) * bins + k0);
                }
            }
#endif
            for (int c = 0; c < C; ++c) // the bins left over (none unless b < chunk)
                for (int k = k0; k < bins; ++k)
                {
                    float ar = 0.0f, ai = 0.0f;
                    for (int q = 1; q <= parts; ++q)
                    {
                        const auto hr = hRe[(size_t)((q - 1) * bins + k)], hi = hIm[(size_t)((q - 1) * bins + k)];
                        const auto slot = (size_t)(slots[(size_t)(q - 1)] + k);
                        const auto xr = cs[c]->xRe[slot], xi = cs[c]->xIm[slot];
                        ar += hr * xr - hi * xi;
                        ai += hr * xi + hi * xr;
                    }
                    yRe[(size_t)((first + c) * bins + k)] = ar;
                    yIm[(size_t)((first + c) * bins + k)] = ai;
                }
            for (int c = 0; c < C; ++c) // bin 0 is DC (re) and Nyquist (im), each real
            {
                float dc = 0.0f, nyquist = 0.0f;
                for (int q = 1; q <= parts; ++q)
                {
                    const auto h = (size_t)((q - 1) * bins), x = (size_t)slots[(size_t)(q - 1)];
                    dc += hRe[h] * cs[c]->xRe[x];
                    nyquist += hIm[h] * cs[c]->xIm[x];
                }
                yRe[(size_t)((first + c) * bins)] = dc;
                yIm[(size_t)((first + c) * bins)] = nyquist;
            }
        }

        int b = 128, n = 256, bins = 128, parts = 1, historyBlocks = 3;
        RealFft fft;
        std::vector<float> hRe, hIm, yRe, yIm, work; // y: per channel
        std::vector<int> slots;                      // FDL offset of the block each partition meets
        std::vector<Channel> channels;
        long long block = 0; // number of the block being filled
        int pos = 0;
        bool spectraValid = true, outputValid = true;
    };

    struct HeadTap
    {
        int offset;
        float weight;
    };
    static constexpr int maxHeadChunk = 1024; // the most samples taken in one step (also limited by the blocks)

    int headLength = 128, numChannels = 0;
    std::vector<HeadTap> head;
    DelayLine headLine;
    std::vector<std::unique_ptr<Level>> levels;
};
} // namespace pa::dsp
