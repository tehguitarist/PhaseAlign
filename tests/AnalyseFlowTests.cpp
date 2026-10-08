#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ThreadAllocations.h"
#include "params/Parameters.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>

// ANALYSE end to end (plan R26): the processor's capture, the session's gating and triggers, applying an option, and
// the editor's button and screen. The search itself is tested against its Python spec in tests/dsp/AnalyseTests.cpp.
using namespace pa::params;
using Catch::Approx;
using pa::analyse::Session;
using UiProps = PhaseAlignProcessor::UiProps;

namespace
{
constexpr double fs = 48000.0;
constexpr int block = 512;

void setParam(PhaseAlignProcessor& p, const char* paramId, float value)
{
    auto* param = p.getValueTreeState().getParameter(paramId);
    param->setValueNotifyingHost(param->convertTo0to1(value));
}

float param(PhaseAlignProcessor& p, const char* paramId)
{
    auto* param = p.getValueTreeState().getParameter(paramId);
    return param->convertFrom0to1(param->getValue());
}

// A drum-like source (a decaying noise burst every 0.3 s over a quiet bed, so every 10 ms has signal), played on the
// sidechain; the track is the same, `lag` samples later and with its polarity inverted if `inverted`.
struct Player
{
    int lag = 30;
    bool inverted = true;
    float sidechainGain = 1.0f;
    bool sidechainIsTrack = false; // the sidechain carries the track's own input (the Logic case, plan R24)
    bool unrelated = false;        // the track is other bursts, from their own generator, at another tempo
    std::mt19937 rng{5}, other{17};
    long long otherCounter = 0;
    std::vector<float> history = std::vector<float>(4096, 0.0f);
    size_t h = 0;
    long long counter = 0;

    void play(PhaseAlignProcessor& proc, double seconds, Session* tickEach = nullptr)
    {
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        juce::MidiBuffer midi;
        for (int done = 0; done < (int)(seconds * fs); done += block)
        {
            juce::AudioBuffer<float> b(4, block);
            for (int i = 0; i < block; ++i)
            {
                const auto n = (double)(counter++ % 14400);
                const auto source = (float)(0.6 * std::exp(-n / 3000.0) + 0.02) * unit(rng);
                history[h] = source;
                const auto late = history[(h + history.size() - (size_t)lag) % history.size()];
                h = (h + 1) % history.size();
                auto track = inverted ? -late : late;
                if (unrelated)
                {
                    const auto m = (double)(otherCounter++ % 17280);
                    track = (float)(0.6 * std::exp(-m / 2000.0) + 0.02) * unit(other);
                }
                const auto sc = sidechainIsTrack ? track : source * sidechainGain;
                b.setSample(0, i, track);
                b.setSample(1, i, track);
                b.setSample(2, i, sc);
                b.setSample(3, i, sc);
            }
            proc.processBlock(b, midi);
            if (tickEach != nullptr)
                tickEach->tickForTesting();
        }
    }
};

struct PlayHead : juce::AudioPlayHead
{
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying(playing);
        return info;
    }
};
} // namespace

TEST_CASE("ANALYSE: a capture of a late, inverted track suggests the delay and polarity that undo it", "[analyse]")
{
    PhaseAlignProcessor proc;
    setParam(proc, id::delayOn, 1.0f);
    setParam(proc, id::phaseOn, 0.0f); // delay and polarity only: one answer
    proc.prepareToPlay(fs, block);
    auto& session = proc.getAnalyseSession();
    Player player;

    // Idle: nothing is captured.
    player.play(proc, 0.5);
    CHECK(session.getState() == Session::State::idle);

    session.start();
    REQUIRE(session.getState() == Session::State::capturing);
    player.play(proc, 11.0, &session);
    CHECK(session.capturedSeconds() == Approx(11.0).margin(0.1));
    CHECK(session.hasSidechain());
    CHECK(session.inputLevelDb() > -20.0f);

    session.analyseNow();
    CHECK(session.getState() == Session::State::analysing);
    session.waitForAnalysisForTesting();
    REQUIRE(session.getState() == Session::State::results);
    const auto* outcome = session.getOutcome();
    REQUIRE(outcome != nullptr);
    REQUIRE(session.numOptions() >= 1);
    const auto& best = outcome->result.options[0].candidate;
    CHECK(best.flip);
    CHECK(best.delayMs == Approx(-player.lag / fs * 1000.0).margin(0.005));
    CHECK(best.mode == pa::analyse::Mode::none);
    CHECK(outcome->result.shiftSamples == 0);
    CHECK(session.chosenNow() == -1); // the panel still has the settings from before

    // Choosing applies it; ORIGINAL goes back.
    session.choose(0);
    CHECK(param(proc, id::polarity) == 1.0f);
    CHECK(param(proc, id::delayMs) == Approx(best.delayMs).margin(1e-5));
    CHECK(param(proc, id::delayOn) == 1.0f);
    CHECK(param(proc, id::phaseOn) == 0.0f);
    CHECK(session.chosenNow() == 0);
    // The chain now undoes the offset: the delay as applied is the one scored, on the knob's own grid.
    CHECK(effectiveDelayMs(param(proc, id::delayMs), fs) == Approx(best.delayMs).margin(1e-9));
    session.choose(-1);
    CHECK(param(proc, id::polarity) == 0.0f);
    CHECK(param(proc, id::delayMs) == 0.0f);
    CHECK(session.chosenNow() == -1);
    // A knob moved by hand matches no row.
    setParam(proc, id::delayMs, 1.0f);
    CHECK(session.chosenNow() == -2);

    // AGAIN: a new capture; ORIGINAL is still the settings from before the first press.
    session.restart();
    CHECK(session.getState() == Session::State::capturing);
    CHECK(session.capturedSeconds() == 0.0);
    CHECK(session.getOriginal().delayMs == 0.0);

    session.stop();
    CHECK(session.getState() == Session::State::idle);
    CHECK(param(proc, id::delayMs) == 1.0f); // stopping leaves the panel as it is
}

