#pragma once

#include "ui/DesignComponent.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace pa::ui
{
// A knob whose cap image (baked lighting included) rotates over the ring dots:
// vertical drag, Shift = fine (x0.1), wheel and arrow keys = one step (the step function decides what a
// step is), Cmd- or Alt-click = reset, double-click = onEditRequest. Repaints only on value change.
class ImageKnob : public DesignComponent
{
  public:
    // step(value, direction) returns the value one step up (+1) or down (-1).
    using StepFunction = std::function<double(double value, int direction)>;

    ImageKnob(ScaledImages&, juce::Rectangle<float> slot, juce::RangedAudioParameter&, StepFunction);

    std::function<void()> onEditRequest;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;

  private:
    void valueChanged(float newValue);
    void nudge(int direction);
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    // Logical pixels of vertical drag for the full range (x10 with Shift).
    static constexpr float dragPixelsForFullRange = 300.0f;
    // Trackpad wheel travel per step.
    static constexpr float smoothWheelStep = 0.05f;

    ScaledImages& images;
    juce::RangedAudioParameter& parameter;
    StepFunction step;
    juce::ParameterAttachment attachment;
    float value = 0.0f;

    float dragNormalised = 0.0f;
    float lastDragY = 0.0f;
    bool dragging = false, gestureActive = false;
    float wheelAccumulator = 0.0f;
};
} // namespace pa::ui
