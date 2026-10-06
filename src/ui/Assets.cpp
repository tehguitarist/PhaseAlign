#include "ui/Assets.h"

#include <BinaryData.h>

namespace pa::ui
{
namespace
{
// Embedded resources are looked up by their original file name, so Layout.h's file list is the
// only place the names live.
std::pair<const char*, int> findResource(const juce::String& fileName)
{
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        const auto* name = BinaryData::namedResourceList[i];
        if (fileName == BinaryData::getNamedResourceOriginalFilename(name))
        {
            int size = 0;
            const auto* data = BinaryData::getNamedResource(name, size);
            return {data, size};
        }
    }
    jassertfalse; // run tools/build_assets.sh and re-run CMake
    return {nullptr, 0};
}

juce::Typeface::Ptr loadTypeface(const char* fileName)
{
    const auto [data, size] = findResource(fileName);
    return data != nullptr ? juce::Typeface::createSystemTypefaceFor(data, (size_t)size) : nullptr;
}

// Halves an ARGB image with a 2x2 box filter. Premultiplied channels average correctly.
juce::Image halve(const juce::Image& src)
{
    const auto w = juce::jmax(1, src.getWidth() / 2), h = juce::jmax(1, src.getHeight() / 2);
    juce::Image dst(juce::Image::ARGB, w, h, false, juce::SoftwareImageType());
    const juce::Image::BitmapData s(src, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData d(dst, juce::Image::BitmapData::writeOnly);
    const auto maxX = src.getWidth() - 1, maxY = src.getHeight() - 1;

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const juce::uint8* p[4] = {s.getPixelPointer(2 * x, 2 * y),
                                       s.getPixelPointer(juce::jmin(2 * x + 1, maxX), 2 * y),
                                       s.getPixelPointer(2 * x, juce::jmin(2 * y + 1, maxY)),
                                       s.getPixelPointer(juce::jmin(2 * x + 1, maxX), juce::jmin(2 * y + 1, maxY))};
            auto* out = d.getPixelPointer(x, y);
            for (int c = 0; c < 4; ++c)
                out[c] = (juce::uint8)((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4);
        }
    }
    return dst;
}

// Ink height of the glyph (not the font's line box).
float glyphHeight(const juce::Font& font, const juce::String& glyph)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText(font, glyph, 0.0f, 0.0f);
    juce::Path path;
    ga.createPath(path);
    return path.getBounds().getHeight();
}

// Scales `options` so `glyph` is `height` tall.
juce::Font sizedByGlyph(const juce::FontOptions& options, const juce::String& glyph, float height)
{
    constexpr float reference = 100.0f;
    const auto measured = glyphHeight(juce::Font(options.withHeight(reference)), glyph);
    return juce::Font(options.withHeight(measured > 0.0f ? reference * height / measured : height));
}

juce::String labelTypefaceName()
{
    static const auto name =
        juce::Font::findAllTypefaceNames().contains("Arial") ? juce::String("Arial") : juce::String("Liberation Sans");
    return name;
}
} // namespace

//==============================================================================
SourceAssets::SourceAssets()
{
    for (size_t i = 0; i < images.size(); ++i)
    {
        const auto [data, size] = findResource(layout::imageFileNames[i]);
        if (data != nullptr)
            images[i] = juce::ImageFileFormat::loadFrom(data, (size_t)size);
        jassert(images[i].isValid());
    }

    readoutTypeface = loadTypeface("DSEG7Classic-Regular.ttf");
    meterTypeface = loadTypeface("IBMPlexMono-Regular.ttf");
    meterBoldTypeface = loadTypeface("IBMPlexMono-SemiBold.ttf");
}

const juce::Image& ScaledImages::get(layout::Image id, int pixelWidth, int pixelHeight)
{
    const auto key = std::make_tuple((int)id, pixelWidth, pixelHeight);
    auto it = cache.find(key);
    if (it == cache.end())
        it = cache.emplace(key, resample(sourceAssets->image(id), pixelWidth, pixelHeight)).first;
    return it->second;
}

