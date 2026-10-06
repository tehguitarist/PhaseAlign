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
    // Averaging speed (R17): slow is P4's setting (settles in about 0.75 s); fast trades steadiness for a display
    // that follows the knobs (settles in about a third of that).
    enum class Speed
    {
        slow,
        fast
    };
    static constexpr double tauFloorSeconds = 0.3, tauCycles = 8.0;
    static constexpr double fastTauFloorSeconds = 0.1, fastTauCycles = 3.0;
    static constexpr double gateDb = -70.0; // a signal below this in a window is not measured
    static constexpr double minHz = 20.0, maxHz = 20000.0;
    static constexpr int curvePoints = 256; // log-spaced, minHz to maxHz (or just below Nyquist)
    static constexpr double curveSmoothingOctaves = 1.0 / 6.0;
    static constexpr double phaseSmoothingOctaves = 1.0 / 24.0; // narrower: a delay turns the phase fast with frequency
    static constexpr float minPhaseCoherence = 0.2f;           // the least coherence worth drawing; narrow windows need more (see .cpp)
    static constexpr double lagRangeMs = 5.0;      // the time view shows -5 to +5 ms
    static constexpr double wideLagRangeMs = 40.0; // and a coarse search beyond it, well inside the frame

    // FFT size: 8192 at 44.1/48 kHz, scaled with the rate (5.9 Hz bins at 48 kHz).
    static int fftSizeFor(double sampleRate);

    void prepare(double sampleRate);
    void setSpeed(Speed);
    Speed getSpeed() const { return speed; }
    double getSampleRate() const { return fs; }
    void reset();

    // Feeds n samples of each stream; returns how many frames were analysed (each one updates the results).
    int process(const float* in, const float* out, const float* sc, int n);

    // Frequency view: r per curve point, NaN where gated (either signal below the gate in that window).
    const std::vector<float>& curveFrequencies() const { return curveHz; }
    const std::vector<float>& curveProcessed() const { return curveX; }
    const std::vector<float>& curveUnprocessed() const { return curveZ; }

    // Phase view: the angle of the cross-spectrum with the sidechain per curve point, in degrees (-180 to 180;
    // positive = this track leads), with its coherence (|cross| / sqrt(power product), 0 to 1). NaN where gated or
    // where the coherence is below minPhaseCoherence.
    const std::vector<float>& phaseProcessed() const { return phaseX; }
    const std::vector<float>& phaseUnprocessed() const { return phaseZ; }
    const std::vector<float>& phaseProcessedCoherence() const { return cohX; }
    const std::vector<float>& phaseUnprocessedCoherence() const { return cohZ; }

    // Preview (R17, used while the screen is held): what the processed results would be if the delay knob were set
    // to `ms` with the phase stage off, worked out from the input's averaged cross-spectrum by turning every bin by
    // its own delay phase. Replaces the processed curve, phase and overall values until cleared; touches nothing
    // else. Exact for the pure delay the plugin applies.
    void setPreviewDelayMs(double ms);
    void clearPreview();
    bool isPreviewing() const { return previewActive; }
    double previewDelayMs() const { return previewMs; }
    // The time view's value for the input at a lag (nearest sample), for the scrub readout.
    float lagUnprocessedAt(double ms) const;

    // Overall broadband r, NaN while gated.
    float overallProcessed() const { return overallX; }
    float overallUnprocessed() const { return overallZ; }

    // Time view, computed on demand (two inverse FFTs). Values are 1 for a pure delay at that lag, -1 for an
    // inverted one; positive lag = the sidechain is later.
    struct Peak
    {
        double lagMs = 0.0;
        float value = 0.0f;
        bool clear = false;  // strong and well above everything else in the range
        bool coarse = false; // from the wide search: to the nearest sample, beyond the time view
    };
    void computeLag();
    double lagMsAt(int index) const { return (index - lagHalf) * 1000.0 / fs; }
    const std::vector<float>& lagProcessed() const { return lagX; }
    const std::vector<float>& lagUnprocessed() const { return lagZ; }
    Peak lagPeakProcessed() const { return peakX; }
    Peak lagPeakUnprocessed() const { return peakZ; }

    // The input's peak for the readout: the refined one in the time view's range, or, when the strongest clear peak
    // within +-wideLagRangeMs lies beyond that range, that one (coarse).
    Peak inputPeak() const;

    // Whether the input's clear peak is beyond what a delay of +-maxMs can reach (by more than half of its 0.1-sample
    // step): the screen then says TRANSIENTS OUT OF DELAY RANGE (plan 2.1a).
    bool inputOutOfReach(double maxMs) const;

    // Seconds of sidechain (by its own samples) spent below the gate since it was last above it.
    double sidechainSilentSeconds() const { return silentSeconds; }
    int getFftSize() const { return fftSize; }
    int getHop() const { return hop; }

  private:
    void analyseFrame();
    void updateResults();
    void setAlpha();
    void lagFunction(const std::vector<double>& re, const std::vector<double>& im, std::vector<float>& out, Peak& peak,
                     Peak* wide = nullptr);
    double windowLevelDb(double powerSum) const; // mean-square level, in dBFS, of a sum of |X|² over bins

    double fs = 0.0;
    int fftSize = 0, hop = 0, numBins = 0, lagHalf = 0, wideHalf = 0, binLo = 0, binHi = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, work[3], fftData;
    std::vector<float> history[3];
    int writePos = 0, sinceFrame = 0, filled = 0;

    std::vector<double> alpha;
    std::vector<double> xyRe, xyIm, zyRe, zyIm, xx, zz, yy;
    double powerToMeanSquare = 0.0, lagNorm = 1.0;

    Speed speed = Speed::slow;
    bool previewActive = false;
    double previewMs = 0.0;
    std::vector<double> previewRe, previewIm;

    std::vector<float> curveHz, curveX, curveZ, phaseX, phaseZ, cohX, cohZ;
    std::vector<int> curveLo, curveHi;     // bin range per curve point, inclusive
    std::vector<int> phaseLo, phaseHi;     // the same for the phase view's narrower windows
    float overallX = 0.0f, overallZ = 0.0f;

    std::vector<float> lagX, lagZ;
    Peak peakX, peakZ, wideZ;
    double silentSeconds = 0.0;
};
} // namespace pa::meter
