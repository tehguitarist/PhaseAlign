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
    CHECK(f.param(id::delayOn) == 1.0f);                           // off by default
    CHECK(buttons[0]->getTooltip().contains("4.5 ms of latency")); // the compensated latency it adds (plan 4.4)
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

TEST_CASE("meter view: click FREQUENCY or TIME OFFSET on the screen's bottom row", "[interaction]")
{
    Fixture f;
    auto& screen = f.editor->getMeterScreen();
    REQUIRE(screen.getView() == pa::ui::MeterScreen::View::frequency);

    // The labels sit either side of the plot centre on the bottom row; elsewhere clicks pass through.
    const auto centreX = 0.5f * (pa::design::meterPlotLeft + pa::design::meterPlotRight);
    const auto time = screen.toLocal(juce::Point<float>(centreX + 80.0f, pa::design::meterFreqTitleY));
    const auto freq = screen.toLocal(juce::Point<float>(centreX - 80.0f, pa::design::meterFreqTitleY));
    CHECK(screen.hitTest(juce::roundToInt(time.x), juce::roundToInt(time.y)));
    CHECK_FALSE(screen.hitTest(screen.getWidth() / 2, screen.getHeight() / 3));

    click(screen, time);
    CHECK(f.proc.getUiState()[UiProps::meterView].toString() == "time");
    CHECK(screen.getView() == pa::ui::MeterScreen::View::time);
    click(screen, freq);
    CHECK(f.proc.getUiState()[UiProps::meterView].toString() == "frequency");
    CHECK(screen.getView() == pa::ui::MeterScreen::View::frequency);

    // With the meter off there is nothing to click.
    f.proc.getUiState().setProperty(UiProps::meterOn, false, nullptr);
    CHECK_FALSE(screen.hitTest(juce::roundToInt(time.x), juce::roundToInt(time.y)));
}
