#pragma once

#include "ui/DesignComponent.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace pa::ui
{
// A latching round button (DELAY / polarity / PHASE) bound to a bool parameter: pressed in = on.
// Circular hit test; toggles on mouse down like a hardware latching switch.
class PanelButton : public DesignComponent
{
  public:
    PanelButton(ScaledImages&, juce::Rectangle<float> slot, juce::RangedAudioParameter&);

    // Called with the parameter's state whenever it changes (drives the LED).
    std::function<void(bool)> onStateChange;

    bool isOn() const { return on; }

    void paint(juce::Graphics&) override;
    bool hitTest(int x, int y) override;
    void mouseDown(const juce::MouseEvent&) override;

  private:
    void toggle();
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    ScaledImages& images;
    juce::ParameterAttachment attachment;
    bool on = false;
};

// A non-interactive image indicator (LED).
class ImageIndicator : public DesignComponent
{
  public:
    ImageIndicator(ScaledImages&, juce::Rectangle<float> slot, layout::Image onImage, layout::Image offImage);

    void setLit(bool shouldBeLit);
    bool isLit() const { return lit; }
    void paint(juce::Graphics&) override;

  private:
    ScaledImages& images;
    layout::Image onImage, offImage;
    bool lit = false;
};

// A square push button with four images: pressed or released, lit or unlit (METER, ANALYSE).
// onClick fires on release inside the button.
class SquareButton : public DesignComponent
{
  public:
    struct Images
    {
        layout::Image pressedUnlit, pressedLit, releasedUnlit, releasedLit;
    };

    SquareButton(ScaledImages&, juce::Rectangle<float> slot, Images);

    std::function<void()> onClick;

    void setLit(bool shouldBeLit);
    bool isLit() const { return lit; }

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

  private:
    void setPressed(bool);
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    ScaledImages& scaledImages;
    Images images;
    bool lit = false, pressed = false;
};
} // namespace pa::ui
