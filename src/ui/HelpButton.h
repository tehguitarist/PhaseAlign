#pragma once

#include "ui/DesignComponent.h"

namespace pa::ui
{
// The "?" at the bottom middle of the panel: switches the tooltips on and off (off by default). Lit while on, dim
// while off. Its own tooltip is always available, so the way to turn them on can be found with them off.
class HelpButton : public DesignComponent
{
  public:
    explicit HelpButton(juce::Rectangle<float> slot);

    void setOn(bool shouldBeOn);
    bool isOn() const { return on; }
    std::function<void(bool)> onToggle; // a click

    juce::String getTooltip() override { return on ? "Disable tooltips" : "Enable tooltips"; }

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

  private:
    void toggle();
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    bool on = false;
};
} // namespace pa::ui
