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
std::unique_ptr<PhaseAlignEditor> makeEditor(PhaseAlignProcessor& p)
{
    auto* editor = dynamic_cast<PhaseAlignEditor*>(p.createEditor());
    REQUIRE(editor != nullptr);
    return std::unique_ptr<PhaseAlignEditor>(editor);
}

template <typename T>
std::vector<T*> childrenOfType(juce::Component& parent)
{
    std::vector<T*> found;
    for (auto* c : parent.getChildren())
        if (auto* t = dynamic_cast<T*>(c))
            found.push_back(t);
    return found;
}

void setParam(PhaseAlignProcessor& p, const char* paramId, float value)
{
    auto* param = p.getValueTreeState().getParameter(paramId);
    param->setValueNotifyingHost(param->convertTo0to1(value));
}

float getParam(PhaseAlignProcessor& p, const char* paramId)
{
    auto* param = p.getValueTreeState().getParameter(paramId);
    return param->convertFrom0to1(param->getValue());
}
} // namespace

TEST_CASE("editor opens at the saved size and keeps every control inside the panel", "[editor]")
{
    PhaseAlignProcessor proc;

    for (const auto scale : {0.75, 1.0, 2.0})
    {
        INFO("scale " << scale);
        proc.getUiState().setProperty(UiProps::uiScale, scale, nullptr);
        auto editor = makeEditor(proc);

        CHECK(editor->getWidth() == juce::roundToInt(977 * scale));
        CHECK(editor->getHeight() == juce::roundToInt(612 * scale));
        CHECK(editor->isResizable());

        for (auto* c : editor->getChildren())
            if (dynamic_cast<pa::ui::DesignComponent*>(c) != nullptr)
                CHECK(editor->getLocalBounds().contains(c->getBounds()));

        // The meter screen covers exactly the baked screen interior.
        const auto expected = pa::design::meterInterior * (float)editor->getWidth() / pa::layout::designWidth;
        CHECK(editor->getMeterScreen().getBounds().toFloat().getX() == Approx(expected.getX()).margin(0.5));
        CHECK(editor->getMeterScreen().getBounds().toFloat().getRight() == Approx(expected.getRight()).margin(0.5));
    }
}

TEST_CASE("resizing is limited to 75-200% with the aspect ratio locked, and is saved", "[editor]")
{
    PhaseAlignProcessor proc;
    auto editor = makeEditor(proc);
    auto* constrainer = editor->getConstrainer();
    REQUIRE(constrainer != nullptr);

    CHECK(constrainer->getMinimumWidth() == 733);
    CHECK(constrainer->getMaximumWidth() == 1954);
    CHECK(constrainer->getFixedAspectRatio() == Approx(1954.0 / 1224.0));

    editor->setSize(1466, 918);
    CHECK((double)proc.getUiState()[UiProps::uiScale] == Approx(1.5).margin(1e-3));
}

