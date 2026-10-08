#include "ui/AnalyseScreen.h"

#include "dsp/PhaseResponse.h"

#include <cmath>

namespace pa::ui
{
namespace
{
using analyse::Session;

// Design-space layout of the screen (meterInterior is x 122 to 1527, y 614 to 1104).
constexpr float textLeft = 160.0f, textRight = 1490.0f;
constexpr float headerBaseline = 664.0f;
constexpr float rowsLeft = 160.0f, rowsRight = 820.0f, firstRowBaseline = 742.0f, rowPitch = 58.0f;
constexpr float previewLeft = 920.0f, previewRight = 1490.0f, previewTop = 700.0f, previewBottom = 1030.0f;
constexpr float bottomRowY = 1075.0f, bottomRowHeight = 26.0f;

const juce::String deg = juce::String::fromUTF8("\xc2\xb0");
const juce::String polaritySign = juce::String::fromUTF8("\xc3\x98");

meter::HitCapture::Settings hitSettings(const analyse::Candidate& c, double fs)
{
    meter::HitCapture::Settings s;
    s.delayMs = c.delayMs;
    s.inverted = c.flip;
    if (c.mode != analyse::Mode::none)
        s.phase = [response = dsp::PhaseStageResponse(c.toSettings(fs), fs)](double hz) { return response(hz); };
    return s;
}

juce::String signedValue(double v, int decimals)
{
    return (v >= 0.0 ? "+" : juce::String::fromUTF8("\xe2\x88\x92")) + juce::String(std::abs(v), decimals);
}
} // namespace

AnalyseScreen::AnalyseScreen(const SourceAssets& assetsIn, analyse::Session& s)
    : DesignComponent(design::meterInterior), assets(assetsIn), session(s)
{
    setOpaque(true);
}

void AnalyseScreen::setDelayUnit(params::DelayUnit u)
{
    if (u == delayUnit)
        return;
    delayUnit = u;
    repaint();
}

void AnalyseScreen::setPreview(Preview p)
{
    if (p == preview)
        return;
    preview = p;
    repaint();
}

void AnalyseScreen::settingsChanged()
{
    if (session.getState() != Session::State::idle)
        repaint();
}

void AnalyseScreen::sessionChanged()
{
    if (session.getOutcome() != seenOutcome)
    {
        seenOutcome = session.getOutcome();
        lastClicked = session.numOptions() > 0 ? 0 : -1; // a new result: show its first option
    }
    repaint();
}

int AnalyseScreen::previewRow() const
{
    return juce::jlimit(-1, session.numOptions() - 1, lastClicked);
}

void AnalyseScreen::updatePreview()
{
    const auto* outcome = session.getOutcome();
    const auto row = previewRow();
    if (outcome == previewOutcome && row == previewFor)
        return;
    previewOutcome = outcome;
    previewFor = row;
    if (outcome == nullptr)
        return;

    const auto fs = outcome->sampleRate;
    const auto before = session.getOriginal().candidate(fs);
    const auto after = session.optionSettings(row).candidate(fs);
    // The options after a manual shift are for the track once shifted by hand; the others and ORIGINAL are as it is.
    const auto shifted = row >= 0 && session.afterShift(row);
    const auto& afterSpectra = shifted ? outcome->shiftedSpectra : outcome->spectra;
    for (int b = 0; b < numBands; ++b)
    {
        bandsBefore[(size_t)b] = analyse::bandCorrelation(outcome->spectra, before, bandEdgesHz[b], bandEdgesHz[b + 1]);
        bandsAfter[(size_t)b] = analyse::bandCorrelation(afterSpectra, after, bandEdgesHz[b], bandEdgesHz[b + 1]);
    }
    hitBefore.captureFrom(outcome->hitInput, outcome->hitSidechain, outcome->hitOnset, fs);
    hitBefore.render(hitSettings(before, fs));
    hitAfter.captureFrom(shifted ? outcome->hitShiftedInput : outcome->hitInput, outcome->hitSidechain,
                         outcome->hitOnset, fs);
    hitAfter.render(hitSettings(after, fs));
}

//==============================================================================
juce::String AnalyseScreen::scopeText(analyse::Scope scope) const
{
    if (scope.delayOn && scope.phaseOn)
        return "DELAY, PHASE AND POLARITY";
    if (scope.delayOn)
        return "DELAY AND POLARITY (PHASE IS OFF)";
    if (scope.phaseOn)
        return "PHASE AND POLARITY (DELAY IS OFF)";
    return "POLARITY ONLY (DELAY AND PHASE ARE OFF)";
}

juce::String AnalyseScreen::phaseText(const analyse::PanelSettings& s) const
{
    if (! s.phaseOn)
        return "PHASE OFF";
    const auto mode = (params::PhaseMode)s.phaseMode;
    const auto shown = params::shownRangeDegrees(s.range180, mode);
    const auto star = params::shownRangeIsApproximate(s.range180, mode) ? "*" : "";
    const auto angle = params::formatPhase(s.phase * shown) + deg + star;
    if (mode == params::PhaseMode::constant)
        return "CONSTANT " + angle;
    return juce::String(mode == params::PhaseMode::high ? "HIGH " : "LOW ") + (s.range180 ? "in " : "out ") + angle;
}

juce::String AnalyseScreen::delayText(double ms) const
{
    const auto fs = session.getOutcome() != nullptr ? session.getOutcome()->sampleRate : 48000.0;
    auto text = params::formatDelay(ms, delayUnit, fs);
    if (! text.startsWithChar('-'))
        text = "+" + text;
    return text + " " + params::unitSuffix(delayUnit);
}

juce::String AnalyseScreen::rowText(int row) const
{
    const auto* outcome = session.getOutcome();
    if (outcome == nullptr)
        return {};
    if (row < 0)
        return "ORIGINAL";
    const auto& option = session.optionAt(row);
    const auto s = session.optionSettings(row);
    juce::StringArray parts;
    if (outcome->scope.phaseOn)
        parts.add(phaseText(s));
    if (s.polarity)
        parts.add(polaritySign);
    if (outcome->scope.delayOn)
        parts.add(delayText(option.candidate.delayMs));
    if (! outcome->scope.phaseOn && ! s.polarity && ! outcome->scope.delayOn)
        parts.add("polarity as is");
    return parts.joinIntoString("  ");
}

//==============================================================================
juce::Rectangle<float> AnalyseScreen::rowArea(int row) const
{
    const auto index = row < 0 ? session.numOptions() : row;
    const auto baseline = firstRowBaseline + rowPitch * (float)index;
    return designRect(rowsLeft - 18.0f, baseline - 38.0f, rowsRight - rowsLeft + 26.0f, rowPitch - 4.0f);
}

juce::Rectangle<float> AnalyseScreen::againArea() const
{
    const auto s = getScale();
    const auto font = meterFont(assets, 13.0f * s).withExtraKerningFactor(0.2f);
    const auto text = session.getState() == Session::State::results ? "AGAIN" : "CLEAR";
    return juce::Rectangle<float>(lx(textLeft) - 14.0f * s, ly(bottomRowY), textWidth(font, text) + 28.0f * s,
                                  bottomRowHeight * s);
}

juce::Rectangle<float> AnalyseScreen::analyseNowArea() const
{
    const auto s = getScale();
    const auto font = meterFont(assets, 13.0f * s).withExtraKerningFactor(0.2f);
    const auto w = textWidth(font, "ANALYSE NOW") + 28.0f * s;
    return juce::Rectangle<float>(lx(textRight) + 14.0f * s - w, ly(bottomRowY), w, bottomRowHeight * s);
}

juce::Rectangle<float> AnalyseScreen::previewToggleArea() const
{
    const auto s = getScale();
    const auto font = meterFont(assets, 13.0f * s).withExtraKerningFactor(0.2f);
    const auto w = textWidth(font, "BANDS  ALIGNMENT") + 28.0f * s;
    const auto centre = lx(0.5f * (previewLeft + previewRight));
    return juce::Rectangle<float>(centre - 0.5f * w, ly(bottomRowY), w, bottomRowHeight * s);
}

void AnalyseScreen::mouseDown(const juce::MouseEvent& e)
{
    const auto p = e.position;
    const auto state = session.getState();
    if (state == Session::State::capturing)
    {
        if (againArea().contains(p))
            session.restart();
        else if (analyseNowArea().contains(p))
            session.analyseNow();
        return;
    }
    if (state != Session::State::results)
        return;
    if (againArea().contains(p))
    {
        session.restart();
        return;
    }
    if (previewToggleArea().contains(p))
    {
        const auto next = p.x < previewToggleArea().getCentreX() ? Preview::bands : Preview::alignment;
        onPreviewSelected ? onPreviewSelected(next) : setPreview(next);
        return;
    }
    for (int row = -1; row < session.numOptions(); ++row)
        if (rowArea(row).contains(p))
        {
            lastClicked = row;
            session.choose(row);
            repaint();
            return;
        }
}

void AnalyseScreen::mouseMove(const juce::MouseEvent& e)
{
    mouse = e.position;
    repaint();
}

void AnalyseScreen::mouseExit(const juce::MouseEvent&)
{
    mouse = {-1.0f, -1.0f};
    repaint();
}

juce::String AnalyseScreen::tooltipAt(juce::Point<float> p) const
{
    const auto state = session.getState();
    if (state == Session::State::capturing)
    {
        if (againArea().contains(p))
            return "Clear: drop what has been captured and start again.";
        if (analyseNowArea().contains(p))
            return "Analyse now, without stopping the transport (for hosts that don't report one). Needs at least " +
                   juce::String((int)Session::minSeconds) + " s of audio.";
        return "ANALYSE is listening: play a section where this track and the sidechain play together, then stop. Only "
               "the stretches where both have signal count. Press ANALYSE again to go back to the meter.";
    }
    if (state == Session::State::results)
    {
        if (againArea().contains(p))
            return "Again: a new capture (your settings from before ANALYSE stay as ORIGINAL).";
        if (previewToggleArea().contains(p))
            return "What the preview shows: the six bands' correlation with the sidechain, or the strongest captured "
                   "hit "
                   "as waveforms. Blue is ORIGINAL, green the row chosen.";
        for (int row = -1; row < session.numOptions(); ++row)
            if (rowArea(row).contains(p))
                return row < 0 ? "ORIGINAL: the settings you had before pressing ANALYSE. Click to go back to them."
                               : "Click to apply this option; play to hear it. r is how well this track then matches "
                                 "the sidechain over the capture (+1 is identical), the second number the change from "
                                 "ORIGINAL.";
        return "ANALYSE's options for the captured audio. The settings are a suggestion: choose by ear. Press ANALYSE "
               "again to go back to the meter (what is applied stays).";
    }
    return "ANALYSE";
}

//==============================================================================
void AnalyseScreen::paint(juce::Graphics& g)
{
    updatePreview();
    g.fillAll(design::screenBlack);
    switch (session.getState())
    {
    case Session::State::idle:
        break;
    case Session::State::capturing:
        paintCapturing(g);
        break;
    case Session::State::analysing:
    {
        paintHeader(g, juce::String(session.capturedSeconds(), 1) + " s CAPTURED");
        paintAnalysing(g);
        break;
    }
    case Session::State::results:
        paintResults(g);
        break;
    }
}

void AnalyseScreen::paintAnalysing(juce::Graphics& g) const
{
    // ANALYSING with its dots coming one at a time (the word held where it sits with all three, so it doesn't move),
    // and the search's progress as a bar.
    const auto s = getScale();
    const auto font = meterFont(assets, 20.0f * s, true).withExtraKerningFactor(0.1f);
    const auto dots = (int)(session.analysingSeconds() / dotSeconds) % 4;
    const auto left = 0.5f * (float)getWidth() - 0.5f * textWidth(font, "ANALYSING...");
    const auto baseline = ly(design::meterZeroY) + 30.0f * s; // the block centred under the header
    g.setColour(design::phosphor);
    drawTextAt(g, font, "ANALYSING" + juce::String::repeatedString(".", dots), left, baseline,
               juce::Justification::left);

    const auto bar =
        juce::Rectangle<float>(560.0f * s, 22.0f * s).withCentre({0.5f * (float)getWidth(), baseline + 40.0f * s});
    g.setColour(design::meterGrid);
    g.drawRect(bar, juce::jmax(1.0f, 1.2f * s));
    const auto inner = bar.reduced(3.0f * s);
    g.setColour(design::phosphor.withAlpha(0.9f));
    g.fillRect(inner.withWidth(inner.getWidth() * juce::jlimit(0.0f, 1.0f, session.analysisProgress())));
}

void AnalyseScreen::paintHeader(juce::Graphics& g, const juce::String& right) const
{
    const auto s = getScale();
    const auto bold = meterFont(assets, 16.0f * s, true).withExtraKerningFactor(0.12f);
    const auto font = meterFont(assets, 13.0f * s).withExtraKerningFactor(0.1f);
    const auto* outcome = session.getOutcome();
    const auto scope = outcome != nullptr ? outcome->scope : session.currentScope();
    g.setColour(design::phosphor);
    drawTextAt(g, bold, "ANALYSE", lx(textLeft), ly(headerBaseline), juce::Justification::left);
    g.setColour(design::meterAxisText);
    drawTextAt(g, font, "SEARCHING " + scopeText(scope), lx(textLeft) + textWidth(bold, "ANALYSE") + 24.0f * s,
               ly(headerBaseline), juce::Justification::left);
    g.setColour(design::meterAxisText.withAlpha(0.7f));
    drawTextAt(g, font, right, lx(textRight), ly(headerBaseline), juce::Justification::right);
    g.setColour(design::meterGrid);
    g.fillRect(juce::Rectangle<float>(lx(textLeft), ly(headerBaseline + 16.0f), lx(textRight) - lx(textLeft),
                                      juce::jmax(1.0f, 1.2f * s)));
}

void AnalyseScreen::paintLabelButton(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text,
                                     bool enabled, bool hot) const
{
    const auto s = getScale();
    const auto font = meterFont(assets, 13.0f * s).withExtraKerningFactor(0.2f);
    const auto colour = enabled ? design::phosphor : design::meterAxisText.withAlpha(0.3f);
    g.setColour(colour.withAlpha(enabled ? (hot ? 0.9f : 0.5f) : 0.25f));
    g.drawRect(area, juce::jmax(1.0f, 1.2f * s));
    g.setColour(colour);
    drawTextAt(g, font, text, area.getCentreX(), area.getY() + 19.0f * s, juce::Justification::horizontallyCentred);
}

void AnalyseScreen::paintMessage(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text,
                                 juce::Colour colour) const
{
    g.setColour(colour);
    g.setFont(meterFont(assets, 20.0f * getScale())); // the screen has the room: easy to read (user, 2026-10-08)
    g.drawFittedText(text, area.toNearestInt(), juce::Justification::topLeft, 3, 1.0f);
}

void AnalyseScreen::paintCapturing(juce::Graphics& g) const
{
    const auto s = getScale();
    const auto seconds = session.capturedSeconds();
    paintHeader(g, juce::String(seconds, 1) + " s CAPTURED");
    const auto font = meterFont(assets, 20.0f * s);
    const auto small = meterFont(assets, 15.0f * s).withExtraKerningFactor(0.1f);

    // What to do now.
    juce::String instruction, status;
    auto statusColour = design::meterAxisText.withAlpha(0.8f);
    const auto minimum = juce::String((int)Session::minSeconds), ideal = juce::String((int)Session::idealSeconds);
    instruction = "Press play: ANALYSE needs at least " + minimum +
                  " s of this track and the sidechain playing together (" + ideal +
                  " s is best). Choose a section typical of the song.";
    if (! session.hasSidechain())
    {
        status = "No sidechain: route the reference track (the other mic) to the sidechain input.";
        statusColour = design::meterWarning;
    }
    else if (session.isTooShort())
    {
        status = "Only " + juce::String(seconds, 1) + " s so far: play some more, then stop again.";
        statusColour = design::meterWarning;
    }
    else if (session.isWaitingForPlay())
        status = seconds >= Session::minSeconds ? "Press play and keep going for better results, or ANALYSE NOW."
                                                : "Waiting for playback.";
    else if (seconds >= Session::idealSeconds)
        status = "Plenty captured: stop playback (or ANALYSE NOW) to analyse.";
    else if (seconds >= Session::minSeconds)
        status = "Keep playing for better results (" + ideal + " s is best), then stop or ANALYSE NOW.";
    else
        status = "Listening. Only stretches where both play count.";
    paintMessage(g, designRect(textLeft, 690.0f, textRight - textLeft, 96.0f), instruction,
                 design::meterAxisText.withAlpha(0.9f));
    g.setColour(statusColour);
    drawTextAt(g, font, status, lx(textLeft), ly(826.0f), juce::Justification::left);

    // Progress: 0 to idealSeconds, marked at the minimum.
    const auto bar = juce::Rectangle<float>::leftTopRightBottom(lx(textLeft), ly(852.0f), lx(textRight), ly(888.0f));
    g.setColour(design::meterGrid);
    g.drawRect(bar, juce::jmax(1.0f, 1.2f * s));
    const auto fraction = (float)juce::jlimit(0.0, 1.0, seconds / Session::idealSeconds);
    g.setColour(design::phosphor.withAlpha(seconds >= Session::minSeconds ? 0.9f : 0.45f));
    g.fillRect(bar.reduced(3.0f * s).withWidth((bar.getWidth() - 6.0f * s) * fraction));
    const auto minX = bar.getX() + bar.getWidth() * (float)(Session::minSeconds / Session::idealSeconds);
    g.setColour(design::meterAxisText.withAlpha(0.8f));
    g.fillRect(juce::Rectangle<float>(minX - 0.6f * s, bar.getY() - 6.0f * s, juce::jmax(1.0f, 1.2f * s),
                                      bar.getHeight() + 12.0f * s));
    drawTextAt(g, small, minimum + " s MINIMUM", minX, bar.getBottom() + 28.0f * s,
               juce::Justification::horizontallyCentred);
    drawTextAt(g, small, ideal + " s", bar.getRight(), bar.getBottom() + 28.0f * s, juce::Justification::right);
    if (seconds >= Session::maxSeconds - 0.05)
        drawTextAt(g, small, "KEEPS THE LAST " + juce::String((int)Session::maxSeconds) + " s", bar.getX(),
                   bar.getBottom() + 28.0f * s, juce::Justification::left);

    // Levels: is anything arriving?
    const auto level = [&](float baseline, const juce::String& name, float db, juce::Colour colour)
    {
        g.setColour(colour);
        drawTextAt(g, small, name, lx(textLeft), ly(baseline), juce::Justification::left);
        const auto meter = juce::Rectangle<float>::leftTopRightBottom(lx(textLeft + 170.0f), ly(baseline - 14.0f),
                                                                      lx(textLeft + 700.0f), ly(baseline));
        g.setColour(design::meterGrid.withAlpha(0.6f));
        g.drawRect(meter, juce::jmax(1.0f, 1.0f * s));
        const auto f = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
        g.setColour(colour.withAlpha(0.85f));
        g.fillRect(meter.reduced(2.0f * s).withWidth((meter.getWidth() - 4.0f * s) * f));
    };
    level(950.0f, "TRACK", session.inputLevelDb(), design::meterInput);
    level(990.0f, "SIDECHAIN", session.sidechainLevelDb(), design::meterAxisText);

    paintLabelButton(g, againArea(), "CLEAR", session.capturedSeconds() > 0.0, againArea().contains(mouse));
    paintLabelButton(g, analyseNowArea(), "ANALYSE NOW", seconds >= Session::minSeconds,
                     analyseNowArea().contains(mouse));
}

void AnalyseScreen::paintResults(juce::Graphics& g) const
{
    const auto* outcome = session.getOutcome();
    if (outcome == nullptr)
        return;
    const auto s = getScale();
    const auto& result = outcome->result;
    paintHeader(g, juce::String(outcome->seconds, 1) + " s ANALYSED");

    const auto rowFont = meterFont(assets, 15.0f * s, true).withExtraKerningFactor(0.05f);
    const auto font = meterFont(assets, 13.0f * s);
    const auto small = meterFont(assets, 12.0f * s).withExtraKerningFactor(0.08f);
    const auto chosen = session.chosenNow();
    const auto shown = previewRow();

    for (int row = -1; row < session.numOptions(); ++row)
    {
        const auto area = rowArea(row);
        const auto baseline = area.getY() + 38.0f * s;
        if (row == shown)
        {
            g.setColour(design::phosphor.withAlpha(0.08f));
            g.fillRect(area);
        }
        if (area.contains(mouse))
        {
            g.setColour(design::phosphor.withAlpha(0.35f));
            g.drawRect(area, juce::jmax(1.0f, 1.0f * s));
        }
        const auto isChosen = row == chosen;
        const auto colour = row < 0 ? design::meterInput : design::phosphor;
        g.setColour(colour.withAlpha(isChosen ? 1.0f : 0.75f));
        if (isChosen) // the panel has this row's settings
            drawTextAt(g, rowFont, juce::String::fromUTF8("\xe2\x96\xb6"), lx(rowsLeft) - 4.0f * s, baseline,
                       juce::Justification::left);
        const auto label = row < 0 ? juce::String("ORIGINAL") : juce::String(row + 1);
        drawTextAt(g, rowFont, label, lx(rowsLeft) + 24.0f * s, baseline, juce::Justification::left);
        if (row >= 0)
            drawTextAt(g, rowFont, rowText(row), lx(rowsLeft) + 60.0f * s, baseline, juce::Justification::left);

        // The score: r, and the change from ORIGINAL.
        const auto score = row < 0 ? outcome->originalScore : session.optionAt(row).candidate.score;
        auto scoreText = "r " + juce::String(score, 2);
        if (row >= 0)
            scoreText += "  " + signedValue(score - outcome->originalScore, 2);
        g.setColour(design::meterAxisText.withAlpha(0.85f));
        drawTextAt(g, font, scoreText, lx(rowsRight), baseline, juce::Justification::right);

        // Flags under the setting.
        if (row >= 0)
        {
            juce::StringArray flags;
            const auto& option = session.optionAt(row);
            if (session.afterShift(row))
                flags.add("AFTER THE SHIFT");
            if (option.small)
                flags.add("SMALL IMPROVEMENT");
            if (option.delayOnly)
                flags.add("DELAY ONLY");
            if (option.lowEndChange < 0.0)
                flags.add("LESS LOW END");
            if (! flags.isEmpty())
            {
                g.setColour(design::meterWarning);
                drawTextAt(g, small, flags.joinIntoString("   "), lx(rowsLeft) + 60.0f * s, baseline + 17.0f * s,
                           juce::Justification::left);
            }
        }
    }

    // What the search says, under the rows: information in grey, what needs a look in amber.
    juce::String note, warning;
    switch (result.verdict)
    {
    case analyse::Verdict::nothing:
        note = "Nothing worth changing: no setting lines this track up clearly better with the sidechain.";
        break;
    case analyse::Verdict::noSignal:
        note = "Not enough signal in both to compare. Press AGAIN and play a section where both play.";
        break;
    case analyse::Verdict::close:
        note = "Two close options: choose by ear.";
        break;
    case analyse::Verdict::delayOnly:
        note = "The waveforms barely match, but the transients sit apart: a delay alone.";
        break;
    case analyse::Verdict::suggest:
        break;
    }
    if (result.shiftSamples != 0 && result.verdict == analyse::Verdict::nothing)
        note.clear(); // the shift is the change
    if (result.shiftSamples != 0)
    {
        const auto asIs = ! result.optionsAsIs.empty(), after = ! result.options.empty();
        warning = juce::String(result.message) + " (positive moves this track later). " +
                  juce::String(! after ? "After that shift nothing else needs changing. " : "") +
                  (asIs && after ? "Rows marked AFTER THE SHIFT are for after it; the others work as it is."
                   : asIs        ? "The option shown works as it is."
                   : after       ? "The options are for after that shift. As it is, no setting helps."
                                 : "As it is, no setting helps.");
    }
    else if (result.message.size() > 0)
        note = note.isEmpty() ? juce::String(result.message) : note + " " + juce::String(result.message) + ".";
    // The two match warnings (user, 2026-10-08): the options stay; this says how far to trust them.
    if (result.chanceLevel)
        warning += juce::String(warning.isEmpty() ? "" : " ") +
                   "The audio seems unrelated: this match is no better than chance. Check the sidechain is the right "
                   "track, or play a longer section; the options are there to try by ear.";
    else if (result.weakMatch)
        warning += juce::String(warning.isEmpty() ? "" : " ") +
                   "A weak match: these tracks share little (different instruments playing the same part, say). "
                   "Choose by ear.";
    const auto noteTop = firstRowBaseline + rowPitch * (float)(session.numOptions() + 1) - 24.0f;
    {
        juce::AttributedString text;
        const auto noteFont = meterFont(assets, 17.0f * s);
        if (note.isNotEmpty())
            text.append(note + (warning.isNotEmpty() ? " " : ""), noteFont, design::meterAxisText.withAlpha(0.85f));
        if (warning.isNotEmpty())
            text.append(warning, noteFont, design::meterWarning);
        text.setWordWrap(juce::AttributedString::byWord);
        const auto box = designRect(rowsLeft, noteTop, rowsRight - rowsLeft, previewBottom + 40.0f - noteTop);
        juce::TextLayout layout;
        layout.createLayout(text, box.getWidth());
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(box.toNearestInt());
        layout.draw(g, box);
    }

    // The preview of the chosen row against ORIGINAL.
    const auto area = juce::Rectangle<float>::leftTopRightBottom(lx(previewLeft), ly(previewTop), lx(previewRight),
                                                                 ly(previewBottom));
    g.setColour(design::meterGrid);
    g.fillRect(juce::Rectangle<float>(lx(0.5f * (rowsRight + previewLeft) - 20.0f), area.getY(),
                                      juce::jmax(1.0f, 1.2f * s), area.getHeight()));
    if (preview == Preview::bands)
        paintBandsPreview(g, area);
    else
        paintAlignmentPreview(g, area);

    paintLabelButton(g, againArea(), "AGAIN", true, againArea().contains(mouse));
    // The preview toggle: both names, the current one lit.
    const auto toggle = previewToggleArea();
    const auto toggleFont = meterFont(assets, 13.0f * s).withExtraKerningFactor(0.2f);
    g.setColour(design::phosphor.withAlpha(toggle.contains(mouse) ? 0.9f : 0.5f));
    g.drawRect(toggle, juce::jmax(1.0f, 1.2f * s));
    const auto bandsWidth = textWidth(toggleFont, "BANDS");
    const auto x0 = toggle.getX() + 14.0f * s;
    g.setColour(preview == Preview::bands ? design::phosphor : design::meterAxisText.withAlpha(0.35f));
    drawTextAt(g, toggleFont, "BANDS", x0, toggle.getY() + 19.0f * s, juce::Justification::left);
    g.setColour(preview == Preview::alignment ? design::phosphor : design::meterAxisText.withAlpha(0.35f));
    drawTextAt(g, toggleFont, "ALIGNMENT", x0 + bandsWidth + textWidth(toggleFont, "  "), toggle.getY() + 19.0f * s,
               juce::Justification::left);
}

void AnalyseScreen::paintBandsPreview(juce::Graphics& g, juce::Rectangle<float> area) const
{
    // Like BANDS: per band, the row as a bar from zero and ORIGINAL as a tick. Over the whole capture.
    const auto s = getScale();
    const auto small = meterFont(assets, 11.0f * s).withExtraKerningFactor(0.05f);
    const auto plot = area.withTrimmedTop(46.0f * s).withTrimmedBottom(28.0f * s);
    const auto yFor = [&](double r)
    { return plot.getCentreY() - (float)juce::jlimit(-1.05, 1.05, r) * 0.5f * plot.getHeight(); };
    g.setColour(design::meterGrid);
    g.fillRect(juce::Rectangle<float>(plot.getX(), yFor(0.0), plot.getWidth(), juce::jmax(1.0f, 1.2f * s)));
    for (const auto r : {1.0, -1.0})
        for (float x = plot.getX(); x < plot.getRight(); x += 10.0f * s)
            g.fillRect(juce::Rectangle<float>(x, yFor(r), 3.0f * s, juce::jmax(1.0f, 1.0f * s)));
    g.setColour(design::meterAxisText.withAlpha(0.7f));
    drawTextAt(g, small, "+1", plot.getX() - 6.0f * s, yFor(1.0) + 4.0f * s, juce::Justification::right);
    drawTextAt(g, small, "-1", plot.getX() - 6.0f * s, yFor(-1.0) + 4.0f * s, juce::Justification::right);

    // The legend: which row is green.
    const auto row = previewRow();
    const auto legend = meterFont(assets, 12.0f * s).withExtraKerningFactor(0.1f);
    g.setColour(design::phosphor);
    drawTextAt(g, legend, row < 0 ? juce::String("ORIGINAL") : "OPTION " + juce::String(row + 1), area.getRight(),
               area.getY() + 12.0f * s, juce::Justification::right);
    if (row >= 0)
    {
        g.setColour(design::meterInput);
        drawTextAt(g, legend, "ORIGINAL", area.getRight() - textWidth(legend, "OPTION 1") - 20.0f * s,
                   area.getY() + 12.0f * s, juce::Justification::right);
    }

    const auto slot = plot.getWidth() / (float)numBands;
    for (int b = 0; b < numBands; ++b)
    {
        const auto centre = plot.getX() + slot * ((float)b + 0.5f);
        const auto after = bandsAfter[(size_t)b], before = bandsBefore[(size_t)b];
        if (! std::isnan(after))
        {
            const auto y0 = yFor(0.0), y1 = yFor(after);
            g.setColour(design::phosphor.withAlpha(0.85f));
            g.fillRect(juce::Rectangle<float>::leftTopRightBottom(centre - 0.2f * slot, std::min(y0, y1),
                                                                  centre + 0.2f * slot, std::max(y0, y1)));
            // The value past the bar's end, or inside it near +-1, on a black patch.
            const auto inside = std::abs(after) > 0.8;
            const auto textY =
                after >= 0.0 ? (inside ? y1 + 16.0f * s : y1 - 5.0f * s) : (inside ? y1 - 5.0f * s : y1 + 14.0f * s);
            const juce::String text(after, 2);
            g.setColour(design::screenBlack.withAlpha(0.85f));
            g.fillRect(juce::Rectangle<float>(textWidth(small, text) + 8.0f * s, 15.0f * s)
                           .withCentre({centre, textY - 4.5f * s}));
            g.setColour(design::phosphor);
            drawTextAt(g, small, text, centre, textY, juce::Justification::horizontallyCentred);
        }
        if (! std::isnan(before))
        {
            g.setColour(design::meterInput.withAlpha(0.95f));
            g.fillRect(
                juce::Rectangle<float>(0.64f * slot, juce::jmax(2.0f, 3.0f * s)).withCentre({centre, yFor(before)}));
        }
        const auto hz = [](double v)
        { return v >= 1000.0 ? juce::String(juce::roundToInt(v / 1000.0)) + "k" : juce::String(juce::roundToInt(v)); };
        g.setColour(design::meterAxisText.withAlpha(0.8f));
        drawTextAt(g, small, hz(bandEdgesHz[b]) + "-" + hz(bandEdgesHz[b + 1]), centre, area.getBottom() - 4.0f * s,
                   juce::Justification::horizontallyCentred);
    }
}

void AnalyseScreen::paintAlignmentPreview(juce::Graphics& g, juce::Rectangle<float> area) const
{
    // The strongest captured hit, 20 ms of it with the onset a quarter of the way in: the sidechain (pale), ORIGINAL
    // (blue) and the row (green), each scaled to its own peak, as ALIGNMENT does.
    if (! hitBefore.valid())
        return;
    const auto s = getScale();
    const auto plot = area.withTrimmedTop(46.0f * s).withTrimmedBottom(28.0f * s);
    const auto fs = hitBefore.getSampleRate();
    const auto span = 0.02 * fs;
    const auto start = (double)hitBefore.onsetIndex() - 0.25 * span;
    const auto peak = [&](const meter::HitCapture& h, int stream)
    {
        float m = 1e-9f;
        for (int i = 0; i < (int)span; ++i)
            m = std::max(m, std::abs(h.at(stream, (long long)start + i)));
        return m;
    };
    const auto trace = [&](const meter::HitCapture& h, int stream, juce::Colour colour, float thickness)
    {
        const auto scale = 0.46f * plot.getHeight() / peak(h, stream);
        juce::Path path;
        const auto points = juce::jmax(2, (int)plot.getWidth());
        for (int i = 0; i < points; ++i)
        {
            const auto t = start + span * i / (points - 1);
            const auto x = plot.getX() + plot.getWidth() * (float)i / (float)(points - 1);
            const auto y = plot.getCentreY() - h.interpolated(stream, t) * scale;
            i == 0 ? path.startNewSubPath(x, y) : path.lineTo(x, y);
        }
        g.setColour(colour);
        g.strokePath(path, juce::PathStrokeType(juce::jmax(1.0f, thickness * s)));
    };
    g.setColour(design::meterGrid);
    g.fillRect(juce::Rectangle<float>(plot.getX(), plot.getCentreY(), plot.getWidth(), juce::jmax(1.0f, 1.0f * s)));
    trace(hitBefore, meter::HitCapture::sidechain, design::meterAxisText.withAlpha(0.55f), 1.5f);
    trace(hitBefore, meter::HitCapture::output, design::meterInput.withAlpha(0.9f), 1.8f);
    trace(hitAfter, meter::HitCapture::output, design::phosphor, 2.0f);

    const auto small = meterFont(assets, 11.0f * s).withExtraKerningFactor(0.05f);
    const auto legend = meterFont(assets, 12.0f * s).withExtraKerningFactor(0.1f);
    const auto row = previewRow();
    g.setColour(design::phosphor);
    const auto name = row < 0 ? juce::String("ORIGINAL") : "OPTION " + juce::String(row + 1);
    drawTextAt(g, legend, name, area.getRight(), area.getY() + 12.0f * s, juce::Justification::right);
    auto x = area.getRight() - textWidth(legend, name) - 20.0f * s;
    if (row >= 0)
    {
        g.setColour(design::meterInput);
        drawTextAt(g, legend, "ORIGINAL", x, area.getY() + 12.0f * s, juce::Justification::right);
        x -= textWidth(legend, "ORIGINAL") + 20.0f * s;
    }
    g.setColour(design::meterAxisText.withAlpha(0.7f));
    drawTextAt(g, legend, "SIDECHAIN", x, area.getY() + 12.0f * s, juce::Justification::right);
    drawTextAt(g, small, "STRONGEST HIT, 20 ms", plot.getX(), area.getBottom() - 4.0f * s, juce::Justification::left);
}
} // namespace pa::ui
