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

// Frequency view grid lines and labels.
constexpr float gridHz[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
constexpr float labelHz[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};

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
    setTooltip("Click the middle label to choose the view: FREQUENCY, the correlation with the sidechain per "
               "frequency; TIME OFFSET, how far this track is from the sidechain, by waveform and by attack (for "
               "drums); PHASE, the angle between them (a slope is a delay, a flat offset a rotation); BANDS, the "
               "correlation in six wide bands; ALIGNMENT, the waveforms on top of each other around a hit (wheel "
               "to zoom). SLOW or FAST sets the averaging. HOLD freezes the screen (it also freezes while the host is "
               "stopped); frozen, turn DELAY or the polarity button, drag across TIME OFFSET, or slide the input in ALIGNMENT, to preview a delay on every "
               "view. INPUT is this track before the plugin, OUTPUT after it.");
    for (auto& s : scratch)
        s.resize((size_t)meter::MeterCapture::capacity);
    if (capture.getSampleRate() > 0.0)
    {
        analyser.prepare(capture.getSampleRate());
        scopeBuffer.prepare(capture.getSampleRate());
    }
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
    if (view == View::time)
        analyser.computeLag(isFrozen());
    if (view == View::scope)
        refreshTrigger(true);
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
    const std::pair<View, const char*> items[] = {{View::frequency, "FREQUENCY"}, {View::time, "TIME OFFSET"},
                                                  {View::phase, "PHASE"},         {View::bands, "BANDS"},
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
        if (view == View::time)
            analyser.computeLag(true);
    }
    else
        analyser.clearPreview(); // a preview belongs to the frozen picture
    repaint();
}

