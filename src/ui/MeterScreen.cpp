#include "ui/MeterScreen.h"
#include "params/Parameters.h"
#include "ui/Design.h"

#include <array>
#include <cmath>

namespace pa::ui
{
namespace
{
constexpr int segmentsPerHalf = 10;  // overall bar: one segment per 0.1 of correlation
constexpr float segmentFill = 0.72f; // of the segment pitch (the rest is the gap)

juce::String hzLabel(float hz)
{
    return hz >= 1000.0f ? juce::String(juce::roundToInt(hz / 1000.0f)) + "k" : juce::String(juce::roundToInt(hz));
}

double nowMs()
{
    return juce::Time::getMillisecondCounterHiRes();
}
} // namespace

MeterScreen::MeterScreen(const SourceAssets& assetsIn, meter::MeterCapture& captureIn)
    : DesignComponent(design::meterInterior), assets(assetsIn), capture(captureIn)
{
    setOpaque(true);
    setInterceptsMouseClicks(true, false); // only the view labels: see hitTest
    setTooltip("Click the middle label to choose the view: BANDS, the correlation with the sidechain in six bands "
               "(a bar is this track after the plugin, a blue tick before it); VECTORSCOPE, this track against the "
               "sidechain as a shape (a line up and down is in phase, a circle 90 degrees off, a line across inverted); "
               "ALIGNMENT, the waveforms on top of each other around a captured hit (wheel or - and + to zoom; CAPTURE "
               "holds the last hit, which the knobs then act on). SLOW or FAST sets the averaging. HOLD freezes the "
               "screen (it also freezes while the host is stopped); frozen, turn DELAY, the polarity button or the "
               "phase and the screen shows the result. Blue is this track before the plugin, green after it; click INPUT, "
               "OUTPUT or SIDECHAIN at the top right to hide or show that trace.");
    for (auto& s : scratch)
        s.resize((size_t)meter::MeterCapture::capacity);
    if (capture.getSampleRate() > 0.0)
    {
        analyser.prepare(capture.getSampleRate());
        scopeBuffer.prepare(capture.getSampleRate());
    }
    shownBandX.fill(std::nanf(""));
    shownBandZ.fill(std::nanf(""));
    shownOverallX = shownOverallZ = std::nanf("");
    applyNeeds();
}

void MeterScreen::applyNeeds()
{
    meter::CorrelationAnalyser::Needs needs{false, false, false, false};
    needs.bands = view == View::bands;
    capture.setWantSide(view == View::vector && isVectorStereo());
    analyser.setNeeds(needs); // the others need nothing but the overall bar: their pictures come from the scope buffer
}

MeterScreen::~MeterScreen()
{
    stopTimer();
    capture.setActive(false);
}

void MeterScreen::setMeterOn(bool shouldBeOn)
{
    meterOn = shouldBeOn;
    updateTimer();
    timerCallback();
}

void MeterScreen::setView(View newView)
{
    if (newView == view)
        return;
    view = newView;
    applyNeeds();
    if (view == View::scope)
    {
        refreshTrigger(true);
        if (captureMode)
            startCapturing();
    }
    if (view == View::vector && isFrozen())
        captureWindow();
    repaint();
}

void MeterScreen::setScopeSpanMs(double ms)
{
    scopeSpanMs = juce::jlimit(0.5, 200.0, ms);
    repaint();
}

// The trigger is the loudest recent onset of the sidechain. While live, it is looked for every few ticks and kept
// while it stays in the buffer, unless a clearly stronger one arrives; frozen, it stays where it is.
void MeterScreen::refreshTrigger(bool force)
{
    if (isFrozen() && ! force)
        return;
    const auto onset = scopeBuffer.findOnset(3.0);
    const auto gone = triggerIndex < 0 || triggerIndex < scopeBuffer.end() - (long long)(3.2 * scopeBuffer.getSampleRate());
    if (onset.index >= 0 && (force || gone || onset.strength > 1.25f * triggerStrength))
    {
        triggerIndex = onset.index;
        triggerStrength = onset.strength;
    }
    else if (gone && onset.index < 0)
    {
        triggerIndex = -1;
        triggerStrength = 0.0f;
    }
}

void MeterScreen::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (view != View::scope || state != State::metering)
        return;
    // A notch is a step of a quarter of the span; scroll up zooms in.
    setScopeSpanMs(scopeSpanMs * std::pow(0.8, (double)wheel.deltaY * 4.0));
}

void MeterScreen::showViewMenu()
{
    if (viewMenuHook)
    {
        viewMenuHook();
        return;
    }
    // A menu in the screen's own look: black, phosphor text, opening upwards from the selector.
    struct MenuLook : juce::LookAndFeel_V4
    {
        explicit MenuLook(juce::Font f) : font(std::move(f))
        {
            setColour(juce::PopupMenu::backgroundColourId, design::screenBlack);
            setColour(juce::PopupMenu::textColourId, design::meterAxisText);
            setColour(juce::PopupMenu::highlightedBackgroundColourId, design::phosphor.withAlpha(0.25f));
            setColour(juce::PopupMenu::highlightedTextColourId, design::phosphor);
        }
        juce::Font getPopupMenuFont() override { return font; }
        juce::Font font;
    };
    menuLookAndFeel = std::make_unique<MenuLook>(meterFont(assets, 13.0f * getScale(), true).withExtraKerningFactor(0.15f));

    juce::PopupMenu menu;
    menu.setLookAndFeel(menuLookAndFeel.get());
    const std::pair<View, const char*> items[] = {{View::bands, "BANDS"},
                                                  {View::vector, "VECTORSCOPE"},
                                                  {View::scope, "ALIGNMENT"}};
    for (const auto& [v, name] : items)
        menu.addItem((int)v + 1, name, true, v == view);
    const auto area = localAreaToGlobal(controlArea(Control::view)).getSmallestIntegerContainer();
    juce::Component::SafePointer<MeterScreen> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetScreenArea(area)
                           .withPreferredPopupDirection(juce::PopupMenu::Options::PopupDirection::upwards)
                           .withMinimumWidth(area.getWidth()),
                       [safe](int result)
                       {
                           if (safe != nullptr && result > 0)
                               safe->chooseView((View)(result - 1));
                       });
}

void MeterScreen::setSpeed(Speed newSpeed)
{
    if (newSpeed == analyser.getSpeed())
        return;
    analyser.setSpeed(newSpeed);
    repaint();
}

bool MeterScreen::isFrozen() const
{
    return held || (meterOn && capture.isTransportStopped());
}

void MeterScreen::setHeld(bool shouldHold)
{
    if (shouldHold == held)
        return;
    held = shouldHold;
    freezeChanged();
}

void MeterScreen::freezeChanged()
{
    const auto frozen = isFrozen();
    if (frozen == wasFrozen)
        return;
    wasFrozen = frozen;
    if (frozen)
    {
        if (view == View::vector)
            captureWindow();
    }
    else
        analyser.clearPreview(); // a preview belongs to the frozen picture
    repaint();
}

// The vectorscope's held picture: the last 0.3 s, with the output rendered through the knobs (as ALIGNMENT's capture).
void MeterScreen::captureWindow()
{
    const auto fs = scopeBuffer.getSampleRate();
    if (fs <= 0.0 || scopeBuffer.total() == 0)
        return;
    const auto length = (long long)(0.3 * fs);
    vectorCapture.captureRange(scopeBuffer, scopeBuffer.end() - length, (int)length);
    vectorCapture.render(knobs);
}

