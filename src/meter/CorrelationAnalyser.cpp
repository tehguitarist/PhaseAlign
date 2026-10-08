#include "meter/CorrelationAnalyser.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pa::meter
{
namespace
{
constexpr float notMeasured = std::numeric_limits<float>::quiet_NaN();
}

int CorrelationAnalyser::fftSizeFor(double sampleRate, Speed speed)
{
    const auto base = speed == Speed::slow ? 8192.0 : 4096.0;
    int size = 2048;
    while (size < base * sampleRate / 48000.0 - 1.0e-6)
        size *= 2;
    return size;
}

void CorrelationAnalyser::prepare(double sampleRate)
{
    fs = sampleRate;
    fftSize = fftSizeFor(fs, speed);
    fft = std::make_unique<juce::dsp::FFT>(juce::roundToInt(std::log2((double)fftSize)));
    hop = fftSize / 4;
    numBins = fftSize / 2 + 1;
    const auto binHz = fs / fftSize;

    window.resize((size_t)fftSize);
    double windowPower = 0.0;
    for (int n = 0; n < fftSize; ++n)
    {
        window[(size_t)n] = (float)(0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * n / fftSize));
        windowPower += (double)window[(size_t)n] * window[(size_t)n];
    }
    // Parseval, one-sided: the mean-square of the windowed signal in a set of bins, per unit of Σ|X|².
    powerToMeanSquare = 2.0 / (fftSize * windowPower);

    fftData.assign((size_t)(2 * fftSize), 0.0f);
    for (int s = 0; s < 3; ++s)
    {
        history[s].assign((size_t)fftSize, 0.0f);
        work[s].assign((size_t)(2 * numBins), 0.0f);
    }

    binLo = (int)std::ceil(minHz / binHz);
    binHi = std::min((int)std::floor(maxHz / binHz), numBins - 2);

    setAlpha();
    for (auto* v : {&xyRe, &zyRe, &zyIm, &xx, &zz, &yy})
        v->assign((size_t)numBins, 0.0);

    for (int b = 0; b < numBands; ++b)
    {
        bandLo[b] = juce::jlimit(binLo, binHi, (int)std::ceil(bandEdgesHz[b] / binHz));
        bandHi[b] = juce::jlimit(binLo, binHi, (int)std::ceil(bandEdgesHz[b + 1] / binHz) - 1);
    }
    previewRe.assign((size_t)numBins, 0.0);

    reset();
}

void CorrelationAnalyser::setAlpha()
{
    const auto binHz = fs / fftSize, hopSeconds = hop / fs;
    const auto floor = speed == Speed::slow ? tauFloorSeconds : fastTauFloorSeconds;
    const auto cycles = speed == Speed::slow ? tauCycles : fastTauCycles;
    alpha.resize((size_t)numBins);
    for (int k = 0; k < numBins; ++k)
    {
        const auto tau = std::max(floor, cycles / std::max(k * binHz, 1.0e-3));
        const auto a = 1.0 - std::exp(-hopSeconds / tau);
        alpha[(size_t)k] = a;
    }
}

void CorrelationAnalyser::setSpeed(Speed newSpeed)
{
    if (newSpeed == speed)
        return;
    speed = newSpeed;
    if (fs > 0.0)
        prepare(fs); // a different frame size: the averages start again
}

void CorrelationAnalyser::setPreviewDelayMs(double ms)
{
    previewActive = true;
    previewMs = ms;
    if (fft != nullptr)
        updateResults();
}

void CorrelationAnalyser::setPreview(double ms, bool inverted, PhaseResponse phase)
{
    previewInverted = inverted;
    previewPhase = std::move(phase);
    setPreviewDelayMs(ms);
}

void CorrelationAnalyser::clearPreview()
{
    if (! previewActive)
        return;
    previewActive = false;
    previewInverted = false;
    previewPhase = nullptr;
    if (fft != nullptr)
        updateResults();
}

void CorrelationAnalyser::setNeeds(const Needs& wanted)
{
    const auto newResults = wanted.bands && ! needs.bands;
    needs = wanted;
    if (fft != nullptr && newResults)
        updateResults(); // from the averages, which always run
}

void CorrelationAnalyser::reset()
{
    for (int s = 0; s < 3; ++s)
        std::fill(history[s].begin(), history[s].end(), 0.0f);
    for (auto* v : {&xyRe, &zyRe, &zyIm, &xx, &zz, &yy})
        std::fill(v->begin(), v->end(), 0.0);
    writePos = sinceFrame = filled = 0;
    bandX.assign((size_t)numBands, notMeasured);
    bandZ.assign((size_t)numBands, notMeasured);
    overallX = overallZ = notMeasured;
    silentSeconds = 0.0;
}

