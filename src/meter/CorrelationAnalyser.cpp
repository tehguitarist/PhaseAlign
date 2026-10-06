#include "meter/CorrelationAnalyser.h"

#include <cmath>
#include <limits>

namespace pa::meter
{
namespace
{
constexpr float notMeasured = std::numeric_limits<float>::quiet_NaN();
}

int CorrelationAnalyser::fftSizeFor(double sampleRate)
{
    int size = 4096;
    while (size < 8192.0 * sampleRate / 48000.0 - 1.0e-6)
        size *= 2;
    return size;
}

void CorrelationAnalyser::prepare(double sampleRate)
{
    fs = sampleRate;
    fftSize = fftSizeFor(fs);
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

    const auto hopSeconds = hop / fs;
    alpha.resize((size_t)numBins);
    for (int k = 0; k < numBins; ++k)
    {
        const auto tau = std::max(tauFloorSeconds, tauCycles / std::max(k * binHz, 1.0e-3));
        alpha[(size_t)k] = 1.0 - std::exp(-hopSeconds / tau);
    }
    for (auto* v : {&xyRe, &xyIm, &zyRe, &zyIm, &xx, &zz, &yy})
        v->assign((size_t)numBins, 0.0);

    // Curve points, each covering the bins within 1/12 octave either side (the nearest bin if none).
    const auto top = std::min(maxHz, binHi * binHz);
    const auto half = std::pow(2.0, curveSmoothingOctaves / 2.0);
    curveHz.resize(curvePoints);
    curveLo.resize(curvePoints);
    curveHi.resize(curvePoints);
    for (int i = 0; i < curvePoints; ++i)
    {
        const auto f = minHz * std::pow(top / minHz, (double)i / (curvePoints - 1));
        auto lo = (int)std::ceil(f / half / binHz);
        auto hi = (int)std::ceil(f * half / binHz) - 1;
        if (hi < lo)
            lo = hi = juce::roundToInt(f / binHz);
        curveHz[(size_t)i] = (float)f;
        curveLo[(size_t)i] = juce::jlimit(binLo, binHi, lo);
        curveHi[(size_t)i] = juce::jlimit(binLo, binHi, hi);
    }

    // The time view's scale: a pure delay (unit cross-spectrum in every band bin) peaks at exactly 1.
    lagHalf = juce::roundToInt(lagRangeMs * fs / 1000.0);
    std::fill(fftData.begin(), fftData.end(), 0.0f);
    for (int k = binLo; k <= binHi; ++k)
        fftData[(size_t)(2 * k)] = 1.0f;
    fft->performRealOnlyInverseTransform(fftData.data());
    lagNorm = fftData[0];

    reset();
}

void CorrelationAnalyser::reset()
{
    for (int s = 0; s < 3; ++s)
        std::fill(history[s].begin(), history[s].end(), 0.0f);
    for (auto* v : {&xyRe, &xyIm, &zyRe, &zyIm, &xx, &zz, &yy})
        std::fill(v->begin(), v->end(), 0.0);
    writePos = sinceFrame = filled = 0;
    curveX.assign((size_t)curvePoints, notMeasured);
    curveZ.assign((size_t)curvePoints, notMeasured);
    overallX = overallZ = notMeasured;
    lagX.assign((size_t)(2 * lagHalf + 1), 0.0f);
    lagZ.assign((size_t)(2 * lagHalf + 1), 0.0f);
    peakX = peakZ = {};
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
        xyRe[kk] += a * (xr * yr + xi * yi - xyRe[kk]); // X·Y*
        xyIm[kk] += a * (xi * yr - xr * yi - xyIm[kk]);
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

    for (size_t i = 0; i < (size_t)curvePoints; ++i)
    {
        curveX[i] = correlation(xyRe, xx, curveLo[i], curveHi[i]);
        curveZ[i] = correlation(zyRe, zz, curveLo[i], curveHi[i]);
    }
    overallX = correlation(xyRe, xx, binLo, binHi);
    overallZ = correlation(zyRe, zz, binLo, binHi);
}

void CorrelationAnalyser::computeLag()
{
    if (fft == nullptr)
        return;
    lagFunction(xyRe, xyIm, lagX, peakX);
    lagFunction(zyRe, zyIm, lagZ, peakZ);
}

void CorrelationAnalyser::lagFunction(const std::vector<double>& re, const std::vector<double>& im,
                                      std::vector<float>& out, Peak& peak)
{
    // PHAT: conj(X·Y*) = X*·Y at unit magnitude, so a sidechain that is later by d peaks at +d.
    std::fill(fftData.begin(), fftData.end(), 0.0f);
    for (int k = binLo; k <= binHi; ++k)
    {
        const auto r = re[(size_t)k], i = -im[(size_t)k];
        const auto mag = std::sqrt(r * r + i * i);
        if (mag > 0.0)
        {
            fftData[(size_t)(2 * k)] = (float)(r / mag);
            fftData[(size_t)(2 * k + 1)] = (float)(i / mag);
        }
    }
    fft->performRealOnlyInverseTransform(fftData.data());

    int best = 0;
    for (int j = 0; j <= 2 * lagHalf; ++j)
    {
        const auto lag = j - lagHalf;
        out[(size_t)j] = (float)(fftData[(size_t)((lag + fftSize) % fftSize)] / lagNorm);
        if (std::abs(out[(size_t)j]) > std::abs(out[(size_t)best]))
            best = j;
    }

    // Parabolic interpolation on |value| for a first sub-sample estimate...
    auto offset = 0.0;
    if (best > 0 && best < 2 * lagHalf)
    {
        const double a = std::abs(out[(size_t)best - 1]), b = std::abs(out[(size_t)best]),
                     c = std::abs(out[(size_t)best + 1]);
        const auto denominator = a - 2.0 * b + c;
        if (denominator < 0.0)
            offset = juce::jlimit(-0.5, 0.5, 0.5 * (a - c) / denominator);
    }

    // ...then Newton steps on the lag function itself, which is band-limited, so it can be evaluated exactly between
    // samples: r(t) = sum over bins of a cos(wt) - b sin(wt), with a + jb the whitened bin. The parabola alone is
    // biased by up to about 0.1 sample on this peak shape; the delay knob steps by 0.1 sample (plan 2.1a).
    const auto centre = (double)(best - lagHalf);
    auto t = centre + offset;
    const auto sign = out[(size_t)best] < 0.0f ? -1.0 : 1.0;
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        double d1 = 0.0, d2 = 0.0;
        for (int k = binLo; k <= binHi; ++k)
        {
            const auto r = re[(size_t)k], i = -im[(size_t)k];
            const auto mag = std::sqrt(r * r + i * i);
            if (mag <= 0.0)
                continue;
            const auto w = 2.0 * juce::MathConstants<double>::pi * k / fftSize;
            const auto c = std::cos(w * t), sn = std::sin(w * t);
            d1 += -w * (r * sn + i * c) / mag;
            d2 += -w * w * (r * c - i * sn) / mag;
        }
        d1 *= sign;
        d2 *= sign;
        if (d2 >= 0.0) // not at a maximum: keep the last estimate
            break;
        t = juce::jlimit(centre - 1.0, centre + 1.0, t - d1 / d2);
    }
    offset = t - centre;

    const auto apart = juce::roundToInt(0.1e-3 * fs);
    float next = 0.0f;
    for (int j = 0; j <= 2 * lagHalf; ++j)
        if (std::abs(j - best) > apart)
            next = std::max(next, std::abs(out[(size_t)j]));

    peak.lagMs = lagMsAt(best) + offset * 1000.0 / fs;
    peak.value = out[(size_t)best];
    peak.clear = std::abs(peak.value) >= 0.3f && next <= 0.5f * std::abs(peak.value);
}
} // namespace pa::meter
