#pragma once

#include "meter/CorrelationAnalyser.h"
#include "meter/HitCapture.h"
#include "meter/MeterCapture.h"
#include "meter/ScopeBuffer.h"
#include "ui/DesignComponent.h"

#include <array>
#include <functional>
#include <memory>

namespace pa::ui
{
// The correlation meter screen (IMPLEMENTATION_PLAN 3 and 4.4). Opaque, with bounds exactly the black screen interior,
// so its repaints never reach the panel behind it.
//
// While the meter is on and the screen is showing (and only then) it turns the processor's capture on, pulls the
// samples at 30 Hz, and repaints itself while something moves. Otherwise nothing runs: no capture on the audio thread,
// no timer, no analysis. Only the current view's work runs (R21).
//
// Three views, chosen from a menu that opens upwards from the bottom row's middle label (R23):
//   - BANDS (the default): six bars of the correlation with the sidechain, output (lit segments) and input (a blue
//     tick), their values, and the overall pair on the right.
//   - VECTORSCOPE: a goniometer (the last 0.25 s on SLOW, 60 ms on FAST). The input and the output each against the sidechain, each scaled to its own level and
//     turned 45 degrees, so a line up and down is in phase, a circle 90 degrees off, a line across inverted.
//   - ALIGNMENT (R19; `scope` in the code): this track, the sidechain and the output as waveforms on top of each other,
//     each scaled to its own peak, triggered on the sidechain's loudest recent onset so a hit stays put. The mouse wheel
//     and the - and + buttons zoom (0.5 to 200 ms across). With CAPTURE on (the default) it holds the last hit it
//     detected (within 12 dB of the strongest recent one) and shows the output the knobs would give for it, rendered
//     from the captured input (`meter/HitCapture`), so the picture sits still while the DELAY, polarity and phase are
//     turned; a new hit replaces it after the knobs have been left alone for two seconds. With CAPTURE off it follows the
//     live audio, and, frozen, dragging slides the input against the sidechain.
//
// The bars ease toward their values at the screen's own pace (a time constant of 0.3 s on SLOW, 0.1 s on FAST), so
// they move smoothly whatever the analysis does.
//
// The row also has the averaging speed (SLOW / FAST) and HOLD. The screen freezes while HOLD is on, and by itself
// while a host that reports its transport is stopped; frozen, the knobs show what they would do (the delay, polarity
// and phase are applied to the held picture).
class MeterScreen : public DesignComponent, private juce::Timer
{
  public:
    enum class State
    {
        off,
        noSidechain, // sidechain bus disabled, or no sidechain signal for over a second
        metering
    };

    enum class View
    {
        bands,
        vector,
        scope
    };
    using Speed = meter::CorrelationAnalyser::Speed;
    // The three things the views draw. Clicking a legend label hides or shows that one, in every view (all shown by
    // default); only ALIGNMENT draws the sidechain.
    enum class Series
    {
        input,
        output,
        sidechain
    };

    static constexpr double noSignalSeconds = 1.0;
    static constexpr double zoomStep = 1.5; // the - and + buttons: a bit at a time
    // A bar whose band falls under the analysis's signal gate (between a kick's hits, say) keeps its last value this long
    // before it is hidden: on FAST the averages are short enough for a band to dip under the gate between every hit.
    static constexpr double gateHoldMs = 800.0;

    MeterScreen(const SourceAssets&, meter::MeterCapture&);
    ~MeterScreen() override;

    void setMeterOn(bool);
    void setView(View);
    View getView() const { return view; }
    std::function<void(View)> onViewSelected; // a choice from the view menu
    void chooseView(View v) { onViewSelected ? onViewSelected(v) : setView(v); }
    std::function<void()> viewMenuHook; // for tests: called in place of opening the menu
    bool isShown(Series s) const { return shown[(size_t)s]; }
    void setShown(Series, bool);
    std::function<void(Series, bool)> onSeriesToggled; // a click on a legend label
    juce::Point<float> legendCentreForTesting(Series s) const;
    void setSpeed(Speed);
    Speed getSpeed() const { return analyser.getSpeed(); }
    std::function<void(Speed)> onSpeedSelected; // a click on SLOW or FAST

    // HOLD: frozen by the user (the label), or by a stopped host transport. Releasing HOLD clears the preview.
    void setHeld(bool);
    bool isHeld() const { return held; }
    bool isFrozen() const;
    bool isPreviewing() const { return analyser.isPreviewing(); }
    void setPreviewDelayMs(double ms); // frozen only; clamped to the delay knob's reach, snapped to 0.1 sample
    // The delay and polarity knobs, and the phase stage's response if given: while frozen, the screen shows the result
    // of those settings (delay 0 when the delay is off). The settings are also kept while not frozen, for ALIGNMENT's
    // captured hit.
    void setHeldSettings(double delayMs, bool inverted, meter::CorrelationAnalyser::PhaseResponse phase = {});

