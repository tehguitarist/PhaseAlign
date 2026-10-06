// Switched-off sections (IMPLEMENTATION_PLAN 4.5, R11): what dims, that it follows the parameters however they
// change, and that dimmed controls stay usable.

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/Design.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace pa::params;
namespace layout = pa::layout;

namespace
{
struct Fixture
{
    Fixture() { open(); }

    void open()
    {
        editor.reset();
        proc.prepareToPlay(48000.0, 512);
        editor.reset(dynamic_cast<PhaseAlignEditor*>(proc.createEditor()));
        REQUIRE(editor != nullptr);
    }

    void set(const char* paramId, float value)
    {
        auto* p = proc.getValueTreeState().getParameter(paramId);
        p->setValueNotifyingHost(p->convertTo0to1(value));
    }

    float get(const char* paramId)
    {
        auto* p = proc.getValueTreeState().getParameter(paramId);
        return p->convertFrom0to1(p->getValue());
    }

    template <typename T>
    std::vector<T*> all() const
    {
        std::vector<T*> found;
        for (auto* c : editor->getChildren())
            if (auto* t = dynamic_cast<T*>(c))
                found.push_back(t);
        std::sort(found.begin(), found.end(), [](auto* a, auto* b) { return a->getX() < b->getX(); });
        return found;
    }

    template <typename T>
    T* at(juce::Rectangle<float> designBounds) const
    {
        for (auto* c : all<T>())
            if (c->getDesignBounds() == designBounds)
                return c;
        FAIL("no component at those design bounds");
        return nullptr;
    }

    // The delay controls first, then the phase controls (RANGE's button and scale label included).
    pa::ui::ImageKnob& delayKnob() const { return *all<pa::ui::ImageKnob>()[0]; }
    pa::ui::ImageKnob& phaseKnob() const { return *all<pa::ui::ImageKnob>()[1]; }
    pa::ui::ToggleSwitch3& unitSwitch() const { return *all<pa::ui::ToggleSwitch3>()[0]; }
    pa::ui::ToggleSwitch3& modeSwitch() const { return *all<pa::ui::ToggleSwitch3>()[1]; }
    pa::ui::Readout& delayReadout() const { return *all<pa::ui::Readout>()[0]; }
    pa::ui::Readout& phaseReadout() const { return *all<pa::ui::Readout>()[1]; }
    pa::ui::ScaleLabel& rangeLabel() const { return *all<pa::ui::ScaleLabel>()[0]; }
    pa::ui::PanelButton& rangeButton() const { return *at<pa::ui::PanelButton>(layout::rangeButton.bounds()); }

    std::vector<pa::ui::DesignComponent*> delaySection() const
    {
        return {&delayKnob(), &unitSwitch(), &delayReadout()};
    }
    std::vector<pa::ui::DesignComponent*> phaseSection() const
    {
        return {&phaseKnob(), &modeSwitch(), &phaseReadout(), &rangeLabel(), &rangeButton()};
    }

    // Everything that never dims: the DELAY, polarity and PHASE buttons and their LEDs, METER and ANALYSE.
    std::vector<pa::ui::DesignComponent*> alwaysFull() const
    {
        std::vector<pa::ui::DesignComponent*> v;
        for (auto* b : all<pa::ui::PanelButton>())
            if (b != &rangeButton())
                v.push_back(b);
        for (auto* l : all<pa::ui::ImageIndicator>())
            v.push_back(l);
        for (auto* s : all<pa::ui::SquareButton>())
            v.push_back(s);
        return v;
    }

    bool dimmed(const std::vector<pa::ui::DesignComponent*>& group) const
    {
        REQUIRE_FALSE(group.empty());
        const auto first = group.front()->isDimmed();
        for (auto* c : group)
            CHECK(c->isDimmed() == first);
        return first;
    }

    void expect(bool delayDimmed, bool phaseDimmed)
    {
        CHECK(dimmed(delaySection()) == delayDimmed);
        CHECK(dimmed(phaseSection()) == phaseDimmed);
        CHECK_FALSE(dimmed(alwaysFull()));
    }

    PhaseAlignProcessor proc;
    std::unique_ptr<PhaseAlignEditor> editor;
};

juce::MouseEvent mouseEvent(juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, bool dragged)
{
    const auto now = juce::Time::getCurrentTime();
    return {juce::Desktop::getInstance().getMainMouseSource(),
            pos,
            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
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
            1,
            dragged};
}

void click(juce::Component& c, juce::Point<float> pos)
{
    c.mouseDown(mouseEvent(c, pos, pos, false));
    c.mouseUp(mouseEvent(c, pos, pos, false));
}

void dragUp(juce::Component& c, float pixels)
{
    const auto from = c.getLocalBounds().getCentre().toFloat();
    c.mouseDown(mouseEvent(c, from, from, false));
    for (int i = 1; i <= 10; ++i)
        c.mouseDrag(mouseEvent(c, from.translated(0.0f, -pixels * (float)i / 10.0f), from, true));
    c.mouseUp(mouseEvent(c, from.translated(0.0f, -pixels), from, true));
}
} // namespace

