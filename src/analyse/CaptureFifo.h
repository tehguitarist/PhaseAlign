#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>

// Audio side of ANALYSE's capture (PLAN 3.5): a lock-free single-producer, single-consumer FIFO of two mono streams,
// this track's input (before the chain) and the sidechain, as they reach the plugin (so lined up with each other; no
// latency to undo). The audio thread pushes only while it is active, which is only while ANALYSE is capturing: no
// cost otherwise. Independent of the meter's capture (plan R26), which may be off or showing another view.
//
// The storage is allocated the first time ANALYSE is used (message thread), before the FIFO is first made active, and
// kept until the processor goes: the audio thread only touches it while active, so it never sees it change.
namespace pa::analyse
{
class CaptureFifo
{
  public:
    // 2^18 samples per stream: 5.5 s at 48 kHz, 1.4 s at 192 kHz, against a 30 Hz reader.
    static constexpr int capacity = 1 << 18;

    // Message thread: allocates on the first call only.
    void allocate()
    {
        if (input != nullptr)
            return;
        input = std::make_unique<float[]>((size_t)capacity);
        sidechain = std::make_unique<float[]>((size_t)capacity);
    }

    // Message thread. Starting discards whatever was left from last time.
    void setActive(bool shouldBeActive)
    {
        jassert(! shouldBeActive || input != nullptr);
        if (shouldBeActive && ! active.load())
            fifo.finishedRead(fifo.getNumReady());
        active.store(shouldBeActive, std::memory_order_release);
    }
    bool isActive() const { return active.load(std::memory_order_acquire); }

    // Audio thread: n samples of each. Real-time safe; a full FIFO drops the rest (both streams alike, so they stay
    // lined up) and counts them.
    void push(const float* in, const float* sc, int n)
    {
        if (! isActive())
            return;
        int start1, size1, start2, size2;
        fifo.prepareToWrite(n, start1, size1, start2, size2);
        std::copy(in, in + size1, input.get() + start1);
        std::copy(sc, sc + size1, sidechain.get() + start1);
        std::copy(in + size1, in + size1 + size2, input.get() + start2);
        std::copy(sc + size1, sc + size1 + size2, sidechain.get() + start2);
        fifo.finishedWrite(size1 + size2);
        if (size1 + size2 < n)
            dropped.fetch_add(n - size1 - size2, std::memory_order_relaxed);
    }

    // Message thread: up to maxSamples of each. Returns how many.
    int pull(float* in, float* sc, int maxSamples)
    {
        int start1, size1, start2, size2;
        fifo.prepareToRead(maxSamples, start1, size1, start2, size2);
        std::copy(input.get() + start1, input.get() + start1 + size1, in);
        std::copy(sidechain.get() + start1, sidechain.get() + start1 + size1, sc);
        std::copy(input.get() + start2, input.get() + start2 + size2, in + size1);
        std::copy(sidechain.get() + start2, sidechain.get() + start2 + size2, sc + size1);
        fifo.finishedRead(size1 + size2);
        return size1 + size2;
    }

    // Samples dropped since the last call (the reader fell behind).
    int takeDropped() { return dropped.exchange(0, std::memory_order_relaxed); }

  private:
    juce::AbstractFifo fifo{capacity};
    std::unique_ptr<float[]> input, sidechain;
    std::atomic<bool> active{false};
    std::atomic<int> dropped{0};
};
} // namespace pa::analyse