void MeterScreen::setHeldSettings(double delayMs, bool inverted, meter::CorrelationAnalyser::PhaseResponse phase)
{
    const auto reach = (double)params::maxDelayMs;
    const auto sampleMs = 1000.0 / juce::jmax(1.0, analyser.getSampleRate());
    const auto step = 0.1 * sampleMs;
    const auto snapped = juce::jlimit(-reach, reach, std::round(delayMs / step) * step);

    // Kept always: ALIGNMENT's captured hit is rendered through them.
    knobs.delayMs = snapped;
    knobs.inverted = inverted;
    knobs.phase = phase;
    lastKnobMs = nowMs();
    if (hitCapture.valid())
    {
        hitCapture.render(knobs);
        if (view == View::scope)
            repaint();
    }
    if (vectorCapture.valid() && isFrozen())
    {
        vectorCapture.render(knobs);
        if (view == View::vector)
            repaint();
    }

    if (! isFrozen())
        return;
    analyser.setPreview(snapped, inverted, std::move(phase));
    repaint();
}

void MeterScreen::setVectorStereo(bool on)
{
    if (on == vectorStereo)
        return;
    vectorStereo = on;
    applyNeeds();
    if (view == View::vector && isFrozen())
        captureWindow();
    repaint();
}

void MeterScreen::setCaptureMode(bool on)
{
    if (on == captureMode)
        return;
    captureMode = on;
    if (on && view == View::scope)
        startCapturing();
    repaint();
}

// Starts looking for hits from now on, and takes the strongest one of the last few seconds at once, so the view has
// something to hold without waiting for the next.
void MeterScreen::startCapturing()
{
    meter::ScopeBuffer::Onset onset;
    if (scopeBuffer.scanBack(3.0, meter::HitCapture::postSeconds, onset))
        captureHit(onset.index);
}

void MeterScreen::captureHit(long long onsetIndex)
{
    hitCapture.capture(scopeBuffer, onsetIndex);
    hitCapture.render(knobs);
    repaint();
}

void MeterScreen::setPreviewDelayMs(double ms)
{
    if (! isFrozen())
        return;
    const auto reach = (double)params::maxDelayMs;
    const auto sampleMs = 1000.0 / juce::jmax(1.0, analyser.getSampleRate());
    const auto step = 0.1 * sampleMs; // the delay knob's step
    analyser.setPreviewDelayMs(juce::jlimit(-reach, reach, std::round(ms / step) * step));
    repaint();
}

void MeterScreen::updateTimer()
{
    const auto shouldRun = meterOn && isShowing();
    if (shouldRun && ! isTimerRunning())
    {
        analyser.reset(); // fresh averages each time the meter starts
        shownBandX.fill(std::nanf(""));
        shownBandZ.fill(std::nanf(""));
        shownOverallX = shownOverallZ = std::nanf("");
        lastEaseMs = nowMs();
        analyser.clearPreview();
        wasFrozen = false;
        lastSamplesMs = nowMs();
        capture.setActive(true);
        startTimerHz(30);
    }
    else if (! shouldRun)
    {
        stopTimer();
        capture.setActive(false);
    }
}

bool MeterScreen::poll() // true: new samples were taken in (not while frozen)
{
    const auto fs = capture.getSampleRate();
    if (! juce::exactlyEqual(fs, analyser.getSampleRate()))
        analyser.prepare(fs);
    if (! juce::exactlyEqual(fs, scopeBuffer.getSampleRate()))
    {
        scopeBuffer.prepare(fs);
        hitCapture.clear();
    }

    float* dest[meter::MeterCapture::numStreams];
    for (int s = 0; s < meter::MeterCapture::numStreams; ++s)
        dest[s] = scratch[s].data();
    const auto n = capture.pull(dest, meter::MeterCapture::capacity);
    if (n == 0)
        return false;

    lastSamplesMs = nowMs();
    if (isFrozen()) // drained and dropped: the picture stays as it was
        return false;
    scopeBuffer.push(dest[meter::MeterCapture::input], dest[meter::MeterCapture::output],
                     dest[meter::MeterCapture::sidechain], n, dest[meter::MeterCapture::inputSide],
                     dest[meter::MeterCapture::outputSide]);
    if (view == View::scope && captureMode)
    {
        // A new hit replaces the held one, unless a knob was turned in the last two seconds (the picture shouldn't
        // change under the hand that is lining it up).
        scopeBuffer.scan();
        meter::ScopeBuffer::Onset onset;
        if (scopeBuffer.takeCompleted(meter::HitCapture::postSeconds, onset) && nowMs() - lastKnobMs >= 2000.0)
            captureHit(onset.index);
    }
    analyser.process(dest[meter::MeterCapture::input], dest[meter::MeterCapture::output],
                     dest[meter::MeterCapture::sidechain], n);
    return true;
}

MeterScreen::State MeterScreen::currentState() const
{
    if (! meterOn)
        return State::off;
    if (isFrozen() && state != State::off) // a frozen picture stays up, whatever the audio is doing
        return state;
    const auto noSignal = analyser.sidechainSilentSeconds() > noSignalSeconds ||
                          nowMs() - lastSamplesMs > noSignalSeconds * 1000.0; // no audio arriving at all
    return capture.hasSidechain() && ! noSignal ? State::metering : State::noSidechain;
}

bool MeterScreen::easeShown(double elapsedMs)
{
    // The bars move toward what the analysis says at the screen's own pace, so they glide however the analysis moves:
    // a time constant of 0.3 s on SLOW, 0.1 s on FAST. An unmeasured (gated) value shows at once; the first value after
    // one jumps to it.
    const auto tau = analyser.getSpeed() == Speed::slow ? 0.3 : 0.1;
    const auto k = (float)(1.0 - std::exp(-juce::jlimit(1.0, 200.0, elapsedMs) / 1000.0 / tau));
    bool moving = false;
    const auto step = [&](float& shown, float target)
    {
        if (std::isnan(target))
        {
            moving = moving || ! std::isnan(shown);
            shown = target;
        }
        else if (std::isnan(shown))
        {
            shown = target;
            moving = true;
        }
        else
        {
            const auto next = shown + k * (target - shown);
            moving = moving || std::abs(next - target) > 0.002f || std::abs(next - shown) > 0.0005f;
            shown = std::abs(next - target) < 0.002f ? target : next;
        }
    };
    for (size_t b = 0; b < shownBandX.size(); ++b)
    {
        step(shownBandX[b], analyser.bandsProcessed()[b]);
        step(shownBandZ[b], analyser.bandsUnprocessed()[b]);
    }
    step(shownOverallX, analyser.overallProcessed());
    step(shownOverallZ, analyser.overallUnprocessed());
    return moving;
}

void MeterScreen::timerCallback()
{
    // A host can hide the window without telling its children; stop (and stop capturing) on the next tick.
    if (isTimerRunning() && ! isShowing())
        updateTimer();

    const auto gotSamples = isTimerRunning() && poll();
    freezeChanged(); // the host's transport can stop or start under us
    if (view == View::scope && ++triggerTick % 3 == 0)
        refreshTrigger(false);
    const auto newState = currentState();
    const auto now = nowMs();
    const auto moving = newState == State::metering && easeShown(now - lastEaseMs);
    lastEaseMs = now;
    // Repaint while something moves: new samples in a live picture, or the bars still gliding.
    if (newState == state && ! (newState == State::metering && (moving || gotSamples)))
        return;
    state = newState;
    repaint();
}

//==============================================================================
float MeterScreen::yForR(float r) const
{
    const auto zero = ly(design::meterZeroY), top = ly(design::meterPlusOneY);
    return zero - juce::jlimit(-1.05f, 1.05f, r) * (zero - top);
}

namespace
{
const char* controlText(int control)
{
    static const char* const text[] = {"", "", "SLOW", "FAST", "HOLD"};
    return text[control];
}

const char* viewName(MeterScreen::View v)
{
    switch (v)
    {
        case MeterScreen::View::bands: return "BANDS";
        case MeterScreen::View::vector: return "VECTORSCOPE";
        case MeterScreen::View::scope: return "ALIGNMENT";
    }
    return "";
}
} // namespace

