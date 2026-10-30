#include "text.h"

#include "surface.h"

#include <algorithm>
#include <iterator>

namespace daq::modules::universal_renderer
{

namespace
{

template <size_t N>
const font::Glyph& glyphIn(const font::Glyph (&glyphs)[N], uint32_t code)
{
    const auto found = std::lower_bound(std::begin(glyphs), std::end(glyphs), code,
                                        [](const font::Glyph& glyph, uint32_t value) { return glyph.code < value; });
    if (found != std::end(glyphs) && found->code == code)
        return *found;
    return glyphIn(glyphs, '?');
}

}  // namespace

uint32_t nextCodePoint(const std::string& text, size_t& i)
{
    const auto byte = [&](size_t at) { return at < text.size() ? static_cast<uint8_t>(text[at]) : 0u; };
    const uint32_t first = byte(i);
    if (first < 0x80)
    {
        i += 1;
        return first;
    }
    if ((first >> 5) == 0x6)
    {
        const uint32_t code = ((first & 0x1F) << 6) | (byte(i + 1) & 0x3F);
        i += 2;
        return code;
    }
    if ((first >> 4) == 0xE)
    {
        const uint32_t code = ((first & 0x0F) << 12) | ((byte(i + 1) & 0x3F) << 6) | (byte(i + 2) & 0x3F);
        i += 3;
        return code;
    }
    i += (first >> 3) == 0x1E ? 4 : 1;
    return '?';
}

const font::Glyph& glyphFor(uint32_t code, bool bold)
{
    return bold ? glyphIn(font::kBoldGlyphs, code) : glyphIn(font::kRegularGlyphs, code);
}

float Surface::textWidth(const std::string& value, bool bold)
{
    float width = 0.0f;
    for (size_t i = 0; i < value.size();)
        width += glyphFor(nextCodePoint(value, i), bold).advance / 64.0f;
    return width;
}

float Surface::lineHeight()
{
    return static_cast<float>(font::kRegularAscent + font::kRegularDescent);
}

}  // namespace daq::modules::universal_renderer
