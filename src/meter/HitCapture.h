#pragma once

#include "meter/ScopeBuffer.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <vector>

// A captured hit (R22): a short stretch of this track's input and of the sidechain around one sidechain onset, held so
// the ALIGNMENT view can sit still while the knobs are turned, and the output the knobs would give for that hit,
// rendered from it. The rendering is exact for the stage's linear filters: the input's spectrum times the response of
// the delay, the polarity flip and the phase stage (dsp::phaseStageResponse), back to the time domain. The streams are
// the capture's, already lined up for the plugin's latency, so the rendered output needs none. GUI thread only.
namespace pa::meter
{
class HitCapture
{
  public:
    static constexpr double preSeconds = 0.06, postSeconds = 0.24;
    enum Stream
    {
        input = 0,
        output = 1, // the rendering of the input through the knobs' settings
        sidechain = 2,
        inputSide = 3,  // (L - R) / 2 of the input, if captured (the vectorscope's STEREO mode)
        outputSide = 4  // and its rendering
    };

    using PhaseResponse = std::function<std::complex<double>(double hz)>;
    struct Settings
    {
        double delayMs = 0.0; // 0 when the delay is off
        bool inverted = false;
        PhaseResponse phase; // empty: the phase stage is off
    };

    bool valid() const { return ! raw[input].empty(); }
    void clear()
    {
        for (auto& r : raw)
            r.clear();
        rendered.clear();
        renderedSide.clear();
    }

    // Copies the window around `onset` out of the buffer.
    void capture(const ScopeBuffer& buffer, long long onsetIndex)
    {
        fs = buffer.getSampleRate();
        first = onsetIndex - (long long)(preSeconds * fs);
        const auto length = (int)((preSeconds + postSeconds) * fs);
        onset = onsetIndex;
        for (int s : {input, sidechain, inputSide})
        {
            raw[s].resize((size_t)length);
            for (int i = 0; i < length; ++i)
                raw[s][(size_t)i] = buffer.at(s, first + i);
        }
        rendered = raw[input]; // until render() is called
        renderedSide = raw[inputSide];
    }

    // Copies `length` samples of input and sidechain from `firstIndex` on (the vectorscope's held window).
    void captureRange(const ScopeBuffer& buffer, long long firstIndex, int length)
    {
        fs = buffer.getSampleRate();
        first = firstIndex;
        onset = firstIndex;
        for (int s : {input, sidechain, inputSide})
        {
            raw[s].resize((size_t)length);
            for (int i = 0; i < length; ++i)
                raw[s][(size_t)i] = buffer.at(s, first + i);
        }
        rendered = raw[input];
        renderedSide = raw[inputSide];
    }

    // Holds a window kept elsewhere (ANALYSE's strongest hit): input and sidechain from index 0, the onset at
    // `onsetIn`.
    void captureFrom(const std::vector<float>& in, const std::vector<float>& sc, int onsetIn, double sampleRate)
    {
        fs = sampleRate;
        first = 0;
        onset = onsetIn;
        raw[input] = in;
        raw[sidechain] = sc;
        raw[inputSide].assign(in.size(), 0.0f);
        rendered = raw[input];
        renderedSide = raw[inputSide];
    }

    long long onsetIndex() const { return onset; }
    long long lastIndex() const { return first + (long long)raw[input].size() - 1; }
    double getSampleRate() const { return fs; }

    // The output the settings give: the input's spectrum times the delay's, the polarity's and the phase stage's
    // response. Zero-padded to a power of two with room for the delay and the all-pass tails.
    void render(const Settings& settings)
    {
        if (! valid())
            return;
        renderStream(raw[input], rendered, settings);
        renderStream(raw[inputSide], renderedSide, settings); // the same linear stages act on the side signal
    }

    // The sample at an absolute index (0 outside the window).
    float at(int stream, long long index) const
    {
        const auto i = index - first;
        const auto& v = stream == output ? rendered : stream == outputSide ? renderedSide : raw[stream];
        return i < 0 || i >= (long long)v.size() ? 0.0f : v[(size_t)i];
    }

    float interpolated(int stream, double index) const
    {
        const auto i = (long long)std::floor(index);
        const auto f = (float)(index - (double)i);
        return at(stream, i) * (1.0f - f) + at(stream, i + 1) * f;
    }

  private:
    double fs = 0.0;
    // The input's spectrum times the settings' response, back to the time domain.
    void renderStream(const std::vector<float>& source, std::vector<float>& result, const Settings& settings)
    {
        const auto n = (int)source.size();
        auto size = 1;
        while (size < 2 * n + (int)(0.02 * fs))
            size *= 2;
        if (! fft || fftSize != size)
        {
            fft = std::make_unique<juce::dsp::FFT>(juce::roundToInt(std::log2((double)size)));
            fftSize = size;
        }
        work.assign((size_t)(2 * size), 0.0f);
        std::copy(source.begin(), source.end(), work.begin());
        fft->performRealOnlyForwardTransform(work.data(), true);
        const auto pi = 3.14159265358979323846;
        for (int k = 0; k <= size / 2; ++k)
        {
            const auto hz = k * fs / size;
            std::complex<double> h = std::polar(1.0, -2.0 * pi * hz * settings.delayMs / 1000.0);
            if (settings.phase)
                h *= settings.phase(hz);
            if (settings.inverted)
                h = -h;
            if (k == 0 || k == size / 2)
                h = h.real(); // the real-only transform keeps those two bins real
            const std::complex<double> x(work[(size_t)(2 * k)], work[(size_t)(2 * k + 1)]);
            const auto y = x * h;
            work[(size_t)(2 * k)] = (float)y.real();
            work[(size_t)(2 * k + 1)] = (float)y.imag();
        }
        fft->performRealOnlyInverseTransform(work.data());
        result.assign(work.begin(), work.begin() + n);
    }

    long long first = 0, onset = 0;
    std::vector<float> raw[5], rendered, renderedSide, work;
    std::unique_ptr<juce::dsp::FFT> fft;
    int fftSize = 0;
};
} // namespace pa::meter
