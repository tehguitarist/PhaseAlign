// Drives the controls with synthesised mouse, wheel and key events, so every gesture in
// IMPLEMENTATION_PLAN 4.4 is checked against the parameter or ui state it should change.

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/Design.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace pa::params;
using Catch::Approx;
using UiProps = PhaseAlignProcessor::UiProps;

namespace
{
struct Fixture
{
    Fixture()
    {
        proc.prepareToPlay(48000.0, 512);
        editor.reset(dynamic_cast<PhaseAlignEditor*>(proc.createEditor()));
        REQUIRE(editor != nullptr);
    }

    float param(const char* paramId)
    {
        auto* p = proc.getValueTreeState().getParameter(paramId);
        return p->convertFrom0to1(p->getValue());
    }

    template <typename T>
    std::vector<T*> controls() const
    {
        std::vector<T*> found;
        for (auto* c : editor->getChildren())
            if (auto* t = dynamic_cast<T*>(c))
                found.push_back(t);
        std::sort(found.begin(), found.end(), [](auto* a, auto* b) { return a->getX() < b->getX(); });
        return found;
    }

    PhaseAlignProcessor proc;
    std::unique_ptr<PhaseAlignEditor> editor;
};

juce::MouseEvent mouseEvent(juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos,
                            juce::ModifierKeys mods = {}, int clicks = 1, bool dragged = false)
{
    const auto now = juce::Time::getCurrentTime();
    return {juce::Desktop::getInstance().getMainMouseSource(),
            pos,
            mods,
            juce::MouseInputSource::defaultPressure,
            juce::MouseInputSource::defaultOrientation,
            juce::MouseInputSource::defaultRotation,
            juce::MouseInputSource::defaultTiltX,
            juce::MouseInputSource::defaultTiltY,
            &c,
            &c,
            now,
            downPos,
            now,
            clicks,
            dragged};
}

const auto leftButton = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);

void click(juce::Component& c, juce::Point<float> pos, juce::ModifierKeys mods = {})
{
    c.mouseDown(mouseEvent(c, pos, pos, mods.withFlags(juce::ModifierKeys::leftButtonModifier)));
    c.mouseUp(mouseEvent(c, pos, pos, mods));
}

void drag(juce::Component& c, juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods = {},
          int steps = 10)
{
    const auto held = mods.withFlags(juce::ModifierKeys::leftButtonModifier);
    c.mouseDown(mouseEvent(c, from, from, held));
    for (int i = 1; i <= steps; ++i)
        c.mouseDrag(mouseEvent(c, from + (to - from) * ((float)i / (float)steps), from, held, 1, true));
    c.mouseUp(mouseEvent(c, to, from, mods, 1, true));
}

void wheel(juce::Component& c, float deltaY, bool smooth)
{
    juce::MouseWheelDetails w{};
    w.deltaY = deltaY;
    w.isSmooth = smooth;
    const auto centre = c.getLocalBounds().getCentre().toFloat();
    c.mouseWheelMove(mouseEvent(c, centre, centre), w);
}
} // namespace

