// Characters of the built-in font, for measuring and drawing text.
#pragma once

#include "font_data.h"

#include <cstdint>
#include <string>

namespace daq::modules::universal_renderer
{

/// The next character of UTF-8 text, from byte `i`, which it moves past.
uint32_t nextCodePoint(const std::string& text, size_t& i);

/// A character's glyph, or the question mark's for one the font lacks.
const font::Glyph& glyphFor(uint32_t code, bool bold);

}  // namespace daq::modules::universal_renderer
