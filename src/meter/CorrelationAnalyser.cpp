#include "meter/CorrelationAnalyser.h"

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
    for (auto* v : {&xyRe, &xyIm, &zyRe, &zyIm, &xx, &zz, &yy})
        v->assign((size_t)numBins, 0.0);

    // Curve points, each covering the bins within 1/12 octave either side (the nearest bin if none).
    const auto top = std::min(maxHz, binHi * binHz);
    const auto half = std::pow(2.0, curveSmoothingOctaves / 2.0);
    curveHz.resize(curvePoints);
    curveLo.resize(curvePoints);
    curveHi.resize(curvePoints);
    phaseLo.resize(curvePoints);
    phaseHi.resize(curvePoints);
    const auto phaseHalf = std::pow(2.0, phaseSmoothingOctaves / 2.0);
    const auto windowFor = [&](double f, double halfWidth, std::vector<int>& los, std::vector<int>& his, int i)
    {
        auto lo = (int)std::ceil(f / halfWidth / binHz);
        auto hi = (int)std::ceil(f * halfWidth / binHz) - 1;
        if (hi < lo)
            lo = hi = juce::roundToInt(f / binHz);
        los[(size_t)i] = juce::jlimit(binLo, binHi, lo);
        his[(size_t)i] = juce::jlimit(binLo, binHi, hi);
    };
    for (int i = 0; i < curvePoints; ++i)
    {
        const auto f = minHz * std::pow(top / minHz, (double)i / (curvePoints - 1));
        curveHz[(size_t)i] = (float)f;
        windowFor(f, half, curveLo, curveHi, i);
        windowFor(f, phaseHalf, phaseLo, phaseHi, i);
    }
    for (int b = 0; b < numBands; ++b)
    {
        bandLo[b] = juce::jlimit(binLo, binHi, (int)std::ceil(bandEdgesHz[b] / binHz));
        bandHi[b] = juce::jlimit(binLo, binHi, (int)std::ceil(bandEdgesHz[b + 1] / binHz) - 1);
    }
    previewRe.assign((size_t)numBins, 0.0);
    previewIm.assign((size_t)numBins, 0.0);

    // The time view's scale: a pure delay (unit cross-spectrum in every band bin) peaks at exactly 1.
    lagHalf = juce::roundToInt(lagRangeMs * fs / 1000.0);
    wideHalf = std::min(juce::roundToInt(wideLagRangeMs * fs / 1000.0), fftSize / 4);
    std::fill(fftData.begin(), fftData.end(), 0.0f);
    for (int k = binLo; k <= binHi; ++k)
        fftData[(size_t)(2 * k)] = 1.0f;
    fft->performRealOnlyInverseTransform(fftData.data());
    lagNorm = fftData[0];
    lagUnit = lagNorm / (double)(binHi - binLo + 1);
    lagWeight.assign((size_t)numBins, 0.0);

    reset();
}

