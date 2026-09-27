// PNG reading with libpng and writing with zlib on every core, for .comp assets and PNG export.
#pragma once
#include "image.h"
#include "imaget.h"
#include <cstdint>
#include <string>
#include <vector>

namespace compositor {

/// zlib level for everything written: level 6, libpng's default, now that the strips compress in parallel.
inline constexpr int pngCompressionLevel = 6;

/// `icc`: the file's embedded colour profile (iCCP), empty when it has none.
struct PngInfo { int width = 0, height = 0, bitDepth = 8; bool gray = false; std::vector<uint8_t> icc; };

/// Header only, without decoding pixels.
bool readPngInfo(const std::string& path, PngInfo& info, std::string* error = nullptr);
/// Decodes any PNG to premultiplied RGBA8 (16-bit and palette images are converted).
std::shared_ptr<Image> readPngImage(const std::string& path, std::string* error = nullptr);
/// Decodes an 8-bit grayscale PNG without alpha; fails for anything else (masks are strict).
std::shared_ptr<GrayImage> readPngGray(const std::string& path, std::string* error = nullptr);

/// Writes straight-alpha RGBA8; `dpi` adds a pHYs chunk when > 0. With `icc`, the image is tagged with that profile
/// (iCCP) instead of as sRGB.
bool writePngImage(const std::string& path, const Image& image, double dpi = 0, std::string* error = nullptr, const std::vector<uint8_t>* icc = nullptr);
bool writePngGray(const std::string& path, const GrayImage& image, std::string* error = nullptr);

/// 16 bits per channel (docs/high-bit-depth-plan.md, section 8): a 16-bit PNG's 0..65535 read into, and written from,
/// the 0..32768 of a 16-bit document. `readPngImage16` takes any PNG (8-bit ones are widened); the gray reader wants a
/// 16-bit grayscale file without alpha, as the project package writes masks of a 16-bit document.
std::shared_ptr<Image16> readPngImage16(const std::string& path, std::string* error = nullptr);
std::shared_ptr<Image16> decodePngImage16(const uint8_t* data, size_t size, std::string* error = nullptr);
std::shared_ptr<Gray16> readPngGray16(const std::string& path, std::string* error = nullptr);
bool encodePngImage16(const Image16& image, std::vector<uint8_t>& out, double dpi = 0, std::string* error = nullptr, const std::vector<uint8_t>* icc = nullptr);
bool writePngImage16(const std::string& path, const Image16& image, double dpi = 0, std::string* error = nullptr, const std::vector<uint8_t>* icc = nullptr);
bool writePngGray16(const std::string& path, const Gray16& image, std::string* error = nullptr);

/// The same, in memory.
bool encodePngImage(const Image& image, std::vector<uint8_t>& out, double dpi = 0, std::string* error = nullptr, const std::vector<uint8_t>* icc = nullptr);
std::shared_ptr<Image> decodePngImage(const uint8_t* data, size_t size, std::string* error = nullptr);

} // namespace compositor
