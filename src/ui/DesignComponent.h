#pragma once

#include "ui/Assets.h"
#include "ui/Design.h"

namespace pa::ui
{
// A component placed in the 1954x1224 design space. The editor lays children out directly in its own
// pixels (not through an AffineTransform on a 1954-wide content component), so every component sits on
// whole logical pixels and its pre-scaled images stay 1:1 with the device. Components convert
// design-space rectangles to local coordinates with toLocal().
class DesignComponent : public juce::Component, public juce::SettableTooltipClient
{
  public:
    explicit DesignComponent(juce::Rectangle<float> designBoundsIn) : designBounds(designBoundsIn) {}

    juce::Rectangle<float> getDesignBounds() const { return designBounds; }

    // s = editor width / design width. Edges are rounded (not position and size separately), so
    // neighbouring components can't drift apart by a pixel.
    void layoutForScale(float s)
    {
        scale = s;
        const auto r = designBounds * s;
        setBounds(juce::Rectangle<int>::leftTopRightBottom(juce::roundToInt(r.getX()), juce::roundToInt(r.getY()),
                                                           juce::roundToInt(r.getRight()),
                                                           juce::roundToInt(r.getBottom())));
    }

    float getScale() const { return scale; }

    // A switched-off section's controls draw at design::dimmedAlpha (IMPLEMENTATION_PLAN 4.5) but stay usable.
    // Repaints only this component, and only when the state changes.
    void setDimmed(bool shouldBeDimmed)
    {
        if (shouldBeDimmed == dimmed)
            return;
        dimmed = shouldBeDimmed;
        repaint();
    }
    bool isDimmed() const { return dimmed; }

    juce::Rectangle<float> toLocal(juce::Rectangle<float> designRect) const
    {
        return designRect * scale - getPosition().toFloat();
    }

    juce::Point<float> toLocal(juce::Point<float> designPoint) const
    {
        return designPoint * scale - getPosition().toFloat();
    }

  protected:
    // 1 normally, design::dimmedAlpha while dimmed.
    float contentAlpha() const { return dimmed ? design::dimmedAlpha : 1.0f; }

  private:
    juce::Rectangle<float> designBounds;
    float scale = 0.5f;
    bool dimmed = false;
};
} // namespace pa::ui