TEST_CASE("ANALYSE: only stretches where both play are kept; too little audio waits for more", "[analyse]")
{
    PhaseAlignProcessor proc;
    proc.prepareToPlay(fs, block);
    auto& session = proc.getAnalyseSession();
    Player player;
    session.start();

    // A silent sidechain: nothing counts.
    player.sidechainGain = 0.0f;
    player.play(proc, 1.0, &session);
    CHECK(session.capturedSeconds() == 0.0);

    // 4 s of both: too short to analyse; the session says so and keeps it.
    player.sidechainGain = 1.0f;
    player.play(proc, 4.0, &session);
    CHECK(session.capturedSeconds() == Approx(4.0).margin(0.1));
    session.analyseNow();
    CHECK(session.getState() == Session::State::capturing);
    CHECK(session.isTooShort());
    player.play(proc, 6.5, &session);
    CHECK_FALSE(session.isTooShort());
    session.analyseNow();
    CHECK(session.getState() == Session::State::analysing);
    session.waitForAnalysisForTesting();
    CHECK(session.getState() == Session::State::results);
}

TEST_CASE("ANALYSE: the analysing bar only moves forward, and shows for at least its minimum time", "[analyse]")
{
    PhaseAlignProcessor proc;
    setParam(proc, id::delayOn, 1.0f);
    proc.prepareToPlay(fs, block);
    auto& session = proc.getAnalyseSession();
    Player player;
    session.start();
    player.play(proc, 11.0, &session);
    session.analyseNow();
    float last = 0.0f;
    int ticks = 0;
    while (session.getState() == Session::State::analysing && ticks++ < 1000)
    {
        const auto p = session.analysisProgress();
        CHECK(p >= last);
        CHECK(p <= 1.0f);
        last = p;
        juce::Thread::sleep(10);
        session.tickForTesting();
    }
    CHECK(session.getState() == Session::State::results);
    CHECK(last > 0.5f);
    CHECK(ticks * 10 >= (int)(Session::minAnalysingSeconds * 1000.0) - 50);
    CHECK(session.analysisProgress() == 1.0f);
}

TEST_CASE("ANALYSE: unrelated material gives nothing, or options flagged as no better than chance", "[analyse]")
{
    PhaseAlignProcessor proc;
    setParam(proc, id::delayOn, 1.0f);
    proc.prepareToPlay(fs, block);
    auto& session = proc.getAnalyseSession();
    Player player;
    player.unrelated = true;
    session.start();
    player.play(proc, 12.0, &session);
    session.analyseNow();
    session.waitForAnalysisForTesting();
    REQUIRE(session.getState() == Session::State::results);
    const auto& r = session.getOutcome()->result;
    INFO("verdict " << (int)r.verdict << ", options " << r.options.size() << ", chance gain " << r.chanceGain);
    CHECK((r.verdict == pa::analyse::Verdict::nothing || r.chanceLevel));
    CHECK(r.shiftSamples == 0);
}

