#include "ui/ToggleSwitch3.h"
#include "ui/Design.h"

namespace pa::ui
{
ToggleSwitch3::ToggleSwitch3(ScaledImages& imagesIn, juce::Rectangle<float> slot, LabelSide sideIn,
                             std::array<Item, 3> itemsIn)
    : DesignComponent(boundsFor(slot, sideIn)), images(imagesIn), switchSlot(slot), side(sideIn),
      items(std::move(itemsIn))
{
}

juce::Rectangle<float> ToggleSwitch3::boundsFor(juce::Rectangle<float> slot, LabelSide side)
{
    const auto reach = design::labelGap + design::labelAreaWidth;
    return side == LabelSide::right ? slot.withRight(slot.getRight() + reach) : slot.withLeft(slot.getX() - reach);
}

juce::Rectangle<float> ToggleSwitch3::labelArea(int item) const
{
    const auto rowY = switchSlot.getCentreY() + design::labelRowSpacing * (float)(item - 1);
    const auto x = side == LabelSide::right ? switchSlot.getRight() + design::labelGap
                                            : switchSlot.getX() - design::labelGap - design::labelAreaWidth;
    return {x, rowY - 0.5f * design::labelRowSpacing, design::labelAreaWidth, design::labelRowSpacing};
}

void ToggleSwitch3::setIndex(int newIndex)
{
    newIndex = juce::jlimit(0, 2, newIndex);
    if (newIndex == index)
        return;
    index = newIndex;
    repaint();
}

void ToggleSwitch3::select(int newIndex)
{
    newIndex = juce::jlimit(0, 2, newIndex);
    if (newIndex == index)
        return;
    setIndex(newIndex);
    if (onSelect)
        onSelect(newIndex);
}

void ToggleSwitch3::paint(juce::Graphics& g)
{
    static constexpr layout::Image positions[] = {layout::Image::toggleSwitchUp, layout::Image::toggleSwitchCentre,
                                                  layout::Image::toggleSwitchDown};
    g.setOpacity(contentAlpha());
    drawFitted(g, images, positions[index], toLocal(switchSlot));

    labels.paint(g, getLocalBounds(), juce::String(index),
                 [this](juce::Graphics& lg, float pixelScale)
                 {
                     const auto capHeight = design::labelCapHeight * getScale();
                     const auto font = labelFont(capHeight);
                     const auto justification =
                         side == LabelSide::right ? juce::Justification::left : juce::Justification::right;

                     auto drawLabel = [&](juce::Graphics& tg, int i)
                     {
                         const auto area = toLocal(labelArea(i));
                         const auto x = side == LabelSide::right ? area.getX() : area.getRight();
                         drawTextAt(tg, font, items[(size_t)i].label, x, area.getCentreY() + 0.5f * capHeight,
                                    justification);
                     };

                     lg.setColour(design::labelDimmed);
                     for (int i = 0; i < 3; ++i)
                         if (i != index)
                             drawLabel(lg, i);

                     drawWithGlow(lg, getLocalBounds(), pixelScale, design::labelGlow, 6.0f * getScale(),
                                  [&](juce::Graphics& tg)
                                  {
                                      tg.setColour(design::labelSelected);
                                      drawLabel(tg, index);
                                  });
                 });
}

int ToggleSwitch3::itemAt(juce::Point<float> local) const
{
    if (toLocal(switchSlot).contains(local))
        return -1;
    for (int i = 0; i < 3; ++i)
        if (toLocal(labelArea(i)).expanded(design::labelGap * getScale(), 0.0f).contains(local))
            return i;
    return -2;
}

void ToggleSwitch3::mouseDown(const juce::MouseEvent& e)
{
    const auto item = itemAt(e.position);
    downOnSwitch = item == -1;
    moved = false;
    dragStartIndex = index;
    dragStartY = e.position.y;
    if (item >= 0)
        select(item);
}

void ToggleSwitch3::mouseDrag(const juce::MouseEvent& e)
{
    if (! downOnSwitch)
        return;
    const auto dy = e.position.y - dragStartY;
    if (std::abs(dy) > 3.0f)
        moved = true;
    if (moved)
        select(dragStartIndex + juce::roundToInt(dy / (design::labelRowSpacing * getScale())));
}

void ToggleSwitch3::mouseUp(const juce::MouseEvent& e)
{
    if (downOnSwitch && ! moved)
        select(index + (e.position.y < toLocal(switchSlot).getCentreY() ? -1 : 1));
    downOnSwitch = false;
}

juce::String ToggleSwitch3::getTooltip()
{
    const auto item = itemAt(getMouseXYRelative().toFloat());
    return item >= 0 ? items[(size_t)item].tooltip : DesignComponent::getTooltip();
}

std::unique_ptr<juce::AccessibilityHandler> ToggleSwitch3::createAccessibilityHandler()
{
    struct ValueInterface final : juce::AccessibilityTextValueInterface
    {
        explicit ValueInterface(ToggleSwitch3& s) : sw(s) {}
        bool isReadOnly() const override { return true; }
        juce::String getCurrentValueAsString() const override { return sw.items[(size_t)sw.index].label; }
        void setValueAsString(const juce::String&) override {}
        ToggleSwitch3& sw;
    };

    return std::make_unique<juce::AccessibilityHandler>(
        *this, juce::AccessibilityRole::comboBox,
        juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press,
                                               [this] { select((index + 1) % 3); }),
        juce::AccessibilityHandler::Interfaces{std::make_unique<ValueInterface>(*this)});
}
} // namespace pa::ui
