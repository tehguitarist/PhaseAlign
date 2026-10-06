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
        const auto limit = std::max(first, bestAt - (long long)(0.005 * fs));
        while (onset > limit && env[(size_t)(onset - first)] > 0.2f * peak)
            --onset;
        result.index = onset;
        result.strength = best;
        return result;
    }

  private:
    double fs = 0.0;
    int size = 0;
    long long pushed = 0;
    std::vector<float> data[numStreams];
};
} // namespace pa::meter
