#pragma once

#include "analyse/CaptureFifo.h"
#include "analyse/Search.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

// ANALYSE from the button's press to the options (PLAN 3.5; IMPLEMENTATION_PLAN R26). Lives in the processor, so a
// capture or a result survives the editor being closed and opened again; message thread only, except the search, which
// runs on its own thread.
//
//   idle ──start()──► capturing ──transport stops with enough audio, or analyseNow()──► analysing ──► results
//                        ▲                                                                              │
//                        └──────────────────────────────── restart() ◄─────────────────────────────────┘
//   stop() from anywhere goes back to idle (the screen shows the meter again); what was applied stays.
//
// Capturing keeps only what can be compared: stretches of 10 ms where both this track and the sidechain have signal
// (above -60 dBFS rms) and differ, while the host's transport plays (when it says), with a sidechain connected. Both
// streams are cut at the same places, so they stay lined up. At least minSeconds of it is needed and idealSeconds is
// recommended (user, 2026-10-08): on 5 s of unrelated material the search's chance gain reached +0.05, over its 0.03
// minimum, against about +0.02 at 10 s and +0.005 at 20 s; on the user's pairs drums settled within 3 to 5 s, while
// sustained parts kept changing with the section played. Past maxSeconds the oldest is dropped. When a host that
// reports its transport stops, the session analyses if it has enough, and otherwise says so and keeps what it has for
// the next play.
namespace pa::analyse
{
// The seven panel settings an option sets, in the parameters' own terms (params/Parameters.h).
struct PanelSettings
{
    bool delayOn = false;
    double delayMs = 0.0;
    bool polarity = false;
    bool phaseOn = false;
    int phaseMode = 0; // the choice index: params::PhaseMode (high, low, constant)
    bool range180 = false;
    double phase = 0.0; // the knob position, 0 to 1

    bool sameAs(const PanelSettings&) const;
    // The search's view of these settings at the given rate (the delay as applied, 0 while DELAY is off).
    Candidate candidate(double sampleRate) const;
};

// What choosing a candidate sets, starting from `base` (the settings before ANALYSE): only what the search covered
// changes. Polarity always; with DELAY on, the delay; with PHASE on, the phase stage (PHASE off for "phase off";
// Constant keeps RANGE out when its angle fits, for the finer knob).
PanelSettings settingsFor(const Candidate&, const PanelSettings& base, Scope);

class Session : private juce::Timer
{
  public:
    enum class State
    {
        idle,
        capturing,
        analysing,
        results
    };
    static constexpr double minSeconds = 10.0, idealSeconds = 30.0, maxSeconds = 30.0;
    static constexpr double chunkSeconds = 0.01, signalFloorDb = -60.0;
    // The analysing screen stays up at least this long, its bar filling over it, so a quick search doesn't just flash.
    static constexpr double minAnalysingSeconds = 0.6;

    // What the session reads from and does to the processor.
    struct Host
    {
        std::function<double()> sampleRate;
        std::function<Scope()> scope; // the DELAY and PHASE buttons now
        std::function<PanelSettings()> settings;
        std::function<void(const PanelSettings&)> apply; // as one host gesture
        std::function<bool()> sidechainPresent;
        std::function<bool()> transportKnown, transportPlaying;
    };

    Session(CaptureFifo&, Host);
    ~Session() override;

    void start();      // the button pressed: capture from nothing
    void stop();       // the button pressed again: back to idle (keeps whatever is applied)
    void restart();    // AGAIN: a new capture (the settings from before ANALYSE stay the ORIGINAL)
    void analyseNow(); // ANALYSE NOW, or the transport stopping: if there is enough

    State getState() const { return state; }
    bool isActive() const { return state != State::idle; }
    std::function<void()> onChange; // the state or what the screen shows changed (message thread)

    // Capturing.
    double capturedSeconds() const;
    bool isTooShort() const { return tooShort; } // stopped (or ANALYSE NOW) before minSeconds
    bool isWaitingForPlay() const;               // a host that reports its transport is stopped
    bool hasSidechain() const { return host.sidechainPresent(); }
    float inputLevelDb() const { return inputLevel; } // the last 0.3 s's peak, dBFS
    float sidechainLevelDb() const { return sidechainLevel; }
    Scope currentScope() const { return host.scope(); }

    // Analysing: how far the bar is, 0 to 1 (the search's own progress, but filling over minAnalysingSeconds at least),
    // and how long it has been going.
    float analysisProgress() const;
    double analysingSeconds() const;

    // Results.
    struct Outcome
    {
        Result result;
        Spectra spectra, shiftedSpectra; // the capture as it is, and with the manual shift made (empty if none)
        Scope scope;
        double seconds = 0.0, sampleRate = 48000.0;
        double originalScore = 0.0; // the settings from before ANALYSE, scored like the options
        // The strongest sidechain onset, for ALIGNMENT's before and after: the window around it of the input as it is,
        // the input with the manual shift made, and the sidechain; the onset sits at `onset`.
        std::vector<float> hitInput, hitShiftedInput, hitSidechain;
        int hitOnset = 0;
    };
    const Outcome* getOutcome() const { return state == State::results ? outcome.get() : nullptr; }
    const PanelSettings& getOriginal() const { return original; }
    int numOptions() const { return outcome != nullptr ? (int)outcome->result.options.size() : 0; }
    PanelSettings optionSettings(int index) const; // index -1: ORIGINAL
    void choose(int index);                        // applies it (-1: ORIGINAL)
    // The row whose settings the panel has now, or -2 if none (the knobs were moved since).
    int chosenNow() const;

    // For tests: what the session would do on a tick, without waiting for the timer.
    void tickForTesting() { timerCallback(); }
    void waitForAnalysisForTesting();

  private:
    void timerCallback() override;
    void takeIn(const float* in, const float* sc, int n);
    void keepChunk();
    void finishAnalysis();
    void cancelWorker();
    void setState(State);
    void changed()
    {
        if (onChange)
            onChange();
    }

    CaptureFifo& fifo;
    Host host;
    State state = State::idle;
    PanelSettings original;

    // The capture: a ring of maxSeconds, written a chunk at a time.
    double fs = 48000.0;
    std::vector<float> ringInput, ringSidechain, scratchInput, scratchSidechain;
    long long written = 0; // samples kept so far (the ring holds the last ringInput.size() of them)
    std::vector<float> chunkInput, chunkSidechain;
    int chunkFill = 0;
    bool tooShort = false, sawPlaying = false;
    float inputLevel = -100.0f, sidechainLevel = -100.0f;
    double inputPeakHold = 0.0, sidechainPeakHold = 0.0;
    int levelSamples = 0;

    std::unique_ptr<Outcome> outcome;
    std::unique_ptr<Outcome> pending; // written by the worker before `done`
    std::atomic<bool> done{false}, cancel{false};
    std::atomic<float> searchProgress{0.0f};
    double analysingSinceMs = 0.0;
    std::thread worker;

    JUCE_DECLARE_NON_COPYABLE(Session)
};
} // namespace pa::analyse