juce::Rectangle<float> MeterScreen::controlArea(Control c) const
{
    // The bottom row: SLOW and FAST at its left end, the view selector in the middle (the name of the view, with an
    // up-pointing triangle: its menu opens upwards), HOLD at its right end.
    const auto s = getScale();
    const auto font = meterFont(assets, 13.0f * s).withExtraKerningFactor(0.2f);
    const auto left = lx(design::meterPlotLeft), right = lx(design::meterPlotRight);
    const auto centre = 0.5f * (left + right);
    const auto rowY = ly(design::meterFreqTitleY) - 14.0f * s;
    const auto pad = 14.0f * s, gap = 28.0f * s, height = 26.0f * s;
    const auto width = [&](Control k) { return textWidth(font, controlText((int)k)) + 2.0f * pad; };
    const auto box = [&](float x, float w) { return juce::Rectangle<float>(x, rowY, w, height); };

    switch (c)
    {
        case Control::view:
        {
            // Wide enough for the longest name, so the selector doesn't move as the view changes.
            const auto w = textWidth(font, "VECTORSCOPE") + 2.0f * pad + 26.0f * s;
            return box(centre - 0.5f * w, w);
        }
        case Control::slow: return box(left - pad, width(c));
        case Control::fast: return box(left - pad + width(Control::slow) + 0.5f * gap, width(c));
        case Control::hold: return box(right + pad - width(c), width(c));
        case Control::none: break;
    }
    return {};
}

MeterScreen::Control MeterScreen::controlAt(juce::Point<float> p) const
{
    for (const auto c : {Control::view, Control::slow, Control::fast, Control::hold})
        if (controlArea(c).contains(p))
            return c;
    return Control::none;
}

bool MeterScreen::scrubArea(juce::Point<float> p) const
{
    // Frozen with CAPTURE off (or no hit held yet), in ALIGNMENT: sliding the input against the sidechain.
    return isFrozen() && view == View::scope && ! (captureMode && hitCapture.valid()) && state == State::metering &&
           juce::Rectangle<float>::leftTopRightBottom(lx(design::meterPlotLeft), ly(design::meterPlusOneY),
                                                      lx(design::meterPlotRight), ly(design::meterMinusOneY))
               .contains(p);
}

juce::Rectangle<float> MeterScreen::captureToggleArea() const
{
    // Top left of the plot, under the span: only in ALIGNMENT.
    const auto s = getScale();
    return view == View::scope ? juce::Rectangle<float>(lx(design::meterPlotLeft) + 8.0f * s, ly(design::meterPlusOneY) + 25.0f * s,
                                                        150.0f * s, 22.0f * s)
                               : juce::Rectangle<float>();
}

juce::Rectangle<float> MeterScreen::zoomButtonArea(bool zoomIn) const
{
    // Two small boxes after the span text, top left of the plot: - zooms out, + zooms in.
    const auto s = getScale();
    return view == View::scope ? juce::Rectangle<float>(lx(design::meterPlotLeft) + (zoomIn ? 190.0f : 158.0f) * s,
                                                        ly(design::meterPlusOneY) + 6.0f * s, 26.0f * s, 20.0f * s)
                               : juce::Rectangle<float>();
}

juce::Rectangle<float> MeterScreen::vectorModeArea() const
{
    // Top left of the plot, under the correlations: only in the VECTORSCOPE of a stereo track.
    const auto s = getScale();
    return view == View::vector && capture.isStereoTrack()
               ? juce::Rectangle<float>(lx(design::meterPlotLeft) + 8.0f * s, ly(design::meterPlusOneY) + 62.0f * s, 200.0f * s, 22.0f * s)
               : juce::Rectangle<float>();
}

bool MeterScreen::legendHit(juce::Point<float> p) const
{
    for (const auto& item : legendItems())
        if (item.area.contains(p))
            return true;
    return false;
}

bool MeterScreen::hitTest(int x, int y)
{
    if (state == State::off)
        return false;
    const auto p = juce::Point<int>(x, y).toFloat();
    return controlAt(p) != Control::none || captureToggleArea().contains(p) || zoomButtonArea(false).contains(p) ||
           zoomButtonArea(true).contains(p) || vectorModeArea().contains(p) || legendHit(p) || scrubArea(p);
}

void MeterScreen::scrubTo(float x)
{
    // Sliding: the input moves with the mouse, so the delay is how far it has been dragged, from where it was.
    const auto width = lx(design::meterPlotRight) - lx(design::meterPlotLeft);
    setPreviewDelayMs(slipStartMs + (double)(x - slipStartX) * scopeSpanMs / (double)width);
}

void MeterScreen::mouseDown(const juce::MouseEvent& e)
{
    switch (controlAt(e.position))
    {
        case Control::view: showViewMenu(); return;
        case Control::slow: onSpeedSelected ? onSpeedSelected(Speed::slow) : setSpeed(Speed::slow); return;
        case Control::fast: onSpeedSelected ? onSpeedSelected(Speed::fast) : setSpeed(Speed::fast); return;
        case Control::hold: setHeld(! held); return;
        case Control::none: break;
    }
    if (zoomButtonArea(false).contains(e.position))
    {
        zoomScope(zoomStep); // zoom out: a wider span
        return;
    }
    if (zoomButtonArea(true).contains(e.position))
    {
        zoomScope(1.0 / zoomStep);
        return;
    }
    for (const auto& item : legendItems())
        if (item.area.contains(e.position))
        {
            const auto on = ! shown[(size_t)item.series];
            onSeriesToggled ? onSeriesToggled(item.series, on) : setShown(item.series, on);
            return;
        }
    if (vectorModeArea().contains(e.position))
    {
        onVectorModeSelected ? onVectorModeSelected(! vectorStereo) : setVectorStereo(! vectorStereo);
        return;
    }
    if (captureToggleArea().contains(e.position))
    {
        onCaptureSelected ? onCaptureSelected(! captureMode) : setCaptureMode(! captureMode);
        return;
    }
    if (scrubArea(e.position))
    {
        slipStartX = e.position.x;
        slipStartMs = analyser.isPreviewing() ? analyser.previewDelayMs() : 0.0;
        scrubTo(e.position.x);
    }
}

void MeterScreen::mouseDrag(const juce::MouseEvent& e)
{
    if (scrubArea(e.position))
        scrubTo(e.position.x);
}

//==============================================================================
void MeterScreen::paint(juce::Graphics& g)
{
    ++paintCount;
    const auto key = juce::String((int)state) + ":" + juce::String((int)view) + ":" +
                     juce::String((int)analyser.getSpeed()) + ":" + juce::String((int)isFrozen()) + ":" +
                     juce::String((int)isVectorStereo());
    layer.paint(g, getLocalBounds(), key,
                [this](juce::Graphics& lg, float pixelScale) { paintStatic(lg, pixelScale); });
    if (state != State::off)
        paintLegend(g);
    if (state == State::metering)
        paintLive(g);
}

void MeterScreen::setShown(Series series, bool on)
{
    if (shown[(size_t)series] == on)
        return;
    shown[(size_t)series] = on;
    repaint();
}

std::vector<MeterScreen::LegendItem> MeterScreen::legendItems() const
{
    // Top right of the plot, right to left: OUTPUT (PREVIEW while the screen shows the knobs' result), INPUT, and in
    // ALIGNMENT the SIDECHAIN. Each is a click target.
    const auto s = getScale();
    const auto font = meterFont(assets, 14.0f * s).withExtraKerningFactor(0.1f);
    const auto previewing = analyser.isPreviewing() && ! (view == View::scope && captureMode && hitCapture.valid());
    std::vector<std::pair<Series, juce::String>> names = {{Series::output, previewing ? "PREVIEW" : "OUTPUT"},
                                                          {Series::input, "INPUT"}};
    if (view == View::scope)
        names.push_back({Series::sidechain, "SIDECHAIN"});
    std::vector<LegendItem> items;
    auto x = lx(design::meterPlotRight);
    const auto baseline = ly(design::meterPlusOneY) - 12.0f * s;
    for (const auto& [series, text] : names)
    {
        const auto width = textWidth(font, text);
        items.push_back({series, juce::Rectangle<float>(x - width - 6.0f * s, baseline - 15.0f * s, width + 12.0f * s, 22.0f * s), text});
        x -= width + 24.0f * s;
    }
    return items;
}

