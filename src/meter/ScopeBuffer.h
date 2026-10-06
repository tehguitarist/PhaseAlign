#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

// The scope view's data (IMPLEMENTATION_PLAN 3, R19): the last few seconds of the three streams the meter captures
// (input, output, sidechain, already lined up by the capture), and a finder for the loudest recent onset of the
// sidechain, which the view uses as its trigger so a hit sits at a fixed place however it is zoomed. GUI thread only.
namespace pa::meter
{
class ScopeBuffer
{
  public:
    static constexpr int numStreams = 3; // input, output, sidechain, as MeterCapture
    static constexpr double seconds = 4.0;
    static constexpr float startRatio = 2.0f; // a hit's 4 ms envelope against its quietest level just before (scan)

    void prepare(double sampleRate)
    {
        fs = sampleRate;
        size = (int)std::ceil(seconds * sampleRate);
        for (auto& s : data)
            s.assign((size_t)size, 0.0f);
        pushed = 0;
    }

    void clear()
    {
        for (auto& s : data)
            std::fill(s.begin(), s.end(), 0.0f);
        pushed = 0;
    }

    double getSampleRate() const { return fs; }
    long long total() const { return pushed; }
    // The oldest and one-past-newest absolute sample indices still held.
    long long oldest() const { return std::max(0LL, pushed - size); }
    long long end() const { return pushed; }

    void push(const float* in, const float* out, const float* sc, int n)
    {
        if (size == 0)
            return;
        const float* src[numStreams] = {in, out, sc};
        for (int s = 0; s < numStreams; ++s)
            for (int i = 0; i < n; ++i)
                data[s][(size_t)((pushed + i) % size)] = src[s][i];
        pushed += n;
    }

    // The sample at an absolute index; 0 outside what is held.
    float at(int stream, long long index) const
    {
        if (index < oldest() || index >= pushed)
            return 0.0f;
        return data[stream][(size_t)(index % size)];
    }

    // The input or sidechain at a fractional index (linear), for the preview's slid copy.
    float interpolated(int stream, double index) const
    {
        const auto i = (long long)std::floor(index);
        const auto f = (float)(index - (double)i);
        return at(stream, i) * (1.0f - f) + at(stream, i + 1) * f;
    }

    struct Onset
    {
        long long index = -1; // where the sidechain starts to rise; -1: nothing loud enough
        float strength = 0.0f; // the rise of its 0.5 ms envelope over the following millisecond
    };

    // The strongest onset of the sidechain within the last `lookbackSeconds` (and at least `marginSeconds` before the
    // newest sample, so there is something to show after it): the steepest rise of its 0.5 ms envelope, then back to
    // where that envelope was a fifth of the way up.
    Onset findOnset(double lookbackSeconds, double marginSeconds = 0.05) const
    {
        Onset result;
        if (size == 0)
            return result;
        const auto first = std::max(oldest() + 8, pushed - (long long)(lookbackSeconds * fs));
        const auto last = pushed - (long long)(marginSeconds * fs);
        const auto smooth = std::max(1, (int)std::lround(0.0005 * fs)), rise = std::max(1, (int)std::lround(0.001 * fs));
        if (last - first < 4LL * (smooth + rise))
            return result;

        // The envelope over the span: the mean of |x| over the last `smooth` samples, by a running sum.
        std::vector<float> env((size_t)(last - first));
        double sum = 0.0;
        for (long long i = first - smooth; i < first; ++i)
            sum += std::abs(at(2, i));
        for (long long i = first; i < last; ++i)
        {
            sum += std::abs(at(2, i)) - std::abs(at(2, i - smooth));
            env[(size_t)(i - first)] = (float)(sum / smooth);
        }

        float best = 0.0f;
        long long bestAt = -1;
        for (long long i = first + rise; i + rise < last; ++i)
        {
            const auto r = env[(size_t)(i + rise - first)] - env[(size_t)(i - first)];
            if (r > best)
            {
                best = r;
                bestAt = i;
            }
        }
        if (bestAt < 0 || best < 1.0e-3f) // quieter than about -60 dBFS: not an onset
            return result;

        const auto peak = env[(size_t)(bestAt + rise - first)];
        auto onset = bestAt;
        const auto limit = std::max(first, bestAt - (long long)(0.008 * fs));
        while (onset > limit && env[(size_t)(onset - first)] > 0.06f * peak)
            --onset;
        result.index = onset;
        result.strength = best;
        return result;
    }

    // Streaming hit detection (R22): feed it as samples arrive. A hit is a local peak of the sidechain envelope's rise
    // (as findOnset) that is within 12 dB of the strongest rise of the last few seconds, is a real start (the 4 ms
    // envelope at least 2.5 times its quietest level of the 5 to 22 ms before: not a ripple in a ringing tail, which a sub bass or a
    // kick's body is full of) and is at least 60 ms after the last hit; it is reported once there is `postSeconds` of audio after it to capture. Cheap: only the new samples
    // are looked at.
    // The running sums of |sidechain| over the windows ending just before `scanned`, taken from the buffer, so that
    // scanning can start anywhere (a sum that starts at 0 would subtract samples it never added, and stay offset).
    void primeSums()
    {
        const auto smooth = std::max(1, (int)std::lround(0.0005 * fs)), smooth4 = std::max(1, (int)std::lround(0.004 * fs));
        envSum = envSum4 = 0.0;
        for (long long i = scanned - smooth; i < scanned; ++i)
            envSum += std::abs(at(2, i));
        for (long long i = scanned - smooth4; i < scanned; ++i)
            envSum4 += std::abs(at(2, i));
    }