    State getState() const { return state; }
    bool isTimerRunningForTesting() const { return isTimerRunning(); }
    int getPaintCount() const { return paintCount; }
    // Centres of the bottom row's labels, in local coordinates (for tests).
    juce::Point<float> viewSelectorCentreForTesting() const { return controlArea(Control::view).getCentre(); }
    juce::Point<float> zoomButtonCentreForTesting(bool zoomIn) const { return zoomButtonArea(zoomIn).getCentre(); }
    double getScopeSpanMs() const { return scopeSpanMs; }
    void setScopeSpanMs(double);
    void zoomScope(double factor) { setScopeSpanMs(scopeSpanMs * factor); } // > 1 zooms out
    // VECTORSCOPE's source: this track against the sidechain (the default), or, on a stereo track, the track's own left
    // and right (the classic goniometer). The side signals are only captured while STEREO is showing.
    void setVectorStereo(bool);
    bool isVectorStereo() const { return vectorStereo && capture.isStereoTrack(); }
    std::function<void(bool)> onVectorModeSelected; // a click on the SIDECHAIN / STEREO label
    void setCaptureMode(bool);
    bool isCaptureMode() const { return captureMode; }
    std::function<void(bool)> onCaptureSelected; // a click on the CAPTURE label
    bool hasCapture() const { return hitCapture.valid(); }
    const meter::HitCapture& getHitCapture() const { return hitCapture; }
    const meter::ScopeBuffer& getScopeBuffer() const { return scopeBuffer; }
    long long getScopeTrigger() const { return triggerIndex; }
    juce::Point<float> speedLabelCentreForTesting(Speed v) const
    {
        return controlArea(v == Speed::slow ? Control::slow : Control::fast).getCentre();
    }
    juce::Point<float> holdLabelCentreForTesting() const { return controlArea(Control::hold).getCentre(); }
    const meter::CorrelationAnalyser& getAnalyser() const { return analyser; }
    // What the bars show now (eased), for tests.
    float shownBand(int band, bool processed) const { return (processed ? shownBandX : shownBandZ)[(size_t)band]; }
    float shownOverall(bool processed) const { return processed ? shownOverallX : shownOverallZ; }
    // The vectorscope's two correlations over its window, NaN when there is none.
    struct VectorReading
    {
        float inputR = 0.0f, outputR = 0.0f; // against the sidechain, or (stereo) between left and right
        bool valid = false, stereo = false;
    };
    VectorReading vectorReading() const;

    void paint(juce::Graphics&) override;
    void visibilityChanged() override { updateTimer(); }
    void parentHierarchyChanged() override { updateTimer(); }
    bool hitTest(int x, int y) override;
    // What is under the mouse, one tip per feature of the current view (also the whole screen, for the view itself).
    juce::String getTooltip() override { return tooltipAt(getMouseXYRelative().toFloat()); }
    juce::String tooltipAt(juce::Point<float> local) const;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

  private:
    void timerCallback() override;
    void updateTimer();
    bool poll(); // pulls and analyses; true if there are new samples
    State currentState() const;
    bool easeShown(double elapsedMs); // moves the shown values toward the analyser's; true while any still moves

    // Local coordinates of design-space positions.
    float lx(float designX) const { return designX * getScale() - (float)getX(); }
    float ly(float designY) const { return designY * getScale() - (float)getY(); }
    float yForR(float r) const;

    // The bottom row's labels. Areas are the click targets.
    enum class Control
    {
        none,
        view,
        slow,
        fast,
        hold
    };
    juce::Rectangle<float> controlArea(Control) const;
    Control controlAt(juce::Point<float>) const;
    bool scrubArea(juce::Point<float>) const;
    juce::Rectangle<float> captureToggleArea() const;
    juce::Rectangle<float> zoomButtonArea(bool zoomIn) const;
    juce::Rectangle<float> vectorModeArea() const;
    bool legendHit(juce::Point<float>) const;
    void captureHit(long long onsetIndex);
    void startCapturing();
    void captureWindow(); // the vectorscope's held window, rendered through the knobs
    void scrubTo(float x);
    void freezeChanged();
    void showViewMenu();
    void refreshTrigger(bool force);
    void applyNeeds(); // only the current view's work runs in the analyser (plan R21)

    void paintStatic(juce::Graphics&, float pixelScale) const;
    void paintGrid(juce::Graphics&) const;
    void paintLive(juce::Graphics&) const;
    void paintScope(juce::Graphics&) const;
    void paintVector(juce::Graphics&) const;
    void vectorPoints(std::vector<juce::Point<float>>& input, std::vector<juce::Point<float>>& output,
                      VectorReading& reading) const;
    void paintBands(juce::Graphics&) const;
    void paintLegend(juce::Graphics&) const;
    struct LegendItem
    {
        Series series;
        juce::Rectangle<float> area;
        juce::String text;
    };
    std::vector<LegendItem> legendItems() const;
    void paintPreviewNote(juce::Graphics&) const;
    void paintOverall(juce::Graphics&) const;
    void paintSegments(juce::Graphics&, float x0, float x1, float r, bool unlitToo) const;

    const SourceAssets& assets;
    meter::MeterCapture& capture;
    meter::CorrelationAnalyser analyser;
    std::vector<float> scratch[meter::MeterCapture::numStreams];
    double lastSamplesMs = 0.0, lastEaseMs = 0.0;
    meter::ScopeBuffer scopeBuffer;
    meter::HitCapture hitCapture, vectorCapture;
    meter::HitCapture::Settings knobs;
    bool captureMode = true, vectorStereo = false;
    double lastKnobMs = -1.0e9;
    long long triggerIndex = -1;
    float triggerStrength = 0.0f;
    double scopeSpanMs = 20.0;
    int triggerTick = 0;
    float slipStartX = 0.0f;
    double slipStartMs = 0.0;
    std::unique_ptr<juce::LookAndFeel> menuLookAndFeel;
    std::array<float, meter::CorrelationAnalyser::numBands> shownBandX{}, shownBandZ{};
    float shownOverallX = 0.0f, shownOverallZ = 0.0f;
    // How long each shown value has been unmeasured (gated), in ms: it is held that long (gateHoldMs) before it goes.
    std::array<double, meter::CorrelationAnalyser::numBands> gatedBandX{}, gatedBandZ{};
    double gatedOverallX = 0.0, gatedOverallZ = 0.0;

    bool shown[3] = {true, true, true};
    bool meterOn = false, held = false, wasFrozen = false;
    View view = View::bands;
    State state = State::off;
    CachedLayer layer;
    int paintCount = 0;
};
} // namespace pa::ui