juce::Point<float> MeterScreen::legendCentreForTesting(Series series) const
{
    for (const auto& item : legendItems())
        if (item.series == series)
            return item.area.getCentre();
    return {};
}

void MeterScreen::paintLegend(juce::Graphics& g) const
{
    const auto s = getScale();
    const auto font = meterFont(assets, 14.0f * s).withExtraKerningFactor(0.1f);
    const auto baseline = ly(design::meterPlusOneY) - 12.0f * s;
    for (const auto& item : legendItems())
    {
        const auto colour = item.series == Series::output  ? design::phosphor
                            : item.series == Series::input ? design::meterInput
                                                           : design::meterAxisText.withAlpha(0.85f);
        const auto on = shown[(size_t)item.series];
        g.setColour(colour.withMultipliedAlpha(on ? 1.0f : 0.3f));
        const auto right = item.area.getRight() - 6.0f * s;
        drawTextAt(g, font, item.text, right, baseline, juce::Justification::right);
        if (! on) // struck through: hidden
            g.drawLine(item.area.getX() + 4.0f * s, baseline - 5.0f * s, right, baseline - 5.0f * s, juce::jmax(1.0f, 1.2f * s));
    }
}

void MeterScreen::paintStatic(juce::Graphics& g, float pixelScale) const
{
    const auto s = getScale();
    g.fillAll(design::screenBlack);

    if (state == State::off)
    {
        const auto font = meterFont(assets, 18.0f * s, true).withExtraKerningFactor(0.1f);
        g.setColour(design::phosphor.withAlpha(0.3f));
        drawTextAt(g, font, "METER OFF", 0.5f * (float)getWidth(), 0.5f * (float)getHeight() + 9.0f * s,
                   juce::Justification::horizontallyCentred);
        return;
    }

    paintGrid(g);

    // Overall column: unlit segments (the live bar lights them).
    const auto overallLeft = lx(design::meterOverallLeft), overallRight = lx(design::meterOverallRight);
    paintSegments(g, overallLeft, overallRight, 0.0f, true);
    if (view == View::bands)
    {
        const auto slot = (lx(design::meterPlotRight) - lx(design::meterPlotLeft)) / (float)meter::CorrelationAnalyser::numBands;
        for (int b = 0; b < meter::CorrelationAnalyser::numBands; ++b)
        {
            const auto centre = lx(design::meterPlotLeft) + slot * ((float)b + 0.5f);
            paintSegments(g, centre - 0.2f * slot, centre + 0.2f * slot, 0.0f, true);
        }
    }

    if (state != State::noSidechain)
        return;

    // Status message, centred on the zero line of the plot.
    const auto font = meterFont(assets, 20.0f * s, true).withExtraKerningFactor(0.08f);
    const juce::String text("NO SIDECHAIN SIGNAL");
    const auto centre =
        toLocal(juce::Point<float>(0.5f * (design::meterPlotLeft + design::meterPlotRight), design::meterZeroY));
    const auto box = juce::Rectangle<float>(textWidth(font, text) + 56.0f * s, 52.0f * s).withCentre(centre);
    g.setColour(design::screenBlack);
    g.fillRect(box);
    g.setColour(design::phosphor.withAlpha(0.45f));
    g.drawRect(box, juce::jmax(1.0f, 1.5f * s));
    drawWithGlow(g, getLocalBounds(), pixelScale, design::phosphor.withAlpha(0.6f), 6.0f * s,
                 [&](juce::Graphics& tg)
                 {
                     tg.setColour(design::phosphor);
                     drawTextAt(tg, font, text, centre.x, centre.y + 10.0f * s,
                                juce::Justification::horizontallyCentred);
                 });
}

