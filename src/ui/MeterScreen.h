#pragma once

#include "meter/CorrelationAnalyser.h"
#include "meter/MeterCapture.h"
#include "meter/ScopeBuffer.h"
#include "ui/DesignComponent.h"

#include <functional>
#include <memory>

namespace pa::ui
{
// The correlation meter screen (IMPLEMENTATION_PLAN 3 and 4.4; views chosen in P4). Opaque, with bounds exactly the
// black screen interior, so its repaints never reach the panel behind it.
//
// While the meter is on and the screen is showing (and only then) it turns the processor's capture on, pulls the
// samples at 30 Hz, analyses them, and repaints itself when there is a new result. Otherwise nothing runs: no
// capture on the audio thread, no timer, no analysis.
//
// Views (clicked on the screen's bottom row): FREQUENCY, correlation against frequency; TIME, the PHAT lag function,
// which shows how far apart in time this track and the sidechain are; PHASE, the angle between this track and the
// sidechain against frequency (R17). All show processed (bright) and unprocessed (dim), and the overall
// processed/unprocessed pair on the right.
//
// ALIGNMENT (R19; `scope` in the code): this track, the sidechain and the output as waveforms on top of each other, each scaled to its own
// peak, triggered on the sidechain's loudest recent onset so a hit stays put. The mouse wheel zooms (0.5 to 200 ms
// across). Frozen, dragging slides the input against the sidechain: the output trace becomes the PREVIEW, the input
// as it would be with the delay knob at that distance.
//
// The view is chosen from a menu that opens upwards from the bottom row's middle label.
//
// The row also has the averaging speed (SLOW / FAST) and HOLD. The screen freezes while HOLD is on, and by itself
// while a host that reports its transport is stopped. Frozen, a drag across the TIME view previews a delay: the
// output trace becomes what the input would read with the delay knob there (analyser preview), in every view.
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
        frequency,
        time,
        phase,
        bands,
        scope
    };
    using Speed = meter::CorrelationAnalyser::Speed;

    static constexpr double noSignalSeconds = 1.0;

    MeterScreen(const SourceAssets&, meter::MeterCapture&);
    ~MeterScreen() override;

    void setMeterOn(bool);
    void setView(View);
    View getView() const { return view; }
    std::function<void(View)> onViewSelected; // a choice from the view menu
    void chooseView(View v) { onViewSelected ? onViewSelected(v) : setView(v); }
    std::function<void()> viewMenuHook; // for tests: called in place of opening the menu
    void setSpeed(Speed);
    Speed getSpeed() const { return analyser.getSpeed(); }
    std::function<void(Speed)> onSpeedSelected; // a click on SLOW or FAST

    // HOLD: frozen by the user (the label), or by a stopped host transport. Releasing HOLD clears the preview.
    void setHeld(bool);
    bool isHeld() const { return held; }
    bool isFrozen() const;
    bool isPreviewing() const { return analyser.isPreviewing(); }
    void setPreviewDelayMs(double ms); // frozen only; clamped to the delay knob's reach, snapped to 0.1 sample
    // The delay and polarity knobs, while frozen: the screen shows the result of those settings (delay 0 when the
    // delay is off), phase stage left out. Not frozen: ignored (the audio shows it itself).
    void setHeldSettings(double delayMs, bool inverted);

    State getState() const { return state; }
    bool isTimerRunningForTesting() const { return isTimerRunning(); }
    int getPaintCount() const { return paintCount; }
    // Centres of the bottom row's labels, in local coordinates (for tests).
    juce::Point<float> viewSelectorCentreForTesting() const { return controlArea(Control::view).getCentre(); }
    double getScopeSpanMs() const { return scopeSpanMs; }
    void setScopeSpanMs(double);
    const meter::ScopeBuffer& getScopeBuffer() const { return scopeBuffer; }
    long long getScopeTrigger() const { return triggerIndex; }
    juce::Point<float> speedLabelCentreForTesting(Speed v) const
    {
        return controlArea(v == Speed::slow ? Control::slow : Control::fast).getCentre();
    }
    juce::Point<float> holdLabelCentreForTesting() const { return controlArea(Control::hold).getCentre(); }
    const meter::CorrelationAnalyser& getAnalyser() const { return analyser; }

    void paint(juce::Graphics&) override;
    void visibilityChanged() override { updateTimer(); }
    void parentHierarchyChanged() override { updateTimer(); }
    bool hitTest(int x, int y) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

  private:
    void timerCallback() override;
    void updateTimer();
    bool poll(); // pulls and analyses; true if there is a new result
    State currentState() const;

    // Local coordinates of design-space positions.
    float lx(float designX) const { return designX * getScale() - (float)getX(); }
    float ly(float designY) const { return designY * getScale() - (float)getY(); }
    float yForR(float r) const;
    float xForHz(float hz) const;
    float xForMs(double ms) const;
    float yForPhase(float degrees) const { return yForR(degrees / 180.0f); }
    double msForX(float x) const;

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
    void scrubTo(float x);
    void freezeChanged();
    void showViewMenu();
    void refreshTrigger(bool force);

    void paintStatic(juce::Graphics&, float pixelScale) const;
    void paintGrid(juce::Graphics&) const;
    void paintLive(juce::Graphics&) const;
    void paintTrace(juce::Graphics&, const std::function<float(int)>& xAt, const std::vector<float>& values,
                    bool processed) const;
    void paintDashedTrace(juce::Graphics&, const std::function<float(int)>& xAt, const std::vector<float>& values,
                          bool processed) const;
    void paintPhase(juce::Graphics&) const;
    void paintScope(juce::Graphics&) const;
    void paintBands(juce::Graphics&) const;
    void paintLagReadout(juce::Graphics&) const;
    void paintPreviewNote(juce::Graphics&) const;
    void paintOverall(juce::Graphics&) const;
    void paintSegments(juce::Graphics&, float x0, float x1, float r, bool unlitToo) const;

    const SourceAssets& assets;
    meter::MeterCapture& capture;
    meter::CorrelationAnalyser analyser;
    std::vector<float> scratch[meter::MeterCapture::numStreams];
    double lastSamplesMs = 0.0;
    meter::ScopeBuffer scopeBuffer;
    long long triggerIndex = -1;
    float triggerStrength = 0.0f;
    double scopeSpanMs = 20.0;
    int triggerTick = 0;
    float slipStartX = 0.0f;
    double slipStartMs = 0.0;
    std::unique_ptr<juce::LookAndFeel> menuLookAndFeel;

    bool meterOn = false, held = false, wasFrozen = false;
    View view = View::frequency;
    State state = State::off;
    CachedLayer layer;
    int paintCount = 0;
};
} // namespace pa::ui