    void startScanning() // from now on: whatever is in the buffer already is not scanned
    {
        if (envRing.empty() || envRingFor != fs)
        {
            envRing.assign((size_t)std::max(8, (int)std::lround(0.030 * fs)), 0.0f);
            env4Ring.assign(envRing.size(), 0.0f);
            envRingFor = fs;
        }
        scanned = pushed;
        pending.clear();
        recentMax = 0.0f;
        prevRise = prevPrevRise = 0.0f;
        lastHit = -(1LL << 40);
        for (auto& e : envRing)
            e = 0.0f;
        for (auto& e : env4Ring)
            e = 0.0f;
        primeSums();
    }

    // Scans the last `lookbackSeconds` already in the buffer, then carries on from the end: the latest hit in it that has
    // `postSeconds` of audio after it (the same detector as scan, so what is found here and later agree).
    bool scanBack(double lookbackSeconds, double postSeconds, Onset& out)
    {
        startScanning();
        scanned = std::max(oldest(), pushed - (long long)(lookbackSeconds * fs));
        primeSums();
        scan();
        return takeCompleted(postSeconds, out);
    }

    // Looks at the samples that arrived since the last call.
    void scan()
    {
        if (size == 0)
            return;
        const auto smooth = std::max(1, (int)std::lround(0.0005 * fs)), rise = std::max(1, (int)std::lround(0.001 * fs));
        const auto smooth4 = std::max(1, (int)std::lround(0.004 * fs));
        if (envRing.empty() || envRingFor != fs)
            startScanning();
        const auto decay = (float)std::exp(-1.0 / (3.0 * fs)); // the strongest recent rise fades over a few seconds
        const auto refractory = (long long)(0.06 * fs);
        const auto ring = (long long)envRing.size();
        if (scanned < oldest() + smooth4 + rise + 4) // the buffer has overrun the scan: restart from what is held
        {
            scanned = oldest() + smooth4 + rise + 4;
            primeSums();
        }
        for (; scanned < pushed; ++scanned)
        {
            const auto n = scanned;
            envSum += std::abs(at(2, n)) - std::abs(at(2, n - smooth));
            const auto env = (float)(envSum / smooth);
            envRing[(size_t)(n % ring)] = env;
            envSum4 += std::abs(at(2, n)) - std::abs(at(2, n - smooth4));
            env4Ring[(size_t)(n % ring)] = (float)(envSum4 / smooth4);
            const auto thisRise = env - envRing[(size_t)((n - rise) % ring)];
            recentMax = std::max(recentMax * decay, thisRise);

            // prevRise (at n - 1) is a local maximum: a candidate.
            const auto candidate = n - 1;
            if (prevRise >= prevPrevRise && prevRise > thisRise && prevRise >= std::max(1.0e-3f, 0.25f * recentMax) &&
                candidate - lastHit >= refractory)
            {
                const auto peakEnv = envRing[(size_t)(candidate % ring)];
                // A real start: the 4 ms energy envelope (which doesn't dip at a sub bass's zero crossings the way the
                // 0.5 ms one does) is at least 2.5 times its quietest level from 5 to 22 ms before.
                const auto peak4 = env4Ring[(size_t)(candidate % ring)];
                auto before = peak4;
                const auto from = (long long)(0.005 * fs), to = std::min<long long>((long long)(0.022 * fs), ring - 2);
                for (long long back = from; back <= to; ++back)
                    before = std::min(before, env4Ring[(size_t)((candidate - back) % ring)]);
                if (peak4 < startRatio * std::max(before, 1.0e-4f))
                {
                    prevPrevRise = prevRise;
                    prevRise = thisRise;
                    continue;
                }
                auto onset = candidate - rise;
                // Back to where the envelope was 6% of the way up: a soft click in front of a loud body is part of the
                // hit's start, so the trigger sits on it, not on the body.
                const auto limit = std::max<long long>(candidate - rise - (long long)(0.008 * fs), n - ring + 2);
                while (onset > limit && envRing[(size_t)(onset % ring)] > 0.06f * peakEnv)
                    --onset;
                pending.push_back({onset, prevRise});
                lastHit = candidate;
            }
            prevPrevRise = prevRise;
            prevRise = thisRise;
        }
    }

    // The latest hit with `postSeconds` of audio after it, if any (earlier ones are dropped). Hits that have scrolled
    // out of the buffer are dropped too.
    bool takeCompleted(double postSeconds, Onset& out)
    {
        const auto post = (long long)(postSeconds * fs);
        bool found = false;
        for (auto it = pending.begin(); it != pending.end();)
        {
            if (it->index < oldest())
                it = pending.erase(it);
            else if (it->index + post <= pushed)
            {
                out = *it;
                found = true;
                it = pending.erase(it);
            }
            else
                ++it;
        }
        return found;
    }

  private:
    double fs = 0.0;
    int size = 0;
    long long pushed = 0;
    std::vector<float> data[numStreams];

    long long scanned = 0, lastHit = -(1LL << 40);
    std::vector<float> envRing, env4Ring;
    double envRingFor = 0.0, envSum = 0.0, envSum4 = 0.0;
    float recentMax = 0.0f, prevRise = 0.0f, prevPrevRise = 0.0f;
    std::vector<Onset> pending;
};
} // namespace pa::meter