TEST_CASE("readouts follow the parameters and the unit switch", "[editor]")
{
    PhaseAlignProcessor proc;
    proc.prepareToPlay(48000.0, 512);
    auto editor = makeEditor(proc);

    const auto readouts = childrenOfType<pa::ui::Readout>(*editor);
    REQUIRE(readouts.size() == 2);
    auto* delayReadout = readouts[0]->getX() < readouts[1]->getX() ? readouts[0] : readouts[1];
    auto* phaseReadout = delayReadout == readouts[0] ? readouts[1] : readouts[0];

    CHECK(delayReadout->getDigits() == "0.000");
    CHECK(phaseReadout->getDigits() == "0.0");

    setParam(proc, id::delayMs, 1.0f);
    setParam(proc, id::phase, 0.5f);
    CHECK(delayReadout->getDigits() == "1.000");
    CHECK(phaseReadout->getDigits() == "45.0");

    // RANGE keeps the knob position, so the angle doubles; the scale label follows.
    const auto labels = childrenOfType<pa::ui::ScaleLabel>(*editor);
    REQUIRE(labels.size() == 1);
    CHECK(labels[0]->getDegrees() == 90);
    setParam(proc, id::phaseRange, 1.0f);
    CHECK(phaseReadout->getDigits() == "90.0");
    CHECK(labels[0]->getDegrees() == 180);

    proc.getUiState().setProperty(UiProps::delayUnit, "samples", nullptr);
    CHECK(delayReadout->getDigits() == "48.0");
    proc.getUiState().setProperty(UiProps::delayUnit, "cm", nullptr);
    CHECK(delayReadout->getDigits() == "34.3");

    // A new sample rate changes the effective value (announced asynchronously).
    proc.getUiState().setProperty(UiProps::delayUnit, "samples", nullptr);
    proc.prepareToPlay(96000.0, 512);
    proc.dispatchPendingMessages();
    CHECK(delayReadout->getDigits() == "96.0");

    // Typed values go back to the parameter in ms.
    REQUIRE(delayReadout->onTextEntered != nullptr);
    delayReadout->onTextEntered("48");
    CHECK(getParam(proc, id::delayMs) == Approx(0.5f));
    delayReadout->onTextEntered("1 ms");
    CHECK(getParam(proc, id::delayMs) == Approx(1.0f));
    delayReadout->onTextEntered("-59.3"); // samples, fractional, negative
    CHECK(delayReadout->getDigits() == "-59.3");
    phaseReadout->onTextEntered("36"); // range 180
    CHECK(getParam(proc, id::phase) == Approx(0.2f));
    phaseReadout->onTextEntered("junk");
    CHECK(getParam(proc, id::phase) == Approx(0.2f));
    setParam(proc, id::phaseRange, 0.0f);
    phaseReadout->onTextEntered("120"); // clamped to the 90 range
    CHECK(getParam(proc, id::phase) == Approx(1.0f));
}

TEST_CASE("the phase readout adds 180 while the polarity is inverted, up to 360", "[editor]")
{
    PhaseAlignProcessor proc;
    auto editor = makeEditor(proc);
    const auto readouts = childrenOfType<pa::ui::Readout>(*editor);
    REQUIRE(readouts.size() == 2);
    auto* phaseReadout = readouts[0]->getX() > readouts[1]->getX() ? readouts[0] : readouts[1];

    setParam(proc, id::polarity, 1.0f);
    CHECK(phaseReadout->getDigits() == "180.0");
    setParam(proc, id::phase, 0.5f);
    CHECK(phaseReadout->getDigits() == "225.0"); // range 90
    setParam(proc, id::phaseRange, 1.0f);
    setParam(proc, id::phase, 1.0f);
    CHECK(phaseReadout->getDigits() == "360.0");
    setParam(proc, id::polarity, 0.0f);
    CHECK(phaseReadout->getDigits() == "180.0");

    // Typed values are what the display shows: the inversion's 180 comes off first, clamped to the knob's travel.
    setParam(proc, id::polarity, 1.0f);
    phaseReadout->onTextEntered("270");
    CHECK(getParam(proc, id::phase) == Approx(0.5f));
    phaseReadout->onTextEntered("90"); // below the inverted start
    CHECK(getParam(proc, id::phase) == Approx(0.0f));
    phaseReadout->onTextEntered("400");
    CHECK(getParam(proc, id::phase) == Approx(1.0f));
}

