// Aseprite import, ported from Patchy (MIT, src/third_party/patchy_psd/README.md):
// src/formats/aseprite_document_io.cpp (the reader; its writer is not ported), following the published
// specification (https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md). Frame 1 only; unknown
// chunks are skipped by length. Compressed cels inflate through zlib instead of Patchy's vendored miniz.
#include "compositor/aseprite.h"
#include "format_io.h"
#include <algorithm>
#include <array>
#include <map>
#include <zlib.h>

namespace compositor {

namespace {

constexpr uint16_t headerMagic = 0xA5E0, frameMagic = 0xF1FA;
constexpr uint16_t chunkLayer = 0x2004, chunkCel = 0x2005, chunkPaletteNew = 0x2019, chunkPaletteOld = 0x0004, chunkPaletteOld2 = 0x0011;

/// Aseprite's blend mode numbers; the ones NekoPhoto lacks are empty.
std::optional<BlendMode> blendFromAseprite(uint16_t mode) {
    switch (mode) {
    case 0: return BlendMode::Normal;
    case 1: return BlendMode::Multiply;
    case 2: return BlendMode::Screen;
    case 3: return BlendMode::Overlay;
    case 4: return BlendMode::Darken;
    case 5: return BlendMode::Lighten;
    case 6: return BlendMode::ColorDodge;
    case 7: return BlendMode::ColorBurn;
    case 10: return BlendMode::Difference;
    case 12: return BlendMode::Hue;
    case 13: return BlendMode::Saturation;
    case 14: return BlendMode::Color;
    case 15: return BlendMode::Luminosity;
    default: return std::nullopt;   // 8 Hard Light, 9 Soft Light, 11 Exclusion, 16 Addition, 17 Subtract, 18 Divide
    }
}

const char* asepriteBlendName(uint16_t mode) {
    switch (mode) {
    case 8: return "Hard Light";
    case 9: return "Soft Light";
    case 11: return "Exclusion";
    case 16: return "Addition";
    case 17: return "Subtract";
    case 18: return "Divide";
    default: return "unknown";
    }
}

struct AseLayer {
    uint16_t flags = 0, type = 0, childLevel = 0, blendMode = 0;
    uint8_t opacity = 255;
    std::string name;
};

struct AseCel {
    int x = 0, y = 0, width = 0, height = 0;
    uint8_t opacity = 255;
    std::vector<uint8_t> pixels;   // in the file's colour depth
};

std::string readString(format_io::Reader& r) {
    const uint16_t length = r.u16();
    std::string text;
    if (!r.has(length)) { r.ok = false; return text; }
    text.assign(reinterpret_cast<const char*>(r.data + r.pos), length);
    r.pos += length;
    return text;
}

bool inflateExact(const uint8_t* data, size_t size, size_t expected, std::vector<uint8_t>& out) {
    out.resize(expected);
    uLongf length = uLongf(expected);
    if (expected == 0) return true;
    return uncompress(out.data(), &length, data, uLong(size)) == Z_OK && length == expected;
}

std::optional<PsdImport> fail(std::string* error, const std::string& why) { if (error) *error = why; return std::nullopt; }

} // namespace

bool isAsepriteData(const uint8_t* data, size_t size) {
    return size >= 128 && uint16_t(data[4] | data[5] << 8) == headerMagic;
}

std::optional<PsdImport> importAsepriteBytes(const std::vector<uint8_t>& bytes, std::string* error) {
    if (bytes.size() >= 4 && std::equal(bytes.begin(), bytes.begin() + 4, "ASEF"))
        return fail(error, "This is an Adobe swatch exchange palette (.ase), not an Aseprite sprite.");
    if (!isAsepriteData(bytes.data(), bytes.size())) return fail(error, "File is not an Aseprite sprite");
    format_io::Reader r(bytes.data(), bytes.size());
    r.skip(6);
    const uint16_t frames = r.u16();
    const int width = r.u16(), height = r.u16();
    const uint16_t depth = r.u16();
    const uint32_t flags = r.u32();
    const bool layerOpacityValid = flags & 1, groupOpacityValid = flags & 2;
    r.skip(2 + 4 + 4);
    const uint8_t transparentIndex = r.u8();
    r.seek(128);
    if (!Document::validDimension(width) || !Document::validDimension(height) || (long long)width * height > Document::pixelBudget)
        return fail(error, "The sprite's size is not one a document can have.");
    if (depth != 8 && depth != 16 && depth != 32) return fail(error, "The sprite's colour depth is not supported.");
    if (frames == 0) return fail(error, "The sprite holds no frames.");
    const size_t bpp = depth / 8u;

    std::vector<AseLayer> layers;
    std::map<uint16_t, AseCel> cels;
    std::vector<std::array<uint8_t, 4>> palette;
    int tilemaps = 0;
    {
        const size_t frameStart = r.pos;
        const uint32_t frameBytes = r.u32();
        if (r.u16() != frameMagic || !r.ok) return fail(error, "The sprite's first frame is damaged.");
        const uint16_t oldChunks = r.u16();
        r.skip(4);
        const uint32_t newChunks = r.u32();
        const uint32_t chunkCount = newChunks ? newChunks : oldChunks;
        const size_t frameEnd = std::min(bytes.size(), frameStart + frameBytes);
        for (uint32_t c = 0; c < chunkCount && r.ok && r.pos + 6 <= frameEnd; c++) {
            const size_t chunkStart = r.pos;
            const uint32_t chunkSize = r.u32();
            const uint16_t type = r.u16();
            if (chunkSize < 6 || chunkSize > bytes.size() - chunkStart) break;   // damaged length: stop here
            const size_t chunkEnd = chunkStart + chunkSize;
            format_io::Reader cr(bytes.data(), chunkEnd);   // reads stop at the chunk's end
            cr.pos = r.pos;
            if (type == chunkLayer) {
                AseLayer l;
                l.flags = cr.u16(); l.type = cr.u16(); l.childLevel = cr.u16();
                cr.skip(4);
                l.blendMode = cr.u16();
                l.opacity = cr.u8();
                cr.skip(3);
                l.name = readString(cr);
                if (l.type == 2) tilemaps++;
                layers.push_back(std::move(l));
            } else if (type == chunkCel) {
                const uint16_t layerIndex = cr.u16();
                AseCel cel;
                cel.x = int16_t(cr.u16());
                cel.y = int16_t(cr.u16());
                cel.opacity = cr.u8();
                const uint16_t celType = cr.u16();
                cr.skip(2 + 5);
                if ((celType == 0 || celType == 2) && cr.ok) {
                    cel.width = cr.u16();
                    cel.height = cr.u16();
                    const size_t pixelBytes = size_t(cel.width) * size_t(cel.height) * bpp;
                    if (!cr.ok) return fail(error, "A cel chunk is truncated.");
                    if (celType == 0) {
                        if (!cr.has(pixelBytes)) return fail(error, "A cel chunk is truncated.");
                        cel.pixels.assign(bytes.begin() + std::ptrdiff_t(cr.pos), bytes.begin() + std::ptrdiff_t(cr.pos + pixelBytes));
                    } else if (!inflateExact(bytes.data() + cr.pos, chunkEnd - cr.pos, pixelBytes, cel.pixels)) {
                        return fail(error, "A cel's compressed pixels could not be read.");
                    }
                    cels.emplace(layerIndex, std::move(cel));
                }
                // Linked cels (type 1) cannot appear in frame 1; tilemap cels (3) belong to skipped layers.
            } else if (type == chunkPaletteNew) {
                const uint32_t size = cr.u32(), first = cr.u32(), last = cr.u32();
                cr.skip(8);
                if (size <= 256 && first <= last && last < 256) {
                    if (palette.size() < size) palette.resize(size, {0, 0, 0, 255});
                    for (uint32_t i = first; i <= last && cr.ok; i++) {
                        const uint16_t entryFlags = cr.u16();
                        const uint8_t rd = cr.u8(), g = cr.u8(), b = cr.u8(), a = cr.u8();
                        if (i < palette.size()) palette[i] = {rd, g, b, a};
                        if (entryFlags & 1) readString(cr);
                    }
                }
            } else if ((type == chunkPaletteOld || type == chunkPaletteOld2) && palette.empty()) {
                const uint16_t packets = cr.u16();
                size_t index = 0;
                for (uint16_t p = 0; p < packets && cr.ok; p++) {
                    index += cr.u8();
                    size_t count = cr.u8();
                    if (count == 0) count = 256;
                    const int scale = type == chunkPaletteOld2 ? 4 : 1;   // 0x0011 stores 0..63
                    for (size_t i = 0; i < count && cr.ok && index + i < 256; i++) {
                        const uint8_t rd = cr.u8(), g = cr.u8(), b = cr.u8();
                        if (index + i >= palette.size()) palette.resize(index + i + 1, {0, 0, 0, 255});
                        palette[index + i] = {uint8_t(std::min(255, rd * scale)), uint8_t(std::min(255, g * scale)), uint8_t(std::min(255, b * scale)), 255};
                    }
                    index += count;
                }
            }
            r.seek(chunkEnd);
        }
    }
    if (layers.empty()) return fail(error, "The sprite holds no layers.");
    if (depth == 8 && palette.empty()) return fail(error, "The indexed sprite is missing its palette.");

    PsdImport result;
    Document& document = result.document;
    document = Document(width, height);
    std::vector<std::string> lossy;
    std::vector<Uuid> parents;   // parents[k]: the folder open at child level k + 1
    long long total = 0;
    for (size_t index = 0; index < layers.size(); index++) {
        const AseLayer& source = layers[index];
        if (source.type == 2) continue;
        const bool isGroup = source.type == 1, visible = source.flags & 1, background = source.flags & 8;
        // Without header flag 2 a group's blend and opacity bytes mean nothing (Aseprite writes opacity 0 there).
        const bool meaningful = !isGroup || groupOpacityValid;
        std::optional<BlendMode> blend = meaningful ? blendFromAseprite(source.blendMode) : BlendMode::Normal;
        if (!blend) { lossy.push_back("\"" + source.name + "\" (" + asepriteBlendName(source.blendMode) + ")"); blend = BlendMode::Normal; }
        const double layerOpacity = layerOpacityValid && meaningful ? source.opacity / 255.0 : 1.0;

        Layer layer;
        auto found = isGroup ? cels.end() : cels.find(uint16_t(index));
        if (isGroup) {
            layer = Layer(source.name, document.size());
            layer.isGroup = true;
            layer.opacity = layerOpacity;
            layer.passThrough = *blend == BlendMode::Normal && layerOpacity >= 1;
        } else if (found == cels.end() || found->second.width <= 0 || found->second.height <= 0) {
            layer = Layer(source.name, document.size());   // no pixels in frame 1
            layer.opacity = layerOpacity;
        } else {
            const AseCel& cel = found->second;
            if (cel.width > maxImageSide || cel.height > maxImageSide) return fail(error, "A cel is larger than a layer may be.");
            total += (long long)cel.width * cel.height;
            if (total > Document::projectPixelBudget) return fail(error, "The layers exceed the gigapixel a project may hold.");
            auto image = std::make_shared<Image>(cel.width, cel.height);
            for (int y = 0; y < cel.height; y++) {
                uint8_t* d = image->row(y);
                for (int x = 0; x < cel.width; x++, d += 4) {
                    const uint8_t* s = cel.pixels.data() + (size_t(y) * size_t(cel.width) + size_t(x)) * bpp;
                    if (depth == 32) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; }
                    else if (depth == 16) { d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; }
                    else {
                        if (s[0] == transparentIndex && !background) continue;
                        const auto& c = palette[std::min<size_t>(s[0], palette.size() - 1)];
                        d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = 255;
                    }
                }
            }
            premultiply(*image);
            layer = Layer(Asset::make(image, source.name), Point(cel.x, cel.y));
            layer.opacity = layerOpacity * cel.opacity / 255.0;
        }
        layer.name = source.name;
        layer.visible = visible;
        layer.blendMode = *blend;

        // File order is bottom first, each group before its children. A level deeper than the open folders
        // (a damaged file, or a child of a skipped tilemap) lands at the top level.
        size_t level = source.childLevel;
        if (level > parents.size()) level = 0;
        if (level > 0) layer.parentId = parents[level - 1];
        parents.resize(level);
        if (isGroup) parents.push_back(layer.id);
        document.layers.push_back(std::move(layer));
        if (document.layers.size() > size_t(Document::maxLayers)) return fail(error, "The sprite has more layers than a document may hold.");
    }
    if (document.layers.empty()) return fail(error, "The sprite holds no layers this reader can use.");
    if (frames > 1) result.notes.push_back("Only the first of " + std::to_string(frames) + " frames was imported.");
    if (!lossy.empty()) {
        std::string names;
        for (const auto& n : lossy) names += (names.empty() ? "" : ", ") + n;
        result.notes.push_back("Blend modes NekoPhoto lacks opened as Normal on: " + names + ".");
    }
    if (tilemaps) result.notes.push_back(std::to_string(tilemaps) + " tilemap layer(s) were left out.");
    return result;
}

std::optional<PsdImport> importAseprite(const std::string& path, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!format_io::readFile(path, bytes, error)) return std::nullopt;
    return importAsepriteBytes(bytes, error);
}

} // namespace compositor
