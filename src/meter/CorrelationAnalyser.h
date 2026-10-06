#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>
#include <vector>

// GUI side of the correlation meter (IMPLEMENTATION_PLAN 3, R6; method and numbers from P4,
// prototype/out/p4/report.md). Hann-windowed FFT frames of the three streams (processed output x, unprocessed
// input z, sidechain y), 75% overlap. Per bin, the cross-spectra X·Y*, Z·Y* and the powers |X|², |Z|², |Y|² are
// smoothed by an exponential average with time constant max(0.3 s, 8 cycles of the bin's frequency). Everything
// the screen shows comes from these sums:
//
//   - the frequency view: r(f) = Σ Re(X·Y*) / sqrt(Σ|X|² Σ|Y|²) over a 1/6-octave window around each point;
//   - the overall pair: the same over every bin from 20 Hz to 20 kHz (unweighted broadband correlation);
//   - the time view: the PHAT lag function (each bin's cross-spectrum set to unit magnitude, inverse FFT), a unit
//     spike at the lag of a pure delay whatever the spectrum, and at 0 ms for a pure phase rotation.
//
// Not real-time code: prepare() allocates; process() doesn't, but runs FFTs. GUI thread only.
namespace pa::meter
{
class CorrelationAnalyser
{
  public:
    static constexpr double tauFloorSeconds = 0.3, tauCycles = 8.0;
    static constexpr double gateDb = -70.0; // a signal below this in a window is not measured
    static constexpr double minHz = 20.0, maxHz = 20000.0;
    static constexpr int curvePoints = 256; // log-spaced, minHz to maxHz (or just below Nyquist)
    static constexpr double curveSmoothingOctaves = 1.0 / 6.0;
    static constexpr double lagRangeMs = 5.0; // the time view shows -5 to +5 ms

    // FFT size: 8192 at 44.1/48 kHz, scaled with the rate (5.9 Hz bins at 48 kHz).
    static int fftSizeFor(double sampleRate);

    void prepare(double sampleRate);
    double getSampleRate() const { return fs; }
    void reset();

    // Feeds n samples of each stream; returns how many frames were analysed (each one updates the results).
    int process(const float* in, const float* out, const float* sc, int n);

    // Frequency view: r per curve point, NaN where gated (either signal below the gate in that window).
    const std::vector<float>& curveFrequencies() const { return curveHz; }
    const std::vector<float>& curveProcessed() const { return curveX; }
    const std::vector<float>& curveUnprocessed() const { return curveZ; }

    // Overall broadband r, NaN while gated.
    float overallProcessed() const { return overallX; }
    float overallUnprocessed() const { return overallZ; }

    // Time view, computed on demand (two inverse FFTs). Values are 1 for a pure delay at that lag, -1 for an
    // inverted one; positive lag = the sidechain is later.
    struct Peak
    {
        double lagMs = 0.0;
        float value = 0.0f;
        bool clear = false; // strong and well above everything else in the range
    };
    void computeLag();
    double lagMsAt(int index) const { return (index - lagHalf) * 1000.0 / fs; }
    const std::vector<float>& lagProcessed() const { return lagX; }
    const std::vector<float>& lagUnprocessed() const { return lagZ; }
    Peak lagPeakProcessed() const { return peakX; }
    Peak lagPeakUnprocessed() const { return peakZ; }

    // Seconds of sidechain (by its own samples) spent below the gate since it was last above it.
    double sidechainSilentSeconds() const { return silentSeconds; }
    int getFftSize() const { return fftSize; }
    int getHop() const { return hop; }

  private:
    void analyseFrame();
    void updateResults();
    void lagFunction(const std::vector<double>& re, const std::vector<double>& im, std::vector<float>& out, Peak& peak);
    double windowLevelDb(double powerSum) const; // mean-square level, in dBFS, of a sum of |X|² over bins

    double fs = 0.0;
    int fftSize = 0, hop = 0, numBins = 0, lagHalf = 0, binLo = 0, binHi = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, work[3], fftData;
    std::vector<float> history[3];
    int writePos = 0, sinceFrame = 0, filled = 0;

    std::vector<double> alpha;
    std::vector<double> xyRe, xyIm, zyRe, zyIm, xx, zz, yy;
    double powerToMeanSquare = 0.0, lagNorm = 1.0;

    std::vector<float> curveHz, curveX, curveZ;
    std::vector<int> curveLo, curveHi; // bin range per curve point, inclusive
    float overallX = 0.0f, overallZ = 0.0f;

    std::vector<float> lagX, lagZ;
    Peak peakX, peakZ;
    double silentSeconds = 0.0;
};
} // namespace pa::meter
