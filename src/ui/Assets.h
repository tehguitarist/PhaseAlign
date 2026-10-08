#pragma once

#include "ui/Layout.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <tuple>

namespace pa::ui
{
// The decoded artwork and embedded typefaces, loaded once and shared by every editor instance.
class SourceAssets
{
  public:
    SourceAssets();

    const juce::Image& image(layout::Image id) const { return images[(size_t)id]; }

    juce::Typeface::Ptr readoutTypeface; // DSEG7 Classic
    juce::Typeface::Ptr meterTypeface;   // IBM Plex Mono
    juce::Typeface::Ptr meterBoldTypeface;

  private:
    std::array<juce::Image, (size_t)layout::Image::count> images;
};

// Per-editor cache of the artwork pre-scaled to exact physical pixel sizes:
// each image is resampled once per size and then painted 1:1, never resampled per paint.
class ScaledImages
{
  public:
    const SourceAssets& source() const { return *sourceAssets; }

    const juce::Image& get(layout::Image id, int pixelWidth, int pixelHeight);
    void clear() { cache.clear(); } // on resize: old sizes won't be asked for again

  private:
    juce::SharedResourcePointer<SourceAssets> sourceAssets;
    std::map<std::tuple<int, int, int>, juce::Image> cache;
};

// High-quality resample: box-filter halvings down to within 2x of the target, then one interpolated
// step (a single interpolated step from 3-4x larger would alias).
juce::Image resample(const juce::Image& source, int width, int height);

// Physical pixels per logical unit of this Graphics context (display scale times any transform).
float physicalScale(juce::Graphics&);

// The part of `area` that `id` covers when fitted centred with its aspect ratio kept.
juce::Rectangle<float> fittedArea(const ScaledImages&, layout::Image id, juce::Rectangle<float> area);

// Draws `id` fitted into `area`, snapped to device pixels and painted 1:1 from the cache.
void drawFitted(juce::Graphics&, ScaledImages&, layout::Image id, juce::Rectangle<float> area);

// Draws `id` fitted into `area` and rotated by `radians` about the area's centre.
void drawRotated(juce::Graphics&, ScaledImages&, layout::Image id, juce::Rectangle<float> area, float radians);

//==============================================================================
// Fonts. Sizes are given as the height of a reference glyph ("H" or "8"), measured from the font,
// so the design values match what was measured on the artwork whatever the font's metrics are.
juce::Font labelFont(float capHeight);        // Arial Bold (Liberation Sans on Linux)
juce::Font scaleLabelFont(float digitHeight); // Arial Hebrew if installed (as in the art), else labelFont's
juce::Font readoutFont(const SourceAssets&, float digitHeight); // DSEG7 Classic
juce::Font meterFont(const SourceAssets&, float capHeight, bool bold = false);

// Draws `text` with its baseline at `baselineY`, aligned left/right/centre on `x` by `justification`.
void drawTextAt(juce::Graphics&, const juce::Font&, const juce::String& text, float x, float baselineY,
                juce::Justification justification);

float textWidth(const juce::Font&, const juce::String& text);

//==============================================================================
// A component's static content rendered once at device resolution and re-rendered only when its
// key (whatever the content depends on) or the pixel size changes.
class CachedLayer
{
  public:
    // `render` draws in the component's local coordinates.
    template <typename RenderFn>
    void paint(juce::Graphics& g, juce::Rectangle<int> localBounds, const juce::String& key, RenderFn&& render)
    {
        const auto scale = physicalScale(g);
        const auto w = juce::roundToInt((float)localBounds.getWidth() * scale);
        const auto h = juce::roundToInt((float)localBounds.getHeight() * scale);
        if (w <= 0 || h <= 0)
            return;

        if (key != cachedKey || image.getWidth() != w || image.getHeight() != h)
        {
            image = juce::Image(juce::Image::ARGB, w, h, true);
            juce::Graphics ig(image);
            ig.addTransform(juce::AffineTransform::scale(scale));
            render(ig, scale);
            cachedKey = key;
        }
        g.drawImageTransformed(image, juce::AffineTransform::scale(1.0f / scale)
                                          .translated((float)localBounds.getX(), (float)localBounds.getY()));
    }

    void invalidate() { cachedKey = {}; }

  private:
    juce::Image image;
    juce::String cachedKey;
};

// Draws content with a soft glow behind it: `content` is drawn into a device-resolution image, which
// is blurred into a glow of `glowColour` and then drawn on top. `radius` is in local units.
template <typename ContentFn>
void drawWithGlow(juce::Graphics& g, juce::Rectangle<int> localBounds, float scale, juce::Colour glowColour,
                  float radius, ContentFn&& content)
{
    const auto w = juce::roundToInt((float)localBounds.getWidth() * scale);
    const auto h = juce::roundToInt((float)localBounds.getHeight() * scale);
    if (w <= 0 || h <= 0)
        return;

    juce::Image layer(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics lg(layer);
        lg.addTransform(juce::AffineTransform::scale(scale));
        content(lg);
    }

    juce::Graphics::ScopedSaveState save(g);
    g.addTransform(juce::AffineTransform::scale(1.0f / scale));
    juce::DropShadow(glowColour, juce::jmax(1, juce::roundToInt(radius * scale)), {}).drawForImage(g, layer);
    g.drawImageAt(layer, 0, 0);
}
} // namespace pa::ui
