#include "PluginEditor.h"
#include "ui/Design.h"

#include <string_view>

namespace
{
namespace id = pa::params::id;
namespace layout = pa::layout;
using pa::params::DelayUnit;

#if JUCE_MAC
const juce::String resetModifier = "Cmd";
#else
const juce::String resetModifier = "Ctrl";
#endif

const juce::String knobHint = "Double-click to type a value, " + resetModifier + "/Alt-click to reset.";

// The latching buttons and their LEDs (if any). Adding one is CSV rows (tools/build_assets.sh) and a row here.
struct ToggleSpec
{
    const char* parameterId;
    layout::Slot button;
    const layout::Slot* led;
    const char* tooltip;
};

const ToggleSpec toggleSpecs[] = {
    {id::delayOn, layout::delayButton, &layout::delayLed, "Delay on/off. The LED is lit while the delay is applied."},
    {id::polarity, layout::phaseInvertButton, &layout::phaseInvertLed,
     "Polarity invert. The LED is lit while the polarity is inverted."},
    {id::phaseOn, layout::phaseButton, &layout::phaseLed,
     "Phase rotation on/off. The LED is lit while the phase stage is active."},
    {id::phaseRange, layout::rangeButton, nullptr,
     "Phase range: the knob's full travel is 90\xc2\xb0 (out) or 180\xc2\xb0 (in). The knob keeps its position, so "
     "switching doubles or halves the angle."},
};

// Delay unit switch: up, centre, down.
constexpr DelayUnit unitPositions[] = {DelayUnit::ms, DelayUnit::samples, DelayUnit::cm};

int unitPosition(DelayUnit u)
{
    for (int i = 0; i < 3; ++i)
        if (unitPositions[i] == u)
            return i;
    return 0;
}
} // namespace

//==============================================================================
PhaseAlignEditor::PanelLookAndFeel::PanelLookAndFeel()
{
    setColour(juce::TooltipWindow::backgroundColourId, juce::Colour(0xf2161615));
    setColour(juce::TooltipWindow::textColourId, juce::Colour(0xffe6e6e2));
    setColour(juce::TooltipWindow::outlineColourId, juce::Colour(0xff4a4a47));
}

void PhaseAlignEditor::PanelLookAndFeel::drawCornerResizer(juce::Graphics& g, int w, int h, bool isMouseOver,
                                                           bool isMouseDragging)
{
    // Three short grip lines, faint until hovered.
    const auto alpha = isMouseOver || isMouseDragging ? 0.7f : 0.3f;
    const auto size = (float)juce::jmin(w, h);
    g.setColour(juce::Colours::white.withAlpha(alpha));
    for (int i = 1; i <= 3; ++i)
    {
        const auto o = size * (float)i / 4.0f;
        g.drawLine((float)w - o, (float)h - 1.0f, (float)w - 1.0f, (float)h - o, 1.0f);
    }
}

