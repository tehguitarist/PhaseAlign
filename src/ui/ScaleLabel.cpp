#include "ui/ScaleLabel.h"
#include "ui/Design.h"

namespace pa::ui
{
namespace
{
juce::Path textPath(const juce::Font& font, const juce::String& text)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText(font, text, 0.0f, 0.0f);
    juce::Path path;
    ga.createPath(path);
    return path;
}
} // namespace

// The margin leaves room on the right for the asterisk, which sits after the degree sign.
ScaleLabel::ScaleLabel(juce::Rectangle<float> slotIn)
    : DesignComponent(slotIn.expanded(8.0f).withTrimmedRight(-24.0f)), slot(slotIn)
{
    setInterceptsMouseClicks(false, false);
}

void ScaleLabel::setDegrees(int newDegrees)
{
    if (newDegrees == degrees)
        return;
    degrees = newDegrees;
    repaint();
}

void ScaleLabel::setAsterisk(bool shown)
{
    if (shown == asterisk)
        return;
    asterisk = shown;
    repaint();
}

void ScaleLabel::paint(juce::Graphics& g)
{
    // Placed by ink bounds rather than font metrics, so it lines up with the art whatever font is installed.
    const auto s = getScale();
    const auto box = toLocal(slot);
    const auto font = scaleLabelFont(design::scaleDigitHeight * s);

    auto digits = textPath(font, juce::String(degrees));
    const auto ink = digits.getBounds();
    const auto digitTop = box.getY() + (design::scaleDegreeSize - 2.0f) * s;
    digits.applyTransform(juce::AffineTransform::translation(box.getX() - ink.getX(), digitTop - ink.getY()));

    auto degree = textPath(font, juce::String::fromUTF8("\xc2\xb0"));
    const juce::Rectangle<float> degreeBox{digits.getBounds().getRight() + design::scaleDegreeGap * s, box.getY(),
                                           design::scaleDegreeSize * s, design::scaleDegreeSize * s};
    degree.applyTransform(degree.getTransformToScaleToFit(degreeBox, true));

    g.setColour(design::scaleLabelColour.withMultipliedAlpha(contentAlpha()));
    g.fillPath(digits);
    g.fillPath(degree);
    if (asterisk)
    {
        auto star = textPath(font, "*");
        const auto starBox = degreeBox.translated(degreeBox.getWidth() + design::scaleDegreeGap * s * 0.6f, 0.0f);
        star.applyTransform(star.getTransformToScaleToFit(starBox, true));
        g.fillPath(star);
    }
}
} // namespace pa::ui
