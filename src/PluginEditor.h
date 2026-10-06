#pragma once

#include "PluginProcessor.h"
#include "params/Parameters.h"
#include "ui/ImageKnob.h"
#include "ui/MeterScreen.h"
#include "ui/PanelButton.h"
#include "ui/Readout.h"
#include "ui/ScaleLabel.h"
#include "ui/ToggleSwitch3.h"

// The panel (IMPLEMENTATION_PLAN 4): baked artwork plus image controls laid out from ui/ui-info.csv.
// Resizable 60% to 200% of 977x612 with a locked aspect ratio; the size is kept in the ui state. Nothing
// repaints while idle: every control repaints only when its value changes, and the meter screen is
// opaque and repaints only itself.
class PhaseAlignEditor : public juce::AudioProcessorEditor,
                         private juce::ValueTree::Listener,
                         private juce::ChangeListener
{
  public:
    explicit PhaseAlignEditor(PhaseAlignProcessor&);
    ~PhaseAlignEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // Test hooks: how often the panel itself has painted, and the meter screen.
    int getPaintCount() const { return paintCount; }
    pa::ui::MeterScreen& getMeterScreen() { return meterScreen; }

  private:
    class PanelLookAndFeel : public juce::LookAndFeel_V4
    {
      public:
        PanelLookAndFeel();
        void drawCornerResizer(juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
    };

    struct Toggle
    {
        std::unique_ptr<pa::ui::PanelButton> button;
        std::unique_ptr<pa::ui::ImageIndicator> led;
    };

    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    pa::params::DelayUnit delayUnit() const;
    double sampleRate() const { return audioProcessor.getDelaySampleRate(); }
    // Read from the parameter itself: inside a parameter callback the processor's raw value may not be updated yet.
    bool range180() const { return parameter(pa::params::id::phaseRange).getValue() >= 0.5f; }
    pa::params::PhaseMode phaseModeNow() const
    {
        const auto& p = parameter(pa::params::id::phaseMode);
        return (pa::params::PhaseMode)juce::roundToInt(p.convertFrom0to1(p.getValue()));
    }
    // What the knob's number means in the current mode and range (Parameters.h shownRangeDegrees): 90, or 180 where it
    // reads exactly. High with RANGE in shows the first section's angle, 0 to 90, marked with an asterisk.
    double shownRange() const { return pa::params::shownRangeDegrees(range180(), phaseModeNow()); }
    bool shownRangeIsApproximate() const { return pa::params::shownRangeIsApproximate(range180(), phaseModeNow()); }
    // The phase readout shows the total rotation, so it starts at 180 while the polarity is inverted.
    double polarityOffset() const { return parameter(pa::params::id::polarity).getValue() >= 0.5f ? 180.0 : 0.0; }
    juce::RangedAudioParameter& parameter(const char* id) const;
    void updateDelayReadout();
    void updateHeldPreview();
    void updatePhaseReadout();
    // The scale label, its asterisk, and the tooltips that describe the current mode and range.
    void updateRangeUi();
    void updateMeter();
    // Dims or restores the delay and phase sections with their on/off parameters (IMPLEMENTATION_PLAN 4.5).
    void updateDimming();

    PhaseAlignProcessor& audioProcessor;
    juce::ValueTree uiState;
    PanelLookAndFeel lookAndFeel;
    pa::ui::ScaledImages images;

    pa::ui::ImageKnob delayKnob, phaseKnob;
    pa::ui::ToggleSwitch3 unitSwitch, modeSwitch;
    std::vector<Toggle> toggles;
    pa::ui::SquareButton meterButton, analyseButton;
    pa::ui::Readout delayReadout, phaseReadout;
    pa::ui::MeterScreen meterScreen;
    pa::ui::ScaleLabel rangeLabel;
    pa::ui::PanelButton* rangeButton = nullptr; // one of `toggles`; dims with the phase section

    juce::ParameterAttachment delayReadoutAttachment, phaseReadoutAttachment, polarityReadoutAttachment,
        rangeAttachment, modeAttachment, delayOnAttachment, phaseOnAttachment;
    juce::TooltipWindow tooltipWindow{this, 700};

    std::vector<pa::ui::DesignComponent*> designComponents;
    int paintCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhaseAlignEditor)
};