TEST_CASE("switches, buttons and LEDs follow the model", "[editor]")
{
    PhaseAlignProcessor proc;
    auto editor = makeEditor(proc);

    auto switches = childrenOfType<pa::ui::ToggleSwitch3>(*editor);
    REQUIRE(switches.size() == 2);
    std::sort(switches.begin(), switches.end(), [](auto* a, auto* b) { return a->getX() < b->getX(); });
    auto* unitSwitch = switches[0];
    auto* modeSwitch = switches[1];

    setParam(proc, id::phaseMode, 2.0f);
    CHECK(modeSwitch->getIndex() == 2);
    modeSwitch->onSelect(1);
    CHECK(getParam(proc, id::phaseMode) == 1.0f);

    unitSwitch->onSelect(2);
    CHECK(proc.getUiState()[UiProps::delayUnit].toString() == "cm");
    proc.getUiState().setProperty(UiProps::delayUnit, "samples", nullptr);
    CHECK(unitSwitch->getIndex() == 1);

    auto leds = childrenOfType<pa::ui::ImageIndicator>(*editor);
    auto buttons = childrenOfType<pa::ui::PanelButton>(*editor);
    REQUIRE(leds.size() == 3);
    REQUIRE(buttons.size() == 4); // DELAY, polarity, PHASE, RANGE (no LED)
    const auto sortByX = [](auto* a, auto* b) { return a->getX() < b->getX(); };
    std::sort(leds.begin(), leds.end(), sortByX);
    std::sort(buttons.begin(), buttons.end(), sortByX);

    // phaseOn defaults on, delayOn and polarity off; each LED follows its button.
    const auto states = [&]
    {
        std::vector<bool> v;
        for (size_t i = 0; i < 3; ++i)
        {
            CHECK(leds[i]->isLit() == buttons[i]->isOn());
            v.push_back(buttons[i]->isOn());
        }
        return v;
    };
    CHECK(states() == std::vector<bool>{false, false, true});
    setParam(proc, id::polarity, 1.0f);
    setParam(proc, id::delayOn, 1.0f);
    CHECK(states() == std::vector<bool>{true, true, true});
}

TEST_CASE("the meter screen's timer runs only while the meter is on and showing", "[editor]")
{
    PhaseAlignProcessor proc;
    auto editor = makeEditor(proc);
    auto& meter = editor->getMeterScreen();

    // The editor isn't on the desktop here, so the screen isn't showing: no timer even with the meter on.
    CHECK(meter.getState() == pa::ui::MeterScreen::State::noSidechain);
    CHECK_FALSE(meter.isTimerRunningForTesting());

    proc.getUiState().setProperty(UiProps::meterOn, false, nullptr);
    CHECK(meter.getState() == pa::ui::MeterScreen::State::off);
    CHECK_FALSE(meter.isTimerRunningForTesting());
}

//==============================================================================
// Hidden: renders the editor to PNGs for comparing against "ui/full example.png" (M3 done-when).
// PA_SNAPSHOT_DIR picks the folder (default: ./snapshots).
TEST_CASE("snapshots", "[.][snapshot]")
{
    PhaseAlignProcessor proc;
    setParam(proc, id::polarity, 1.0f);   // all three LEDs lit, as in the example
    setParam(proc, id::phaseRange, 1.0f); // the example's "180°" scale label

    const auto dirName = juce::SystemStats::getEnvironmentVariable("PA_SNAPSHOT_DIR", "snapshots");
    const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile(dirName);
    dir.createDirectory();

    struct Shot
    {
        double uiScale;
        float pixelScale;
        const char* name;
    };
    for (const auto& shot :
         {Shot{0.75, 1.0f, "ui_075.png"}, Shot{1.0, 1.0f, "ui_100.png"}, Shot{2.0, 1.0f, "ui_200.png"},
          Shot{1.0, 2.0f, "ui_100_retina.png"}, Shot{1.5, 1.0f, "ui_150.png"}})
    {
        proc.getUiState().setProperty(UiProps::uiScale, shot.uiScale, nullptr);
        auto editor = makeEditor(proc);
        const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, shot.pixelScale);

        const auto file = dir.getChildFile(shot.name);
        file.deleteFile();
        juce::FileOutputStream out(file);
        REQUIRE(out.openedOk());
        REQUIRE(juce::PNGImageFormat().writeImageToStream(image, out));
        WARN("wrote " << file.getFullPathName() << " (" << image.getWidth() << "x" << image.getHeight() << ")");
    }

    // Variants: other units, modes and the meter off.
    proc.getUiState().setProperty(UiProps::uiScale, 1.0, nullptr);
    proc.getUiState().setProperty(UiProps::delayUnit, "samples", nullptr);
    proc.getUiState().setProperty(UiProps::meterOn, false, nullptr);
    setParam(proc, id::delayMs, -2.5f);
    setParam(proc, id::phaseRange, 0.0f);
    setParam(proc, id::phase, 0.75f); // 67.5 degrees at range 90
    setParam(proc, id::phaseMode, 2.0f);
    setParam(proc, id::delayOn, 0.0f);
    auto editor = makeEditor(proc);
    const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 2.0f);
    const auto file = dir.getChildFile("ui_100_retina_variant.png");
    file.deleteFile();
    juce::FileOutputStream out(file);
    REQUIRE(juce::PNGImageFormat().writeImageToStream(image, out));

    // The delay readout at its widest, negative, in each unit (the ghost has a sign position).
    for (const auto* unit : {"ms", "samples", "cm"})
    {
        proc.getUiState().setProperty(UiProps::delayUnit, unit, nullptr);
        setParam(proc, id::delayMs, -4.0f);
        auto e = makeEditor(proc);
        const auto readout = e->createComponentSnapshot(
            e->getLocalBounds().removeFromTop(e->getHeight() / 2).removeFromLeft(e->getWidth() / 3), true, 2.0f);
        const auto f = dir.getChildFile("delay_readout_negative_" + juce::String(unit) + ".png");
        f.deleteFile();
        juce::FileOutputStream o(f);
        REQUIRE(juce::PNGImageFormat().writeImageToStream(readout, o));
    }
}