void MeterScreen::setHeldSettings(double delayMs, bool inverted, meter::CorrelationAnalyser::PhaseResponse phase)
{
    if (! isFrozen())
        return;
    const auto reach = (double)params::maxDelayMs;
    const auto sampleMs = 1000.0 / juce::jmax(1.0, analyser.getSampleRate());
    const auto step = 0.1 * sampleMs;
    analyser.setPreview(juce::jlimit(-reach, reach, std::round(delayMs / step) * step), inverted, std::move(phase));
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

bool MeterScreen::poll()
{
    const auto fs = capture.getSampleRate();
    if (! juce::exactlyEqual(fs, analyser.getSampleRate()))
        analyser.prepare(fs);
    if (! juce::exactlyEqual(fs, scopeBuffer.getSampleRate()))
        scopeBuffer.prepare(fs);

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
                     dest[meter::MeterCapture::sidechain], n);
    const auto frames = analyser.process(dest[meter::MeterCapture::input], dest[meter::MeterCapture::output],
                                         dest[meter::MeterCapture::sidechain], n);
    if (frames > 0 && view == View::time)
        analyser.computeLag();
    return frames > 0;
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

void MeterScreen::timerCallback()
{
    // A host can hide the window without telling its children; stop (and stop capturing) on the next tick.
    if (isTimerRunning() && ! isShowing())
        updateTimer();

    const auto fresh = isTimerRunning() && poll();
    freezeChanged(); // the host's transport can stop or start under us
    if (view == View::scope && ++triggerTick % 3 == 0)
        refreshTrigger(false);
    const auto newState = currentState();
    if (newState == state && ! (fresh && state == State::metering))
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

float MeterScreen::xForHz(float hz) const
{
    const auto left = lx(design::meterPlotLeft), right = lx(design::meterPlotRight);
    return left + (right - left) * std::log(hz / 20.0f) / std::log(1000.0f);
}

float MeterScreen::xForMs(double ms) const
{
    const auto left = lx(design::meterPlotLeft), right = lx(design::meterPlotRight);
    const auto range = meter::CorrelationAnalyser::lagRangeMs;
    return left + (right - left) * (float)((ms + range) / (2.0 * range));
}

double MeterScreen::msForX(float x) const
{
    const auto left = lx(design::meterPlotLeft), right = lx(design::meterPlotRight);
    const auto range = meter::CorrelationAnalyser::lagRangeMs;
    return ((double)(x - left) / (double)(right - left) * 2.0 - 1.0) * range;
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
        case MeterScreen::View::frequency: return "FREQUENCY";
        case MeterScreen::View::time: return "TIME OFFSET";
        case MeterScreen::View::phase: return "PHASE";
        case MeterScreen::View::bands: return "BANDS";
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
            const auto w = textWidth(font, "TIME OFFSET") + 2.0f * pad + 26.0f * s;
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
    return isFrozen() && (view == View::time || view == View::scope) && state == State::metering &&
           juce::Rectangle<float>::leftTopRightBottom(lx(design::meterPlotLeft), ly(design::meterPlusOneY),
                                                      lx(design::meterPlotRight), ly(design::meterMinusOneY))
               .contains(p);
}

bool MeterScreen::hitTest(int x, int y)
{
    if (state == State::off)
        return false;
    const auto p = juce::Point<int>(x, y).toFloat();
    return controlAt(p) != Control::none || scrubArea(p);
}

void MeterScreen::scrubTo(float x)
{
    if (view == View::scope)
    {
        // Sliding: the input moves with the mouse, so the delay is how far it has been dragged, from where it was.
        const auto width = lx(design::meterPlotRight) - lx(design::meterPlotLeft);
        setPreviewDelayMs(slipStartMs + (double)(x - slipStartX) * scopeSpanMs / (double)width);
    }
    else
        setPreviewDelayMs(msForX(x));
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
                     juce::String((int)analyser.getSpeed()) + ":" + juce::String((int)isFrozen());
    layer.paint(g, getLocalBounds(), key,
                [this](juce::Graphics& lg, float pixelScale) { paintStatic(lg, pixelScale); });
    if (state == State::metering)
        paintLive(g);
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

    // Vertical grid and x labels for the view.
    g.setColour(design::meterGrid.withAlpha(0.7f));
    if (view == View::bands)
    {
        // Six slots; the labels say which band each is.
        g.setColour(design::meterAxisText);
        const auto slot = (right - left) / (float)meter::CorrelationAnalyser::numBands;
        for (int b = 0; b < meter::CorrelationAnalyser::numBands; ++b)
        {
            const auto lo = (float)meter::CorrelationAnalyser::bandEdgesHz[b],
                       hi = (float)meter::CorrelationAnalyser::bandEdgesHz[b + 1];
            drawTextAt(g, labelFont, hzLabel(lo) + "-" + hzLabel(hi),
                       left + slot * ((float)b + 0.5f), labelY, juce::Justification::horizontallyCentred);
            if (b > 0)
                g.drawDashedLine({left + slot * (float)b, top, left + slot * (float)b, bottom}, dashes, 2, thin);
        }
    }
    else if (view == View::scope)
    {
        // The time ticks move with the zoom, so they are drawn with the traces.
    }
    else if (view != View::time)
    {
        for (const auto hz : gridHz)
            g.drawDashedLine({xForHz(hz), top, xForHz(hz), bottom}, dashes, 2, thin);
        g.setColour(design::meterAxisText);
        for (const auto hz : labelHz)
            drawTextAt(g, labelFont, hzLabel(hz), juce::jlimit(left + 8.0f * s, right - 8.0f * s, xForHz(hz)), labelY,
                       juce::Justification::horizontallyCentred);
    }
    else
    {
        // The delay knob's reach: this track can be moved by -4 to +4 ms.
        const auto reach = (double)params::maxDelayMs;
        g.setColour(design::phosphor.withAlpha(0.05f));
        g.fillRect(juce::Rectangle<float>::leftTopRightBottom(xForMs(-reach), top, xForMs(reach), bottom));
        g.setColour(design::meterGrid.withAlpha(0.7f));
        for (int ms = -4; ms <= 4; ++ms)
            if (ms != 0)
                g.drawDashedLine({xForMs(ms), top, xForMs(ms), bottom}, dashes, 2, thin);
        g.setColour(design::meterAxisText);
        g.drawLine(xForMs(0.0), top, xForMs(0.0), bottom, thin);
        for (int ms = -5; ms <= 5; ++ms)
            drawTextAt(g, labelFont, ms > 0 ? "+" + juce::String(ms) : juce::String(ms),
                       juce::jlimit(left + 8.0f * s, right - 8.0f * s, xForMs(ms)), labelY,
                       juce::Justification::horizontallyCentred);
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

    const auto axisFont = meterFont(assets, 20.0f * s);
    // The phase view's labels are one character wider, so a size smaller to fit left of the plot.
    const auto yFont = view == View::phase ? meterFont(assets, 16.0f * s) : axisFont;
    const auto yLabel = [&](const char* text, float atY, float atX, juce::Justification j)
    { drawTextAt(g, yFont, text, atX, atY + 10.0f * s, j); };
    const auto isPhase = view == View::phase;
    yLabel(isPhase ? "180" : "+1", top, left - 16.0f * s, juce::Justification::right);
    yLabel("0", zero, left - 16.0f * s, juce::Justification::right);
    yLabel(isPhase ? "-180" : "-1", bottom, left - 16.0f * s, juce::Justification::right);

    // Legend, top right of the plot.
    const auto legendFont = meterFont(assets, 14.0f * s).withExtraKerningFactor(0.1f);
    const auto legendY = ly(design::meterPlusOneY) - 12.0f * s;
    g.setColour(design::phosphor);
    drawTextAt(g, legendFont, "OUTPUT", right, legendY, juce::Justification::right);
    const auto processedWidth = textWidth(legendFont, "OUTPUT");
    g.setColour(design::phosphor.withAlpha(0.45f));
    drawTextAt(g, legendFont, "INPUT", right - processedWidth - 24.0f * s, legendY, juce::Justification::right);

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
    if (view == View::frequency)
    {
        const auto& hz = analyser.curveFrequencies();
        const auto xAt = [&](int i) { return xForHz(hz[(size_t)i]); };
        paintTrace(g, xAt, analyser.curveUnprocessed(), false);
        paintTrace(g, xAt, analyser.curveProcessed(), true);
    }
    else if (view == View::phase)
    {
        paintPhase(g);
    }
    else if (view == View::bands)
    {
        paintBands(g);
    }
    else if (view == View::scope)
    {
        paintScope(g);
    }
    else
    {
        const auto xAt = [&](int i) { return xForMs(analyser.lagMsAt(i)); };
        paintTrace(g, xAt, analyser.lagUnprocessed(), false);
        paintTrace(g, xAt, analyser.lagProcessed(), true);
        paintDashedTrace(g, xAt, analyser.attackUnprocessed(), false);
        paintDashedTrace(g, xAt, analyser.attackProcessed(), true);
        paintLagReadout(g);
    }
    paintPreviewNote(g);
    paintOverall(g);
}

void MeterScreen::paintPhase(juce::Graphics& g) const
{
    // The angle of this track against the sidechain: a straight slope is a delay, a flat offset is a rotation. Each
    // segment is as bright as the weaker of its two points' coherence, and the line breaks where the angle wraps.
    const auto s = getScale();
    const auto& hz = analyser.curveFrequencies();
    const auto plot =
        juce::Rectangle<float>::leftTopRightBottom(lx(design::meterPlotLeft), ly(design::meterPlusOneY) - 4.0f * s,
                                                   lx(design::meterPlotRight), ly(design::meterMinusOneY) + 4.0f * s);
    juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(plot.getSmallestIntegerContainer());

    const auto draw = [&](const std::vector<float>& angle, const std::vector<float>& coherence, bool processed)
    {
        const auto thickness = processed ? juce::jmax(1.0f, 2.2f * s) : juce::jmax(1.0f, 1.6f * s);
        for (size_t i = 0; i + 1 < angle.size(); ++i)
        {
            const auto a = angle[i], b = angle[i + 1];
            if (std::isnan(a) || std::isnan(b))
            {
                if (! std::isnan(a)) // a lone point
                {
                    g.setColour(design::phosphor.withAlpha(processed ? 0.9f : 0.4f));
                    g.fillEllipse(juce::Rectangle<float>(thickness * 1.6f, thickness * 1.6f)
                                      .withCentre({xForHz(hz[i]), yForPhase(a)}));
                }
                continue;
            }
            if (std::abs(a - b) > 180.0f)
                continue; // wrapped
            const auto c = juce::jlimit(0.0f, 1.0f, 0.3f + 0.7f * std::min(coherence[i], coherence[i + 1]));
            g.setColour(design::phosphor.withAlpha((processed ? 1.0f : 0.42f) * c));
            g.drawLine(xForHz(hz[i]), yForPhase(a), xForHz(hz[i + 1]), yForPhase(b), thickness);
        }
    };
    draw(analyser.phaseUnprocessed(), analyser.phaseUnprocessedCoherence(), false);
    draw(analyser.phaseProcessed(), analyser.phaseProcessedCoherence(), true);
}

void MeterScreen::paintBands(juce::Graphics& g) const
{
    // Per band: the output as a bar from zero, the input as a tick across the slot, and the output's value.
    const auto s = getScale();
    const auto left = lx(design::meterPlotLeft), right = lx(design::meterPlotRight);
    const auto slot = (right - left) / (float)meter::CorrelationAnalyser::numBands;
    const auto zero = ly(design::meterZeroY);
    const auto valueFont = meterFont(assets, 16.0f * s, true);
    const auto& x = analyser.bandsProcessed();
    const auto& z = analyser.bandsUnprocessed();
    for (int b = 0; b < meter::CorrelationAnalyser::numBands; ++b)
    {
        const auto centre = left + slot * ((float)b + 0.5f);
        const auto r = x[(size_t)b];
        if (! std::isnan(r))
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
        if (! std::isnan(z[(size_t)b]))
        {
            g.setColour(design::phosphor.withAlpha(0.5f));
            g.fillRect(juce::Rectangle<float>(0.64f * slot, juce::jmax(2.0f, 3.0f * s))
                           .withCentre({centre, yForR(z[(size_t)b])}));
        }
    }
}

void MeterScreen::paintScope(juce::Graphics& g) const
{
    // The three streams around the trigger, on top of each other, each scaled to its own peak: this is for comparing
    // shapes and lining up where the hit starts. The sidechain is the pale one, the input dim green, the output
    // bright green (the preview, while there is one, in its place).
    const auto s = getScale();
    const auto left = lx(design::meterPlotLeft), right = lx(design::meterPlotRight);
    const auto top = ly(design::meterPlusOneY), bottom = ly(design::meterMinusOneY);
    const auto fs = scopeBuffer.getSampleRate();
    const auto labelFont = meterFont(assets, 14.0f * s);
    const auto labelY = ly(design::meterFreqLabelY) + 7.0f * s;

    if (triggerIndex < 0 || fs <= 0.0)
    {
        g.setColour(design::meterAxisText.withAlpha(0.5f));
        drawTextAt(g, meterFont(assets, 16.0f * s, true), "WAITING FOR A HIT ON THE SIDECHAIN",
                   0.5f * (left + right), ly(design::meterZeroY) + 6.0f * s, juce::Justification::horizontallyCentred);
        return;
    }

    const auto width = right - left;
    const auto spanSamples = scopeSpanMs * fs / 1000.0;
    const auto firstSample = (double)triggerIndex - 0.25 * spanSamples; // the hit starts a quarter of the way in
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
        const auto tx = xOf((double)triggerIndex);
        g.drawLine(tx, top, tx, bottom, juce::jmax(1.0f, 1.5f * s));
        g.setColour(design::meterAxisText.withAlpha(0.6f));
        drawTextAt(g, meterFont(assets, 12.0f * s).withExtraKerningFactor(0.1f), "SPAN " + juce::String(scopeSpanMs, scopeSpanMs < 10.0 ? 1 : 0) + " MS",
                   left + 14.0f * s, top + 20.0f * s, juce::Justification::left);
    }

    // One trace: `sampleAt(index)` for a (possibly fractional) index. Dense spans draw a min-to-max line per pixel,
    // sparse ones join the samples.
    const auto drawTrace = [&](const std::function<float(double)>& sampleAt, juce::Colour colour, float thickness)
    {
        const auto pixels = std::max(1, (int)width);
        const auto perPixel = spanSamples / (double)pixels;
        const auto lastIndex = scopeBuffer.end() - 1;

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
    drawTrace([&](double i) { return scopeBuffer.interpolated(2, i); }, design::meterAxisText.withAlpha(0.85f), thickness);
    drawTrace([&](double i) { return scopeBuffer.interpolated(0, i); }, design::phosphor.withAlpha(0.45f), thickness);
    if (analyser.isPreviewing()) // the input slid later by the preview delay
        drawTrace([&](double i) { return (analyser.isPreviewInverted() ? -1.0f : 1.0f) * scopeBuffer.interpolated(0, i - previewSamples); },
                  design::phosphor,
                  juce::jmax(1.0f, 2.2f * s));
    else
        drawTrace([&](double i) { return scopeBuffer.interpolated(1, i); }, design::phosphor, juce::jmax(1.0f, 2.2f * s));

    // The sidechain's name goes with the legend (INPUT and OUTPUT are in the grid).
    const auto legendFont = meterFont(assets, 14.0f * s).withExtraKerningFactor(0.1f);
    const auto legendY = top - 12.0f * s;
    const auto inputRight = right - textWidth(legendFont, analyser.isPreviewing() ? "PREVIEW" : "OUTPUT") - 24.0f * s;
    g.setColour(design::meterAxisText.withAlpha(0.85f));
    drawTextAt(g, legendFont, "SIDECHAIN", inputRight - textWidth(legendFont, "INPUT") - 24.0f * s, legendY,
               juce::Justification::right);
}

void MeterScreen::paintPreviewNote(juce::Graphics& g) const
{
    const auto s = getScale();
    const auto x = lx(design::meterPlotLeft) + 14.0f * s;
    if (analyser.isPreviewing())
    {
        // The legend's OUTPUT becomes PREVIEW: that trace is the input as it would read with the delay set here.
        const auto legendFont = meterFont(assets, 14.0f * s).withExtraKerningFactor(0.1f);
        const auto right = lx(design::meterPlotRight), legendY = ly(design::meterPlusOneY) - 12.0f * s;
        const auto cover = juce::Rectangle<float>(right + 4.0f * s - 440.0f * s, legendY - 17.0f * s, 440.0f * s, 24.0f * s);
        g.setColour(design::screenBlack);
        g.fillRect(cover);
        g.setColour(design::phosphor);
        drawTextAt(g, legendFont, "PREVIEW", right, legendY, juce::Justification::right);
        g.setColour(design::phosphor.withAlpha(0.45f));
        drawTextAt(g, legendFont, "INPUT", right - textWidth(legendFont, "PREVIEW") - 24.0f * s, legendY,
                   juce::Justification::right);
        if (view == View::scope)
        {
            g.setColour(design::meterAxisText.withAlpha(0.85f));
            drawTextAt(g, legendFont, "SIDECHAIN",
                       right - textWidth(legendFont, "PREVIEW") - textWidth(legendFont, "INPUT") - 48.0f * s, legendY,
                       juce::Justification::right);
        }

        const auto ms = std::abs(analyser.previewDelayMs()) < 0.0005 ? 0.0 : analyser.previewDelayMs();
        const auto font = meterFont(assets, 16.0f * s, true);
        // Below the time view's readouts, at the top left elsewhere.
        const auto y = view == View::bands ? ly(design::meterMinusOneY) - 14.0f * s
                                           : ly(design::meterPlusOneY) + (view == View::time ? 112.0f : view == View::scope ? 44.0f : 24.0f) * s;
        const juce::String note = "PREVIEW  DELAY " + juce::String(ms >= 0.0 ? "+" : "") + juce::String(ms, 3) + " ms" +
                                  (analyser.isPreviewInverted() ? "  INVERTED" : "") +
                                  (analyser.previewHasPhase() && view == View::scope ? "  (PHASE NOT SHOWN HERE)" : "");
        g.setColour(design::screenBlack.withAlpha(0.85f));
        g.fillRect(juce::Rectangle<float>(textWidth(font, note) + 16.0f * s, 22.0f * s).withX(x - 8.0f * s).withY(y - 16.0f * s));
        g.setColour(design::phosphor);
        drawTextAt(g, font, "PREVIEW  DELAY " + juce::String(ms >= 0.0 ? "+" : "") + juce::String(ms, 3) + " ms", x, y,
                   juce::Justification::left);
        if (view == View::time)
        {
            const auto px = xForMs(ms);
            g.setColour(design::phosphor.withAlpha(0.8f));
            g.drawLine(px, ly(design::meterPlusOneY), px, ly(design::meterMinusOneY), juce::jmax(1.0f, 1.5f * s));
        }
    }
    else if (isFrozen() && view == View::scope)
    {
        g.setColour(design::meterAxisText.withAlpha(0.5f));
        drawTextAt(g, meterFont(assets, 12.0f * s), "DRAG TO SLIDE THE INPUT", x, ly(design::meterPlusOneY) + 44.0f * s,
                   juce::Justification::left);
    }
    else if (isFrozen() && view == View::time)
    {
        g.setColour(design::meterAxisText.withAlpha(0.5f));
        drawTextAt(g, meterFont(assets, 12.0f * s), "DRAG TO PREVIEW A DELAY", x, ly(design::meterPlusOneY) + 112.0f * s,
                   juce::Justification::left);
    }
}

void MeterScreen::paintTrace(juce::Graphics& g, const std::function<float(int)>& xAt, const std::vector<float>& values,
                             bool processed) const
{
    const auto s = getScale();
    const auto zero = ly(design::meterZeroY);
    juce::Path line, fill;
    bool inRun = false;
    float runStartX = 0.0f, lastX = 0.0f;
    const auto closeRun = [&]
    {
        if (inRun)
        {
            fill.lineTo(lastX, zero);
            fill.lineTo(runStartX, zero);
            fill.closeSubPath();
        }
        inRun = false;
    };

    for (int i = 0; i < (int)values.size(); ++i)
    {
        const auto v = values[(size_t)i];
        if (std::isnan(v)) // gated: not drawn
        {
            closeRun();
            continue;
        }
        const auto x = xAt(i), y = yForR(v);
        if (! inRun)
        {
            line.startNewSubPath(x, y);
            fill.startNewSubPath(x, zero);
            fill.lineTo(x, y);
            runStartX = x;
            inRun = true;
        }
        else
        {
            line.lineTo(x, y);
            fill.lineTo(x, y);
        }
        lastX = x;
    }
    closeRun();

    const auto plot =
        juce::Rectangle<float>::leftTopRightBottom(lx(design::meterPlotLeft), ly(design::meterPlusOneY) - 4.0f * s,
                                                   lx(design::meterPlotRight), ly(design::meterMinusOneY) + 4.0f * s);
    juce::Graphics::ScopedSaveState save(g); // the clip is for the trace only
    g.reduceClipRegion(plot.getSmallestIntegerContainer());
    const juce::PathStrokeType::JointStyle joint = juce::PathStrokeType::curved;
    if (processed)
    {
        g.setColour(design::phosphor.withAlpha(0.12f));
        g.fillPath(fill);
        g.setColour(design::phosphor.withAlpha(0.18f)); // soft glow under the line
        g.strokePath(line, juce::PathStrokeType(6.0f * s, joint));
        g.setColour(design::phosphor);
        g.strokePath(line, juce::PathStrokeType(juce::jmax(1.0f, 2.2f * s), joint));
    }
    else
    {
        g.setColour(design::phosphor.withAlpha(0.42f));
        g.strokePath(line, juce::PathStrokeType(juce::jmax(1.0f, 1.6f * s), joint));
    }
}

void MeterScreen::paintDashedTrace(juce::Graphics& g, const std::function<float(int)>& xAt,
                                   const std::vector<float>& values, bool processed) const
{
    const auto s = getScale();
    juce::Path line;
    for (int i = 0; i < (int)values.size(); ++i)
    {
        const auto x = xAt(i), y = yForR(std::max(0.0f, values[(size_t)i])); // attacks line up positively only
        if (i == 0)
            line.startNewSubPath(x, y);
        else
            line.lineTo(x, y);
    }
    const float dashes[] = {5.0f * s, 4.0f * s};
    juce::Path dashed;
    juce::PathStrokeType(1.0f).createDashedStroke(dashed, line, dashes, 2);
    const auto plot =
        juce::Rectangle<float>::leftTopRightBottom(lx(design::meterPlotLeft), ly(design::meterPlusOneY),
                                                   lx(design::meterPlotRight), ly(design::meterMinusOneY));
    juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(plot.getSmallestIntegerContainer());
    g.setColour(design::phosphor.withAlpha(processed ? 0.9f : 0.4f));
    g.strokePath(dashed, juce::PathStrokeType(juce::jmax(1.0f, 1.6f * s)));
}

void MeterScreen::paintLagReadout(juce::Graphics& g) const
{
    // Two readings of the offset, for input and output: from the waveforms (best for steady material and mic pairs),
    // and from the attacks (best for hits whose waveforms differ, such as a kick against a sample). Once aligned,
    // the output's should sit at 0.
    const auto s = getScale();
    const auto font = meterFont(assets, 16.0f * s, true);
    const auto small = meterFont(assets, 12.0f * s).withExtraKerningFactor(0.1f);
    const auto x = lx(design::meterPlotLeft) + 14.0f * s;
    const auto column1 = x + 120.0f * s, column2 = x + 300.0f * s;
    auto y = ly(design::meterPlusOneY) + 20.0f * s;

    const auto describe = [](const meter::CorrelationAnalyser::Peak& p)
    {
        if (! p.clear)
            return juce::String("--");
        const auto ms = std::abs(p.lagMs) < 0.0005 ? 0.0 : p.lagMs; // no "-0.000"
        // Beyond the view the reading is coarse (whole samples), so it gets one decimal.
        auto text = (ms > 0.0 ? "+" : "") + juce::String(ms, p.coarse ? 1 : 3) + " ms";
        if (p.value < 0.0f)
            text << " INV";
        return text;
    };

    g.setColour(design::meterAxisText.withAlpha(0.7f));
    drawTextAt(g, small, "WAVEFORM", column1, y, juce::Justification::left);
    drawTextAt(g, small, "ATTACK", column2, y, juce::Justification::left);
    y += 20.0f * s;

    const auto in = analyser.inputPeak(), out = analyser.lagPeakProcessed();
    const auto attackIn = analyser.attackInput(), attackOut = analyser.attackPeakProcessed();
    g.setColour(design::phosphor.withAlpha(0.6f));
    drawTextAt(g, font, "INPUT", x, y, juce::Justification::left);
    drawTextAt(g, font, describe(in), column1, y, juce::Justification::left);
    drawTextAt(g, font, describe(attackIn), column2, y, juce::Justification::left);
    y += 22.0f * s;
    g.setColour(design::phosphor);
    drawTextAt(g, font, "OUTPUT", x, y, juce::Justification::left);
    drawTextAt(g, font, describe(out), column1, y, juce::Justification::left);
    drawTextAt(g, font, describe(attackOut), column2, y, juce::Justification::left);

    // A peak the delay knob can't reach (plan 2.1a), by more than half its 0.1-sample step; up to 40 ms away.
    if (analyser.inputOutOfReach(params::maxDelayMs))
    {
        y += 22.0f * s;
        g.setColour(design::meterAxisText.withAlpha(0.8f));
        drawTextAt(g, meterFont(assets, 12.0f * s), "TRANSIENTS OUT OF DELAY RANGE", x, y, juce::Justification::left);
    }

    // Peak markers on the zero line (not for a coarse peak: it is beyond the view).
    for (const auto* p : {&in, &out})
        if (p->clear && ! p->coarse)
        {
            const auto px = xForMs(p->lagMs);
            const auto py = yForR(p->value);
            g.setColour(design::phosphor.withAlpha(p == &out ? 1.0f : 0.6f));
            g.fillEllipse(juce::Rectangle<float>(7.0f * s, 7.0f * s).withCentre({px, py}));
        }
}

void MeterScreen::paintOverall(juce::Graphics& g) const
{
    const auto s = getScale();
    const auto left = lx(design::meterOverallLeft), right = lx(design::meterOverallRight);
    const auto x = analyser.overallProcessed(), z = analyser.overallUnprocessed();
    if (! std::isnan(x))
        paintSegments(g, left, right, x, false);
    if (! std::isnan(z)) // unprocessed: a tick across the bar
    {
        g.setColour(design::meterAxisText.withAlpha(0.9f));
        g.fillRect(juce::Rectangle<float>(left - 6.0f * s, yForR(z) - 1.5f * s, right - left + 12.0f * s, 3.0f * s));
    }
}

void MeterScreen::paintSegments(juce::Graphics& g, float x0, float x1, float r, bool unlitToo) const
{
    // Ten segments each side of zero; those between zero and r are lit, the rest drawn faintly (by the static layer).
    const auto s = getScale();
    const auto zero = ly(design::meterZeroY);
    const auto pitch = (design::meterZeroY - design::meterPlusOneY) * s / (float)segmentsPerHalf;
    const auto height = pitch * segmentFill;
    const auto lit = juce::roundToInt(std::abs(r) * segmentsPerHalf);

    for (int i = 0; i < segmentsPerHalf; ++i)
    {
        const auto offset = pitch * (float)i + 0.5f * (pitch - height);
        const auto above = juce::Rectangle<float>(x0, zero - offset - height, x1 - x0, height);
        const auto below = juce::Rectangle<float>(x0, zero + offset, x1 - x0, height);
        const auto litAbove = r > 0.0f && i < lit, litBelow = r < 0.0f && i < lit;
        if (litAbove || unlitToo)
        {
            g.setColour(design::phosphor.withAlpha(litAbove ? 1.0f : 0.08f));
            g.fillRect(above);
        }
        if (litBelow || unlitToo)
        {
            g.setColour(design::phosphor.withAlpha(litBelow ? 0.75f : 0.08f));
            g.fillRect(below);
        }
    }
}
} // namespace pa::ui