TEST_CASE("dimming follows delayOn and phaseOn", "[dimming]")
{
    Fixture f;
    f.set(id::delayOn, 1.0f);
    f.set(id::phaseOn, 1.0f);
    f.expect(false, false);

    f.set(id::delayOn, 0.0f);
    f.expect(true, false);
    f.set(id::phaseOn, 0.0f);
    f.expect(true, true);
    f.set(id::delayOn, 1.0f);
    f.expect(false, true);
    f.set(id::phaseOn, 1.0f);
    f.expect(false, false);
}

TEST_CASE("dimming is right when the editor opens and when state is loaded", "[dimming]")
{
    Fixture f;
    f.set(id::delayOn, 0.0f);
    f.set(id::phaseOn, 1.0f);
    juce::MemoryBlock saved;
    f.proc.getStateInformation(saved);

    // A new editor on an instance whose sections are already off opens dimmed, with no change to trigger it.
    f.set(id::delayOn, 1.0f);
    f.set(id::phaseOn, 0.0f);
    f.open();
    f.expect(false, true);

    // Loading a saved state into a showing editor.
    f.proc.setStateInformation(saved.getData(), (int)saved.getSize());
    f.expect(true, false);

    // And into a fresh instance.
    PhaseAlignProcessor other;
    other.prepareToPlay(48000.0, 512);
    other.setStateInformation(saved.getData(), (int)saved.getSize());
    std::unique_ptr<PhaseAlignEditor> otherEditor(dynamic_cast<PhaseAlignEditor*>(other.createEditor()));
    REQUIRE(otherEditor != nullptr);
    CHECK(otherEditor->getChildren().size() > 0);
    for (auto* k : otherEditor->getChildren())
        if (auto* knob = dynamic_cast<pa::ui::ImageKnob*>(k))
            CHECK(knob->isDimmed() == (knob->getDesignBounds() == layout::delayKnob.bounds()));
}

TEST_CASE("dimming follows host automation from another thread", "[dimming]")
{
    Fixture f;
    f.set(id::delayOn, 1.0f);
    f.set(id::phaseOn, 1.0f);

    // The host writes the parameters from its own thread; the editor catches up on the message thread.
    std::thread host(
        [&]
        {
            f.proc.getValueTreeState().getParameter(id::delayOn)->setValueNotifyingHost(0.0f);
            f.proc.getValueTreeState().getParameter(id::phaseOn)->setValueNotifyingHost(0.0f);
        });
    host.join();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    f.expect(true, true);
}

TEST_CASE("dimmed controls stay fully usable", "[dimming]")
{
    Fixture f;
    f.set(id::delayOn, 0.0f);
    f.set(id::phaseOn, 0.0f);
    f.set(id::phaseRange, 0.0f);
    f.set(id::delayMs, 0.0f);
    f.set(id::phase, 0.0f);
    f.expect(true, true);

    SECTION("knobs: drag and keys")
    {
        dragUp(f.delayKnob(), 150.0f);
        CHECK(f.get(id::delayMs) > 0.1f);
        dragUp(f.phaseKnob(), 150.0f);
        CHECK(f.get(id::phase) == Catch::Approx(0.5f).margin(1e-3f));

        const auto before = f.get(id::delayMs);
        f.delayKnob().keyPressed(juce::KeyPress(juce::KeyPress::downKey));
        CHECK(f.get(id::delayMs) < before);
        const auto phaseBefore = f.get(id::phase);
        f.phaseKnob().keyPressed(juce::KeyPress(juce::KeyPress::upKey));
        CHECK(f.get(id::phase) > phaseBefore);
    }

    SECTION("switches: click the switch and a label")
    {
        auto& mode = f.modeSwitch();
        const auto sw = mode.toLocal(layout::phaseSwitch.bounds());
        click(mode, sw.getCentre().translated(0.0f, sw.getHeight() * 0.3f)); // lower half: one step down
        CHECK(f.get(id::phaseMode) == 1.0f);
        mode.onSelect(2);
        CHECK(f.get(id::phaseMode) == 2.0f);

        auto& unit = f.unitSwitch();
        const auto usw = unit.toLocal(layout::delaySwitch.bounds());
        click(unit, usw.getCentre().translated(0.0f, usw.getHeight() * 0.3f));
        CHECK(f.proc.getUiState()[PhaseAlignProcessor::UiProps::delayUnit].toString() == "samples");
    }

    SECTION("RANGE still toggles")
    {
        auto& range = f.rangeButton();
        click(range, range.getLocalBounds().getCentre().toFloat());
        CHECK(f.get(id::phaseRange) == 1.0f);
        CHECK(f.rangeLabel().getDegrees() == 180);
    }

    f.expect(true, true); // using them doesn't un-dim them
}

