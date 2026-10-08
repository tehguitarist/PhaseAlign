#pragma once

#include <juce_dsp/juce_dsp.h>

#include <complex>
#include <functional>
#include <memory>
#include <vector>

// GUI side of the correlation meter. Hann-windowed FFT frames of the three streams (processed output x, unprocessed
// input z, sidechain y), 75% overlap. Per bin, the cross-spectra X·Y*, Z·Y* and the powers |X|², |Z|², |Y|² are
// smoothed by an exponential average with time constant max(0.75 s, 12 cycles of the bin's frequency) on SLOW and
// max(0.08 s, 3 cycles) on FAST. What the screen shows comes from these sums: r over each of the six BANDS, and the
// overall pair over every bin from 20 Hz to 20 kHz (unweighted broadband correlation), each Σ Re(X·Y*) / sqrt(Σ|X|²
// Σ|Y|²).
//
// Not real-time code: prepare() allocates; process() doesn't, but runs FFTs. GUI thread only.
namespace pa::meter
{
class CorrelationAnalyser
{
  public:
    // Averaging speed: slow settles in about 0.75 s); fast trades steadiness for a display
    // that follows the knobs (settles in about a third of that).
    enum class Speed
    {
        slow,
        fast
    };
    static constexpr double tauFloorSeconds = 0.75, tauCycles = 12.0;
    // Fast also halves the analysis frame (4096 points at 48 kHz: 85 ms, a new result every 21 ms; 11.7 Hz bins), so
    // it responds in about 0.1 s rather than 0.25 s. Switching speed restarts the averages.
    static constexpr double fastTauFloorSeconds = 0.08, fastTauCycles = 3.0;
    static constexpr double gateDb = -70.0; // a signal below this in a window is not measured
    static constexpr double minHz = 20.0, maxHz = 20000.0;

    // FFT size: 8192 at 44.1/48 kHz, scaled with the rate (5.9 Hz bins at 48 kHz).
    static int fftSizeFor(double sampleRate, Speed speed = Speed::slow);

    void prepare(double sampleRate);
    // What the screen's current view needs: only that work runs. The per-bin averages behind the overall bar
    // always run (three FFTs a frame); the six bands only for BANDS. Switching them on fills them at once from the
    // averages.
    struct Needs
    {
        bool bands = true;
    };
    void setNeeds(const Needs&);
    const Needs& getNeeds() const { return needs; }

    void setSpeed(Speed);
    Speed getSpeed() const { return speed; }
    double getSampleRate() const { return fs; }
    void reset();

    // Feeds n samples of each stream; returns how many frames were analysed (each one updates the results).
    int process(const float* in, const float* out, const float* sc, int n);

    // Preview (used while the screen is held): what the processed results would be if the delay knob were set
    // to `ms` with the phase stage off, worked out from the input's averaged cross-spectrum by turning every bin by
    // its own delay phase. Replaces the processed bands and overall values until cleared; touches nothing else. Exact
    // for the pure delay the plugin applies.
    void setPreviewDelayMs(double ms); // keeps the polarity as it is
    // Delay and polarity flip together, and optionally the phase stage's response (a function of frequency in Hz, unit
    // magnitude, a lag as a negative angle: dsp::phaseStageResponse), for the knobs while held.
    using PhaseResponse = std::function<std::complex<double>(double hz)>;
    void setPreview(double ms, bool inverted, PhaseResponse phase = {});
    bool previewHasPhase() const { return (bool)previewPhase; }
    bool isPreviewInverted() const { return previewInverted; }
    void clearPreview();
    bool isPreviewing() const { return previewActive; }
    double previewDelayMs() const { return previewMs; }
    // Bands view: r over six bands, bandEdgesHz[i] to bandEdgesHz[i + 1]; NaN where gated. The processed ones
    // follow the preview.
    static constexpr int numBands = 6;
    static constexpr double bandEdgesHz[numBands + 1] = {20.0, 100.0, 250.0, 630.0, 1600.0, 4000.0, 20000.0};
    const std::vector<float>& bandsProcessed() const { return bandX; }
    const std::vector<float>& bandsUnprocessed() const { return bandZ; }

    // Overall broadband r, NaN while gated.
    float overallProcessed() const { return overallX; }
    float overallUnprocessed() const { return overallZ; }

    // Seconds of sidechain (by its own samples) spent below the gate since it was last above it.
    double sidechainSilentSeconds() const { return silentSeconds; }
    int getFftSize() const { return fftSize; }
    int getHop() const { return hop; }

  private:
    void analyseFrame();
    void updateResults();
    void setAlpha();
    double windowLevelDb(double powerSum) const; // mean-square level, in dBFS, of a sum of |X|² over bins

    double fs = 0.0;
    int fftSize = 0, hop = 0, numBins = 0, binLo = 0, binHi = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, work[3], fftData;
    std::vector<float> history[3];
    int writePos = 0, sinceFrame = 0, filled = 0;

    std::vector<double> alpha;
    std::vector<double> xyRe, zyRe, zyIm, xx, zz, yy; // zyIm: the input's, for the preview's turn
    double powerToMeanSquare = 0.0;

    Needs needs;
    Speed speed = Speed::slow;
    bool previewActive = false, previewInverted = false;
    PhaseResponse previewPhase;
    double previewMs = 0.0;
    std::vector<double> previewRe;

    std::vector<float> bandX, bandZ;
    int bandLo[numBands] = {}, bandHi[numBands] = {};
    float overallX = 0.0f, overallZ = 0.0f;
    double silentSeconds = 0.0;
};
} // namespace pa::meter