void MeterScreen::paintGrid(juce::Graphics& g) const
{
    const auto s = getScale();
    const auto line = juce::jmax(1.0f, 2.0f * s);
    const auto thin = juce::jmax(1.0f, 1.2f * s);
    const auto left = lx(design::meterPlotLeft), right = lx(design::meterPlotRight);
    const auto top = ly(design::meterPlusOneY), zero = ly(design::meterZeroY), bottom = ly(design::meterMinusOneY);
    const float dashes[] = {3.0f * s, 6.0f * s};
    const auto labelFont = meterFont(assets, 14.0f * s);
    const auto labelY = ly(design::meterFreqLabelY) + 7.0f * s;

    const auto axisFont = meterFont(assets, 20.0f * s);
    if (view == View::vector)
    {
        // The goniometer's frame: a circle, the two axes, and what the shapes mean.
        const auto cx = 0.5f * (left + right), radius = 0.5f * (bottom - top) - 8.0f * s;
        g.setColour(design::meterGrid.withAlpha(0.8f));
        g.drawEllipse(cx - radius, zero - radius, 2.0f * radius, 2.0f * radius, thin);
        g.drawEllipse(cx - 0.5f * radius, zero - 0.5f * radius, radius, radius, thin);
        g.setColour(design::meterAxisText.withAlpha(0.7f));
        g.drawLine(cx, zero - radius - 4.0f * s, cx, zero + radius + 4.0f * s, thin);
        g.drawLine(cx - radius - 4.0f * s, zero, cx + radius + 4.0f * s, zero, thin);
        const auto small = meterFont(assets, 12.0f * s).withExtraKerningFactor(0.1f);
        const auto stereo = isVectorStereo();
        g.setColour(design::meterAxisText.withAlpha(0.75f));
        drawTextAt(g, small, stereo ? "MONO" : "IN PHASE", cx + 10.0f * s, top + 6.0f * s, juce::Justification::left);
        drawTextAt(g, small, stereo ? "SIDE" : "OUT OF PHASE", cx - radius - 12.0f * s, zero + 4.0f * s,
                   juce::Justification::right);
        drawTextAt(g, small, stereo ? "SIDE" : "OUT OF PHASE", cx + radius + 12.0f * s, zero + 4.0f * s,
                   juce::Justification::left);
        if (stereo) // the left and right channels lie on the diagonals
        {
            const auto d = radius * 0.7071f;
            drawTextAt(g, small, "L", cx - d - 10.0f * s, zero - d + 4.0f * s, juce::Justification::right);
            drawTextAt(g, small, "R", cx + d + 10.0f * s, zero - d + 4.0f * s, juce::Justification::left);
            g.setColour(design::meterGrid.withAlpha(0.6f));
            g.drawLine(cx - d, zero - d, cx + d, zero + d, thin);
            g.drawLine(cx + d, zero - d, cx - d, zero + d, thin);
        }
        g.setColour(design::meterAxisText.withAlpha(0.5f));
        const auto note = right - 14.0f * s;
        drawTextAt(g, small, stereo ? "UP AND DOWN: MONO" : "UP AND DOWN: IN PHASE", note, top + 22.0f * s,
                   juce::Justification::right);
        drawTextAt(g, small, stereo ? "ALONG L OR R: ONE SIDE" : "A CIRCLE: 90 DEGREES OFF", note,
                   top + 40.0f * s, juce::Justification::right);
        drawTextAt(g, small, stereo ? "ACROSS: L = -R" : "ACROSS: INVERTED", note, top + 58.0f * s,
                   juce::Justification::right);
        drawTextAt(g, small, stereo ? "THIS TRACK'S LEFT AND RIGHT" : "BOTH AGAINST THE SIDECHAIN", note,
                   bottom - 12.0f * s, juce::Justification::right);
    }
    else
    {
        // Vertical grid and x labels for BANDS (ALIGNMENT's ticks move with the zoom, so they are drawn with the traces).
        if (view == View::bands)
        {
            g.setColour(design::meterGrid.withAlpha(0.7f));
            const auto slot = (right - left) / (float)meter::CorrelationAnalyser::numBands;
            for (int b = 0; b < meter::CorrelationAnalyser::numBands; ++b)
            {
                const auto lo = (float)meter::CorrelationAnalyser::bandEdgesHz[b],
                           hi = (float)meter::CorrelationAnalyser::bandEdgesHz[b + 1];
                g.setColour(design::meterAxisText);
                drawTextAt(g, labelFont, hzLabel(lo) + "-" + hzLabel(hi), left + slot * ((float)b + 0.5f), labelY,
                           juce::Justification::horizontallyCentred);
                if (b > 0)
                {
                    g.setColour(design::meterGrid.withAlpha(0.7f));
                    g.drawDashedLine({left + slot * (float)b, top, left + slot * (float)b, bottom}, dashes, 2, thin);
                }
            }
        }

        // Half-scale lines and axes.
        g.setColour(design::meterGrid.withAlpha(0.7f));
        for (const auto v : {0.5f, -0.5f})
            g.drawDashedLine({left, yForR(v), right, yForR(v)}, dashes, 2, thin);
        g.setColour(design::meterGrid);
        g.drawLine(left, top, right, top, thin);
        g.drawLine(left, bottom, right, bottom, thin);
        g.setColour(design::meterAxisText);
        g.drawLine(left, top, left, bottom, line);
        g.drawLine(left, zero, right, zero, line);
        const auto yLabel = [&](const char* text, float atY)
        { drawTextAt(g, axisFont, text, left - 16.0f * s, atY + 10.0f * s, juce::Justification::right); };
        yLabel("+1", top);
        yLabel("0", zero);
        yLabel("-1", bottom);
    }

    // The bottom row: the selected view, speed and (while frozen) HOLD lit; the others dimmed, like the panel's labels.
    const auto viewFont = meterFont(assets, 13.0f * s).withExtraKerningFactor(0.2f);
    const auto titleY = ly(design::meterFreqTitleY) + 6.5f * s;
    const auto label = [&](Control c, bool lit, juce::Justification j)
    {
        const auto area = controlArea(c);
        const auto pad = 14.0f * s;
        g.setColour(design::meterAxisText.withAlpha(lit ? 0.95f : 0.3f));
        const auto x = j == juce::Justification::left ? area.getX() + pad : area.getRight() - pad;
        drawTextAt(g, viewFont, controlText((int)c), x, titleY, j);
    };
    label(Control::slow, analyser.getSpeed() == Speed::slow, juce::Justification::left);
    label(Control::fast, analyser.getSpeed() == Speed::fast, juce::Justification::left);
    label(Control::hold, isFrozen(), juce::Justification::right);

    // The view selector: the current view's name, with a triangle pointing up (the menu opens upwards), in a frame with
    // the same clearance as HOLD's: 8 above the capitals and below the baseline, 14 either side (the box is as wide as
    // the longest name, so it doesn't change size as the view does).
    {
        const auto area = controlArea(Control::view);
        g.setColour(design::meterAxisText.withAlpha(0.95f));
        drawTextAt(g, viewFont, viewName(view), area.getX() + 14.0f * s, titleY, juce::Justification::left);
        juce::Path triangle;
        const auto cx = area.getRight() - 20.0f * s, cy = titleY - 6.5f * s;
        triangle.addTriangle(cx - 6.0f * s, cy + 3.5f * s, cx + 6.0f * s, cy + 3.5f * s, cx, cy - 4.0f * s);
        g.fillPath(triangle);
        g.setColour(design::meterAxisText.withAlpha(0.45f));
        g.drawRect(juce::Rectangle<float>::leftTopRightBottom(area.getX(), titleY - 13.0f * s - 8.0f * s, area.getRight(),
                                                              titleY + 8.0f * s),
                   thin);
    }
    if (isFrozen()) // a frame round HOLD while the picture is frozen
    {
        g.setColour(design::meterAxisText.withAlpha(0.6f));
        // Equal clearance round the drawn letters: the text's width includes the kerning after its last letter, so
        // the right edge is pulled in by that much. Capitals are 13 high above the baseline.
        const auto hold = controlArea(Control::hold);
        const auto textRight = hold.getRight() - 14.0f * s;
        const auto trailing = viewFont.getExtraKerningFactor() * viewFont.getHeight();
        const auto textLeft = textRight - textWidth(viewFont, controlText((int)Control::hold));
        const auto margin = 8.0f * s;
        g.setColour(design::meterAxisText.withAlpha(0.6f));
        g.drawRect(juce::Rectangle<float>::leftTopRightBottom(textLeft - margin, titleY - 13.0f * s - margin,
                                                              textRight - trailing + margin, titleY + margin),
                   thin);
    }

    // Separator and the overall column's axis.
    g.setColour(design::meterAxisText.withAlpha(0.9f));
    g.drawLine(lx(design::meterSeparatorX), ly(622.0f), lx(design::meterSeparatorX), ly(1097.0f), 3.0f * s);
    const auto axisLeft = lx(design::meterOverallAxisX + 45.0f), axisRight = lx(design::meterOverallRight + 25.0f);
    g.setColour(design::meterGrid);
    g.drawLine(axisLeft, top, axisRight, top, thin);
    g.drawLine(axisLeft, bottom, axisRight, bottom, thin);
    g.setColour(design::meterAxisText);
    g.drawLine(axisLeft, zero, axisRight, zero, line);
    const auto overallAxis = lx(design::meterOverallAxisX);
    drawTextAt(g, axisFont, "+1", overallAxis, top + 10.0f * s, juce::Justification::horizontallyCentred);
    drawTextAt(g, axisFont, "0", overallAxis, zero + 10.0f * s, juce::Justification::horizontallyCentred);
    drawTextAt(g, axisFont, "-1", overallAxis, bottom + 10.0f * s, juce::Justification::horizontallyCentred);
    g.setColour(design::meterAxisText.withAlpha(0.6f));
    drawTextAt(g, viewFont, "ALL", 0.5f * (lx(design::meterOverallLeft) + lx(design::meterOverallRight)), titleY,
               juce::Justification::horizontallyCentred);
}

void MeterScreen::paintLive(juce::Graphics& g) const
{
    if (view == View::bands)
        paintBands(g);
    else if (view == View::vector)
        paintVector(g);
    else
        paintScope(g);
    paintPreviewNote(g);
    paintOverall(g);
}

void MeterScreen::paintBands(juce::Graphics& g) const
{
    // Per band: the output as a bar from zero, the input as a tick across the slot, and the output's value.
    const auto s = getScale();
    const auto left = lx(design::meterPlotLeft), right = lx(design::meterPlotRight);
    const auto slot = (right - left) / (float)meter::CorrelationAnalyser::numBands;
    const auto valueFont = meterFont(assets, 16.0f * s, true);
    const auto& x = shownBandX;
    const auto& z = shownBandZ;
    for (int b = 0; b < meter::CorrelationAnalyser::numBands; ++b)
    {
        const auto centre = left + slot * ((float)b + 0.5f);
        const auto r = x[(size_t)b];
        if (shown[(size_t)Series::output] && ! std::isnan(r))
        {
            // Segmented like the overall bar (the unlit ones are in the static layer).
            paintSegments(g, centre - 0.2f * slot, centre + 0.2f * slot, r, false);
            // The value sits past the bar's end, or inside it when that would run into the legend or the axis, on a
            // black patch so the segments don't cut through it.
            const auto y = yForR(r);
            const auto above = r >= 0.0f;
            const auto inside = std::abs(r) > 0.8f;
            const auto textY = above ? (inside ? y + 24.0f * s : y - 8.0f * s) : (inside ? y - 8.0f * s : y + 22.0f * s);
            const juce::String text = (r > 0.0f ? "+" : "") + juce::String(r, 2);
            g.setColour(design::screenBlack.withAlpha(0.85f));
            g.fillRect(juce::Rectangle<float>(textWidth(valueFont, text) + 10.0f * s, 20.0f * s)
                           .withCentre({centre, textY - 6.0f * s}));
            g.setColour(design::phosphor);
            drawTextAt(g, valueFont, text, centre, textY, juce::Justification::horizontallyCentred);
        }
        if (shown[(size_t)Series::input] && ! std::isnan(z[(size_t)b]))
        {
            g.setColour(design::meterInput.withAlpha(0.95f));
            g.fillRect(juce::Rectangle<float>(0.64f * slot, juce::jmax(2.0f, 3.0f * s))
                           .withCentre({centre, yForR(z[(size_t)b])}));
        }
    }
}