TEST_CASE("knobs: drag, fine drag, reset, wheel and keys", "[interaction]")
{
    Fixture f;
    const auto knobs = f.controls<pa::ui::ImageKnob>();
    REQUIRE(knobs.size() == 2);
    auto& delayKnob = *knobs[0];
    auto& phaseKnob = *knobs[1];
    const auto centre = phaseKnob.getLocalBounds().getCentre().toFloat();

    SECTION("vertical drag: 300 px is the full range; Shift is ten times finer")
    {
        drag(phaseKnob, centre, centre.translated(0.0f, -150.0f));
        CHECK(f.param(id::phase) == Approx(0.5f).margin(1e-4f));
        drag(phaseKnob, centre, centre.translated(0.0f, -150.0f), juce::ModifierKeys::shiftModifier);
        CHECK(f.param(id::phase) == Approx(0.55f).margin(1e-4f));
        drag(phaseKnob, centre, centre.translated(0.0f, 1000.0f));
        CHECK(f.param(id::phase) == 0.0f);
    }

    SECTION("Cmd- or Alt-click resets to the default")
    {
        drag(delayKnob, centre, centre.translated(0.0f, -100.0f));
        REQUIRE(f.param(id::delayMs) > 1.0f);
        click(delayKnob, centre, juce::ModifierKeys::commandModifier);
        CHECK(f.param(id::delayMs) == 0.0f);
        drag(delayKnob, centre, centre.translated(0.0f, -100.0f));
        click(delayKnob, centre, juce::ModifierKeys::altModifier);
        CHECK(f.param(id::delayMs) == 0.0f);
    }

    SECTION("a click without movement changes nothing")
    {
        click(phaseKnob, centre);
        CHECK(f.param(id::phase) == 0.0f);
    }

    SECTION("wheel and arrow keys step one sample or half a degree")
    {
        wheel(delayKnob, 0.1f, false);
        wheel(delayKnob, 0.1f, false);
        CHECK(delayInSamples(f.param(id::delayMs), 48000.0) == 2);
        wheel(delayKnob, -0.1f, false);
        CHECK(delayInSamples(f.param(id::delayMs), 48000.0) == 1);

        // Half a degree of angle per step, whatever the range.
        wheel(phaseKnob, 0.16f, true); // trackpad: one step per 0.05 of travel
        CHECK(f.param(id::phase) * 90.0f == Approx(1.5f));

        phaseKnob.keyPressed(juce::KeyPress(juce::KeyPress::upKey));
        phaseKnob.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
        CHECK(f.param(id::phase) * 90.0f == Approx(2.5f));
        delayKnob.keyPressed(juce::KeyPress(juce::KeyPress::downKey));
        CHECK(delayInSamples(f.param(id::delayMs), 48000.0) == 0);

        // With Shift, the delay steps by 0.1 sample.
        const auto saved = juce::ModifierKeys::currentModifiers;
        juce::ModifierKeys::currentModifiers = juce::ModifierKeys(juce::ModifierKeys::shiftModifier);
        delayKnob.keyPressed(juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier, 0));
        wheel(delayKnob, -0.1f, false);
        juce::ModifierKeys::currentModifiers = saved;
        CHECK(delayInSamples(f.param(id::delayMs), 48000.0) == Approx(-0.2));
        CHECK_FALSE(delayKnob.keyPressed(juce::KeyPress('x')));
    }

    SECTION("double-click opens the readout editor")
    {
        const auto readouts = f.controls<pa::ui::Readout>();
        REQUIRE(readouts.size() == 2);
        phaseKnob.mouseDoubleClick(mouseEvent(phaseKnob, centre, centre, leftButton, 2));
        CHECK(readouts[1]->isEditing());
        CHECK_FALSE(readouts[0]->isEditing());
    }
}