// Hidden: puts the editor on screen and checks that, once drawn, nothing repaints while idle
// (M3 done-when). Needs a display, so it's not part of the ctest run.
TEST_CASE("idle editor does not repaint", "[.][desktop]")
{
    PhaseAlignProcessor proc;
    auto editor = makeEditor(proc);
    editor->addToDesktop(juce::ComponentPeer::windowHasTitleBar);
    editor->setVisible(true);

    auto* mm = juce::MessageManager::getInstance();
    // Wait for the first paint (slow on a busy machine), then let the window settle before measuring.
    for (int waited = 0; editor->getPaintCount() == 0 && waited < 5000; waited += 100)
        mm->runDispatchLoopUntil(100);
    REQUIRE(editor->getPaintCount() > 0);
    mm->runDispatchLoopUntil(500);
    CHECK(editor->getMeterScreen().isTimerRunningForTesting()); // meter on and showing

    const auto panelPaints = editor->getPaintCount();
    const auto meterPaints = editor->getMeterScreen().getPaintCount();
    mm->runDispatchLoopUntil(2000);
    CHECK(editor->getPaintCount() == panelPaints);
    CHECK(editor->getMeterScreen().getPaintCount() == meterPaints);

    // A value change repaints, but the meter screen is untouched by it.
    setParam(proc, id::phase, 0.5f);
    mm->runDispatchLoopUntil(300);
    CHECK(editor->getPaintCount() > panelPaints);
    CHECK(editor->getMeterScreen().getPaintCount() == meterPaints);

    // Meter off stops its timer.
    proc.getUiState().setProperty(UiProps::meterOn, false, nullptr);
    CHECK_FALSE(editor->getMeterScreen().isTimerRunningForTesting());
    editor->removeFromDesktop();
}

