#pragma once

#include "ui/DesignComponent.h"

namespace pa::ui
{
// A scale label that changes with a setting, drawn to match the labels baked into the panel: the phase knob's
// upper end, which reads 90° or 180° with RANGE, with an asterisk after the degree sign where the number is
// approximate (High at RANGE 180). Digits are left-aligned in the slot under a raised degree sign.
class ScaleLabel : public DesignComponent
{
  public:
    explicit ScaleLabel(juce::Rectangle<float> slot);

    void setDegrees(int newDegrees);
    int getDegrees() const { return degrees; }
    void setAsterisk(bool shown);
    bool hasAsterisk() const { return asterisk; }

    void paint(juce::Graphics&) override;

  private:
    juce::Rectangle<float> slot; // the text's ink box; the component has a margin so rounding never clips it
    int degrees = 90;
    bool asterisk = false;
};
} // namespace pa::ui