TEST_CASE("toggle switches: click halves, drag and labels", "[interaction]")
{
    Fixture f;
    const auto switches = f.controls<pa::ui::ToggleSwitch3>();
    REQUIRE(switches.size() == 2);
    auto& unitSwitch = *switches[0];
    auto& modeSwitch = *switches[1];

    // The switch image is at the right end of the delay switch and the left end of the phase switch.
    const auto s = unitSwitch.getScale();
    const auto unitLever = pa::layout::delaySwitch.bounds() * s - unitSwitch.getPosition().toFloat();
    const auto modeLever = pa::layout::phaseSwitch.bounds() * s - modeSwitch.getPosition().toFloat();

    SECTION("clicking the lower or upper half steps down or up")
    {
        click(modeSwitch, modeLever.getCentre().translated(0.0f, 10.0f));
        CHECK(f.param(id::phaseMode) == 1.0f);
        click(modeSwitch, modeLever.getCentre().translated(0.0f, 10.0f));
        CHECK(f.param(id::phaseMode) == 2.0f);
        click(modeSwitch, modeLever.getCentre().translated(0.0f, 10.0f)); // already at the bottom
        CHECK(f.param(id::phaseMode) == 2.0f);
        click(modeSwitch, modeLever.getCentre().translated(0.0f, -10.0f));
        CHECK(f.param(id::phaseMode) == 1.0f);
    }

    SECTION("dragging moves one position per label row")
    {
        const auto row = 69.5f * s;
        drag(unitSwitch, unitLever.getCentre(), unitLever.getCentre().translated(0.0f, 2.0f * row));
        CHECK(f.proc.getUiState()[UiProps::delayUnit].toString() == "cm");
        drag(unitSwitch, unitLever.getCentre(), unitLever.getCentre().translated(0.0f, -row));
        CHECK(f.proc.getUiState()[UiProps::delayUnit].toString() == "samples");
    }

    SECTION("clicking a label selects it")
    {
        // Labels are on the far side from the lever, one row above or below the centre.
        const auto row = 69.5f * s;
        const juce::Point<float> constantLabel{modeLever.getX() - 60.0f * s, modeLever.getCentreY() + row};
        click(modeSwitch, constantLabel);
        CHECK(f.param(id::phaseMode) == 2.0f);

        const juce::Point<float> msLabel{unitLever.getRight() + 60.0f * s, unitLever.getCentreY() - row};
        f.proc.getUiState().setProperty(UiProps::delayUnit, "cm", nullptr);
        click(unitSwitch, msLabel);
        CHECK(f.proc.getUiState()[UiProps::delayUnit].toString() == "ms");
    }
}

TEST_CASE("tooltips quote the latencies the DSP reports", "[interaction]")
{
    // Every number in them comes from the DSP, so a change there can't leave the text stale.
    Fixture f;
    const auto switches = f.controls<pa::ui::ToggleSwitch3>();
    REQUIRE(switches.size() == 2);
    const auto& mode = *switches[1];
    const auto ms = [](int samples, double fs, int decimals)
    {
        const auto v = 1000.0 * samples / fs;
        return (decimals > 0 ? juce::String(v, decimals) : juce::String(juce::roundToInt(v))) + " ms";
    };
    using pa::dsp::HiLoStage;
    for (const auto item : {0, 1}) // High, Low
    {
        const auto& text = mode.getItemTooltip(item);
        CHECK(text.contains(ms(HiLoStage::latencyFor(44100.0), 44100.0, 1) + " of latency at 44.1 kHz"));
        CHECK(text.contains(ms(HiLoStage::latencyFor(48000.0), 48000.0, 1) + " at 48 kHz"));
        CHECK(text.contains(juce::String(HiLoStage::latencyFor(96000.0)) + " samples at 96 kHz"));
        CHECK(HiLoStage::latencyFor(176400.0) == 0); // "none from 176.4 kHz up"
        CHECK_FALSE(text.containsIgnoreCase("no latency"));
    }
    CHECK(mode.getItemTooltip(0).contains("150 Hz"));
    CHECK(mode.getItemTooltip(1).contains("75 Hz"));
    CHECK(mode.getItemTooltip(2).contains("about " + ms(pa::dsp::ConstantRotator::latencyFor(48000.0), 48000.0, 0) +
                                          " of latency"));
}

