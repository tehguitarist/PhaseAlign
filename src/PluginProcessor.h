#pragma once

#include "dsp/Chain.h"
#include "meter/MeterCapture.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

// Runs the DSP chain (polarity, phase, delay; src/dsp/Chain.h) on the main bus, which is mono or stereo
// (in == out). The sidechain bus (mono, stereo or disabled) is the reference signal for the correlation meter.
//
// Latency (plan 2.1a, 2.4): 4 ms plus the interpolation's lookahead (under 0.6 ms) while the delay is on, so the delay
// knob can move the track earlier as well as later, plus about 48 ms in Constant mode for its linear-phase Hilbert FIR.
// A delayOn change fades the audio out and back in around the switch (src/dsp/Chain.h), and the reported latency is
// updated on the message thread.
//
// State (IMPLEMENTATION_PLAN 1.2): the parameter tree, plus a "ui" child holding editor settings that
// are not host parameters, plus stateVersion on the root.
class PhaseAlignProcessor : public juce::AudioProcessor,
                            public juce::ChangeBroadcaster,
                            private juce::AudioProcessorParameter::Listener,
                            private juce::AsyncUpdater
{
  public:
    static constexpr int stateVersion = 1;

    // Properties of the "ui" state child.
    struct UiProps
    {
        static inline const juce::Identifier type{"ui"};
        static inline const juce::Identifier delayUnit{"delayUnit"}; // "ms" / "samples" / "cm"
        static inline const juce::Identifier meterOn{"meterOn"};     // bool
        static inline const juce::Identifier uiScale{"uiScale"};     // 0.6 to 2, 1 = 977x612; opens at 0.8
        static inline const juce::Identifier meterView{"meterView"}; // "bands" / "vector" / "scope"
        static inline const juce::Identifier meterSpeed{"meterSpeed"}; // "slow" / "fast"
        static inline const juce::Identifier vectorStereo{"vectorStereo"}; // bool: VECTORSCOPE shows the track's L/R
        static inline const juce::Identifier showInput{"showInput"};         // bool: the legend toggles (all on by default)
        static inline const juce::Identifier showOutput{"showOutput"};       // bool
        static inline const juce::Identifier showSidechain{"showSidechain"}; // bool
        static inline const juce::Identifier alignCapture{"alignCapture"}; // bool: ALIGNMENT holds the last hit
        static inline const juce::Identifier tooltipsOn{"tooltipsOn"};     // bool: the "?" switch (off by default)
    };
    static constexpr double minUiScale = 0.6, maxUiScale = 2.0;
    static constexpr double defaultUiScale = 0.8; // 782x490 (user, 2026-10-06)

    PhaseAlignProcessor();
    ~PhaseAlignProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    // Host bypass: fades every stage out, then passes the input through bit-exact, delayed by the reported latency.
    // Coming back from bypass fades the stages back in.
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    // IIR decay plus 8.6 ms of delay, or Constant's half-length (about 43 ms) (plan 1.3).
    double getTailLengthSeconds() const override { return 0.1; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState& getValueTreeState() { return parameters; }

    // Message thread only. The tree object stays the same for the processor's lifetime, so the editor
    // can hold on to it and listen; loading state copies properties into it.
    juce::ValueTree& getUiState() { return uiState; }

    // The rate the delay is rounded at. Before the host prepares us this is 48 kHz. A change is
    // announced through ChangeBroadcaster (asynchronously, on the message thread).
    double getDelaySampleRate() const { return sampleRate.load(); }

    bool isSidechainConnected() const;

    // 90 or 180: what the phase knob's full travel means (any thread).
    double getPhaseRangeDegrees() const;
    // What the phase knob's number means now (params::shownRangeDegrees): the host's parameter text and the panel
    // agree.
    double getShownPhaseRangeDegrees() const;

    // The correlation meter's feed. The editor's meter screen turns it on while it is showing and the meter is on.
    pa::meter::MeterCapture& getMeterCapture() { return meterCapture; }

    // The chain's targets from the current parameter values (any thread).
    pa::dsp::ChainSettings currentSettings() const;

    // The latency the current parameter values need, in samples (any thread; reads the parameters themselves, so it is
    // right inside a parameter callback too).
    int requiredLatency() const;

  private:
    void parameterValueChanged(int, float) override;
    void parameterGestureChanged(int, bool) override {}
    void handleAsyncUpdate() override { setLatencySamples(requiredLatency()); }

    static juce::ValueTree defaultUiState();
    void restoreUiState(const juce::ValueTree& loaded);
    bool sidechainIsOwnInput(const juce::AudioBuffer<float>& main, const juce::AudioBuffer<float>& sidechain);
    int identicalSamples = 0; // audio thread: how long the sidechain has been a copy of the input
    void runChain(juce::AudioBuffer<float>&, const pa::dsp::ChainSettings&);

    juce::AudioProcessorValueTreeState parameters;
    juce::ValueTree uiState{defaultUiState()};
    std::atomic<double> sampleRate{48000.0};
    std::atomic<float>* phaseRangeValue = nullptr;
    juce::RangedAudioParameter *delayOnParameter = nullptr, *phaseModeParameter = nullptr;
    std::atomic<float>*delayMsValue = nullptr, *delayOnValue = nullptr, *phaseValue = nullptr,
    *phaseModeValue = nullptr, *polarityValue = nullptr, *phaseOnValue = nullptr;

    pa::dsp::Chain chain;
    pa::meter::MeterCapture meterCapture;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhaseAlignProcessor)
};
