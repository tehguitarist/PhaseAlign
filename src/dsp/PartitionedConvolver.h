#pragma once

#include "dsp/DelayLine.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <memory>
#include <vector>

// Zero-latency partitioned convolution (IMPLEMENTATION_PLAN 2.4, 2.6). Our own rather than juce::dsp::Convolution for
// exact control of latency and of when it computes:
//
// - The kernel's first B taps (the head) are applied directly, sample by sample (only its nonzero taps).
// - The rest is uniform partitioned FFT convolution (overlap-save), block B: each block of B inputs is transformed
//   with the previous block (FFT size 2B) into a frequency-domain delay line (FDL), and the tail's contribution to the
//   next output block is sum over partitions p >= 1 of H_p X_{j+1-p}, inverse-transformed. Since every tail partition
//   is at least B samples late, that is ready before it is needed, so output sample n is the convolution at n.
// - process() takes how much work to do (Work). `full`: the output. `spectra`: the FDL is kept current (one forward FFT
//   per block) but no output is computed, so full output can resume at any sample for the cost of one output block.
//   `history`: only the time-domain input history (P + 1 blocks) is kept, no FFTs; going back to the others rebuilds
//   the FDL from that history at once (P forward FFTs, a one-off spike). So a caller can stop the output whenever it is
//   multiplied by 0, at no cost to correctness.
//
// Not real-time: prepare() allocates. process() doesn't allocate, lock or log.
namespace pa::dsp
{
class PartitionedConvolver
{
  public:
    void prepare(const std::vector<float>& kernel, int blockSize, int numChannelsIn)
    {
        b = blockSize;
        int order = 0;
        while ((1 << order) < 2 * b)
            ++order;
        n = 1 << order;
        bins = n / 2 + 1;
        fft = std::make_unique<juce::dsp::FFT>(order);
        partitions = std::max(1, ((int)kernel.size() + b - 1) / b);
        numChannels = numChannelsIn;
        work.assign((size_t)(2 * n), 0.0f);

        // The head: its nonzero taps.
        head.clear();
        for (int k = 0; k < b && k < (int)kernel.size(); ++k)
            if (! std::equal_to<float>()(kernel[(size_t)k], 0.0f))
                head.push_back({k, kernel[(size_t)k]});

        // Tail partition spectra (p >= 1; slot 0 unused), split into real and imaginary parts so the multiply-adds
        // vectorise.
        hRe.assign((size_t)(partitions * bins), 0.0f);
        hIm.assign((size_t)(partitions * bins), 0.0f);
        for (int p = 1; p < partitions; ++p)
        {
            std::fill(work.begin(), work.end(), 0.0f);
            for (int k = 0; k < b && p * b + k < (int)kernel.size(); ++k)
                work[(size_t)k] = kernel[(size_t)(p * b + k)];
            fft->performRealOnlyForwardTransform(work.data(), true);
            for (int k = 0; k < bins; ++k)
            {
                hRe[(size_t)(p * bins + k)] = work[(size_t)(2 * k)];
                hIm[(size_t)(p * bins + k)] = work[(size_t)(2 * k + 1)];
            }
        }

        historyBlocks = partitions + 2; // P + 1 complete blocks plus the one filling
        channels.assign((size_t)numChannels, {});
        for (auto& c : channels)
        {
            c.history.assign((size_t)(historyBlocks * b), 0.0f);
            c.xRe.assign((size_t)(partitions * bins), 0.0f);
            c.xIm.assign((size_t)(partitions * bins), 0.0f);
            c.out.assign((size_t)b, 0.0f);
        }
        headLine.prepare(numChannels, b, b);
        yRe.assign((size_t)bins, 0.0f);
        yIm.assign((size_t)bins, 0.0f);
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
        for (auto& c : channels)
        {
            std::fill(c.history.begin(), c.history.end(), 0.0f);
            std::fill(c.xRe.begin(), c.xRe.end(), 0.0f);
            std::fill(c.xIm.begin(), c.xIm.end(), 0.0f);
            std::fill(c.out.begin(), c.out.end(), 0.0f);
        }
        headLine.clear();
        block = 0;
        pos = 0;
        spectraValid = outputValid = true; // all-zero history: the zero FDL and output are right
    }

    int getLatency() const { return 0; }
    int getBlockSize() const { return b; }

