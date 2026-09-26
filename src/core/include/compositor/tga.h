// Truevision TGA: reads 8-bit indexed (24/32-bit colour maps), 8-bit grayscale and 24/32-bit truecolour, raw
// or RLE, any origin; writes 32-bit RLE with straight alpha, top-left origin.
// Ported from Patchy (MIT, src/third_party/patchy_psd/README.md): src/formats/tga_document_io.cpp.
#pragma once
#include "image.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace compositor {

/// Whether the bytes start with a TGA header this reader takes (TGA has no signature, so this is a plausibility check).
bool isTgaData(const uint8_t* data, size_t size);
/// Decodes to premultiplied RGBA8. A 32-bit file whose alpha is zero everywhere opens opaque (a common authoring slip).
std::shared_ptr<Image> decodeTgaImage(const uint8_t* data, size_t size, std::string* error = nullptr);
std::shared_ptr<Image> readTgaImage(const std::string& path, std::string* error = nullptr);

/// Encodes premultiplied RGBA8 as a 32-bit RLE TGA. Sides above 65535 fail.
bool encodeTgaImage(const Image& image, std::vector<uint8_t>& out, std::string* error = nullptr);
bool writeTgaImage(const std::string& path, const Image& image, std::string* error = nullptr);

} // namespace compositor
