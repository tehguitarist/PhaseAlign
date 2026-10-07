#include "ui/HelpButton.h"
#include "ui/Design.h"

namespace pa::ui
{
HelpButton::HelpButton(juce::Rectangle<float> slot) : DesignComponent(slot)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setTitle("Tooltips");
}

void HelpButton::setOn(bool shouldBeOn)
{
    if (shouldBeOn == on)
        return;
    on = shouldBeOn;
    repaint();
}

void HelpButton::paint(juce::Graphics& g)
{
    // A "?" in Arial Bold like the panel's labels: white with a glow while on, the dimmed label grey while off.
    const auto s = getScale();
    const auto font = labelFont(design::helpCapHeight * s);
    const auto bounds = getLocalBounds().toFloat();
    // Centred on its ink, so it sits in the middle of the slot whatever the font's metrics.
    juce::GlyphArrangement ga;
    ga.addLineOfText(font, "?", 0.0f, 0.0f);
    juce::Path glyph;
    ga.createPath(glyph);
    const auto ink = glyph.getBounds();
    glyph.applyTransform(juce::AffineTransform::translation(bounds.getCentreX() - ink.getCentreX(),
                                                            bounds.getCentreY() - ink.getCentreY()));
    if (on)
    {
        for (const auto [grow, alpha] : {std::pair{6.0f, 0.12f}, std::pair{3.0f, 0.2f}})
        {
            g.setColour(design::labelGlow.withMultipliedAlpha(alpha / 0.6f));
            g.strokePath(glyph,
                         juce::PathStrokeType(grow * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }
    g.setColour(on ? design::labelSelected : design::labelDimmed);
    g.fillPath(glyph);
}

void HelpButton::mouseDown(const juce::MouseEvent&)
{
    toggle();
}

void HelpButton::toggle()
{
    if (onToggle)
        onToggle(! on);
    else
        setOn(! on);
}

std::unique_ptr<juce::AccessibilityHandler> HelpButton::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(
        *this, juce::AccessibilityRole::toggleButton,
        juce::AccessibilityActions().addAction(juce::AccessibilityActionType::toggle, [this] { toggle(); }));
}
} // namespace pa::ui