void MeterScreen::paintScope(juce::Graphics& g) const
{
    // The three streams around the trigger, on top of each other, each scaled to its own peak: this is for comparing
    // shapes and lining up where the hit starts. The sidechain is the pale one, the input dim green, the output
    // bright green (the preview, while there is one, in its place). With CAPTURE on and a hit held, all three come from
    // the held hit, and the bright one is the input as the knobs would make it (HitCapture::render).
    const auto s = getScale();
    const auto left = lx(design::meterPlotLeft), right = lx(design::meterPlotRight);
    const auto top = ly(design::meterPlusOneY), bottom = ly(design::meterMinusOneY);
    const auto held = captureMode && hitCapture.valid();
    const auto fs = held ? hitCapture.getSampleRate() : scopeBuffer.getSampleRate();
    const auto trigger = held ? hitCapture.onsetIndex() : triggerIndex;
    const auto lastSample = held ? hitCapture.lastIndex() : scopeBuffer.end() - 1;
    const auto streamAt = [&](int stream, double i)
    { return held ? hitCapture.interpolated(stream, i) : scopeBuffer.interpolated(stream, i); };
    const auto labelFont = meterFont(assets, 14.0f * s);
    const auto labelY = ly(design::meterFreqLabelY) + 7.0f * s;

    if (trigger < 0 || fs <= 0.0)
    {
        g.setColour(design::meterAxisText.withAlpha(0.5f));
        drawTextAt(g, meterFont(assets, 16.0f * s, true), "WAITING FOR A HIT ON THE SIDECHAIN",
                   0.5f * (left + right), ly(design::meterZeroY) + 6.0f * s, juce::Justification::horizontallyCentred);
        return;
    }

    const auto width = right - left;
    const auto spanSamples = scopeSpanMs * fs / 1000.0;
    const auto firstSample = (double)trigger - 0.25 * spanSamples; // the hit starts a quarter of the way in
    const auto xOf = [&](double sampleIndex) { return left + (float)((sampleIndex - firstSample) / spanSamples) * width; };
    const auto previewSamples = analyser.isPreviewing() ? analyser.previewDelayMs() * fs / 1000.0 : 0.0;

    // Time ticks, in ms from the hit, at a round step with about ten across.
    {
        double step = 0.1;
        for (const auto candidate : {0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0})
        {
            step = candidate;
            if (scopeSpanMs / candidate <= 10.0)
                break;
        }
        const auto dashes = std::array<float, 2>{3.0f * s, 6.0f * s};
        for (auto t = std::ceil(-0.25 * scopeSpanMs / step) * step; t <= 0.75 * scopeSpanMs + 1.0e-9; t += step)
        {
            const auto x = left + (float)((t + 0.25 * scopeSpanMs) / scopeSpanMs) * width;
            if (std::abs(t) > 1.0e-9)
            {
                g.setColour(design::meterGrid.withAlpha(0.7f));
                g.drawDashedLine({x, top, x, bottom}, dashes.data(), 2, juce::jmax(1.0f, 1.2f * s));
            }
            const auto text = std::abs(t) < 1.0e-9 ? juce::String("0")
                                                    : (t > 0 ? "+" : "") + juce::String(t, step < 1.0 ? 1 : 0);
            g.setColour(design::meterAxisText);
            drawTextAt(g, labelFont, text, juce::jlimit(left + 8.0f * s, right - 8.0f * s, x), labelY,
                       juce::Justification::horizontallyCentred);
        }
        g.setColour(design::meterAxisText.withAlpha(0.9f)); // the trigger
        const auto tx = xOf((double)trigger);
        g.drawLine(tx, top, tx, bottom, juce::jmax(1.0f, 1.5f * s));
    }

    // One trace: `sampleAt(index)` for a (possibly fractional) index. Dense spans draw a min-to-max line per pixel,
    // sparse ones join the samples.
    const auto drawTrace = [&](const std::function<float(double)>& sampleAt, juce::Colour colour, float thickness)
    {
        const auto pixels = std::max(1, (int)width);
        const auto perPixel = spanSamples / (double)pixels;
        const auto lastIndex = lastSample;

        // Its own peak over the window.
        float peak = 1.0e-6f;
        const auto count = std::min<long long>((long long)spanSamples + 2, 20000);
        const auto stride = std::max(1.0, spanSamples / (double)count);
        for (double i = firstSample; i < firstSample + spanSamples; i += stride)
            peak = std::max(peak, std::abs(sampleAt(i)));
        const auto yOf = [&](float v) { return yForR(0.95f * v / peak); };

        g.setColour(colour);
        const auto clip = juce::Rectangle<float>::leftTopRightBottom(left, top - 4.0f * s, right, bottom + 4.0f * s);
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(clip.getSmallestIntegerContainer());
        if (perPixel <= 1.5)
        {
            juce::Path path;
            bool started = false;
            const auto from = (long long)std::floor(firstSample) - 1, to = (long long)std::ceil(firstSample + spanSamples) + 1;
            for (auto i = from; i <= to && i <= lastIndex; ++i)
            {
                const auto x = xOf((double)i), y = yOf(sampleAt((double)i));
                if (! started)
                {
                    path.startNewSubPath(x, y);
                    started = true;
                }
                else
                    path.lineTo(x, y);
            }
            g.strokePath(path, juce::PathStrokeType(thickness, juce::PathStrokeType::curved));
        }
        else
        {
            float previousMid = 0.0f;
            bool havePrevious = false;
            for (int px = 0; px < pixels; ++px)
            {
                const auto a = firstSample + perPixel * px, b = a + perPixel;
                if ((long long)a > lastIndex)
                    break;
                float lo = 1.0e9f, hi = -1.0e9f;
                const auto step = std::max(1.0, perPixel / 12.0);
                for (double i = a; i < b; i += step)
                {
                    const auto v = sampleAt(i);
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                const auto x = left + (float)px + 0.5f;
                const auto yHi = yOf(hi), yLo = yOf(lo);
                // Join to the previous column so a rising or falling stretch stays one line.
                if (havePrevious)
                    g.drawLine(x - 1.0f, previousMid, x, 0.5f * (yHi + yLo), thickness);
                g.drawLine(x, yHi, x, std::max(yLo, yHi + 0.5f), thickness);
                previousMid = 0.5f * (yHi + yLo);
                havePrevious = true;
            }
        }
    };

    const auto thickness = juce::jmax(1.0f, 1.6f * s);
    if (shown[(size_t)Series::sidechain])
        drawTrace([&](double i) { return streamAt(2, i); }, design::meterAxisText.withAlpha(0.85f), thickness);
    if (shown[(size_t)Series::input])
        drawTrace([&](double i) { return streamAt(0, i); }, design::meterInput.withAlpha(0.85f), thickness);
    if (shown[(size_t)Series::output])
    {
        if (held) // the captured input as the knobs would make it
            drawTrace([&](double i) { return streamAt(1, i); }, design::phosphor, juce::jmax(1.0f, 2.2f * s));
        else if (analyser.isPreviewing()) // the input slid later by the preview delay
            drawTrace([&](double i)
                      { return (analyser.isPreviewInverted() ? -1.0f : 1.0f) * scopeBuffer.interpolated(0, i - previewSamples); },
                      design::phosphor, juce::jmax(1.0f, 2.2f * s));
        else
            drawTrace([&](double i) { return streamAt(1, i); }, design::phosphor, juce::jmax(1.0f, 2.2f * s));
    }

    // The span, the CAPTURE toggle and what the bright trace is, over the traces on a dark patch.
    {
        const auto small = meterFont(assets, 12.0f * s).withExtraKerningFactor(0.1f);
        const auto toggle = captureToggleArea();
        const auto patchWidth = 232.0f * s;
        g.setColour(design::screenBlack.withAlpha(0.8f));
        g.fillRect(juce::Rectangle<float>(left + 6.0f * s, top + 6.0f * s, patchWidth, captureMode ? 62.0f * s : 40.0f * s));
        g.setColour(design::meterAxisText.withAlpha(0.6f));
        drawTextAt(g, small, "SPAN " + juce::String(scopeSpanMs, scopeSpanMs < 10.0 ? 1 : 0) + " MS", left + 14.0f * s,
                   top + 20.0f * s, juce::Justification::left);

        // The zoom buttons.
        for (const auto zoomIn : {false, true})
        {
            const auto b = zoomButtonArea(zoomIn);
            g.setColour(design::meterAxisText.withAlpha(0.55f));
            g.drawRect(b, juce::jmax(1.0f, 1.0f * s));
            g.setColour(design::meterAxisText.withAlpha(0.9f));
            const auto c = b.getCentre();
            const auto arm = 5.0f * s, w = juce::jmax(1.0f, 1.5f * s);
            g.drawLine(c.x - arm, c.y, c.x + arm, c.y, w);
            if (zoomIn)
                g.drawLine(c.x, c.y - arm, c.x, c.y + arm, w);
        }
        g.setColour(design::meterAxisText.withAlpha(captureMode ? 0.95f : 0.45f));
        drawTextAt(g, small, captureMode ? "CAPTURE  ON" : "CAPTURE  OFF", toggle.getX() + 8.0f * s, top + 41.0f * s,
                   juce::Justification::left);
        g.setColour(design::meterAxisText.withAlpha(0.45f));
        g.drawRect(juce::Rectangle<float>(toggle.getX(), toggle.getY(), textWidth(small, "CAPTURE  OFF") + 20.0f * s, toggle.getHeight()),
                   juce::jmax(1.0f, 1.0f * s));
        if (captureMode)
        {
            g.setColour(design::meterAxisText.withAlpha(0.6f));
            drawTextAt(g, small, held ? "OUTPUT: THIS HIT THROUGH THE KNOBS" : "WAITING FOR A HIT TO CAPTURE",
                       left + 14.0f * s, top + 62.0f * s, juce::Justification::left);
        }
    }
}

void MeterScreen::vectorPoints(std::vector<juce::Point<float>>& input, std::vector<juce::Point<float>>& output,
                               VectorReading& reading) const
{
    // The last 60 ms of the three streams (frozen: of the held window, with the output through the knobs), each pair
    // scaled so both have a standard deviation of 1 and turned 45 degrees: u = (a - b) / sqrt 2 across, v = (a + b) /
    // sqrt 2 up, with a this track and b the sidechain. Identical signals fall on the v axis.
    input.clear();
    output.clear();
    reading = {};
    const auto frozenWindow = isFrozen() && vectorCapture.valid();
    const auto fs = frozenWindow ? vectorCapture.getSampleRate() : scopeBuffer.getSampleRate();
    if (fs <= 0.0)
        return;
    const auto count = (long long)(0.06 * fs);
    const auto last = frozenWindow ? vectorCapture.lastIndex() : scopeBuffer.end() - 1;
    if (! frozenWindow && scopeBuffer.total() < count + 8)
        return;
    const auto at = [&](int stream, long long i)
    { return frozenWindow ? vectorCapture.at(stream, i) : scopeBuffer.at(stream, i); };

    if (isVectorStereo())
    {
        // The track's own left and right: M is (L + R) / 2 and S is (L - R) / 2, so L = M + S and R = M - S. Across is
        // (R - L) / sqrt 2 = -sqrt 2 S (so L lies on the left) and up is (L + R) / sqrt 2 = sqrt 2 M, both over the standard deviation of L and R
        // together, so the shape's size doesn't follow the level.
        const auto step = std::max<long long>(1, count / 3000);
        const auto stereoSet = [&](int streamM, int streamS, std::vector<juce::Point<float>>& points, float& r)
        {
            double ll = 0.0, rr = 0.0, lr = 0.0;
            for (long long i = last - count + 1; i <= last; ++i)
            {
                const double m = at(streamM, i), side = at(streamS, i);
                const auto l = m + side, rch = m - side;
                ll += l * l;
                rr += rch * rch;
                lr += l * rch;
            }
            const auto sigma = std::sqrt(0.5 * (ll + rr) / (double)count);
            if (sigma < 1.0e-6 || ll <= 0.0 || rr <= 0.0)
                return false;
            r = (float)juce::jlimit(-1.0, 1.0, lr / std::sqrt(ll * rr));
            for (long long i = last - count + 1; i <= last; i += step)
            {
                const double m = at(streamM, i), side = at(streamS, i);
                points.push_back({(float)(-1.41421356 * side / sigma), (float)(1.41421356 * m / sigma)}); // L on the left
            }
            return true;
        };
        const auto okInput = stereoSet(0, 3, input, reading.inputR);
        const auto okOutput = stereoSet(1, 4, output, reading.outputR);
        reading.valid = okInput && okOutput;
        reading.stereo = true;
        return;
    }

    double sum2[3] = {0, 0, 0};
    for (long long i = last - count + 1; i <= last; ++i)
        for (int stream = 0; stream < 3; ++stream)
            sum2[stream] += (double)at(stream, i) * at(stream, i);
    const auto n = (double)count;
    const double sigma[3] = {std::sqrt(sum2[0] / n), std::sqrt(sum2[1] / n), std::sqrt(sum2[2] / n)};
    if (sigma[0] < 1.0e-6 || sigma[1] < 1.0e-6 || sigma[2] < 1.0e-6)
        return;

    const auto step = std::max<long long>(1, count / 3000);
    double crossInput = 0.0, crossOutput = 0.0;
    for (long long i = last - count + 1; i <= last; ++i)
    {
        const auto b = (double)at(2, i) / sigma[2];
        const auto aIn = (double)at(0, i) / sigma[0], aOut = (double)at(1, i) / sigma[1];
        crossInput += aIn * b;
        crossOutput += aOut * b;
        if ((i - (last - count + 1)) % step == 0)
        {
            const auto root = 0.70710678;
            input.push_back({(float)((aIn - b) * root), (float)((aIn + b) * root)});
            output.push_back({(float)((aOut - b) * root), (float)((aOut + b) * root)});
        }
    }
    reading.inputR = (float)juce::jlimit(-1.0, 1.0, crossInput / n);
    reading.outputR = (float)juce::jlimit(-1.0, 1.0, crossOutput / n);
    reading.valid = true;
}

MeterScreen::VectorReading MeterScreen::vectorReading() const
{
    std::vector<juce::Point<float>> input, output;
    VectorReading reading;
    vectorPoints(input, output, reading);
    return reading;
}

void MeterScreen::paintVector(juce::Graphics& g) const
{
    // The input (blue) and the output (green) each against the sidechain, as paths through the last 60 ms of samples,
    // the older part fainter. Both scaled to their own levels, so level differences don't tilt the shape: only the phase
    // relation shows.
    const auto s = getScale();
    const auto left = lx(design::meterPlotLeft), right = lx(design::meterPlotRight);
    const auto top = ly(design::meterPlusOneY), zero = ly(design::meterZeroY), bottom = ly(design::meterMinusOneY);
    const auto cx = 0.5f * (left + right), radius = 0.5f * (bottom - top) - 8.0f * s;
    const auto scale = radius / 2.4f; // 2.4 standard deviations reach the circle

    // The source toggle, on a stereo track.
    if (capture.isStereoTrack())
    {
        const auto area = vectorModeArea();
        const auto small = meterFont(assets, 12.0f * s).withExtraKerningFactor(0.1f);
        const auto label = juce::String("SOURCE  ") + (isVectorStereo() ? "STEREO" : "SIDECHAIN");
        g.setColour(design::meterAxisText.withAlpha(0.95f));
        drawTextAt(g, small, label, area.getX() + 8.0f * s, area.getY() + 15.0f * s, juce::Justification::left);
        g.setColour(design::meterAxisText.withAlpha(0.45f));
        g.drawRect(juce::Rectangle<float>(area.getX(), area.getY(), textWidth(small, "SOURCE  SIDECHAIN") + 20.0f * s, area.getHeight()),
                   juce::jmax(1.0f, 1.0f * s));
    }

    std::vector<juce::Point<float>> input, output;
    VectorReading reading;
    vectorPoints(input, output, reading);
    if (! reading.valid)
        return;

    const auto clip = juce::Rectangle<float>(left, top - 2.0f * s, right - left, bottom - top + 4.0f * s);
    juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(clip.getSmallestIntegerContainer());
    const auto drawSet = [&](const std::vector<juce::Point<float>>& points, juce::Colour colour, float size)
    {
        // Dots (a goniometer plots the samples, not lines between them), the older third fainter.
        const auto total = (int)points.size();
        for (int part = 0; part < 3; ++part) // oldest, middle, newest
        {
            g.setColour(colour.withMultipliedAlpha(part == 0 ? 0.3f : part == 1 ? 0.55f : 1.0f));
            for (int i = total * part / 3; i < total * (part + 1) / 3; ++i)
                g.fillRect(cx + points[(size_t)i].x * scale - 0.5f * size, zero - points[(size_t)i].y * scale - 0.5f * size,
                           size, size);
        }
    };
    if (shown[(size_t)Series::input])
        drawSet(input, design::meterInput, juce::jmax(1.5f, 1.8f * s));
    if (shown[(size_t)Series::output])
        drawSet(output, design::phosphor, juce::jmax(1.5f, 2.0f * s));

    // The correlations over the window, top left.
    const auto font = meterFont(assets, 16.0f * s, true);
    const auto text = [](float r) { return (r > 0.0f ? "+" : "") + juce::String(r, 2); };
    g.setColour(design::screenBlack.withAlpha(0.85f));
    g.fillRect(juce::Rectangle<float>(left + 6.0f * s, top + 6.0f * s, 250.0f * s, 46.0f * s));
    const auto what = reading.stereo ? juce::String("L/R r ") : juce::String("r ");
    if (shown[(size_t)Series::output])
    {
        g.setColour(design::phosphor);
        drawTextAt(g, font, "OUTPUT  " + what + text(reading.outputR), left + 14.0f * s, top + 24.0f * s,
                   juce::Justification::left);
    }
    if (shown[(size_t)Series::input])
    {
        g.setColour(design::meterInput);
        drawTextAt(g, font, "INPUT   " + what + text(reading.inputR), left + 14.0f * s, top + 44.0f * s,
                   juce::Justification::left);
    }
}

void MeterScreen::paintPreviewNote(juce::Graphics& g) const
{
    if (view == View::scope && captureMode && hitCapture.valid())
        return; // the held hit is the preview: the bright trace already is the knobs' result
    const auto s = getScale();
    const auto x = lx(design::meterPlotLeft) + 14.0f * s;
    if (analyser.isPreviewing())
    {
        const auto ms = std::abs(analyser.previewDelayMs()) < 0.0005 ? 0.0 : analyser.previewDelayMs();
        const auto font = meterFont(assets, 16.0f * s, true);
        const auto y = view == View::bands ? ly(design::meterMinusOneY) - 14.0f * s
                                           : ly(design::meterPlusOneY) + (view == View::scope ? 66.0f : 84.0f) * s;
        const juce::String note = "PREVIEW  DELAY " + juce::String(ms >= 0.0 ? "+" : "") + juce::String(ms, 3) + " ms" +
                                  (analyser.isPreviewInverted() ? "  INVERTED" : "") +
                                  (analyser.previewHasPhase() && view == View::scope ? "  (PHASE NOT SHOWN HERE)" : "");
        g.setColour(design::screenBlack.withAlpha(0.85f));
        g.fillRect(juce::Rectangle<float>(textWidth(font, note) + 16.0f * s, 22.0f * s).withX(x - 8.0f * s).withY(y - 16.0f * s));
        g.setColour(design::phosphor);
        drawTextAt(g, font, note, x, y, juce::Justification::left);
    }
    else if (isFrozen() && view == View::scope)
    {
        g.setColour(design::meterAxisText.withAlpha(0.5f));
        drawTextAt(g, meterFont(assets, 12.0f * s), "DRAG TO SLIDE THE INPUT", x, ly(design::meterPlusOneY) + 66.0f * s,
                   juce::Justification::left);
    }
}

void MeterScreen::paintOverall(juce::Graphics& g) const
{
    const auto s = getScale();
    const auto left = lx(design::meterOverallLeft), right = lx(design::meterOverallRight);
    const auto x = shownOverallX, z = shownOverallZ;
    if (shown[(size_t)Series::output] && ! std::isnan(x))
        paintSegments(g, left, right, x, false);
    if (shown[(size_t)Series::input] && ! std::isnan(z)) // unprocessed: a tick across the bar
    {
        g.setColour(design::meterInput.withAlpha(0.95f));
        g.fillRect(juce::Rectangle<float>(left - 6.0f * s, yForR(z) - 1.5f * s, right - left + 12.0f * s, 3.0f * s));
    }
}

void MeterScreen::paintSegments(juce::Graphics& g, float x0, float x1, float r, bool unlitToo) const
{
    // Ten segments each side of zero; those between zero and r are lit, the rest drawn faintly (by the static layer).
    // The segment at the end of the bar fades in with the fraction of it reached, so a moving value glides.
    const auto s = getScale();
    const auto zero = ly(design::meterZeroY);
    const auto pitch = (design::meterZeroY - design::meterPlusOneY) * s / (float)segmentsPerHalf;
    const auto height = pitch * segmentFill;
    const auto level = std::abs(r) * (float)segmentsPerHalf;
    const auto whole = (int)std::floor(level);
    const auto fraction = level - (float)whole;

    for (int i = 0; i < segmentsPerHalf; ++i)
    {
        const auto offset = pitch * (float)i + 0.5f * (pitch - height);
        const auto above = juce::Rectangle<float>(x0, zero - offset - height, x1 - x0, height);
        const auto below = juce::Rectangle<float>(x0, zero + offset, x1 - x0, height);
        const auto reach = i < whole ? 1.0f : i == whole ? fraction : 0.0f; // how far into this segment the bar reaches
        const auto litAbove = r > 0.0f && reach > 0.0f, litBelow = r < 0.0f && reach > 0.0f;
        if (litAbove || unlitToo)
        {
            g.setColour(design::phosphor.withAlpha(litAbove ? 0.08f + 0.92f * reach : 0.08f));
            g.fillRect(above);
        }
        if (litBelow || unlitToo)
        {
            g.setColour(design::phosphor.withAlpha(litBelow ? 0.08f + 0.67f * reach : 0.08f));
            g.fillRect(below);
        }
    }
}
} // namespace pa::ui