//==============================================================================
PhaseAlignEditor::PhaseAlignEditor(PhaseAlignProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p), uiState(p.getUiState()),
      delayKnob(images, layout::delayKnob.bounds(), parameter(id::delayMs),
                [this](double ms, int direction)
                {
                    const auto fine = juce::ModifierKeys::currentModifiers.isShiftDown(); // 0.1 sample
                    return pa::params::stepDelayMs(ms, direction, sampleRate(), fine);
                }),
      phaseKnob(images, layout::phaseKnob.bounds(), parameter(id::phase),
                [this](double position, int direction)
                {
                    const auto range = phaseRange();
                    return pa::params::stepPhaseDeg(position * range, direction, range) / range;
                }),
      unitSwitch(images, layout::delaySwitch.bounds(), pa::ui::ToggleSwitch3::LabelSide::right,
                 {{{"MILLISECONDS", "Show the delay in milliseconds."},
                   {"SAMPLES", "Show the delay in samples at the current sample rate."},
                   {"CENTIMETERS", "Show the delay as a distance in centimetres (sound at 343 m/s)."}}}),
      modeSwitch(images, layout::phaseSwitch.bounds(), pa::ui::ToggleSwitch3::LabelSide::left,
                 {{{"HIGH", "High: frequency-dependent rotation centred higher up. No latency."},
                   {"LOW", "Low: frequency-dependent rotation centred an octave lower. No latency."},
                   {"CONSTANT", "Constant: the same rotation at every frequency. Adds about 43 ms of latency, "
                                "which the host compensates."}}}),
      meterButton(
          images, layout::meterButton.bounds(),
          {layout::Image::meterInOff, layout::Image::meterInOn, layout::Image::meterOutOff, layout::Image::meterOutOn}),
      // ANALYSE is not functional in v0.7: always unlit, but it still presses (IMPLEMENTATION_PLAN 4.3).
      analyseButton(images, layout::analyseButton.bounds(),
                    {layout::Image::analyseInOff, layout::Image::analyseInOff, layout::Image::analyseOutOff,
                     layout::Image::analyseOutOff}),
      delayReadout(images.source(), layout::delayDisplay.bounds(), pa::design::delayReadoutInterior),
      phaseReadout(images.source(), layout::phaseDisplay.bounds(), pa::design::phaseReadoutInterior),
      meterScreen(images.source(), p.getMeterCapture()), rangeLabel(layout::upperRangeLabel.bounds()),
      delayReadoutAttachment(parameter(id::delayMs), [this](float) { updateDelayReadout(); }),
      phaseReadoutAttachment(parameter(id::phase), [this](float) { updatePhaseReadout(); }),
      polarityReadoutAttachment(parameter(id::polarity), [this](float) { updatePhaseReadout(); }),
      rangeAttachment(parameter(id::phaseRange),
                      [this](float)
                      {
                          rangeLabel.setDegrees(juce::roundToInt(phaseRange()));
                          updatePhaseReadout();
                      }),
      modeAttachment(parameter(id::phaseMode), [this](float v) { modeSwitch.setIndex(juce::roundToInt(v)); }),
      delayOnAttachment(parameter(id::delayOn), [this](float) { updateDimming(); }),
      phaseOnAttachment(parameter(id::phaseOn), [this](float) { updateDimming(); })
{
    setLookAndFeel(&lookAndFeel);
    setOpaque(true);

    // Knobs and readouts.
    delayKnob.setTooltip(juce::String::fromUTF8("Delay: \xe2\x88\x92"
                                                "4 to +4 ms in steps of 0.1 sample; negative moves this track "
                                                "earlier. Shift-drag for fine control; mouse wheel or arrow keys "
                                                "nudge by one sample, 0.1 sample with Shift. ") +
                         knobHint);
    phaseKnob.setTooltip(juce::String::fromUTF8("Phase rotation: 0 to 90\xc2\xb0 or 0 to 180\xc2\xb0, set by RANGE. "
                                                "Shift-drag for fine control; mouse wheel or arrow keys nudge by "
                                                "0.5\xc2\xb0. ") +
                         knobHint);
    delayKnob.onEditRequest = [this] { delayReadout.showEditor(); };
    phaseKnob.onEditRequest = [this] { phaseReadout.showEditor(); };

    delayReadout.setTooltip("Effective delay, in the unit chosen with the switch. Double-click to type a value; "
                            "add ms, samp or cm to type it in another unit.");
    phaseReadout.setTooltip("Phase rotation in degrees, plus 180 while the polarity is inverted. Double-click to "
                            "type a value.");
    phaseReadout.setAnchor(pa::design::phaseReadoutInterior.getCentreX());
    delayReadout.onTextEntered = [this](const juce::String& text)
    {
        if (const auto ms = pa::params::parseDelay(text, delayUnit(), sampleRate()))
            delayReadoutAttachment.setValueAsCompleteGesture((float)*ms);
    };
    phaseReadout.onTextEntered = [this](const juce::String& text)
    {
        // The typed value is what the display shows, so take the inversion's 180 back off first.
        const auto range = phaseRange();
        if (const auto shown = pa::params::parsePhase(text, pa::params::maxShownPhaseDeg))
            phaseReadoutAttachment.setValueAsCompleteGesture(
                (float)(juce::jlimit(0.0, range, *shown - polarityOffset()) / range));
    };

    // Switches.
    unitSwitch.setTooltip("Delay readout unit.");
    unitSwitch.onSelect = [this](int position)
    { uiState.setProperty(PhaseAlignProcessor::UiProps::delayUnit, toString(unitPositions[position]), nullptr); };
    modeSwitch.setTooltip("Phase mode.");
    modeSwitch.onSelect = [this](int position) { modeAttachment.setValueAsCompleteGesture((float)position); };

    // Latching buttons with their LEDs.
    for (const auto& spec : toggleSpecs)
    {
        Toggle t;
        t.button = std::make_unique<pa::ui::PanelButton>(images, spec.button.bounds(), parameter(spec.parameterId));
        t.button->setTooltip(juce::String::fromUTF8(spec.tooltip));
        if (spec.led != nullptr)
        {
            t.led = std::make_unique<pa::ui::ImageIndicator>(images, spec.led->bounds(), layout::Image::ledOn,
                                                             layout::Image::ledOff);
            t.button->onStateChange = [led = t.led.get()](bool on) { led->setLit(on); };
            t.led->setLit(t.button->isOn());
        }
        if (std::string_view(spec.parameterId) == id::phaseRange)
            rangeButton = t.button.get();
        toggles.push_back(std::move(t));
    }

    // Meter.
    meterButton.setTooltip(
        juce::String::fromUTF8("Correlation meter on/off: how well this track lines up with the sidechain in each band "
                               "(+1 in phase, \xe2\x88\x92"
                               "1 out of phase). Needs a sidechain input."));
    meterButton.onClick = [this]
    { uiState.setProperty(PhaseAlignProcessor::UiProps::meterOn, ! meterButton.isLit(), nullptr); };
    analyseButton.setTooltip("Auto-suggest: coming in a future version");
    meterScreen.onViewSelected = [this](pa::ui::MeterScreen::View v)
    {
        uiState.setProperty(PhaseAlignProcessor::UiProps::meterView,
                            v == pa::ui::MeterScreen::View::time ? "time" : "frequency", nullptr);
    };

    designComponents = {&delayKnob,     &phaseKnob,    &unitSwitch,   &modeSwitch,  &meterButton,
                        &analyseButton, &delayReadout, &phaseReadout, &meterScreen, &rangeLabel};
    for (auto& t : toggles)
    {
        if (t.led != nullptr)
            designComponents.push_back(t.led.get());
        designComponents.push_back(t.button.get());
    }
    for (auto* c : designComponents)
        addAndMakeVisible(c);

    delayReadoutAttachment.sendInitialUpdate();
    phaseReadoutAttachment.sendInitialUpdate();
    rangeAttachment.sendInitialUpdate();
    modeAttachment.sendInitialUpdate();
    updateDimming(); // also draws the phase readout
    unitSwitch.setIndex(unitPosition(delayUnit()));
    updateMeter();

    uiState.addListener(this);
    audioProcessor.addChangeListener(this);

    // Size: 75% to 200% of the 977x612 reference (opening at 80%), aspect locked, restored from the ui state. The saved
    // scale is read first: setting the limits clamps (and so resizes and saves) whatever size the editor has then.
    using pa::design::defaultHeight, pa::design::defaultWidth;
    const auto scale = juce::jlimit(
        PhaseAlignProcessor::minUiScale, PhaseAlignProcessor::maxUiScale,
        (double)uiState.getProperty(PhaseAlignProcessor::UiProps::uiScale, PhaseAlignProcessor::defaultUiScale));
    setSize(juce::roundToInt(defaultWidth * scale), juce::roundToInt(defaultHeight * scale));
    setResizable(true, true);
    setResizeLimits(juce::roundToInt(defaultWidth * PhaseAlignProcessor::minUiScale),
                    juce::roundToInt(defaultHeight * PhaseAlignProcessor::minUiScale),
                    juce::roundToInt(defaultWidth * PhaseAlignProcessor::maxUiScale),
                    juce::roundToInt(defaultHeight * PhaseAlignProcessor::maxUiScale));
    getConstrainer()->setFixedAspectRatio(layout::designWidth / layout::designHeight);
}

