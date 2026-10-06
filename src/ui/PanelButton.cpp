#include "ui/PanelButton.h"

namespace pa::ui
{
PanelButton::PanelButton(ScaledImages& imagesIn, juce::Rectangle<float> slot, juce::RangedAudioParameter& param)
    : DesignComponent(slot), images(imagesIn), attachment(param,
                                                          [this](float v)
                                                          {
                                                              const auto newState = v >= 0.5f;
                                                              if (newState == on)
                                                                  return;
                                                              on = newState;
                                                              repaint();
                                                              if (onStateChange)
                                                                  onStateChange(on);
                                                          })
{
    setTitle(param.getName(64));
    attachment.sendInitialUpdate();
}

void PanelButton::paint(juce::Graphics& g)
{
    g.setOpacity(contentAlpha());
    drawFitted(g, images, on ? layout::Image::buttonIn : layout::Image::buttonOut, getLocalBounds().toFloat());
}

bool PanelButton::hitTest(int x, int y)
{
    const auto centre = getLocalBounds().toFloat().getCentre();
    return centre.getDistanceFrom({(float)x + 0.5f, (float)y + 0.5f}) <= 0.5f * (float)getWidth();
}

void PanelButton::mouseDown(const juce::MouseEvent&)
{
    toggle();
}

void PanelButton::toggle()
{
    attachment.setValueAsCompleteGesture(on ? 0.0f : 1.0f);
}

std::unique_ptr<juce::AccessibilityHandler> PanelButton::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(
        *this, juce::AccessibilityRole::toggleButton,
        juce::AccessibilityActions().addAction(juce::AccessibilityActionType::toggle, [this] { toggle(); }));
}

//==============================================================================
ImageIndicator::ImageIndicator(ScaledImages& imagesIn, juce::Rectangle<float> slot, layout::Image onImageIn,
                               layout::Image offImageIn)
    : DesignComponent(slot), images(imagesIn), onImage(onImageIn), offImage(offImageIn)
{
    setInterceptsMouseClicks(false, false);
}

void ImageIndicator::setLit(bool shouldBeLit)
{
    if (shouldBeLit == lit)
        return;
    lit = shouldBeLit;
    repaint();
}

void ImageIndicator::paint(juce::Graphics& g)
{
    drawFitted(g, images, lit ? onImage : offImage, getLocalBounds().toFloat());
}

//==============================================================================
SquareButton::SquareButton(ScaledImages& scaledImagesIn, juce::Rectangle<float> slot, Images imagesIn)
    : DesignComponent(slot), scaledImages(scaledImagesIn), images(imagesIn)
{
}

void SquareButton::setLit(bool shouldBeLit)
{
    if (shouldBeLit == lit)
        return;
    lit = shouldBeLit;
    repaint();
}

void SquareButton::setPressed(bool shouldBePressed)
{
    if (shouldBePressed == pressed)
        return;
    pressed = shouldBePressed;
    repaint();
}

void SquareButton::paint(juce::Graphics& g)
{
    const auto id =
        pressed ? (lit ? images.pressedLit : images.pressedUnlit) : (lit ? images.releasedLit : images.releasedUnlit);
    drawFitted(g, scaledImages, id, getLocalBounds().toFloat());
}

void SquareButton::mouseDown(const juce::MouseEvent&)
{
    setPressed(true);
}

void SquareButton::mouseDrag(const juce::MouseEvent& e)
{
    setPressed(getLocalBounds().contains(e.getPosition()));
}

void SquareButton::mouseUp(const juce::MouseEvent& e)
{
    const auto clicked = pressed && getLocalBounds().contains(e.getPosition());
    setPressed(false);
    if (clicked && onClick)
        onClick();
}

std::unique_ptr<juce::AccessibilityHandler> SquareButton::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(
        *this, juce::AccessibilityRole::button,
        juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press,
                                               [this]
                                               {
                                                   if (onClick)
                                                       onClick();
                                               }));
}
} // namespace pa::ui