TEST_CASE("ANALYSE: a sidechain that is a copy of the track counts as none", "[analyse]")
{
    PhaseAlignProcessor proc;
    proc.prepareToPlay(fs, block);
    auto& session = proc.getAnalyseSession();
    Player player;
    player.sidechainIsTrack = true;
    session.start();
    player.play(proc, 2.0, &session);
    CHECK_FALSE(session.hasSidechain());
    CHECK(session.capturedSeconds() == 0.0);
}

TEST_CASE("ANALYSE: the transport stopping analyses, and nothing is kept while it is stopped", "[analyse]")
{
    PhaseAlignProcessor proc;
    PlayHead head;
    proc.setPlayHead(&head);
    proc.prepareToPlay(fs, block);
    auto& session = proc.getAnalyseSession();
    Player player;
    session.start();

    head.playing = false;
    player.play(proc, 1.0, &session);
    CHECK(session.isWaitingForPlay());
    CHECK(session.capturedSeconds() == 0.0);

    // Stopping too early says so and keeps the audio for the next play.
    head.playing = true;
    player.play(proc, 2.0, &session);
    head.playing = false;
    player.play(proc, 0.1, &session);
    CHECK(session.getState() == Session::State::capturing);
    CHECK(session.isTooShort());
    head.playing = true;
    player.play(proc, 9.0, &session);
    CHECK(session.capturedSeconds() == Approx(11.0).margin(0.1));
    head.playing = false;
    player.play(proc, 0.1, &session);
    CHECK(session.getState() == Session::State::analysing);
    session.waitForAnalysisForTesting();
    CHECK(session.getState() == Session::State::results);
    proc.setPlayHead(nullptr);
}

TEST_CASE("ANALYSE: past 30 s the oldest audio goes; a new sample rate starts again", "[analyse]")
{
    PhaseAlignProcessor proc;
    proc.prepareToPlay(fs, block);
    auto& session = proc.getAnalyseSession();
    Player player;
    session.start();
    player.play(proc, 32.0, &session);
    CHECK(session.capturedSeconds() == Approx(Session::maxSeconds).margin(0.01));

    proc.prepareToPlay(96000.0, block);
    session.tickForTesting();
    CHECK(session.getState() == Session::State::capturing);
    CHECK(session.capturedSeconds() == 0.0);
    session.stop();
}

TEST_CASE("ANALYSE: stopping while the search runs cancels it", "[analyse]")
{
    PhaseAlignProcessor proc;
    proc.prepareToPlay(fs, block);
    auto& session = proc.getAnalyseSession();
    Player player;
    session.start();
    player.play(proc, 20.0, &session);
    session.analyseNow();
    REQUIRE(session.getState() == Session::State::analysing);
    session.stop(); // joins the cancelled search
    CHECK(session.getState() == Session::State::idle);
    CHECK(session.getOutcome() == nullptr);
}

TEST_CASE("ANALYSE: the audio thread doesn't allocate while capturing", "[analyse]")
{
    PhaseAlignProcessor proc;
    proc.prepareToPlay(fs, block);
    auto& session = proc.getAnalyseSession();
    session.start(); // allocates its buffers here, on the message thread
    juce::AudioBuffer<float> b(4, block);
    juce::MidiBuffer midi;
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> unit(-0.5f, 0.5f);
    long allocations = 0;
    for (int callback = 0; callback < 400; ++callback)
    {
        for (int ch = 0; ch < 4; ++ch)
            for (int i = 0; i < block; ++i)
                b.setSample(ch, i, unit(rng));
        const auto before = pa::test::threadAllocations();
        proc.processBlock(b, midi);
        allocations += pa::test::threadAllocations() - before;
        if (callback % 10 == 0)
            session.tickForTesting();
    }
    CHECK(allocations == 0);
}

TEST_CASE("ANALYSE: applying an option is one gesture over the parameters it changes", "[analyse]")
{
    PhaseAlignProcessor proc;
    proc.prepareToPlay(fs, block);
    struct Recorder : juce::AudioProcessorParameter::Listener
    {
        std::vector<std::string> events;
        void parameterValueChanged(int, float) override { events.push_back("value"); }
        void parameterGestureChanged(int, bool starting) override { events.push_back(starting ? "begin" : "end"); }
    } recorder;
    for (auto* p : proc.getParameters())
        p->addListener(&recorder);

    pa::analyse::PanelSettings s = proc.panelSettings();
    s.delayOn = true;
    s.delayMs = -1.5;
    s.polarity = true;
    s.phaseMode = (int)PhaseMode::constant;
    s.range180 = true;
    s.phase = 0.5;
    proc.applyPanelSettings(s);
    for (auto* p : proc.getParameters())
        p->removeListener(&recorder);

    // Six changed (phaseOn was already on): six begins, then six values, then six ends.
    REQUIRE(recorder.events.size() == 18);
    for (size_t i = 0; i < 18; ++i)
        CHECK(recorder.events[i] == (i < 6 ? "begin" : i < 12 ? "value" : "end"));
    CHECK(proc.panelSettings().sameAs(s));
}