PhaseAlignEditor::~PhaseAlignEditor()
{
    audioProcessor.removeChangeListener(this);
    uiState.removeListener(this);
    setLookAndFeel(nullptr);
}

juce::RangedAudioParameter& PhaseAlignEditor::parameter(const char* parameterId) const
{
    auto* param = audioProcessor.getValueTreeState().getParameter(parameterId);
    jassert(param != nullptr);
    return *param;
}

//==============================================================================
void PhaseAlignEditor::paint(juce::Graphics& g)
{
    ++paintCount;
    g.fillAll(juce::Colours::black); // behind the artwork's transparent rounded corners
    pa::ui::drawFitted(g, images, layout::Image::pluginBase, getLocalBounds().toFloat());
}

void PhaseAlignEditor::resized()
{
    const auto scale = (float)getWidth() / layout::designWidth;
    images.clear();
    for (auto* c : designComponents)
        c->layoutForScale(scale);

    uiState.setProperty(PhaseAlignProcessor::UiProps::uiScale, (double)getWidth() / pa::design::defaultWidth, nullptr);
}

//==============================================================================
void PhaseAlignEditor::valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier& property)
{
    if (property == PhaseAlignProcessor::UiProps::delayUnit)
    {
        unitSwitch.setIndex(unitPosition(delayUnit()));
        updateDelayReadout();
    }
    else if (property == PhaseAlignProcessor::UiProps::meterOn || property == PhaseAlignProcessor::UiProps::meterView)
    {
        updateMeter();
    }
}

