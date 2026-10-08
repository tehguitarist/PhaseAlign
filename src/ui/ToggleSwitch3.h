#pragma once

#include "ui/DesignComponent.h"

namespace pa::ui
{
// A 3-way toggle switch with clickable labels. Position 0 is up, 1 centre,
// 2 down. Click the upper or lower half of the switch to step, drag it up or down, or click a label.
// Only the selected label is highlighted.
class ToggleSwitch3 : public DesignComponent
{
  public:
    enum class LabelSide
    {
        left, // right-aligned to the left of the switch
        right // left-aligned to the right of the switch
    };

    struct Item
    {
        juce::String label, tooltip;
    };

    ToggleSwitch3(ScaledImages&, juce::Rectangle<float> switchSlot, LabelSide, std::array<Item, 3>);

    // Sets the shown position without calling onSelect (for model updates).
    void setIndex(int newIndex);
    int getIndex() const { return index; }

    // Called when a position is picked.
    std::function<void(int)> onSelect;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    juce::String getTooltip() override;
    const juce::String& getItemTooltip(int item) const { return items[(size_t)item].tooltip; }

  private:
    static juce::Rectangle<float> boundsFor(juce::Rectangle<float> switchSlot, LabelSide);
    juce::Rectangle<float> labelArea(int item) const; // design space
    int itemAt(juce::Point<float> local) const;       // -1 = the switch itself
    void select(int newIndex);
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    ScaledImages& images;
    juce::Rectangle<float> switchSlot;
    LabelSide side;
    std::array<Item, 3> items;
    int index = 0;

    int dragStartIndex = 0;
    float dragStartY = 0.0f;
    bool downOnSwitch = false, moved = false;
    CachedLayer labels;
};
} // namespace pa::ui