TEST_CASE("readouts dim only their value, and polarity always applies", "[dimming]")
{
    Fixture f;
    f.set(id::phaseRange, 0.0f);
    f.set(id::phase, 0.5f); // 45 degrees
    f.set(id::delayOn, 1.0f);

    SECTION("phase off, polarity normal: the knob's angle, dimmed")
    {
        f.set(id::phaseOn, 0.0f);
        f.set(id::polarity, 0.0f);
        CHECK(f.phaseReadout().isDimmed());
        CHECK(f.phaseReadout().getDigits() == formatPhase(45.0));
    }

    SECTION("phase off, polarity inverted: 180.0 at full brightness; the polarity switch isn't dimmed")
    {
        f.set(id::phaseOn, 0.0f);
        f.set(id::polarity, 1.0f);
        CHECK_FALSE(f.phaseReadout().isDimmed());
        CHECK(f.phaseReadout().getDigits() == formatPhase(180.0));
        CHECK(f.phaseKnob().isDimmed()); // the rest of the section still dims
        CHECK_FALSE(f.dimmed(f.alwaysFull()));
    }

    SECTION("phase on: never dimmed, and the inversion adds 180 as before")
    {
        f.set(id::phaseOn, 1.0f);
        f.set(id::polarity, 1.0f);
        CHECK_FALSE(f.phaseReadout().isDimmed());
        CHECK(f.phaseReadout().getDigits() == formatPhase(225.0));
    }

    SECTION("the delay readout dims with the delay section, whatever the polarity")
    {
        f.set(id::polarity, 1.0f);
        f.set(id::delayOn, 0.0f);
        CHECK(f.delayReadout().isDimmed());
        f.set(id::delayOn, 1.0f);
        CHECK_FALSE(f.delayReadout().isDimmed());
    }
}

// Hidden: both sections off, at 100% and 100% Retina, for judging the dimmed alpha (PA_SNAPSHOT_DIR).
TEST_CASE("dimming snapshots", "[.][snapshot]")
{
    PhaseAlignProcessor proc;
    const auto set = [&](const char* paramId, float v)
    {
        auto* p = proc.getValueTreeState().getParameter(paramId);
        p->setValueNotifyingHost(p->convertTo0to1(v));
    };
    set(id::delayMs, 2.5f);
    set(id::phaseRange, 1.0f);
    set(id::phase, 0.6f);
    set(id::polarity, 0.0f);

    const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile(
        juce::SystemStats::getEnvironmentVariable("PA_SNAPSHOT_DIR", "snapshots"));
    dir.createDirectory();
    proc.getUiState().setProperty(PhaseAlignProcessor::UiProps::uiScale, 1.0, nullptr);

    struct Shot
    {
        bool delayOn, phaseOn;
        float pixelScale;
        const char* name;
    };
    for (const auto& shot :
         {Shot{false, false, 1.0f, "dim_both_off_100.png"}, Shot{false, false, 2.0f, "dim_both_off_100_retina.png"},
          Shot{true, true, 2.0f, "dim_both_on_100_retina.png"}, Shot{false, true, 1.0f, "dim_delay_off_100.png"},
          Shot{true, false, 1.0f, "dim_phase_off_100.png"}})
    {
        set(id::delayOn, shot.delayOn ? 1.0f : 0.0f);
        set(id::phaseOn, shot.phaseOn ? 1.0f : 0.0f);
        std::unique_ptr<PhaseAlignEditor> editor(dynamic_cast<PhaseAlignEditor*>(proc.createEditor()));
        REQUIRE(editor != nullptr);
        const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, shot.pixelScale);
        const auto file = dir.getChildFile(shot.name);
        file.deleteFile();
        juce::FileOutputStream out(file);
        REQUIRE(out.openedOk());
        REQUIRE(juce::PNGImageFormat().writeImageToStream(image, out));
        WARN("wrote " << file.getFullPathName() << " (" << image.getWidth() << "x" << image.getHeight() << ")");
    }
}
