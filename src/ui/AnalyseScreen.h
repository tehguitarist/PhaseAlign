#pragma once

#include "analyse/Session.h"
#include "meter/HitCapture.h"
#include "params/Parameters.h"
#include "ui/DesignComponent.h"

#include <array>
#include <functional>

namespace pa::ui
{
// ANALYSE's screen (PLAN 3.5, IMPLEMENTATION_PLAN R26), in place of the meter's while the ANALYSE button is lit. It
// draws what the session is doing and passes clicks to it; the session (in the processor) does the work.
//
//   - Capturing: what is being searched (the DELAY and PHASE buttons decide; polarity always), how much audio is in
//     (a bar to the recommended 30 s, marked at the 10 s minimum), the two levels, and what to do next. CLEAR starts the capture again;
//     ANALYSE NOW analyses without waiting for the transport to stop (hosts that don't report one, the standalone app).
//   - Analysing: a line, for the fraction of a second it takes.
//   - Results: up to two options and ORIGINAL (the settings before ANALYSE). Clicking a row applies it, so the next
//   play
//     is heard with it; the row the panel matches is marked. Beside them, the chosen row against ORIGINAL on the
//     captured audio: the six BANDS (ORIGINAL as the blue tick, the row as the green bar) or ALIGNMENT (the strongest
//     captured hit: sidechain, ORIGINAL in blue, the row in green). AGAIN captures anew.
class AnalyseScreen : public DesignComponent
{
  public:
    enum class Preview
    {
        bands,
        alignment
    };

    AnalyseScreen(const SourceAssets&, analyse::Session&);

    void setDelayUnit(params::DelayUnit);
    void setPreview(Preview);
    Preview getPreview() const { return preview; }
    std::function<void(Preview)> onPreviewSelected;
    // The panel's settings changed (a knob, or an option applied): the marked row and the preview may change.
    void settingsChanged();
    void sessionChanged();

    // The row shown in the preview: the last one clicked (option 1 when a result arrives).
    int previewRow() const;

    // Click targets, in local coordinates (for tests).
    juce::Rectangle<float> rowArea(int row) const; // 0, 1: options; -1: ORIGINAL
    juce::Rectangle<float> againArea() const;      // AGAIN while results show, CLEAR while capturing
    juce::Rectangle<float> analyseNowArea() const;
    juce::Rectangle<float> previewToggleArea() const;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    juce::String getTooltip() override { return tooltipAt(getMouseXYRelative().toFloat()); }
    juce::String tooltipAt(juce::Point<float>) const;

    // What the rows say (for tests): the setting in the panel's own numbers, as the readouts would show it.
    juce::String rowText(int row) const;

  private:
    float lx(float designX) const { return designX * getScale() - (float)getX(); }
    float ly(float designY) const { return designY * getScale() - (float)getY(); }
    juce::Rectangle<float> designRect(float x, float y, float w, float h) const { return toLocal({x, y, w, h}); }

    void paintHeader(juce::Graphics&, const juce::String& right) const;
    void paintCapturing(juce::Graphics&) const;
    void paintResults(juce::Graphics&) const;
    void paintBandsPreview(juce::Graphics&, juce::Rectangle<float>) const;
    void paintAlignmentPreview(juce::Graphics&, juce::Rectangle<float>) const;
    void paintLabelButton(juce::Graphics&, juce::Rectangle<float>, const juce::String&, bool enabled, bool hot) const;
    void paintMessage(juce::Graphics&, juce::Rectangle<float>, const juce::String&, juce::Colour) const;
    juce::String scopeText(analyse::Scope) const;
    juce::String phaseText(const analyse::PanelSettings&) const;
    juce::String delayText(double ms) const;
    void updatePreview(); // recomputes the bars and the rendered hits for previewRow()

    const SourceAssets& assets;
    analyse::Session& session;
    params::DelayUnit delayUnit = params::DelayUnit::ms;
    Preview preview = Preview::bands;
    int lastClicked = 0;
    const analyse::Session::Outcome* seenOutcome = nullptr;
    juce::Point<float> mouse{-1.0f, -1.0f};

    // The preview, for previewRow(): band r before (ORIGINAL) and after, and the hit rendered both ways.
    static constexpr int numBands = 6;
    static constexpr double bandEdgesHz[numBands + 1] = {20.0, 100.0, 250.0, 630.0, 1600.0, 4000.0, 20000.0};
    int previewFor = -3;
    const analyse::Session::Outcome* previewOutcome = nullptr;
    std::array<double, numBands> bandsBefore{}, bandsAfter{};
    meter::HitCapture hitBefore, hitAfter;
};
} // namespace pa::ui