int CorrelationAnalyser::process(const float* in, const float* out, const float* sc, int n)
{
    if (fft == nullptr)
        return 0;

    int frames = 0;
    for (int i = 0; i < n; ++i)
    {
        history[0][(size_t)writePos] = in[i];
        history[1][(size_t)writePos] = out[i];
        history[2][(size_t)writePos] = sc[i];
        writePos = (writePos + 1) % fftSize;
        filled = std::min(filled + 1, fftSize);
        if (++sinceFrame >= hop && filled == fftSize)
        {
            analyseFrame();
            sinceFrame = 0;
            ++frames;
        }
    }
    if (frames > 0)
        updateResults();
    return frames;
}

void CorrelationAnalyser::analyseFrame()
{
    for (int s = 0; s < 3; ++s)
    {
        // Oldest sample first: writePos is where the next sample goes.
        for (int n = 0; n < fftSize; ++n)
            fftData[(size_t)n] = history[s][(size_t)((writePos + n) % fftSize)] * window[(size_t)n];
        std::fill(fftData.begin() + fftSize, fftData.end(), 0.0f);
        fft->performRealOnlyForwardTransform(fftData.data(), true);
        std::copy(fftData.begin(), fftData.begin() + 2 * numBins, work[s].begin());
    }

    const auto& z = work[0];
    const auto& x = work[1];
    const auto& y = work[2];
    double framePowerY = 0.0;
    for (int k = binLo; k <= binHi; ++k)
    {
        const auto re = (size_t)(2 * k), im = re + 1;
        const double xr = x[re], xi = x[im], zr = z[re], zi = z[im], yr = y[re], yi = y[im];
        const auto a = alpha[(size_t)k];
        const auto kk = (size_t)k;
        const auto py = yr * yr + yi * yi;
        xyRe[kk] += a * (xr * yr + xi * yi - xyRe[kk]); // Re(X·Y*): only its real part is read
        zyRe[kk] += a * (zr * yr + zi * yi - zyRe[kk]);
        zyIm[kk] += a * (zi * yr - zr * yi - zyIm[kk]);
        xx[kk] += a * (xr * xr + xi * xi - xx[kk]);
        zz[kk] += a * (zr * zr + zi * zi - zz[kk]);
        yy[kk] += a * (py - yy[kk]);
        framePowerY += py;
    }

    if (windowLevelDb(framePowerY) < gateDb)
        silentSeconds += hop / fs;
    else
        silentSeconds = 0.0;
}

double CorrelationAnalyser::windowLevelDb(double powerSum) const
{
    return 10.0 * std::log10(powerSum * powerToMeanSquare + 1.0e-30);
}

void CorrelationAnalyser::updateResults()
{
    // Processed results come from the output's cross-spectrum, or, while previewing, from the input's turned by a
    // delay of previewMs: Z·Y* · e^(-jwt) is the cross-spectrum of the input delayed by t.
    const std::vector<double>* pRe = &xyRe;
    const std::vector<double>* pPow = &xx;
    if (previewActive)
    {
        const auto samples = previewMs * fs / 1000.0;
        for (int k = binLo; k <= binHi; ++k)
        {
            const auto w = 2.0 * juce::MathConstants<double>::pi * k / fftSize * samples;
            const auto c = std::cos(w), s = std::sin(w);
            const auto kk = (size_t)k;
            // The input's cross-spectrum turned by the delay, then by the phase stage and a polarity flip (180
            // degrees).
            std::complex<double> turned(zyRe[kk] * c + zyIm[kk] * s, zyIm[kk] * c - zyRe[kk] * s);
            if (previewPhase)
                turned *= previewPhase(k * fs / fftSize);
            if (previewInverted)
                turned = -turned;
            previewRe[kk] = turned.real();
        }
        pRe = &previewRe;
        pPow = &zz;
    }

    // r over bins lo..hi, or NaN if either signal there is below the gate.
    const auto correlation = [this](const std::vector<double>& cross, const std::vector<double>& power, int lo, int hi)
    {
        double sc = 0.0, sp = 0.0, sy = 0.0;
        for (int k = lo; k <= hi; ++k)
        {
            sc += cross[(size_t)k];
            sp += power[(size_t)k];
            sy += yy[(size_t)k];
        }
        if (windowLevelDb(std::min(sp, sy)) < gateDb)
            return notMeasured;
        return (float)juce::jlimit(-1.0, 1.0, sc / std::sqrt(sp * sy));
    };

    for (int b = 0; needs.bands && b < numBands; ++b)
    {
        bandX[(size_t)b] = correlation(*pRe, *pPow, bandLo[b], bandHi[b]);
        bandZ[(size_t)b] = correlation(zyRe, zz, bandLo[b], bandHi[b]);
    }
    overallX = correlation(*pRe, *pPow, binLo, binHi);
    overallZ = correlation(zyRe, zz, binLo, binHi);
}
} // namespace pa::meter