    // n samples of each channel in `in`; with Work::full, the convolution into `out` (which may alias `in`); otherwise
    // `out` gets zeros, or is untouched if null.
    void process(const float* const* in, float* const* out, int numSamples, Work mode)
    {
        for (int start = 0; start < numSamples;)
        {
            if (mode != Work::history && ! spectraValid)
                rebuildSpectra();
            if (mode == Work::full && ! outputValid)
            {
                for (auto& c : channels) // the tail's part of this block, from the FDL up to block - 1
                    computeTail(c, block);
                outputValid = true;
            }

            const auto m = std::min(numSamples - start, b - pos);
            const auto slot = wrap(block, historyBlocks) * b;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto& c = channels[(size_t)ch];
                std::copy(in[ch] + start, in[ch] + start + m, c.history.data() + slot + pos);
                headLine.write(ch, in[ch] + start, m);
                if (mode == Work::full)
                {
                    // The tail, plus the head applied directly: x[i - k] for each nonzero head tap k. Taps outside and
                    // outputs inside, so it vectorises.
                    auto* o = out[ch] + start;
                    float acc[maxHeadChunk];
                    std::copy(c.out.data() + pos, c.out.data() + pos + m, acc);
                    const auto* x = headLine.span(ch, m - 1, 0, b + m - 1) + (b - 1); // x[i - k] = x[i - k]
                    for (const auto& t : head)
                        for (int i = 0; i < m; ++i)
                            acc[i] += t.weight * x[i - t.offset];
                    std::copy(acc, acc + m, o);
                }
                else if (out != nullptr)
                    std::fill(out[ch] + start, out[ch] + start + m, 0.0f);
            }
            headLine.advance(m);
            pos += m;
            start += m;

            if (pos == b) // a block is complete
            {
                for (auto& c : channels)
                {
                    if (mode != Work::history)
                        computeSpectrum(c, block);
                    if (mode == Work::full)
                        computeTail(c, block + 1);
                }
                spectraValid = mode != Work::history;
                outputValid = mode == Work::full;
                ++block;
                pos = 0;
            }
        }
    }

  private:
    struct Channel
    {
        std::vector<float> history;  // historyBlocks blocks, by block number modulo historyBlocks
        std::vector<float> xRe, xIm; // FDL, circular: the spectrum of input block j is in slot j mod P
        std::vector<float> out;      // the output block being played
    };

    static int wrap(long long j, int count) { return (int)(((j % count) + count) % count); }

    // FFT of input blocks j - 1 and j into the FDL as the newest entry (it replaces block j - P, which drops out).
    void computeSpectrum(Channel& c, long long j)
    {
        const auto prev = wrap(j - 1, historyBlocks) * b;
        const auto cur = wrap(j, historyBlocks) * b;
        std::copy(c.history.data() + prev, c.history.data() + prev + b, work.data());
        std::copy(c.history.data() + cur, c.history.data() + cur + b, work.data() + b);
        std::fill(work.data() + 2 * b, work.data() + 2 * n, 0.0f);
        fft->performRealOnlyForwardTransform(work.data(), true);
        const auto newest = wrap(j, partitions) * bins;
        for (int k = 0; k < bins; ++k)
        {
            c.xRe[(size_t)(newest + k)] = work[(size_t)(2 * k)];
            c.xIm[(size_t)(newest + k)] = work[(size_t)(2 * k + 1)];
        }
    }

    // The tail's part of output block j (played while block j fills): sum over p >= 1 of H_p X_{j-p}, from the FDL up
    // to block j - 1, inverse-transformed.
    void computeTail(Channel& c, long long j)
    {
        std::fill(yRe.begin(), yRe.end(), 0.0f);
        std::fill(yIm.begin(), yIm.end(), 0.0f);
        for (int p = 1; p < partitions; ++p)
        {
            const auto* hr = hRe.data() + p * bins;
            const auto* hi = hIm.data() + p * bins;
            const auto slot = wrap(j - p, partitions) * bins; // input block j - p meets partition p
            const auto* xr = c.xRe.data() + slot;
            const auto* xi = c.xIm.data() + slot;
            for (int k = 0; k < bins; ++k)
            {
                yRe[(size_t)k] += hr[k] * xr[k] - hi[k] * xi[k];
                yIm[(size_t)k] += hr[k] * xi[k] + hi[k] * xr[k];
            }
        }
        for (int k = 0; k < bins; ++k)
        {
            work[(size_t)(2 * k)] = yRe[(size_t)k];
            work[(size_t)(2 * k + 1)] = yIm[(size_t)k];
        }
        std::fill(work.data() + 2 * bins, work.data() + 2 * n, 0.0f);
        fft->performRealOnlyInverseTransform(work.data());
        // Overlap-save: the last B samples of the 2B are the valid part (block j - p's convolution, shifted by pB).
        std::copy(work.data() + b, work.data() + 2 * b, c.out.data());
    }

    // After `history` work: the FDL from the last P complete blocks.
    void rebuildSpectra()
    {
        for (auto& c : channels)
            for (auto j = block - partitions; j < block; ++j)
                computeSpectrum(c, j);
        spectraValid = true;
        outputValid = false;
    }

    struct HeadTap
    {
        int offset;
        float weight;
    };
    static constexpr int maxHeadChunk = 1024; // the largest block size used

    int b = 256, n = 512, bins = 257, partitions = 1, numChannels = 0, historyBlocks = 3;
    std::vector<HeadTap> head;
    DelayLine headLine;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> hRe, hIm, yRe, yIm, work;
    std::vector<Channel> channels;
    long long block = 0; // number of the block being filled
    int pos = 0;
    bool spectraValid = true, outputValid = true;
};
} // namespace pa::dsp