TEST_CASE("buttons: latching toggles, METER and ANALYSE", "[interaction]")
{
    Fixture f;
    const auto buttons = f.controls<pa::ui::PanelButton>();
    REQUIRE(buttons.size() == 4); // DELAY, polarity, PHASE, RANGE by x

    const auto centre = buttons[1]->getLocalBounds().getCentre().toFloat();
    click(*buttons[1], centre);
    CHECK(f.param(id::polarity) == 1.0f);
    click(*buttons[1], centre);
    CHECK(f.param(id::polarity) == 0.0f);
    click(*buttons[0], centre);
    CHECK(f.param(id::delayOn) == 1.0f); // off by default
    // The compensated latency it adds (plan 4.4), as the chain reports it.
    const auto delayMs44 = 1000.0 * pa::dsp::Chain::delayLatencyFor(44100.0, maxDelayTenths(44100.0)) / 44100.0;
    CHECK(buttons[0]->getTooltip().contains(juce::String(delayMs44, 1) + " ms of latency at 44.1 kHz"));
    click(*buttons[2], centre);
    CHECK(f.param(id::phaseOn) == 0.0f);

    // RANGE: out = 90 (default), in = 180; the knob position is kept.
    auto& range = *buttons[3];
    CHECK_FALSE(range.isOn());
    click(range, range.getLocalBounds().getCentre().toFloat());
    CHECK(f.param(id::phaseRange) == 1.0f);
    CHECK(range.isOn());

    // Round hit test: the corners of the square bounds are outside the button.
    CHECK(buttons[0]->hitTest(buttons[0]->getWidth() / 2, buttons[0]->getHeight() / 2));
    CHECK_FALSE(buttons[0]->hitTest(1, 1));

    const auto squares = f.controls<pa::ui::SquareButton>();
    REQUIRE(squares.size() == 2);
    std::vector<pa::ui::SquareButton*> byY(squares.begin(), squares.end());
    std::sort(byY.begin(), byY.end(), [](auto* a, auto* b) { return a->getY() < b->getY(); });
    auto& meter = *byY[0];
    auto& analyse = *byY[1];
    const auto meterCentre = meter.getLocalBounds().getCentre().toFloat();

    REQUIRE(meter.isLit());
    click(meter, meterCentre);
    CHECK((bool)f.proc.getUiState()[UiProps::meterOn] == false);
    CHECK_FALSE(meter.isLit());
    CHECK(f.editor->getMeterScreen().getState() == pa::ui::MeterScreen::State::off);

    // Releasing outside cancels the click.
    meter.mouseDown(mouseEvent(meter, meterCentre, meterCentre, leftButton));
    meter.mouseDrag(mouseEvent(meter, {-50.0f, -50.0f}, meterCentre, leftButton, 1, true));
    meter.mouseUp(mouseEvent(meter, {-50.0f, -50.0f}, meterCentre, {}, 1, true));
    CHECK((bool)f.proc.getUiState()[UiProps::meterOn] == false);

    click(meter, meterCentre);
    CHECK((bool)f.proc.getUiState()[UiProps::meterOn] == true);

    // ANALYSE presses but does nothing in v0.7, and stays unlit.
    click(analyse, analyse.getLocalBounds().getCentre().toFloat());
    CHECK_FALSE(analyse.isLit());
    CHECK(analyse.getTooltip().contains("future version"));
}

TEST_CASE("meter view: the selector in the middle of the bottom row opens a menu; choosing sets the view", "[interaction]")
{
    Fixture f;
    auto& screen = f.editor->getMeterScreen();
    REQUIRE(screen.getView() == pa::ui::MeterScreen::View::bands); // the default
    using View = pa::ui::MeterScreen::View;

    // The selector sits under the plot centre; a click opens the menu (here: counted). The rest of the screen is there for
    // its tooltips and ignores clicks.
    const auto selector = screen.viewSelectorCentreForTesting();
    CHECK(screen.hitTest(juce::roundToInt(selector.x), juce::roundToInt(selector.y)));
    CHECK(screen.hitTest(screen.getWidth() / 2, screen.getHeight() / 3));
    int opened = 0;
    screen.viewMenuHook = [&] { ++opened; };
    click(screen, selector);
    CHECK(opened == 1);

    // What the menu does with a choice.
    for (const auto& [view, name] : {std::pair{View::vector, "vector"}, std::pair{View::scope, "scope"},
                                      std::pair{View::bands, "bands"}})
    {
        screen.chooseView(view);
        CHECK(f.proc.getUiState()[UiProps::meterView].toString() == name);
        CHECK(screen.getView() == view);
    }

    // The old three-label row is gone: a click left of the selector on the row does nothing to the view.
    click(screen, {selector.x - 200.0f, selector.y});
    CHECK(opened == 1);

    // With the meter off there is nothing to click.
    f.proc.getUiState().setProperty(UiProps::meterOn, false, nullptr);
    CHECK_FALSE(screen.hitTest(juce::roundToInt(selector.x), juce::roundToInt(selector.y)));
}

