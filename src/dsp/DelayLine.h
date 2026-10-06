#pragma once

#include <algorithm>
#include <vector>

// Multichannel circular buffer with a power-of-two size, so wrapping is a mask (IMPLEMENTATION_PLAN 2.1).
// Used by the delay stage, the meter's alignment, and later by Constant mode's dry path. Every sample is stored twice,
// a buffer length apart, so any run of up to size + 1 consecutive samples can be read as one contiguous span (the
// delay's interpolation kernels).
namespace pa::dsp
{
class DelayLine
{
  public:
    // Allocates (not real-time safe). A block of up to `maxBlock` samples is written before it is read, so
    // reads of up to `maxDelay` samples back from any sample of that block stay inside the buffer.
    void prepare(int numChannels, int maxDelay, int maxBlock)
    {
        size = 1;
        while (size < maxDelay + maxBlock + 1)
            size *= 2;
        mask = size - 1;
        buffers.assign((size_t)numChannels, std::vector<float>((size_t)(2 * size), 0.0f));
        writePos = 0;
    }

    void clear()
    {
        for (auto& b : buffers)
            std::fill(b.begin(), b.end(), 0.0f);
    }

    int getNumChannels() const { return (int)buffers.size(); }

    // Writes n samples at the current block position; advance() moves past them once they have been read.
    // n <= size. As at most two contiguous runs, each stored twice.
    void write(int channel, const float* x, int n)
    {
        auto* b = buffers[(size_t)channel].data();
        const auto first = std::min(n, size - writePos);
        std::copy(x, x + first, b + writePos);
        std::copy(x, x + first, b + writePos + size);
        std::copy(x + first, x + n, b);
        std::copy(x + first, x + n, b + size);
    }

    // The sample written `delay` samples before sample i of the current block.
    float read(int channel, int i, int delay) const
    {
        return buffers[(size_t)channel][(size_t)((writePos + i - delay) & mask)];
    }

    // read(channel, i, delay) for i = 0 .. n - 1, into out.
    void read(int channel, int n, int delay, float* out) const
    {
        const auto* x = span(channel, n - 1, delay, n);
        std::copy(x, x + n, out);
    }

    // `length` consecutive samples, oldest first, the newest being the one read(channel, i, newestDelay) gives.
    // length <= size + 1, and the oldest must be within maxDelay of the block.
    const float* span(int channel, int i, int newestDelay, int length) const
    {
        return buffers[(size_t)channel].data() + ((writePos + i - newestDelay - (length - 1)) & mask);
    }

    void advance(int n) { writePos = (writePos + n) & mask; }

  private:
    std::vector<std::vector<float>> buffers;
    int size = 1, mask = 0;
    int writePos = 0;
};
} // namespace pa::dsp
