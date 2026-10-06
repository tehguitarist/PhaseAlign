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
//     spike at the lag of a pure delay whatever the spectrum, and at 0 ms for a pure phase rotation. Each bin is
//     weighted by its squared coherence with the sidechain, so bins where the two signals have nothing in common
//     (the whole top of the spectrum, for a kick drum) don't bury the peak in noise; with full coherence everywhere
//     it is plain PHAT.
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
    // Fast also halves the analysis frame (4096 points at 48 kHz: 85 ms, a new result every 21 ms; 11.7 Hz bins), so
    // it responds in about 0.1 s rather than 0.25 s. Switching speed restarts the averages.
    static constexpr double fastTauFloorSeconds = 0.04, fastTauCycles = 2.0;
    static constexpr double gateDb = -70.0; // a signal below this in a window is not measured
    static constexpr double minHz = 20.0, maxHz = 20000.0;
    static constexpr int curvePoints = 256; // log-spaced, minHz to maxHz (or just below Nyquist)
    static constexpr double curveSmoothingOctaves = 1.0 / 6.0;
    static constexpr double phaseSmoothingOctaves = 1.0 / 24.0; // narrower: a delay turns the phase fast with frequency
    static constexpr float minPhaseCoherence = 0.2f;           // the least coherence worth drawing; narrow windows need more (see .cpp)
    static constexpr double lagRangeMs = 5.0;      // the time view shows -5 to +5 ms
    static constexpr double wideLagRangeMs = 40.0; // and a coarse search beyond it, well inside the frame

    // FFT size: 8192 at 44.1/48 kHz, scaled with the rate (5.9 Hz bins at 48 kHz).
    static int fftSizeFor(double sampleRate, Speed speed = Speed::slow);

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

    // Bands view (R17): r over six bands, bandEdgesHz[i] to bandEdgesHz[i + 1]; NaN where gated. The processed ones
    // follow the preview like the curve does.
    static constexpr int numBands = 6;
    static constexpr double bandEdgesHz[numBands + 1] = {20.0, 100.0, 250.0, 630.0, 1600.0, 4000.0, 20000.0};
    const std::vector<float>& bandsProcessed() const { return bandX; }
    const std::vector<float>& bandsUnprocessed() const { return bandZ; }

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
        float runnerUp = 0.0f; // the strongest value outside the peak's main lobe (attack readings; for tuning)
    };
    // `settled`: the screen is frozen (or just switched to this view), so the current picture is all there is to go
    // on: readings count as steady straight away.
    void computeLag(bool settled = false);
    double lagMsAt(int index) const { return (index - lagHalf) * 1000.0 / fs; }
    const std::vector<float>& lagProcessed() const { return lagX; }
    const std::vector<float>& lagUnprocessed() const { return lagZ; }
    Peak lagPeakProcessed() const { return peakX; }
    Peak lagPeakUnprocessed() const { return peakZ; }

    // The input's peak for the readout: the refined one in the time view's range, or, when the strongest clear peak
    // within +-wideLagRangeMs lies beyond that range, that one (coarse).
    Peak inputPeak() const;

    // Attack view (R18): the same offsets read from the ATTACKS of the signals, not their waveforms, for material
    // whose waveforms don't match (a kick against a kick sample: different pitch, tail and click, so the waveform
    // lag lands on whatever the sub bass happens to line up on). Each stream is split into three bands (so a snare's
    // top, a guitar's mids and a bass note's body each get their say), and in each band rectified, smoothed (over about a
    // cycle of the band's lower notes, or a note's own ripple reads as repeated attacks), taken to the log, and only its rises kept (log-envelope flux). The cross-correlation of those,
    // normalised per band and averaged over the bands, peaks where the attacks line up. Computed with computeLag().
    struct AttackBand
    {
        double lowHz, highHz, smoothMs; // highHz 0: no upper limit
    };
    static constexpr int numAttackBands = 3;
    static constexpr AttackBand attackBands[numAttackBands] = {{35.0, 150.0, 12.0}, {150.0, 600.0, 5.0}, {600.0, 0.0, 1.5}};
    Peak attackPeakProcessed() const { return attackX; }
    Peak attackPeakUnprocessed() const { return attackInput(); }
    const std::vector<float>& attackProcessed() const { return attackLagX; }
    const std::vector<float>& attackUnprocessed() const { return attackLagZ; }
    Peak attackInput() const { return attackWideZ.clear && std::abs(attackWideZ.lagMs) > lagRangeMs ? attackWideZ : attackZ; }

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
    void lagFunction(const std::vector<double>& re, const std::vector<double>& im, const std::vector<double>& power,
                     std::vector<float>& out, Peak& peak, Peak* wide = nullptr);
    double windowLevelDb(double powerSum) const; // mean-square level, in dBFS, of a sum of |X|² over bins

    // A reading is only called clear once most of the last few computations agree (a hit that has just landed can
    // put a spurious peak up for a frame, and between hits a clear one can lapse): at least steadyNeeded of the last
    // steadyCount are clear and within steadyMs of each other. Its lag is then their median, which doesn't jitter.
    static constexpr int steadyCount = 8, steadyNeeded = 6;
    static constexpr double steadyMs = 0.3;
    static constexpr int incumbentFrames = 60; // about 2 s at the screen's 30 Hz
    struct Steadiness
    {
        double lag[steadyCount] = {};
        bool clear[steadyCount] = {};
        int count = 0, pos = 0;
        double lastLag = 0.0;     // the last lag shown...
        int sinceShown = 1 << 20; // ...and how many computations ago: for a while it is the incumbent, which keeps
                                  // the peak when a rival is not clearly stronger
        bool incumbent() const { return sinceShown < incumbentFrames; }
        bool update(Peak&, bool settled); // true: clear; sets the peak's lag to the median of the clear ones
    };
    Steadiness steadyX, steadyZ, steadyWideZ;
    void attackFunction(bool processed, std::vector<float>& out, Peak& peak, Peak* wide, const Steadiness& steady,
                        const Steadiness& steadyWide);
    float attackFeature(int stream, int band, float x);

    double fs = 0.0;
    int fftSize = 0, hop = 0, numBins = 0, lagHalf = 0, wideHalf = 0, binLo = 0, binHi = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, work[3], fftData;
    std::vector<float> history[3];
    int writePos = 0, sinceFrame = 0, filled = 0;

    std::vector<double> alpha;
    std::vector<double> xyRe, xyIm, zyRe, zyIm, xx, zz, yy;
    double powerToMeanSquare = 0.0, lagNorm = 1.0, lagUnit = 1.0; // lagUnit: one unit-weight bin's value at lag 0
    std::vector<double> lagWeight, lagFloor; // per bin: the weight and the coherence below which it is zero

    Speed speed = Speed::slow;
    bool previewActive = false;
    double previewMs = 0.0;
    std::vector<double> previewRe, previewIm;

    std::vector<float> curveHz, curveX, curveZ, phaseX, phaseZ, cohX, cohZ;
    std::vector<int> curveLo, curveHi;     // bin range per curve point, inclusive
    std::vector<float> bandX, bandZ;
    int bandLo[numBands] = {}, bandHi[numBands] = {};
    std::vector<int> phaseLo, phaseHi;     // the same for the phase view's narrower windows
    float overallX = 0.0f, overallZ = 0.0f;

    // The attack features (see attackFunction): per stream and band, the filters' state, a moving average of |x| and
    // the previous log value; their ring histories; and per band the averaged cross-spectra of the feature signals.
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };
    struct AttackChannel
    {
        std::vector<double> s1, s2, ring; // filter states (per section) and the moving average
        double sum = 0.0, previousLog = 0.0;
        int pos = 0;
    };
    std::vector<Biquad> attackSections[numAttackBands];
    AttackChannel attackChannel[3][numAttackBands];
    std::vector<float> featHistory[3][numAttackBands];
    std::vector<double> fxyRe[numAttackBands], fxyIm[numAttackBands], fzyRe[numAttackBands], fzyIm[numAttackBands];
    double fxx[numAttackBands] = {}, fzz[numAttackBands] = {}, fyy[numAttackBands] = {};
    double attackAlpha = 0.0;
    std::vector<float> attackLagX, attackLagZ, attackMix;
    Peak attackX, attackZ, attackWideZ;

    std::vector<float> lagX, lagZ;
    Peak peakX, peakZ, wideZ;
    double silentSeconds = 0.0;
};
} // namespace pa::meter
