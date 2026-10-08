#pragma once

#include "ui/DesignComponent.h"

#include <optional>

namespace pa::ui
{
// A green 7-segment readout with a glow, unlit "8" ghost segments and a unit suffix.
// Double-click opens an in-place text editor; typed text goes to onTextEntered. The glowing text
// is rendered once per value and size, so a repaint is a single image draw.
class Readout : public DesignComponent
{
  public:
    Readout(const SourceAssets&, juce::Rectangle<float> slot, juce::Rectangle<float> interior);

    // digits: the value ("1.23"); ghost: the unlit segments behind it ("8.88"); suffix: the unit.
    void setValue(const juce::String& digits, const juce::String& ghost, const juce::String& suffix);
    const juce::String& getDigits() const { return digits; }
    const juce::String& getSuffix() const { return suffix; }

    // Places the digits so the centre of the second digit from the right sits at designX, growing to the left,
    // with the suffix after them. Without an anchor, the digits are right-aligned against the suffix, which is
    // right-aligned in the display.
    void setAnchor(float designX);

    std::function<void(const juce::String&)> onTextEntered;

    void showEditor();
    bool isEditing() const { return editor.isVisible(); }

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

  private:
    void hideEditor();
    void commitEdit();

    const SourceAssets& assets;
    juce::Rectangle<float> interior;
    juce::String digits, ghost, suffix;
    std::optional<float> anchorX;
    CachedLayer layer;
    juce::TextEditor editor;
};
} // namespace pa::ui