TEST_CASE("ANALYSE: what an option sets on the panel", "[analyse]")
{
    using pa::analyse::Candidate;
    using pa::analyse::Mode;
    pa::analyse::PanelSettings base;
    base.delayOn = true;
    base.delayMs = 0.5;
    base.phaseOn = true;
    base.phaseMode = (int)PhaseMode::low;
    base.range180 = false;
    base.phase = 0.3;

    // Constant within 90 degrees keeps RANGE out (the finer knob); beyond it, RANGE in.
    Candidate c;
    c.mode = Mode::constant;
    c.wide = true;
    c.theta = 60.0;
    c.delayMs = -1.0;
    auto s = pa::analyse::settingsFor(c, base, {true, true});
    CHECK(s.phaseMode == (int)PhaseMode::constant);
    CHECK_FALSE(s.range180);
    CHECK(s.phase == Approx(60.0 / 90.0));
    CHECK(s.delayMs == -1.0);
    c.theta = 120.0;
    s = pa::analyse::settingsFor(c, base, {true, true});
    CHECK(s.range180);
    CHECK(s.phase == Approx(120.0 / 180.0));

    // HIGH in: the shape's own range; the knob position is the angle over 180.
    c.mode = Mode::hi;
    c.wide = true;
    c.theta = 135.0;
    s = pa::analyse::settingsFor(c, base, {true, true});
    CHECK(s.phaseMode == (int)PhaseMode::high);
    CHECK(s.range180);
    CHECK(s.phase == Approx(0.75));

    // "Phase off" switches PHASE off and leaves the phase knob, mode and range as they were.
    c.mode = Mode::none;
    c.flip = true;
    s = pa::analyse::settingsFor(c, base, {true, true});
    CHECK_FALSE(s.phaseOn);
    CHECK(s.phaseMode == base.phaseMode);
    CHECK(s.phase == base.phase);
    CHECK(s.polarity);

    // What wasn't searched isn't touched.
    c.mode = Mode::lo;
    c.theta = 45.0;
    s = pa::analyse::settingsFor(c, base, {false, false});
    CHECK(s.delayMs == base.delayMs);
    CHECK(s.phase == base.phase);
    CHECK(s.polarity);
}

//==============================================================================
namespace
{
std::unique_ptr<PhaseAlignEditor> makeEditor(PhaseAlignProcessor& proc)
{
    std::unique_ptr<PhaseAlignEditor> e(dynamic_cast<PhaseAlignEditor*>(proc.createEditor()));
    REQUIRE(e != nullptr);
    return e;
}

void click(juce::Component& c, juce::Point<float> pos)
{
    const auto now = juce::Time::getCurrentTime();
    const juce::MouseEvent down(juce::Desktop::getInstance().getMainMouseSource(), pos,
                                juce::ModifierKeys::leftButtonModifier, juce::MouseInputSource::defaultPressure,
                                juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &c, &c, now,
                                pos, now, 1, false);
    c.mouseDown(down);
    const juce::MouseEvent up(juce::Desktop::getInstance().getMainMouseSource(), pos, {},
                              juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                              juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                              juce::MouseInputSource::defaultTiltY, &c, &c, now, pos, now, 1, false);
    c.mouseUp(up);
}
} // namespace

