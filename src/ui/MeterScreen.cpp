#include "ui/MeterScreen.h"
#include "params/Parameters.h"
#include "ui/Design.h"

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
    setTooltip("Meter view: correlation against frequency, or the time offset between this track and the sidechain.");
    for (auto& s : scratch)
        s.resize((size_t)meter::MeterCapture::capacity);
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
        analyser.computeLag();
    repaint();
}

void MeterScreen::updateTimer()
{
    const auto shouldRun = meterOn && isShowing();
    if (shouldRun && ! isTimerRunning())
    {
        analyser.reset(); // fresh averages each time the meter starts
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

    float* dest[meter::MeterCapture::numStreams];
    for (int s = 0; s < meter::MeterCapture::numStreams; ++s)
        dest[s] = scratch[s].data();
    const auto n = capture.pull(dest, meter::MeterCapture::capacity);
    if (n == 0)
        return false;

    lastSamplesMs = nowMs();
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

juce::Rectangle<float> MeterScreen::viewLabelArea(View v) const
{
    // The bottom row, either side of the plot centre: FREQUENCY to the left, TIME to the right.
    const auto centre = 0.5f * (lx(design::meterPlotLeft) + lx(design::meterPlotRight));
    const auto s = getScale();
    const auto row = juce::Rectangle<float>(0.0f, ly(design::meterFreqTitleY) - 14.0f * s, 260.0f * s, 26.0f * s);
    return v == View::frequency ? row.withRightX(centre - 20.0f * s) : row.withX(centre + 20.0f * s);
}

bool MeterScreen::hitTest(int x, int y)
{
    if (state == State::off)
        return false;
    const auto p = juce::Point<int>(x, y).toFloat();
    return viewLabelArea(View::frequency).contains(p) || viewLabelArea(View::time).contains(p);
}

void MeterScreen::mouseDown(const juce::MouseEvent& e)
{
    const auto clicked = viewLabelArea(View::time).contains(e.position) ? View::time : View::frequency;
    if (onViewSelected)
        onViewSelected(clicked);
    else
        setView(clicked);
}

//==============================================================================
void MeterScreen::paint(juce::Graphics& g)
{
    ++paintCount;
    const auto key = juce::String((int)state) + ":" + juce::String((int)view);
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
    if (view == View::frequency)
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
    const auto yLabel = [&](const char* text, float atY, float atX, juce::Justification j)
    { drawTextAt(g, axisFont, text, atX, atY + 10.0f * s, j); };
    yLabel("+1", top, left - 16.0f * s, juce::Justification::right);
    yLabel("0", zero, left - 16.0f * s, juce::Justification::right);
    yLabel("-1", bottom, left - 16.0f * s, juce::Justification::right);

    // Legend, top right of the plot.
    const auto legendFont = meterFont(assets, 14.0f * s).withExtraKerningFactor(0.1f);
    const auto legendY = ly(design::meterPlusOneY) - 12.0f * s;
    g.setColour(design::phosphor);
    drawTextAt(g, legendFont, "OUTPUT", right, legendY, juce::Justification::right);
    const auto processedWidth = textWidth(legendFont, "OUTPUT");
    g.setColour(design::phosphor.withAlpha(0.45f));
    drawTextAt(g, legendFont, "INPUT", right - processedWidth - 24.0f * s, legendY, juce::Justification::right);

    // View selector: the selected label lit, the other dimmed (like the panel's switch labels).
    const auto viewFont = meterFont(assets, 13.0f * s).withExtraKerningFactor(0.2f);
    const auto titleY = ly(design::meterFreqTitleY) + 6.5f * s;
    const auto freqArea = viewLabelArea(View::frequency), timeArea = viewLabelArea(View::time);
    g.setColour(design::meterAxisText.withAlpha(view == View::frequency ? 0.95f : 0.3f));
    drawTextAt(g, viewFont, "FREQUENCY (Hz)", freqArea.getRight(), titleY, juce::Justification::right);
    g.setColour(design::meterAxisText.withAlpha(view == View::time ? 0.95f : 0.3f));
    drawTextAt(g, viewFont, "TIME OFFSET (ms)", timeArea.getX(), titleY, juce::Justification::left);
    g.setColour(design::meterAxisText.withAlpha(0.3f));
    g.drawLine(0.5f * (freqArea.getRight() + timeArea.getX()), titleY - 11.0f * s,
               0.5f * (freqArea.getRight() + timeArea.getX()), titleY + 2.0f * s, thin);

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
    yLabel("+1", top, overallAxis, juce::Justification::horizontallyCentred);
    yLabel("0", zero, overallAxis, juce::Justification::horizontallyCentred);
    yLabel("-1", bottom, overallAxis, juce::Justification::horizontallyCentred);
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
    else
    {
        const auto xAt = [&](int i) { return xForMs(analyser.lagMsAt(i)); };
        paintTrace(g, xAt, analyser.lagUnprocessed(), false);
        paintTrace(g, xAt, analyser.lagProcessed(), true);
        paintLagReadout(g);
    }
    paintOverall(g);
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

void MeterScreen::paintLagReadout(juce::Graphics& g) const
{
    // Where the input's peak sits says how much delay aligns it; the output's should sit at 0 once it is aligned.
    const auto s = getScale();
    const auto font = meterFont(assets, 16.0f * s, true);
    const auto x = lx(design::meterPlotLeft) + 14.0f * s;
    auto y = ly(design::meterPlusOneY) + 24.0f * s;

    const auto describe = [](const meter::CorrelationAnalyser::Peak& p)
    {
        if (! p.clear)
            return juce::String("--");
        const auto ms = std::abs(p.lagMs) < 0.0005 ? 0.0 : p.lagMs; // no "-0.000"
        auto text = (ms > 0.0 ? "+" : "") + juce::String(ms, 3) + " ms";
        if (p.value < 0.0f)
            text << " INVERTED";
        return text;
    };

    const auto in = analyser.lagPeakUnprocessed(), out = analyser.lagPeakProcessed();
    g.setColour(design::phosphor.withAlpha(0.6f));
    drawTextAt(g, font, "INPUT   " + describe(in), x, y, juce::Justification::left);
    y += 22.0f * s;
    g.setColour(design::phosphor);
    drawTextAt(g, font, "OUTPUT  " + describe(out), x, y, juce::Justification::left);

    // A peak the delay knob can't reach (plan 2.1a), by more than half its 0.1-sample step.
    const auto halfStepMs = 0.05 * 1000.0 / analyser.getSampleRate();
    if (in.clear && std::abs(in.lagMs) > params::maxDelayMs + halfStepMs)
    {
        y += 22.0f * s;
        g.setColour(design::meterAxisText.withAlpha(0.8f));
        drawTextAt(g, meterFont(assets, 12.0f * s), "TRANSIENTS OUT OF DELAY RANGE", x, y, juce::Justification::left);
    }

    // Peak markers on the zero line.
    for (const auto* p : {&in, &out})
        if (p->clear)
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