TEST_CASE("meter scope: the wheel zooms between 0.5 and 200 ms", "[interaction]")
{
    Fixture f;
    auto& screen = f.editor->getMeterScreen();
    screen.chooseView(pa::ui::MeterScreen::View::scope);
    CHECK(screen.getScopeSpanMs() == Approx(20.0));
    screen.setScopeSpanMs(0.1);
    CHECK(screen.getScopeSpanMs() == Approx(0.5));
    screen.setScopeSpanMs(5000.0);
    CHECK(screen.getScopeSpanMs() == Approx(200.0));

    // The - and + buttons: a bit at a time, within the same limits.
    screen.setScopeSpanMs(20.0);
    click(screen, screen.zoomButtonCentreForTesting(true)); // + zooms in
    CHECK(screen.getScopeSpanMs() == Approx(20.0 / pa::ui::MeterScreen::zoomStep));
    click(screen, screen.zoomButtonCentreForTesting(false)); // - zooms out
    click(screen, screen.zoomButtonCentreForTesting(false));
    CHECK(screen.getScopeSpanMs() == Approx(20.0 * pa::ui::MeterScreen::zoomStep));
    for (int i = 0; i < 20; ++i)
        click(screen, screen.zoomButtonCentreForTesting(true));
    CHECK(screen.getScopeSpanMs() == Approx(0.5));

    // They are only there in ALIGNMENT.
    screen.chooseView(pa::ui::MeterScreen::View::bands);
    CHECK_FALSE(screen.tooltipAt(screen.zoomButtonCentreForTesting(true)).startsWith("Zoom"));
}

TEST_CASE("meter speed: SLOW and FAST at the left of the bottom row, kept in the UI state", "[interaction]")
{
    Fixture f;
    auto& screen = f.editor->getMeterScreen();
    CHECK(screen.getSpeed() == pa::ui::MeterScreen::Speed::slow);
    CHECK(f.proc.getUiState()[UiProps::meterSpeed].toString() == "slow");

    const auto fast = screen.speedLabelCentreForTesting(pa::ui::MeterScreen::Speed::fast);
    const auto slow = screen.speedLabelCentreForTesting(pa::ui::MeterScreen::Speed::slow);
    click(screen, fast);
    CHECK(f.proc.getUiState()[UiProps::meterSpeed].toString() == "fast");
    CHECK(screen.getSpeed() == pa::ui::MeterScreen::Speed::fast);
    CHECK(screen.getAnalyser().getSpeed() == pa::ui::MeterScreen::Speed::fast);
    click(screen, slow);
    CHECK(screen.getSpeed() == pa::ui::MeterScreen::Speed::slow);
}

TEST_CASE("meter hold: HOLD, or a stopped host, freezes the screen; a frozen screen previews a delay", "[interaction]")
{
    Fixture f;
    auto& screen = f.editor->getMeterScreen();
    CHECK_FALSE(screen.isFrozen());

    // Not frozen: a preview is refused.
    screen.setPreviewDelayMs(1.0);
    CHECK_FALSE(screen.isPreviewing());

    const auto hold = screen.holdLabelCentreForTesting();
    click(screen, hold);
    CHECK(screen.isHeld());
    CHECK(screen.isFrozen());

    screen.setPreviewDelayMs(1.04);
    CHECK(screen.isPreviewing());
    CHECK(screen.getAnalyser().previewDelayMs() == Approx(1.04).margin(0.001));
    screen.setPreviewDelayMs(9.0); // clamped to the knob's reach
    CHECK(screen.getAnalyser().previewDelayMs() == Approx(pa::params::maxDelayMs).margin(0.001));

    click(screen, hold); // releasing HOLD drops the preview
    CHECK_FALSE(screen.isFrozen());
    CHECK_FALSE(screen.isPreviewing());

    // The host's transport: stopped freezes, playing doesn't; unknown (standalone) doesn't.
    f.proc.getMeterCapture().setTransport(false, false);
    CHECK_FALSE(screen.isFrozen());
    f.proc.getMeterCapture().setTransport(true, false);
    CHECK(screen.isFrozen());
    f.proc.getMeterCapture().setTransport(true, true);
    CHECK_FALSE(screen.isFrozen());
}