// Hidden: the meter end to end on screen (M4 done-when). A delayed copy of the input is fed as the sidechain
// through processBlock while the editor shows; the screen must capture, analyse and read +1 once the delay is
// set, repaint only itself and only while metering, and stop capturing when the meter is off or the editor
// closes. Writes snapshots of both views (unaligned and aligned) to PA_SNAPSHOT_DIR.
TEST_CASE("meter on screen: reads +1 when aligned, and stops when off", "[.][desktop]")
{
    const auto fs = 48000.0;
    const int block = 512, d = 50;
    PhaseAlignProcessor proc;
    setParam(proc, id::delayOn, 1.0f); // with its latency, which the meter aligns
    proc.prepareToPlay(fs, block);
    auto& capture = proc.getMeterCapture();
    auto editor = makeEditor(proc);
    editor->addToDesktop(juce::ComponentPeer::windowHasTitleBar);
    editor->setVisible(true);
    auto& screen = editor->getMeterScreen();
    auto* mm = juce::MessageManager::getInstance();
    mm->runDispatchLoopUntil(300);
    REQUIRE(capture.isActive());

    const auto dirName = juce::SystemStats::getEnvironmentVariable("PA_SNAPSHOT_DIR", "snapshots");
    const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile(dirName);
    dir.createDirectory();
    const auto snapshot = [&](const char* name)
    {
        const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 2.0f);
        const auto file = dir.getChildFile(name);
        file.deleteFile();
        juce::FileOutputStream out(file);
        REQUIRE(juce::PNGImageFormat().writeImageToStream(image, out));
    };

    juce::Random rng(3);
    std::vector<float> history(1024, 0.0f); // the source's recent past: the sidechain is it `lag` samples later
    size_t h = 0;
    int lag = d;
    float sidechainGain = 1.0f;
    // Roughly real time: a block of audio, then about a block's worth of message loop.
    const auto play = [&](double seconds)
    {
        juce::MidiBuffer midi;
        for (int done = 0; done < (int)(seconds * fs); done += block)
        {
            juce::AudioBuffer<float> b(4, block);
            for (int i = 0; i < block; ++i)
            {
                const auto x = (rng.nextFloat() - 0.5f) * 0.6f;
                history[h] = x;
                const auto late = history[(h + history.size() - (size_t)lag) % history.size()] * sidechainGain;
                h = (h + 1) % history.size();
                b.setSample(0, i, x);
                b.setSample(1, i, x);
                b.setSample(2, i, late);
                b.setSample(3, i, late);
            }
            proc.processBlock(b, midi);
            mm->runDispatchLoopUntil(10);
        }
    };

    play(2.5);
    CHECK(screen.getState() == pa::ui::MeterScreen::State::metering);
    CHECK(screen.getAnalyser().overallProcessed() < 0.5f);
    snapshot("meter_frequency_unaligned.png");
    proc.getUiState().setProperty(UiProps::meterView, "time", nullptr);
    play(0.3);
    snapshot("meter_time_unaligned.png");

    setParam(proc, id::delayMs, (float)(1000.0 * d / fs));
    play(2.5);
    CHECK(screen.getAnalyser().overallProcessed() > 0.99f);
    snapshot("meter_time_aligned.png");
    proc.getUiState().setProperty(UiProps::meterView, "frequency", nullptr);
    play(0.3);
    snapshot("meter_frequency_aligned.png");

    // An offset beyond the knob's reach (4.5 ms) says so in the time view.
    lag = 216;
    proc.getUiState().setProperty(UiProps::meterView, "time", nullptr);
    play(2.5);
    snapshot("meter_time_out_of_range.png");
    lag = d;
    proc.getUiState().setProperty(UiProps::meterView, "frequency", nullptr);
    play(2.5);

    // The panel itself never repaints for the meter.
    const auto panelPaints = editor->getPaintCount();
    const auto meterPaints = screen.getPaintCount();
    play(1.0);
    CHECK(editor->getPaintCount() == panelPaints);
    CHECK(screen.getPaintCount() > meterPaints + 10);

    // Sidechain silent: NO SIDECHAIN SIGNAL after a second, then no more repaints.
    sidechainGain = 0.0f;
    play(1.8);
    CHECK(screen.getState() == pa::ui::MeterScreen::State::noSidechain);
    const auto idlePaints = screen.getPaintCount();
    play(1.0);
    CHECK(screen.getPaintCount() == idlePaints);
    snapshot("meter_no_sidechain.png");

    // Meter off: capture stops.
    proc.getUiState().setProperty(UiProps::meterOn, false, nullptr);
    CHECK_FALSE(capture.isActive());
    proc.getUiState().setProperty(UiProps::meterOn, true, nullptr);
    CHECK(capture.isActive());

    // Hidden window: capture stops on the next tick. Closed editor: stopped.
    editor->setVisible(false);
    mm->runDispatchLoopUntil(200);
    CHECK_FALSE(capture.isActive());
    editor->removeFromDesktop();
    editor.reset();
    CHECK_FALSE(capture.isActive());
}

TEST_CASE("a new instance opens at 80% of the reference size", "[editor]")
{
    PhaseAlignProcessor proc;
    auto editor = makeEditor(proc);
    CHECK(editor->getWidth() == 782);
    CHECK(editor->getHeight() == 490);
}
