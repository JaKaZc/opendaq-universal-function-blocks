// A JPEG decoder for camera pictures in the Picture output, so the module
// depends on no image library. It reads baseline JPEG, which cameras write:
// Huffman coding, 8-bit samples, one or three components with any chroma
// subsampling, and restart markers, and it turns the picture as its EXIF
// orientation says. Progressive JPEG is declined.
#pragma once

#include <cstdint>
#include <vector>

namespace daq::modules::universal_renderer
{

/// Decodes a JPEG into RGB, three bytes a pixel, row by row from the top.
/// Returns false, leaving the outputs undefined, for data it cannot read.
bool decodeJpeg(const std::vector<uint8_t>& data, int& width, int& height, std::vector<uint8_t>& rgb);

}  // namespace daq::modules::universal_renderer