TEST_CASE("meter hold: while frozen, the delay and polarity knobs preview their result", "[interaction]")
{
    Fixture f;
    auto& screen = f.editor->getMeterScreen();
    const auto setParam = [&](const char* paramId, float v)
    {
        auto* p = f.proc.getValueTreeState().getParameter(paramId);
        p->setValueNotifyingHost(p->convertTo0to1(v));
    };

    // Not frozen: the audio shows it, the screen doesn't preview.
    setParam(id::delayMs, 1.0f);
    CHECK_FALSE(screen.isPreviewing());

    screen.setHeld(true);
    setParam(id::delayMs, 1.2f); // delay is off: counts as 0
    CHECK(screen.isPreviewing());
    CHECK(screen.getAnalyser().previewDelayMs() == Approx(0.0).margin(1e-9));
    setParam(id::delayOn, 1.0f);
    CHECK(screen.getAnalyser().previewDelayMs() == Approx(1.2).margin(0.03)); // 0.1-sample steps at 48 kHz
    setParam(id::polarity, 1.0f);
    CHECK(screen.getAnalyser().isPreviewInverted());
    setParam(id::delayMs, -2.0f);
    CHECK(screen.getAnalyser().previewDelayMs() == Approx(-2.0).margin(0.03));
    CHECK(screen.getAnalyser().isPreviewInverted());

    screen.setHeld(false); // letting go drops it
    CHECK_FALSE(screen.isPreviewing());
    setParam(id::delayMs, 0.5f);
    CHECK_FALSE(screen.isPreviewing());
}

TEST_CASE("meter hold: while frozen, the phase knob and mode preview too (phase on)", "[interaction]")
{
    Fixture f;
    auto& screen = f.editor->getMeterScreen();
    const auto setParam = [&](const char* paramId, float v)
    {
        auto* p = f.proc.getValueTreeState().getParameter(paramId);
        p->setValueNotifyingHost(p->convertTo0to1(v));
    };

    screen.setHeld(true);
    setParam(id::phaseOn, 0.0f);
    CHECK_FALSE(screen.getAnalyser().previewHasPhase()); // phase off: no phase in the preview
    setParam(id::phaseOn, 1.0f);
    CHECK(screen.getAnalyser().previewHasPhase());
    setParam(id::phaseMode, 2.0f); // CONSTANT
    setParam(id::phase, 0.5f);     // half the range
    CHECK(screen.getAnalyser().previewHasPhase());
    CHECK(screen.isPreviewing());
    screen.setHeld(false);
    CHECK_FALSE(screen.getAnalyser().previewHasPhase());
}

TEST_CASE("meter legend: clicking INPUT, OUTPUT or SIDECHAIN hides or shows it, kept in the UI state", "[interaction]")
{
    Fixture f;
    auto& screen = f.editor->getMeterScreen();
    using Series = pa::ui::MeterScreen::Series;
    f.proc.getUiState().setProperty(UiProps::meterOn, true, nullptr);

    // All shown by default.
    for (const auto s : {Series::input, Series::output, Series::sidechain})
        CHECK(screen.isShown(s));
    CHECK((bool)f.proc.getUiState()[UiProps::showInput]);

    // The legend is a click target (the screen must be metering: give it a state by showing it as the desktop does; here
    // the labels are hit-tested whatever the audio is doing, as long as the meter is on).
    const auto input = screen.legendCentreForTesting(Series::input);
    const auto output = screen.legendCentreForTesting(Series::output);
    CHECK(screen.hitTest(juce::roundToInt(input.x), juce::roundToInt(input.y)));
    click(screen, input);
    CHECK_FALSE(screen.isShown(Series::input));
    CHECK_FALSE((bool)f.proc.getUiState()[UiProps::showInput]);
    CHECK(screen.isShown(Series::output)); // only the one
    click(screen, output);
    CHECK_FALSE(screen.isShown(Series::output));
    click(screen, input);
    click(screen, output);
    CHECK(screen.isShown(Series::input));
    CHECK(screen.isShown(Series::output));

    // The sidechain label is only there in ALIGNMENT.
    CHECK(screen.legendCentreForTesting(Series::sidechain) == juce::Point<float>());
    screen.chooseView(pa::ui::MeterScreen::View::scope);
    const auto sidechain = screen.legendCentreForTesting(Series::sidechain);
    CHECK(screen.hitTest(juce::roundToInt(sidechain.x), juce::roundToInt(sidechain.y)));
    click(screen, sidechain);
    CHECK_FALSE(screen.isShown(Series::sidechain));
    CHECK_FALSE((bool)f.proc.getUiState()[UiProps::showSidechain]);
}

