// PNG reading and writing with libpng, for .comp assets and PNG export.
#pragma once
#include "image.h"
#include <cstdint>
#include <string>
#include <vector>

namespace compositor {

struct PngInfo { int width = 0, height = 0, bitDepth = 8; bool gray = false; };

/// Header only, without decoding pixels.
bool readPngInfo(const std::string& path, PngInfo& info, std::string* error = nullptr);
/// Decodes any PNG to premultiplied RGBA8 (16-bit and palette images are converted).
std::shared_ptr<Image> readPngImage(const std::string& path, std::string* error = nullptr);
/// Decodes an 8-bit grayscale PNG without alpha; fails for anything else (masks are strict).
std::shared_ptr<GrayImage> readPngGray(const std::string& path, std::string* error = nullptr);

/// Writes straight-alpha RGBA8; `dpi` adds a pHYs chunk when > 0.
bool writePngImage(const std::string& path, const Image& image, double dpi = 0, std::string* error = nullptr);
bool writePngGray(const std::string& path, const GrayImage& image, std::string* error = nullptr);

/// The same, in memory.
bool encodePngImage(const Image& image, std::vector<uint8_t>& out, double dpi = 0, std::string* error = nullptr);
std::shared_ptr<Image> decodePngImage(const uint8_t* data, size_t size, std::string* error = nullptr);

} // namespace compositor
