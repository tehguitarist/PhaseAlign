#pragma once

#include "dsp/DelayLine.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

// Audio side of the correlation meter (IMPLEMENTATION_PLAN 3, R6): a lock-free single-producer, single-consumer
// FIFO of three mono streams (input, output, sidechain). The audio thread pushes only while the meter is active
// (editor showing AND meter on); otherwise push() returns at once. If the GUI falls behind, samples are dropped
// from all three streams together, so the streams stay aligned with each other.
//
// Latency alignment: the output is later than the input by the chain's latency (4 ms while the delay is on, plan 2.1a;
// later also Constant's, 2.4), so push() delays the input and sidechain by that much before queueing them. The
// alignment history is cleared each time capture starts, and it is not written while capture is off.
namespace pa::meter
{
class MeterCapture
{
  public:
    enum Stream
    {
        input,
        output,
        sidechain,
        numStreams
    };

    // 2^16 samples per stream: 1.4 s at 48 kHz, 0.34 s at 192 kHz, against a 30 Hz reader. Allocated once here, so
    // a sample-rate change never reallocates under the reader's feet.
    static constexpr int capacity = 1 << 16;

    MeterCapture()
    {
        for (auto& b : buffers)
            b.assign((size_t)capacity, 0.0f);
    }

    // GUI side: start or stop capture. Starting discards whatever was left from the last time.
    void setActive(bool shouldBeActive)
    {
        if (shouldBeActive && ! active.load())
            discard();
        active.store(shouldBeActive);
    }

    bool isActive() const { return active.load(std::memory_order_relaxed); }

    // Audio side: also kept up to date while inactive (cheap stores), so the GUI knows them before it starts.
    void setSampleRate(double fs) { sampleRate.store(fs); }

    void setSidechainPresent(bool present) { sidechainPresent.store(present, std::memory_order_relaxed); }

    // The host's transport, when it says (not every host, and not the standalone app, does). The screen freezes while
    // a host that reports it is stopped.
    void setTransport(bool known, bool playing)
    {
        transportKnown.store(known, std::memory_order_relaxed);
        transportPlaying.store(playing, std::memory_order_relaxed);
    }
    bool isTransportStopped() const
    {
        return transportKnown.load(std::memory_order_relaxed) && ! transportPlaying.load(std::memory_order_relaxed);
    }

    double getSampleRate() const { return sampleRate.load(); }

    bool hasSidechain() const { return sidechainPresent.load(std::memory_order_relaxed); }

    // Audio side, not real-time (allocates): sizes the alignment for latencies up to maxLatency, in pushes of up to
    // maxBlock samples.
    void prepareAlignment(int maxLatency, int maxBlock)
    {
        alignment.prepare(2, std::max(0, maxLatency), std::max(1, maxBlock));
        alignmentMax = std::max(0, maxLatency);
        alignmentBlock = std::max(1, maxBlock);
        alignmentFresh = false;
    }

    // Audio thread: n mono samples of each stream; the output is `latency` samples behind the input and sidechain.
    // Real-time safe. Without prepareAlignment(), the latency must be 0.
    void push(const float* in, const float* out, const float* sc, int n, int latency = 0)
    {
        if (! isActive())
        {
            alignmentFresh = false;
            return;
        }
        if (alignment.getNumChannels() == 0)
        {
            queue(in, out, sc, n);
            return;
        }
        if (! alignmentFresh)
        {
            alignment.clear();
            alignmentFresh = true;
        }

        constexpr int chunk = 64;
        float inAligned[chunk], scAligned[chunk];
        const auto delay = std::clamp(latency, 0, alignmentMax);
        for (int start = 0; start < n;)
        {
            const auto m = std::min({chunk, alignmentBlock, n - start});
            alignment.write(0, in + start, m);
            alignment.write(1, sc + start, m);
            alignment.read(0, m, delay, inAligned);
            alignment.read(1, m, delay, scAligned);
            alignment.advance(m);
            queue(inAligned, out + start, scAligned, m);
            start += m;
        }
    }

    // GUI thread: up to maxSamples of each stream into dest[stream]. Returns how many.
    int pull(float* const* dest, int maxSamples)
    {
        int start1, size1, start2, size2;
        fifo.prepareToRead(maxSamples, start1, size1, start2, size2);
        for (int s = 0; s < numStreams; ++s)
        {
            const auto* b = buffers[(size_t)s].data();
            std::copy(b + start1, b + start1 + size1, dest[s]);
            std::copy(b + start2, b + start2 + size2, dest[s] + size1);
        }
        fifo.finishedRead(size1 + size2);
        return size1 + size2;
    }

    int getNumReady() const { return fifo.getNumReady(); }

  private:
    void queue(const float* in, const float* out, const float* sc, int n)
    {
        int start1, size1, start2, size2;
        fifo.prepareToWrite(n, start1, size1, start2, size2);
        const float* src[numStreams] = {in, out, sc};
        for (int s = 0; s < numStreams; ++s)
        {
            std::copy(src[s], src[s] + size1, buffers[(size_t)s].data() + start1);
            std::copy(src[s] + size1, src[s] + size1 + size2, buffers[(size_t)s].data() + start2);
        }
        fifo.finishedWrite(size1 + size2); // a full FIFO writes fewer than n: the rest is dropped
    }

    void discard() { fifo.finishedRead(fifo.getNumReady()); }

    juce::AbstractFifo fifo{capacity};
    std::array<std::vector<float>, numStreams> buffers;
    std::atomic<bool> active{false}, sidechainPresent{false}, transportKnown{false}, transportPlaying{false};
    std::atomic<double> sampleRate{48000.0};

    // Audio thread only.
    pa::dsp::DelayLine alignment; // input and sidechain
    int alignmentMax = 0, alignmentBlock = 1;
    bool alignmentFresh = false;
};
} // namespace pa::meter