TEST_CASE("tooltips: the ? switches them, off by default, and the meter has one tip per feature", "[interaction]")
{
    Fixture f;
    auto& help = f.editor->getHelpButton();
    auto& screen = f.editor->getMeterScreen();
    using View = pa::ui::MeterScreen::View;

    // Off by default; only the ? itself keeps its tip, which says how to turn them on.
    CHECK_FALSE(help.isOn());
    CHECK(help.getTooltip() == "Enable tooltips");
    CHECK(f.editor->tooltipAllowed(help));
    CHECK_FALSE(f.editor->tooltipAllowed(screen));
    click(help, help.getLocalBounds().getCentre().toFloat());
    CHECK(help.isOn());
    CHECK(f.proc.getUiState()[UiProps::tooltipsOn]);
    CHECK(f.editor->tooltipAllowed(screen));
    CHECK(help.getTooltip() == "Disable tooltips");
    click(help, help.getLocalBounds().getCentre().toFloat());
    CHECK_FALSE(help.isOn());

    // Each feature of each view says what it is.
    f.proc.getUiState().setProperty(UiProps::meterOn, true, nullptr);
    const auto tip = [&](juce::Point<float> p) { return screen.tooltipAt(p); };
    CHECK(tip(screen.viewSelectorCentreForTesting()).startsWith("View:"));
    CHECK(tip(screen.speedLabelCentreForTesting(pa::ui::MeterScreen::Speed::slow)).startsWith("SLOW"));
    CHECK(tip(screen.speedLabelCentreForTesting(pa::ui::MeterScreen::Speed::fast)).startsWith("FAST"));
    CHECK(tip(screen.holdLabelCentreForTesting()).startsWith("HOLD"));
    CHECK(tip(screen.legendCentreForTesting(pa::ui::MeterScreen::Series::input)).startsWith("INPUT"));
    CHECK(tip(screen.legendCentreForTesting(pa::ui::MeterScreen::Series::output)).startsWith("OUTPUT"));
    CHECK(tip({(float)screen.getWidth() * 0.9f, (float)screen.getHeight() * 0.4f}).startsWith("ALL"));
    CHECK(tip({(float)screen.getWidth() * 0.1f, (float)screen.getHeight() * 0.3f}).contains(" Hz: "));
    CHECK(tip({(float)screen.getWidth() * 0.5f, (float)screen.getHeight() * 0.3f}) !=
          tip({(float)screen.getWidth() * 0.1f, (float)screen.getHeight() * 0.3f})); // per band

    screen.chooseView(View::vector);
    CHECK(tip({(float)screen.getWidth() * 0.4f, (float)screen.getHeight() * 0.5f}).startsWith("VECTORSCOPE"));
    screen.chooseView(View::scope);
    CHECK(tip(screen.zoomButtonCentreForTesting(false)).startsWith("Zoom out"));
    CHECK(tip(screen.zoomButtonCentreForTesting(true)).startsWith("Zoom in"));
    CHECK(tip(screen.legendCentreForTesting(pa::ui::MeterScreen::Series::sidechain)).startsWith("SIDECHAIN"));
    CHECK(tip({(float)screen.getWidth() * 0.4f, (float)screen.getHeight() * 0.5f}).startsWith("ALIGNMENT"));

    // Nothing to say while the meter is off.
    f.proc.getUiState().setProperty(UiProps::meterOn, false, nullptr);
    CHECK(tip(screen.viewSelectorCentreForTesting()).isEmpty());
}
