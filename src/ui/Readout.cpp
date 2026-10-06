#include "ui/Readout.h"
#include "ui/Design.h"

namespace pa::ui
{
Readout::Readout(const SourceAssets& assetsIn, juce::Rectangle<float> slot, juce::Rectangle<float> interiorIn)
    : DesignComponent(slot), assets(assetsIn), interior(interiorIn)
{
    editor.setColour(juce::TextEditor::backgroundColourId, design::screenBlack);
    editor.setColour(juce::TextEditor::textColourId, design::readoutGreen);
    editor.setColour(juce::TextEditor::highlightColourId, design::readoutGreen.withAlpha(0.35f));
    editor.setColour(juce::TextEditor::highlightedTextColourId, juce::Colours::white);
    editor.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    editor.setColour(juce::TextEditor::focusedOutlineColourId, design::readoutGreen.withAlpha(0.5f));
    editor.setColour(juce::CaretComponent::caretColourId, design::readoutGreen);
    editor.setJustification(juce::Justification::centred);
    editor.setInputRestrictions(16);
    editor.setSelectAllWhenFocused(true);
    editor.onReturnKey = [this] { commitEdit(); };
    editor.onEscapeKey = [this] { hideEditor(); };
    editor.onFocusLost = [this] { hideEditor(); };
    addChildComponent(editor);
}

void Readout::setValue(const juce::String& newDigits, const juce::String& newGhost, const juce::String& newSuffix)
{
    if (newDigits == digits && newGhost == ghost && newSuffix == suffix)
        return;
    digits = newDigits;
    ghost = newGhost;
    suffix = newSuffix;
    repaint();
}

void Readout::setAnchor(float designX)
{
    anchorX = designX;
    layer.invalidate();
    repaint();
}

void Readout::paint(juce::Graphics& g)
{
    layer.paint(g, getLocalBounds(), digits + "|" + ghost + "|" + suffix + "|" + juce::String((int)isDimmed()),
                [this](juce::Graphics& lg, float pixelScale)
                {
                    const auto s = getScale();
                    const auto r = toLocal(interior);
                    const auto digitFont = readoutFont(assets, design::readoutDigitHeight * s);
                    // A degree sign sits at the top of the digits, like the panel's printed scale; units sit on the
                    // baseline.
                    const auto isDegree = suffix == juce::String::fromUTF8("\xc2\xb0");
                    const auto suffixCap = isDegree ? design::readoutDegreeCapHeight : design::readoutSuffixCapHeight;
                    const auto suffixFont = labelFont(suffixCap * s);
                    const auto baseline = r.getCentreY() + 0.5f * design::readoutDigitHeight * s;
                    const auto suffixGap = 4.0f * s;
                    auto suffixRight = r.getRight() - 10.0f * s;
                    auto digitsRight = suffixRight - textWidth(suffixFont, suffix) - suffixGap;
                    if (anchorX.has_value())
                    {
                        // DSEG7 digits share one advance (its point has none), so this holds for any value.
                        digitsRight = *anchorX * s - (float)getX() + 1.5f * textWidth(digitFont, "8");
                        suffixRight = digitsRight + suffixGap + textWidth(suffixFont, suffix);
                    }

                    lg.setColour(design::readoutGreen.withAlpha(design::readoutGhostAlpha));
                    drawTextAt(lg, digitFont, ghost, digitsRight, baseline, juce::Justification::right);

                    // Only the value dims; the ghost segments above stay as they are.
                    const auto alpha = contentAlpha();
                    drawWithGlow(
                        lg, getLocalBounds(), pixelScale, design::readoutGlow.withMultipliedAlpha(alpha), 7.0f * s,
                        [&](juce::Graphics& tg)
                        {
                            tg.setColour(design::readoutGreen.withMultipliedAlpha(alpha));
                            drawTextAt(tg, digitFont, digits, digitsRight, baseline, juce::Justification::right);
                            const auto suffixBaseline =
                                isDegree ? baseline - (design::readoutDigitHeight - suffixCap) * s : baseline;
                            drawTextAt(tg, suffixFont, suffix, suffixRight, suffixBaseline, juce::Justification::right);
                        });
                });
}

void Readout::resized()
{
    editor.setBounds(toLocal(interior).toNearestInt());
    editor.setFont(labelFont(26.0f * getScale()));
    editor.applyFontToAllText(editor.getFont());
}

void Readout::mouseDoubleClick(const juce::MouseEvent&)
{
    showEditor();
}

void Readout::showEditor()
{
    resized();
    editor.setText(digits, false);
    editor.setVisible(true);
    if (editor.isShowing())
        editor.grabKeyboardFocus();
    editor.selectAll();
}

void Readout::hideEditor()
{
    editor.setVisible(false);
}

void Readout::commitEdit()
{
    const auto text = editor.getText();
    hideEditor();
    if (onTextEntered)
        onTextEntered(text);
}
} // namespace pa::ui