void PhaseAlignEditor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    updateDelayReadout();
} // sample rate

DelayUnit PhaseAlignEditor::delayUnit() const
{
    return pa::params::delayUnitFromString(uiState.getProperty(PhaseAlignProcessor::UiProps::delayUnit).toString());
}

void PhaseAlignEditor::updateDelayReadout()
{
    const auto unit = delayUnit();
    const auto fs = sampleRate();
    const auto ms = (double)parameter(id::delayMs).convertFrom0to1(parameter(id::delayMs).getValue());

    // The ghost has a sign position (only the minus segment can light there) ahead of the digits.
    juce::String ghost = "-8.888";
    if (unit == DelayUnit::samples)
        ghost = "-" + juce::String::repeatedString("8", juce::String(pa::params::maxDelaySamples(fs)).length()) + ".8";
    else if (unit == DelayUnit::cm)
        ghost = "-888.8";

    delayReadout.setValue(pa::params::formatDelay(ms, unit, fs), ghost, pa::params::unitSuffix(unit));
}

void PhaseAlignEditor::updatePhaseReadout()
{
    // The total rotation actually applied. Phase on: the knob's angle plus 180 while the polarity is inverted (so up to
    // 360). Phase off: the polarity alone, so 180 at full brightness while inverted; otherwise the knob's angle,
    // dimmed.
    const auto phaseOn = parameter(id::phaseOn).getValue() >= 0.5f;
    const auto inverted = polarityOffset() > 0.0;
    const auto knobDeg = (double)parameter(id::phase).getValue() * phaseRange(); // phase is 0 to 1
    const auto deg = phaseOn ? knobDeg + polarityOffset() : (inverted ? 180.0 : knobDeg);
    phaseReadout.setDimmed(! phaseOn && ! inverted);
    phaseReadout.setValue(pa::params::formatPhase(deg), "888.8", juce::String::fromUTF8("\xc2\xb0"));
}

void PhaseAlignEditor::updateMeter()
{
    const auto on = (bool)uiState.getProperty(PhaseAlignProcessor::UiProps::meterOn);
    meterButton.setLit(on);
    meterScreen.setView(uiState.getProperty(PhaseAlignProcessor::UiProps::meterView).toString() == "time"
                            ? pa::ui::MeterScreen::View::time
                            : pa::ui::MeterScreen::View::frequency);
    meterScreen.setMeterOn(on);
}

void PhaseAlignEditor::updateDimming()
{
    const auto delayOn = parameter(id::delayOn).getValue() >= 0.5f;
    const auto phaseOn = parameter(id::phaseOn).getValue() >= 0.5f;

    // The DELAY, polarity and PHASE buttons and their LEDs, METER and ANALYSE are never dimmed.
    for (auto* c : std::initializer_list<pa::ui::DesignComponent*>{&delayKnob, &unitSwitch, &delayReadout})
        c->setDimmed(! delayOn);
    for (auto* c : std::initializer_list<pa::ui::DesignComponent*>{&phaseKnob, &modeSwitch, &rangeLabel, rangeButton})
        if (c != nullptr)
            c->setDimmed(! phaseOn);
    updatePhaseReadout(); // its dimming also depends on the polarity
}