juce::Image resample(const juce::Image& source, int width, int height)
{
    if (! source.isValid() || width <= 0 || height <= 0)
        return {};

    auto current = juce::SoftwareImageType().convert(source.convertedToFormat(juce::Image::ARGB));
    while (current.getWidth() >= 2 * width && current.getHeight() >= 2 * height)
        current = halve(current);

    juce::Image result(juce::Image::ARGB, width, height, true);
    juce::Graphics g(result);
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    g.drawImage(current, 0, 0, width, height, 0, 0, current.getWidth(), current.getHeight());
    return result;
}

float physicalScale(juce::Graphics& g)
{
    return g.getInternalContext().getPhysicalPixelScaleFactor();
}

juce::Rectangle<float> fittedArea(const ScaledImages& images, layout::Image id, juce::Rectangle<float> area)
{
    const auto& src = images.source().image(id);
    return juce::RectanglePlacement(juce::RectanglePlacement::centred)
        .appliedTo(juce::Rectangle<float>((float)src.getWidth(), (float)src.getHeight()), area);
}

void drawFitted(juce::Graphics& g, ScaledImages& images, layout::Image id, juce::Rectangle<float> area)
{
    const auto scale = physicalScale(g);
    const auto placed = fittedArea(images, id, area) * scale;

    // Snap to whole device pixels so the cached image maps 1:1 and stays sharp.
    const auto x = std::round(placed.getX()), y = std::round(placed.getY());
    const auto w = juce::jmax(1, juce::roundToInt(placed.getWidth()));
    const auto h = juce::jmax(1, juce::roundToInt(placed.getHeight()));
    g.drawImageTransformed(images.get(id, w, h), juce::AffineTransform::translation(x, y).scaled(1.0f / scale));
}

void drawRotated(juce::Graphics& g, ScaledImages& images, layout::Image id, juce::Rectangle<float> area, float radians)
{
    const auto scale = physicalScale(g);
    const auto placed = fittedArea(images, id, area);
    const auto w = juce::jmax(1, juce::roundToInt(placed.getWidth() * scale));
    const auto h = juce::jmax(1, juce::roundToInt(placed.getHeight() * scale));

    juce::Graphics::ScopedSaveState save(g);
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    g.drawImageTransformed(images.get(id, w, h), juce::AffineTransform::translation(-0.5f * (float)w, -0.5f * (float)h)
                                                     .scaled(1.0f / scale)
                                                     .rotated(radians)
                                                     .translated(placed.getCentre()));
}

//==============================================================================
juce::Font labelFont(float capHeight)
{
    return sizedByGlyph(juce::FontOptions(labelTypefaceName(), 10.0f, juce::Font::bold), "H", capHeight);
}

juce::Font scaleLabelFont(float digitHeight)
{
    static const auto name = juce::Font::findAllTypefaceNames().contains("Arial Hebrew") ? juce::String("Arial Hebrew")
                                                                                         : labelTypefaceName();
    return sizedByGlyph(juce::FontOptions(name, 10.0f, juce::Font::plain), "0", digitHeight);
}

juce::Font readoutFont(const SourceAssets& assets, float digitHeight)
{
    return sizedByGlyph(juce::FontOptions(assets.readoutTypeface), "8", digitHeight);
}

juce::Font meterFont(const SourceAssets& assets, float capHeight, bool bold)
{
    return sizedByGlyph(juce::FontOptions(bold ? assets.meterBoldTypeface : assets.meterTypeface), "H", capHeight);
}

void drawTextAt(juce::Graphics& g, const juce::Font& font, const juce::String& text, float x, float baselineY,
                juce::Justification justification)
{
    const auto width = textWidth(font, text);
    if (justification.testFlags(juce::Justification::right))
        x -= width;
    else if (justification.testFlags(juce::Justification::horizontallyCentred))
        x -= 0.5f * width;

    juce::GlyphArrangement ga;
    ga.addLineOfText(font, text, x, baselineY);
    ga.draw(g);
}

float textWidth(const juce::Font& font, const juce::String& text)
{
    return juce::GlyphArrangement::getStringWidth(font, text);
}
} // namespace pa::ui
