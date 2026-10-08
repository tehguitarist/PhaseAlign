#pragma once

#include "dsp/Chain.h"

#include <atomic>
#include <complex>
#include <string>
#include <utility>
#include <vector>

// ANALYSE's search (PLAN 3.5; IMPLEMENTATION_PLAN R26): given a capture of this track's input and the sidechain (mono,
// lined up with each other), the settings of the delay, polarity and phase stage that make the input correlate best
// with the sidechain, as up to two options to choose from. A port of prototype/analyse.py (`suggest_with_shift` and
// what it calls), which is the spec and the golden reference (prototype/analyse_golden.py, tests/dsp/AnalyseTests.cpp).
//
// Method, briefly (the Python's docstring has it all): one cross-spectrum of the whole capture (Hann frames of 8192 at
// 48 kHz, scaled with the rate, 75% overlap); every candidate G = polarity x phase response (dsp::PhaseStageResponse) x
// delay scored in closed form as the mean over the active 1/3-octave bands of Re(sum G Sxy) / sqrt(Sxx Syy). The delay
// is searched with one inverse FFT per phase setting (the band mean is linear in the spectrum, so the Python's one
// transform per band and setting adds up to one), zero-padded 4x, then refined on the knob's 0.1-sample grid. The
// attack lag (plan R18, whole capture) fixes the delay when its peak is strong; options must gain over doing nothing; a
// big offset asks for a manual shift in samples.
//
// Offline: allocates, and takes a fraction of a second to a few seconds. Run it on a background thread; `cancel` (may
// be null) stops it early, and the result is then empty with `cancelled` set.
namespace pa::analyse
{
enum class Mode
{
    none, // the phase stage left out
    lo,
    hi,
    constant
};

struct Candidate
{
    Mode mode = Mode::none;
    bool wide = false;    // RANGE in (Hi/Lo's two-section shapes); Constant is searched over 0 to 180, so in
    double theta = 0.0;   // the panel angle the chain takes (ChainSettings::phaseDegrees): 0 to 90, or 0 to 180 if wide
    double delayMs = 0.0; // positive delays the input
    bool flip = false;
    double score = 0.0;

    // The chain's settings for this candidate (delayTenths at `sampleRate`, delayOn when the delay isn't 0).
    dsp::ChainSettings toSettings(double sampleRate) const;
};

// The attack lag over the whole capture: the delay (ms, positive = delay the input) at which the three bands' attack
// features line up best, whether its peak is clear of rivals, its height (normalised cross-correlation) and the
// strongest rival outside its lobe, as a fraction of it.
struct AttackReading
{
    double lagMs = 0.0;
    bool clear = false;
    double peak = 0.0, runnerUp = 1.0;
};

// What the search may change (the DELAY and PHASE buttons, user 2026-10-08). Polarity is always searched.
struct Scope
{
    bool delayOn = true, phaseOn = true;
};

struct Option
{
    Candidate candidate;
    double gain = 0.0; // score over doing nothing
    double lowEndChange =
        0.0; // the score of the bands below 300 Hz with the option, minus without: < 0 is "less low end"
    bool fromAttacks = false; // the delay came from the attack reading (else the waveform score)
    bool delayOnly = false;
};

enum class Verdict
{
    suggest,   // one clear winner
    close,     // two options within 0.01 of each other
    delayOnly, // the waveforms barely correlate, but the attacks clearly sit apart
    nothing,   // no change worth making
    noSignal   // too little signal in one of the two to score anything
};

struct Result
{
    std::vector<Option> options; // 0 to 2
    double baseline = 0.0;       // the score of doing nothing
    AttackReading attack;
    Verdict verdict = Verdict::nothing;
    bool lessLowEnd = false; // any option lowers the correlation below 300 Hz
    // Warnings on the options (shown, not hidden; user 2026-10-08): the best option doesn't beat what the search finds
    // by chance on this material (the sidechain circularly shifted) by chanceMargin, or its score stays under
    // weakMatch.
    bool chanceLevel = false, weakMatch = false;
    double chanceGain = 0.0; // what the chance test found (set with the options from the waveform score)
    int shiftSamples = 0;    // a manual shift to make first (positive delays the track); the options are for after it
    std::string message;     // the shift advice, or (DELAY off) where the transients sit; empty if neither
    bool cancelled = false;
};

// The search's constants (prototype/analyse.py).
inline constexpr double maxDelayMs = 4.0, wideReachMs = 10.0, attackStrong = 0.35, minGain = 0.03, minDelayMs = 0.3,
                        minMargin = 0.01, lowEndHz = 300.0, angleStep = 2.5;
// The chance test (prototype/analyse.py CHANCE_*, WEAK_MATCH, with the measurements behind them): the sidechain is
// turned round by these fractions of the capture.
inline constexpr double chanceShifts[] = {0.37, 0.61};
inline constexpr double chanceMargin = 0.02, weakMatch = 0.12;

// The cross-spectrum of a capture, summed over frames, and its active 1/3-octave bands.
struct Spectra
{
    double fs = 48000.0;
    int n = 0;                             // frame length
    std::vector<std::complex<double>> sxy; // sum over frames of X conj(Y), X = input, Y = sidechain; n/2 + 1 bins
    std::vector<double> sxx, syy;
    std::vector<std::pair<int, int>> edges; // active bands: [first bin, end bin)
    std::vector<double> centres;
    std::vector<double> norms; // per band: sqrt(sum Sxx * sum Syy)

    static Spectra compute(const float* x, const float* y, int length, double fs);
    double binHz(int k) const { return k * fs / n; }
    bool valid() const { return ! edges.empty(); }
};

// The score (mean band correlation) of a fully specified candidate, and of the bands below `belowHz` only (nan if
// none).
double scoreAt(const Spectra&, const Candidate&, double belowHz = 0.0);
// The correlation over [loHz, hiHz) as one band (the meter's BANDS view, for the before/after preview).
double bandCorrelation(const Spectra&, const Candidate&, double loHz, double hiHz);
// Doing nothing: phase off, no flip, no delay.
inline Candidate nothingChanged()
{
    return {};
}

AttackReading attackLag(const float* x, const float* y, int length, double fs);

// The ranked families of candidates: a lag search over the knob's reach (or `reachMs`), optionally inside [windowLoMs,
// windowHiMs].
struct SearchOptions
{
    int top = 6;
    bool phaseOn = true;
    double reachMs = maxDelayMs;
    bool windowed = false;
    double windowLoMs = 0.0, windowHiMs = 0.0;
};
std::vector<Candidate> search(const Spectra&, const SearchOptions&, const std::atomic<bool>* cancel = nullptr);

// suggest(), then the shift advice: prototype/analyse.py suggest_with_shift. `progress` (may be null) is kept at the
// fraction of the work done, 0 to 0.99, never going back (any thread may read it).
Result suggestWithShift(const float* x, const float* y, int length, double fs, Scope,
                        const std::atomic<bool>* cancel = nullptr, std::atomic<float>* progress = nullptr);

// "LOW in 62.5°, Ø, +1.21 ms" style, for tests and logs (the screen writes its own, in panel numbers).
std::string describe(const Candidate&);
} // namespace pa::analyse
