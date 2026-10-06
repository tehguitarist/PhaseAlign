#include "ui/ImageKnob.h"
#include "ui/Design.h"

namespace pa::ui
{
ImageKnob::ImageKnob(ScaledImages& imagesIn, juce::Rectangle<float> slot, juce::RangedAudioParameter& param,
                     StepFunction stepIn)
    : DesignComponent(slot), images(imagesIn), parameter(param), step(std::move(stepIn)),
      attachment(param, [this](float v) { valueChanged(v); })
{
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(true);
    setTitle(param.getName(64));
    attachment.sendInitialUpdate();
}

void ImageKnob::valueChanged(float newValue)
{
    if (juce::exactlyEqual(newValue, value))
        return;
    value = newValue;
    repaint();
}

void ImageKnob::paint(juce::Graphics& g)
{
    const auto norm = parameter.convertTo0to1(value);
    const auto angle = juce::degreesToRadians(design::knobSweepDegrees * (2.0f * norm - 1.0f));
    g.setOpacity(contentAlpha());
    drawRotated(g, images, layout::Image::knobs, getLocalBounds().toFloat(), angle);
}

void ImageKnob::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isCommandDown() || e.mods.isAltDown())
    {
        attachment.setValueAsCompleteGesture(parameter.convertFrom0to1(parameter.getDefaultValue()));
        dragging = false;
        return;
    }

    // The gesture starts on the first movement, so a plain or double click doesn't touch automation.
    dragging = true;
    dragNormalised = parameter.convertTo0to1(value);
    lastDragY = e.position.y;
}

void ImageKnob::mouseDrag(const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    // Incremental, so pressing or releasing Shift mid-drag doesn't make the value jump.
    const auto dy = lastDragY - e.position.y;
    lastDragY = e.position.y;
    if (dy == 0.0f)
        return;

    const auto fine = e.mods.isShiftDown() ? 0.1f : 1.0f;
    dragNormalised = juce::jlimit(0.0f, 1.0f, dragNormalised + dy * fine / dragPixelsForFullRange);

    if (! gestureActive)
    {
        attachment.beginGesture();
        gestureActive = true;
    }
    attachment.setValueAsPartOfGesture(parameter.convertFrom0to1(dragNormalised));
}

void ImageKnob::mouseUp(const juce::MouseEvent&)
{
    if (gestureActive)
        attachment.endGesture();
    gestureActive = false;
    dragging = false;
}

void ImageKnob::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (! e.mods.isCommandDown() && ! e.mods.isAltDown() && onEditRequest)
        onEditRequest();
}

void ImageKnob::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    auto delta = std::abs(wheel.deltaX) > std::abs(wheel.deltaY) ? -wheel.deltaX : wheel.deltaY;
    if (wheel.isReversed)
        delta = -delta;
    if (delta == 0.0f)
        return;

    if (! wheel.isSmooth) // a mouse wheel: one step per notch
    {
        nudge(delta > 0.0f ? 1 : -1);
        return;
    }

    wheelAccumulator += delta;
    while (std::abs(wheelAccumulator) >= smoothWheelStep)
    {
        const auto direction = wheelAccumulator > 0.0f ? 1 : -1;
        nudge(direction);
        wheelAccumulator -= (float)direction * smoothWheelStep;
    }
}

bool ImageKnob::keyPressed(const juce::KeyPress& key)
{
    if (key.isKeyCode(juce::KeyPress::upKey) || key.isKeyCode(juce::KeyPress::rightKey))
        nudge(1);
    else if (key.isKeyCode(juce::KeyPress::downKey) || key.isKeyCode(juce::KeyPress::leftKey))
        nudge(-1);
    else
        return false;
    return true;
}

void ImageKnob::nudge(int direction)
{
    const auto next = (float)step(value, direction);
    if (! juce::exactlyEqual(next, value))
        attachment.setValueAsCompleteGesture(next);
}

std::unique_ptr<juce::AccessibilityHandler> ImageKnob::createAccessibilityHandler()
{
    struct ValueInterface final : juce::AccessibilityRangedNumericValueInterface
    {
        explicit ValueInterface(ImageKnob& k) : knob(k) {}

        bool isReadOnly() const override { return false; }
        double getCurrentValue() const override { return knob.value; }
        void setValue(double v) override { knob.attachment.setValueAsCompleteGesture((float)v); }
        AccessibleValueRange getRange() const override
        {
            const auto& range = knob.parameter.getNormalisableRange();
            return {{range.start, range.end}, 0.0};
        }

        ImageKnob& knob;
    };

    return std::make_unique<juce::AccessibilityHandler>(
        *this, juce::AccessibilityRole::slider, juce::AccessibilityActions(),
        juce::AccessibilityHandler::Interfaces{std::make_unique<ValueInterface>(*this)});
}
} // namespace pa::ui