void CorrelationAnalyser::setAlpha()
{
    const auto binHz = fs / fftSize, hopSeconds = hop / fs;
    const auto floor = speed == Speed::slow ? tauFloorSeconds : fastTauFloorSeconds;
    const auto cycles = speed == Speed::slow ? tauCycles : fastTauCycles;
    alpha.resize((size_t)numBins);
    lagFloor.resize((size_t)numBins);
    for (int k = 0; k < numBins; ++k)
    {
        const auto tau = std::max(floor, cycles / std::max(k * binHz, 1.0e-3));
        const auto a = 1.0 - std::exp(-hopSeconds / tau);
        alpha[(size_t)k] = a;
        // Unrelated signals still show a coherence of about 0.9 / sqrt(n) over n independent frames averaged (the
        // exponential average spans (2 - a) / a frames, a pair of 75%-overlapped ones being about one): the lag
        // function counts a bin only above 1.2 times that.
        const auto independent = std::max(1.0, 0.5 * (2.0 - a) / a);
        lagFloor[(size_t)k] = std::min(0.9, 1.06 / std::sqrt(independent));
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

void CorrelationAnalyser::clearPreview()
{
    if (! previewActive)
        return;
    previewActive = false;
    if (fft != nullptr)
        updateResults();
}

float CorrelationAnalyser::lagUnprocessedAt(double ms) const
{
    if (lagZ.empty())
        return 0.0f;
    const auto j = juce::jlimit(0, (int)lagZ.size() - 1, juce::roundToInt(ms * fs / 1000.0) + lagHalf);
    return lagZ[(size_t)j];
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
    phaseX.assign((size_t)curvePoints, notMeasured);
    phaseZ.assign((size_t)curvePoints, notMeasured);
    bandX.assign((size_t)numBands, notMeasured);
    bandZ.assign((size_t)numBands, notMeasured);
    cohX.assign((size_t)curvePoints, 0.0f);
    cohZ.assign((size_t)curvePoints, 0.0f);
    overallX = overallZ = notMeasured;
    lagX.assign((size_t)(2 * lagHalf + 1), 0.0f);
    lagZ.assign((size_t)(2 * lagHalf + 1), 0.0f);
    peakX = peakZ = wideZ = {};
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
    // Processed results come from the output's cross-spectrum, or, while previewing, from the input's turned by a
    // delay of previewMs: Z·Y* · e^(-jwt) is the cross-spectrum of the input delayed by t.
    const std::vector<double>* pRe = &xyRe;
    const std::vector<double>* pIm = &xyIm;
    const std::vector<double>* pPow = &xx;
    if (previewActive)
    {
        const auto samples = previewMs * fs / 1000.0;
        for (int k = binLo; k <= binHi; ++k)
        {
            const auto w = 2.0 * juce::MathConstants<double>::pi * k / fftSize * samples;
            const auto c = std::cos(w), s = std::sin(w);
            const auto kk = (size_t)k;
            previewRe[kk] = zyRe[kk] * c + zyIm[kk] * s;
            previewIm[kk] = zyIm[kk] * c - zyRe[kk] * s;
        }
        pRe = &previewRe;
        pIm = &previewIm;
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

    // The angle of the cross-spectrum summed over bins lo..hi, and its coherence.
    const auto phase = [this](const std::vector<double>& re, const std::vector<double>& im,
                              const std::vector<double>& power, int lo, int hi, float& angle, float& coherence)
    {
        double sr = 0.0, si = 0.0, sp = 0.0, sy = 0.0;
        for (int k = lo; k <= hi; ++k)
        {
            sr += re[(size_t)k];
            si += im[(size_t)k];
            sp += power[(size_t)k];
            sy += yy[(size_t)k];
        }
        coherence = 0.0f;
        angle = notMeasured;
        if (windowLevelDb(std::min(sp, sy)) < gateDb)
            return;
        coherence = (float)juce::jlimit(0.0, 1.0, std::hypot(sr, si) / std::sqrt(sp * sy));
        // Uncorrelated signals still read about 0.9 / sqrt(n) over n independent averages (a few frames of each bin
        // in the window), so a window of only a bin or two needs more than a wide one to count as a real angle.
        const auto floor = std::max((double)minPhaseCoherence, 1.2 / std::sqrt(6.0 * (hi - lo + 1)));
        if (coherence >= floor)
            angle = (float)(std::atan2(si, sr) * 180.0 / juce::MathConstants<double>::pi);
    };

    for (size_t i = 0; i < (size_t)curvePoints; ++i)
    {
        curveX[i] = correlation(*pRe, *pPow, curveLo[i], curveHi[i]);
        curveZ[i] = correlation(zyRe, zz, curveLo[i], curveHi[i]);
        phase(*pRe, *pIm, *pPow, phaseLo[i], phaseHi[i], phaseX[i], cohX[i]);
        phase(zyRe, zyIm, zz, phaseLo[i], phaseHi[i], phaseZ[i], cohZ[i]);
    }
    for (int b = 0; b < numBands; ++b)
    {
        bandX[(size_t)b] = correlation(*pRe, *pPow, bandLo[b], bandHi[b]);
        bandZ[(size_t)b] = correlation(zyRe, zz, bandLo[b], bandHi[b]);
    }
    overallX = correlation(*pRe, *pPow, binLo, binHi);
    overallZ = correlation(zyRe, zz, binLo, binHi);
}

void CorrelationAnalyser::computeLag()
{
    if (fft == nullptr)
        return;
    lagFunction(xyRe, xyIm, xx, lagX, peakX);
    lagFunction(zyRe, zyIm, zz, lagZ, peakZ, &wideZ);
}

CorrelationAnalyser::Peak CorrelationAnalyser::inputPeak() const
{
    return wideZ.clear && std::abs(wideZ.lagMs) > lagRangeMs ? wideZ : peakZ;
}

bool CorrelationAnalyser::inputOutOfReach(double maxMs) const
{
    const auto peak = inputPeak();
    const auto halfStepMs = 0.05 * 1000.0 / fs;
    return peak.clear && std::abs(peak.lagMs) > maxMs + halfStepMs;
}

void CorrelationAnalyser::lagFunction(const std::vector<double>& re, const std::vector<double>& im,
                                      const std::vector<double>& power, std::vector<float>& out, Peak& peak,
                                      Peak* wide)
{
    // PHAT: conj(X·Y*) = X*·Y at unit magnitude, so a sidechain that is later by d peaks at +d. Each bin counts in
    // proportion to its squared coherence, |X·Y*|² / (|X|²|Y|²): a bin the two signals share nothing in is noise, and
    // there can be a thousand of those against a few that carry the signal.
    std::fill(fftData.begin(), fftData.end(), 0.0f);
    double weightSum = 0.0;
    for (int k = binLo; k <= binHi; ++k)
    {
        const auto kk = (size_t)k;
        const auto r = re[kk], i = -im[kk];
        const auto mag2 = r * r + i * i;
        const auto denominator = power[kk] * yy[kk];
        // Weight 0 at the bin's noise floor of coherence (see setAlpha), 1 at full coherence, squared.
        const auto coherenceFloor = lagFloor[kk];
        const auto coherence = mag2 > 0.0 && denominator > 0.0 ? std::sqrt(std::min(1.0, mag2 / denominator)) : 0.0;
        const auto w = coherence > coherenceFloor ? std::pow((coherence - coherenceFloor) / (1.0 - coherenceFloor), 2.0) : 0.0;
        lagWeight[kk] = w;
        weightSum += w;
        if (w > 0.0)
        {
            const auto mag = std::sqrt(mag2);
            fftData[(size_t)(2 * k)] = (float)(w * r / mag);
            fftData[(size_t)(2 * k + 1)] = (float)(w * i / mag);
        }
    }
    fft->performRealOnlyInverseTransform(fftData.data());
    const auto norm = std::max(lagUnit * weightSum, 1.0e-30);

    // The coarse wide search: the strongest lag within +-wideHalf samples, whole samples only, with the same test for
    // being clear. Beyond the time view's range, it is what says the delay can't reach (plan 2.1a).
    const auto apart = juce::roundToInt(0.1e-3 * fs);
    if (wide != nullptr)
    {
        const auto at = [&](int lag) { return (float)(fftData[(size_t)((lag + fftSize) % fftSize)] / norm); };
        int bestLag = 0;
        for (int lag = -wideHalf; lag <= wideHalf; ++lag)
            if (std::abs(at(lag)) > std::abs(at(bestLag)))
                bestLag = lag;
        // The runner-up is the strongest value outside the peak's main lobe (to where it first crosses zero, and at
        // least 0.1 ms either side): a band-limited peak, a kick's, is wide and has side lobes of its own.
        const auto sign = at(bestLag) < 0.0f ? -1.0f : 1.0f;
        int lobeLo = bestLag, lobeHi = bestLag;
        while (lobeLo > -wideHalf && sign * at(lobeLo - 1) > 0.0f)
            --lobeLo;
        while (lobeHi < wideHalf && sign * at(lobeHi + 1) > 0.0f)
            ++lobeHi;
        lobeLo = std::min(lobeLo, bestLag - apart);
        lobeHi = std::max(lobeHi, bestLag + apart);
        float runnerUp = 0.0f;
        for (int lag = -wideHalf; lag <= wideHalf; ++lag)
            if (lag < lobeLo || lag > lobeHi)
                runnerUp = std::max(runnerUp, std::abs(at(lag)));
        wide->lagMs = bestLag * 1000.0 / fs;
        wide->value = at(bestLag);
        wide->clear = std::abs(wide->value) >= 0.3f && runnerUp <= 0.5f * std::abs(wide->value);
        wide->coarse = true;
    }

    int best = 0;
    for (int j = 0; j <= 2 * lagHalf; ++j)
    {
        const auto lag = j - lagHalf;
        out[(size_t)j] = (float)(fftData[(size_t)((lag + fftSize) % fftSize)] / norm);
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
            const auto weight = lagWeight[(size_t)k];
            const auto r = re[(size_t)k], i = -im[(size_t)k];
            const auto mag = std::sqrt(r * r + i * i);
            if (weight <= 0.0 || mag <= 0.0)
                continue;
            const auto w = 2.0 * juce::MathConstants<double>::pi * k / fftSize;
            const auto c = std::cos(w * t), sn = std::sin(w * t);
            d1 += -weight * w * (r * sn + i * c) / mag;
            d2 += -weight * w * w * (r * c - i * sn) / mag;
        }
        d1 *= sign;
        d2 *= sign;
        if (d2 >= 0.0) // not at a maximum: keep the last estimate
            break;
        t = juce::jlimit(centre - 1.0, centre + 1.0, t - d1 / d2);
    }
    offset = t - centre;

    const auto peakSign = out[(size_t)best] < 0.0f ? -1.0f : 1.0f;
    int lobeLo = best, lobeHi = best;
    while (lobeLo > 0 && peakSign * out[(size_t)lobeLo - 1] > 0.0f)
        --lobeLo;
    while (lobeHi < 2 * lagHalf && peakSign * out[(size_t)lobeHi + 1] > 0.0f)
        ++lobeHi;
    lobeLo = std::min(lobeLo, best - apart);
    lobeHi = std::max(lobeHi, best + apart);
    float next = 0.0f;
    for (int j = 0; j <= 2 * lagHalf; ++j)
        if (j < lobeLo || j > lobeHi)
            next = std::max(next, std::abs(out[(size_t)j]));

    peak.lagMs = lagMsAt(best) + offset * 1000.0 / fs;
    peak.value = out[(size_t)best];
    peak.clear = std::abs(peak.value) >= 0.3f && next <= 0.5f * std::abs(peak.value);
}
} // namespace pa::meter