TEST_CASE("ANALYSE in the editor: the button lights and swaps the screen; rows apply; the state outlives the editor",
          "[analyse]")
{
    PhaseAlignProcessor proc;
    setParam(proc, id::delayOn, 1.0f);
    proc.prepareToPlay(fs, block);
    auto& session = proc.getAnalyseSession();
    auto editor = makeEditor(proc);
    auto& button = editor->getAnalyseButton();
    auto& screen = editor->getAnalyseScreen();
    REQUIRE_FALSE(button.isLit());
    CHECK(editor->getMeterScreen().isVisible());
    CHECK_FALSE(screen.isVisible());

    click(button, button.getLocalBounds().getCentre().toFloat());
    CHECK(button.isLit());
    CHECK(session.getState() == Session::State::capturing);
    CHECK(screen.isVisible());
    CHECK_FALSE(editor->getMeterScreen().isVisible());
    CHECK(screen.tooltipAt(screen.analyseNowArea().getCentre()).contains("Analyse now"));

    Player player;
    player.play(proc, 11.0, &session);
    click(screen, screen.analyseNowArea().getCentre());
    session.waitForAnalysisForTesting();
    REQUIRE(session.getState() == Session::State::results);
    REQUIRE(session.numOptions() >= 1);
    CHECK(screen.rowText(0).contains(juce::String::fromUTF8("\xc3\x98"))); // the polarity flip
    CHECK(screen.rowText(0).contains("ms"));

    click(screen, screen.rowArea(0).getCentre());
    CHECK(param(proc, id::polarity) == 1.0f);
    CHECK(screen.previewRow() == 0);
    click(screen, screen.rowArea(-1).getCentre());
    CHECK(param(proc, id::polarity) == 0.0f);
    CHECK(screen.previewRow() == -1);

    // The preview toggle is kept in the ui state.
    const auto toggle = screen.previewToggleArea();
    click(screen, {toggle.getRight() - 4.0f, toggle.getCentreY()});
    CHECK(proc.getUiState()[UiProps::analysePreview].toString() == "alignment");
    CHECK(screen.getPreview() == pa::ui::AnalyseScreen::Preview::alignment);

    // Closing and opening the editor keeps the results on screen.
    editor.reset();
    editor = makeEditor(proc);
    CHECK(editor->getAnalyseButton().isLit());
    CHECK(editor->getAnalyseScreen().isVisible());
    CHECK(editor->getAnalyseScreen().getPreview() == pa::ui::AnalyseScreen::Preview::alignment);

    // Pressing ANALYSE again goes back to the meter.
    click(editor->getAnalyseButton(), editor->getAnalyseButton().getLocalBounds().getCentre().toFloat());
    CHECK(session.getState() == Session::State::idle);
    CHECK_FALSE(editor->getAnalyseButton().isLit());
    CHECK(editor->getMeterScreen().isVisible());
    CHECK_FALSE(editor->getAnalyseScreen().isVisible());
}

// Hidden: the ANALYSE screen's states as PNGs in PA_SNAPSHOT_DIR (default ./snapshots), at 100% and 2x pixels.
TEST_CASE("ANALYSE snapshots", "[.][snapshot]")
{
    const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile(
        juce::SystemStats::getEnvironmentVariable("PA_SNAPSHOT_DIR", "snapshots"));
    dir.createDirectory();
    PhaseAlignProcessor proc;
    setParam(proc, id::delayOn, 1.0f);
    proc.prepareToPlay(fs, block);
    proc.getUiState().setProperty(UiProps::uiScale, 1.0, nullptr);
    auto& session = proc.getAnalyseSession();
    auto editor = makeEditor(proc);
    const auto snapshot = [&](const char* name)
    {
        const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 2.0f);
        const auto file = dir.getChildFile(name);
        file.deleteFile();
        juce::FileOutputStream out(file);
        REQUIRE(juce::PNGImageFormat().writeImageToStream(image, out));
        WARN("wrote " << file.getFullPathName());
    };

    Player player;
    player.lag = 70;
    session.start();
    player.play(proc, 6.4, &session);
    snapshot("analyse_capturing.png");
    session.analyseNow();
    snapshot("analyse_too_short.png");
    player.play(proc, 8.0, &session);
    snapshot("analyse_capturing_enough.png"); // past the minimum: the text asks for more playing
    session.analyseNow();
    snapshot("analyse_analysing.png");
    juce::Thread::sleep(400); // part way: the bar filling, a dot or two
    snapshot("analyse_analysing_part_way.png");
    session.waitForAnalysisForTesting();
    snapshot("analyse_results_bands.png");
    session.choose(0);
    editor->getAnalyseScreen().setPreview(pa::ui::AnalyseScreen::Preview::alignment);
    snapshot("analyse_results_alignment.png");

    // A shift beyond the knob's reach (8 ms) and PHASE searched too.
    session.stop();
    setParam(proc, id::polarity, 0.0f);
    setParam(proc, id::delayMs, 0.0f);
    Player far;
    far.lag = 384;
    far.inverted = false;
    session.start();
    far.play(proc, 11.0, &session);
    session.analyseNow();
    session.waitForAnalysisForTesting();
    editor->getAnalyseScreen().setPreview(pa::ui::AnalyseScreen::Preview::bands);
    snapshot("analyse_results_shift.png");
    session.stop();

    // Unrelated material.
    Player other;
    other.unrelated = true;
    session.start();
    other.play(proc, 12.0, &session);
    session.analyseNow();
    session.waitForAnalysisForTesting();
    snapshot("analyse_results_unrelated.png");
    session.stop();
}
