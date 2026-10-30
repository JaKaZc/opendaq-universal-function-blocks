// A PNG encoder for the Picture output, with its own deflate, so the module
// depends on no compression library.
#pragma once

#include <cstdint>
#include <vector>

namespace daq::modules::universal_renderer
{

/// Encodes an RGB picture, three bytes a pixel, row by row from the top.
std::vector<uint8_t> encodePng(const std::vector<uint8_t>& rgb, int width, int height);

}  // namespace daq::modules::universal_renderer
