#pragma once

#include "dsp/DelayLine.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

// Audio side of the correlation meter (IMPLEMENTATION_PLAN 3, R6): a lock-free single-producer, single-consumer
// FIFO of mono streams (input, output, sidechain, and, when asked for, the input's and output's side signals). The audio thread pushes only while the meter is active
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
        inputSide,  // (L - R) / 2 of the input and the output: only filled while wantSide() (the vectorscope's STEREO
        outputSide, // mode on a stereo track); zeros otherwise
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
    bool isTransportKnown() const { return transportKnown.load(std::memory_order_relaxed); }
    bool isTransportPlaying() const { return transportPlaying.load(std::memory_order_relaxed); }
    bool isTransportStopped() const
    {
        return transportKnown.load(std::memory_order_relaxed) && ! transportPlaying.load(std::memory_order_relaxed);
    }

    // The vectorscope's STEREO mode needs the side signals: the GUI asks for them, the audio thread fills them only then.
    void setWantSide(bool want) { wantSideFlag.store(want, std::memory_order_relaxed); }
    bool wantsSide() const { return wantSideFlag.load(std::memory_order_relaxed); }
    // Audio side: whether the main bus is stereo (a mono track has no side).
    void setStereoTrack(bool stereo) { stereoFlag.store(stereo, std::memory_order_relaxed); }
    bool isStereoTrack() const { return stereoFlag.load(std::memory_order_relaxed); }

    double getSampleRate() const { return sampleRate.load(); }

    bool hasSidechain() const { return sidechainPresent.load(std::memory_order_relaxed); }

    // Audio side, not real-time (allocates): sizes the alignment for latencies up to maxLatency, in pushes of up to
    // maxBlock samples.
    void prepareAlignment(int maxLatency, int maxBlock)
    {
        alignment.prepare(3, std::max(0, maxLatency), std::max(1, maxBlock)); // input, sidechain, input side
        alignmentMax = std::max(0, maxLatency);
        alignmentBlock = std::max(1, maxBlock);
        alignmentFresh = false;
    }

    // Audio thread: n mono samples of each stream; the output is `latency` samples behind the input and sidechain.
    // inSide and outSide (null: zeros) are the side signals, used while wantsSide(). Real-time safe. Without
    // prepareAlignment(), the latency must be 0.
    void push(const float* in, const float* out, const float* sc, int n, int latency = 0, const float* inSide = nullptr,
              const float* outSide = nullptr)
    {
        if (! isActive())
        {
            alignmentFresh = false;
            return;
        }
        if (alignment.getNumChannels() == 0)
        {
            queue(in, out, sc, n, inSide, outSide);
            return;
        }
        if (! alignmentFresh)
        {
            alignment.clear();
            alignmentFresh = true;
        }

        constexpr int chunk = 64;
        float inAligned[chunk], scAligned[chunk], sideAligned[chunk], zeros[chunk] = {};
        const auto delay = std::clamp(latency, 0, alignmentMax);
        for (int start = 0; start < n;)
        {
            const auto m = std::min({chunk, alignmentBlock, n - start});
            alignment.write(0, in + start, m);
            alignment.write(1, sc + start, m);
            alignment.write(2, inSide != nullptr ? inSide + start : zeros, m);
            alignment.read(0, m, delay, inAligned);
            alignment.read(1, m, delay, scAligned);
            alignment.read(2, m, delay, sideAligned);
            alignment.advance(m);
            queue(inAligned, out + start, scAligned, m, inSide != nullptr ? sideAligned : nullptr,
                  outSide != nullptr ? outSide + start : nullptr);
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
    void queue(const float* in, const float* out, const float* sc, int n, const float* inSide = nullptr,
               const float* outSide = nullptr)
    {
        int start1, size1, start2, size2;
        fifo.prepareToWrite(n, start1, size1, start2, size2);
        const float* src[numStreams] = {in, out, sc, inSide, outSide};
        for (int s = 0; s < numStreams; ++s)
        {
            auto* b = buffers[(size_t)s].data();
            if (src[s] == nullptr) // not wanted: zeros
            {
                std::fill(b + start1, b + start1 + size1, 0.0f);
                std::fill(b + start2, b + start2 + size2, 0.0f);
                continue;
            }
            std::copy(src[s], src[s] + size1, b + start1);
            std::copy(src[s] + size1, src[s] + size1 + size2, b + start2);
        }
        fifo.finishedWrite(size1 + size2); // a full FIFO writes fewer than n: the rest is dropped
    }

    void discard() { fifo.finishedRead(fifo.getNumReady()); }

    juce::AbstractFifo fifo{capacity};
    std::array<std::vector<float>, numStreams> buffers;
    std::atomic<bool> active{false}, sidechainPresent{false}, transportKnown{false}, transportPlaying{false},
        wantSideFlag{false}, stereoFlag{false};
    std::atomic<double> sampleRate{48000.0};

    // Audio thread only.
    pa::dsp::DelayLine alignment; // input and sidechain
    int alignmentMax = 0, alignmentBlock = 1;
    bool alignmentFresh = false;
};
} // namespace pa::meter
