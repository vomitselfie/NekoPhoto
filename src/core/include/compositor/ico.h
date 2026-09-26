// Windows icons (.ico) and cursors (.cur): every size in the file opens as its own layer, named "WxH", the
// largest on top and the only one visible; the canvas is the largest. BMP entries (1/2/4/8/24/32-bit, with the
// AND mask) and PNG entries are read. Export writes a multi-size .ico: 32-bit BMP entries, the 256 px one PNG.
// Ported from Patchy (MIT, src/third_party/patchy_psd/README.md): src/formats/ico_document_io.cpp.
#pragma once
#include "psd.h"
#include <optional>
#include <string>
#include <vector>

namespace compositor {

bool isIcoData(const uint8_t* data, size_t size);
std::optional<PsdImport> importIcoBytes(const std::vector<uint8_t>& bytes, std::string* error = nullptr);
std::optional<PsdImport> importIco(const std::string& path, std::string* error = nullptr);

/// The sizes an exported icon holds unless told otherwise.
inline const std::vector<int> defaultIcoSizes = {16, 32, 48, 256};

/// Encodes a square icon at each of `sizes` (1..256): `flattened` (premultiplied) scaled to fit, centred on a
/// transparent square with an area average. When `document` is given, a layer named "WxH" of exactly that size
/// (as an opened icon has) supplies that size's pixels instead.
bool encodeIco(const Image& flattened, const std::vector<int>& sizes, std::vector<uint8_t>& out, std::string* error = nullptr,
               const Document* document = nullptr);
bool writeIco(const std::string& path, const Image& flattened, const std::vector<int>& sizes = defaultIcoSizes, std::string* error = nullptr,
              const Document* document = nullptr);

} // namespace compositor
