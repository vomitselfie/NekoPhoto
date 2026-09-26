// ICO / CUR reading and ICO writing, ported from Patchy (MIT, src/third_party/patchy_psd/README.md):
// src/formats/ico_document_io.cpp, adapted to compositor::Document (a layer per size) and to NekoPhoto's own
// PNG codec for PNG-compressed entries. Cursor hotspots are not kept.
#include "compositor/ico.h"
#include "compositor/png.h"
#include "format_io.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace compositor {

namespace {

constexpr size_t dirSize = 6, dirEntrySize = 16;
constexpr uint32_t infoHeaderSize = 40;
constexpr uint16_t typeIcon = 1, typeCursor = 2;
constexpr int maxIconSize = 256;

/// Straight-alpha RGBA, tightly packed.
struct Rgba {
    int width = 0, height = 0;
    std::vector<uint8_t> px;
    uint8_t* at(int x, int y) { return px.data() + (size_t(y) * size_t(width) + size_t(x)) * 4; }
    const uint8_t* at(int x, int y) const { return px.data() + (size_t(y) * size_t(width) + size_t(x)) * 4; }
};

size_t stride32(int width, int bits) { return (size_t(width) * size_t(bits) + 31) / 32 * 4; }

bool hasPngSignature(const uint8_t* d, size_t n) {
    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    return n >= 8 && std::equal(sig, sig + 8, d);
}

/// A BITMAPINFOHEADER entry (height doubled for the AND mask): XOR pixels bottom-up, then a 1-bit mask.
bool decodeBmpEntry(const uint8_t* bytes, size_t size, Rgba& image) {
    format_io::Reader r(bytes, size);
    const uint32_t headerSize = r.u32();
    if (headerSize < infoHeaderSize) return false;
    const int32_t width = int32_t(r.u32());
    const int32_t doubledHeight = int32_t(r.u32());
    r.skip(2);
    const uint16_t bitCount = r.u16();
    const uint32_t compression = r.u32();
    r.skip(12);
    uint32_t colorsUsed = r.u32();
    r.skip(4);
    r.skip(headerSize - infoHeaderSize);
    if (!r.ok || compression != 0 || width <= 0 || width > maxIconSize) return false;
    const bool hasMask = doubledHeight > 0 && doubledHeight % 2 == 0;
    const int height = hasMask ? doubledHeight / 2 : doubledHeight;
    if (height <= 0 || height > maxIconSize) return false;

    std::vector<std::array<uint8_t, 3>> palette;
    if (bitCount <= 8) {
        if (bitCount != 1 && bitCount != 2 && bitCount != 4 && bitCount != 8) return false;
        if (colorsUsed == 0) colorsUsed = 1u << bitCount;
        if (colorsUsed > 256) return false;
        for (uint32_t i = 0; i < colorsUsed; i++) {
            const uint8_t b = r.u8(), g = r.u8(), rd = r.u8();
            r.skip(1);
            palette.push_back({rd, g, b});
        }
    } else if (bitCount != 24 && bitCount != 32) {
        return false;
    }
    if (!r.ok) return false;
    const size_t xorStride = stride32(width, bitCount), andStride = stride32(width, 1);
    if (r.remaining() < xorStride * size_t(height)) return false;
    const size_t xorOffset = r.pos, andOffset = xorOffset + xorStride * size_t(height);
    const bool maskAvailable = hasMask && size >= andOffset + andStride * size_t(height);

    image.width = width;
    image.height = height;
    image.px.assign(size_t(width) * size_t(height) * 4, 0);
    bool anyAlpha = false;
    for (int y = 0; y < height; y++) {
        const uint8_t* row = bytes + xorOffset + xorStride * size_t(height - 1 - y);
        for (int x = 0; x < width; x++) {
            uint8_t* out = image.at(x, y);
            if (bitCount == 32) {
                const uint8_t* p = row + size_t(x) * 4;
                out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = p[3];
                anyAlpha = anyAlpha || p[3] != 0;
            } else if (bitCount == 24) {
                const uint8_t* p = row + size_t(x) * 3;
                out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = 255;
            } else {
                uint32_t index = 0;
                switch (bitCount) {
                case 8: index = row[x]; break;
                case 4: index = (row[x / 2] >> (x % 2 == 0 ? 4 : 0)) & 0x0f; break;
                case 2: index = (row[x / 4] >> (6 - 2 * (x % 4))) & 0x03; break;
                default: index = (row[x / 8] >> (7 - x % 8)) & 0x01; break;
                }
                if (index >= palette.size()) return false;
                out[0] = palette[index][0]; out[1] = palette[index][1]; out[2] = palette[index][2]; out[3] = 255;
            }
        }
    }
    // The AND mask gives depths without alpha their transparency, and 32-bit entries whose alpha is zero
    // everywhere (a common authoring slip) theirs.
    if ((bitCount != 32 || !anyAlpha) && maskAvailable) {
        for (int y = 0; y < height; y++) {
            const uint8_t* row = bytes + andOffset + andStride * size_t(height - 1 - y);
            for (int x = 0; x < width; x++) image.at(x, y)[3] = ((row[x / 8] >> (7 - x % 8)) & 1) ? 0 : 255;
        }
    } else if (bitCount == 32 && !anyAlpha) {
        for (size_t i = 3; i < image.px.size(); i += 4) image.px[i] = 255;
    }
    return true;
}

Rgba fromImage(const Image& premultiplied) {
    Image straight = premultiplied;
    unpremultiply(straight);
    Rgba out;
    out.width = straight.width();
    out.height = straight.height();
    out.px.resize(size_t(out.width) * size_t(out.height) * 4);
    for (int y = 0; y < out.height; y++) std::copy(straight.row(y), straight.row(y) + size_t(out.width) * 4, out.at(0, y));
    return out;
}

std::shared_ptr<Image> toImage(const Rgba& in) {
    auto image = std::make_shared<Image>(in.width, in.height);
    for (int y = 0; y < in.height; y++) std::copy(in.at(0, y), in.at(0, y) + size_t(in.width) * 4, image->row(y));
    premultiply(*image);
    return image;
}

/// Fits `source` into a transparent `size` square, keeping its aspect, by an alpha-weighted area average.
Rgba resampleToSquare(const Rgba& source, int size) {
    Rgba out;
    out.width = out.height = size;
    out.px.assign(size_t(size) * size_t(size) * 4, 0);
    const int maxSide = std::max(source.width, source.height);
    const double scale = double(size) / double(maxSide);
    const int tw = std::max(1, int(std::lround(source.width * scale))), th = std::max(1, int(std::lround(source.height * scale)));
    const int ox = (size - tw) / 2, oy = (size - th) / 2;
    for (int dy = 0; dy < th; dy++) {
        for (int dx = 0; dx < tw; dx++) {
            uint8_t* dst = out.at(dx + ox, dy + oy);
            const double x0 = double(dx) * source.width / tw, x1 = double(dx + 1) * source.width / tw;
            const double y0 = double(dy) * source.height / th, y1 = double(dy + 1) * source.height / th;
            const int ix0 = int(std::floor(x0)), iy0 = int(std::floor(y0));
            const int ix1 = std::min(source.width, int(std::ceil(x1))), iy1 = std::min(source.height, int(std::ceil(y1)));
            double sr = 0, sg = 0, sb = 0, sa = 0, area = 0;
            for (int sy = iy0; sy < iy1; sy++) {
                const double hy = std::min<double>(y1, sy + 1) - std::max<double>(y0, sy);
                for (int sx = ix0; sx < ix1; sx++) {
                    const double wgt = (std::min<double>(x1, sx + 1) - std::max<double>(x0, sx)) * hy;
                    const uint8_t* s = source.at(sx, sy);
                    const double a = s[3];
                    sr += s[0] * a * wgt; sg += s[1] * a * wgt; sb += s[2] * a * wgt; sa += a * wgt;
                    area += wgt;
                }
            }
            if (area <= 0 || sa <= 0) continue;
            dst[0] = uint8_t(std::clamp<long>(std::lround(sr / sa), 0, 255));
            dst[1] = uint8_t(std::clamp<long>(std::lround(sg / sa), 0, 255));
            dst[2] = uint8_t(std::clamp<long>(std::lround(sb / sa), 0, 255));
            dst[3] = uint8_t(std::clamp<long>(std::lround(sa / area), 0, 255));
        }
    }
    return out;
}

std::vector<uint8_t> encodeBmpEntry(const Rgba& image) {
    format_io::Writer w;
    const int width = image.width, height = image.height;
    const size_t xorStride = stride32(width, 32), andStride = stride32(width, 1);
    w.u32(infoHeaderSize);
    w.u32(uint32_t(width));
    w.u32(uint32_t(height * 2));
    w.u16(1);
    w.u16(32);
    w.u32(0);
    w.u32(uint32_t((xorStride + andStride) * size_t(height)));
    w.u32(0); w.u32(0); w.u32(0); w.u32(0);
    for (int y = height - 1; y >= 0; y--)
        for (int x = 0; x < width; x++) { const uint8_t* p = image.at(x, y); w.u8(p[2]); w.u8(p[1]); w.u8(p[0]); w.u8(p[3]); }
    for (int y = height - 1; y >= 0; y--) {
        std::vector<uint8_t> row(andStride, 0);
        for (int x = 0; x < width; x++) if (image.at(x, y)[3] < 128) row[size_t(x / 8)] |= uint8_t(0x80 >> (x % 8));
        w.bytes.insert(w.bytes.end(), row.begin(), row.end());
    }
    return std::move(w.bytes);
}

} // namespace

bool isIcoData(const uint8_t* d, size_t n) {
    if (n < dirSize) return false;
    const uint16_t reserved = uint16_t(d[0] | d[1] << 8), type = uint16_t(d[2] | d[3] << 8), count = uint16_t(d[4] | d[5] << 8);
    return reserved == 0 && (type == typeIcon || type == typeCursor) && count >= 1;
}

std::optional<PsdImport> importIcoBytes(const std::vector<uint8_t>& bytes, std::string* error) {
    if (!isIcoData(bytes.data(), bytes.size())) { if (error) *error = "File is not an ICO or CUR image"; return std::nullopt; }
    format_io::Reader r(bytes.data(), bytes.size());
    r.skip(4);
    const uint16_t count = r.u16();
    struct Entry { uint32_t size, offset; };
    std::vector<Entry> entries;
    for (uint16_t i = 0; i < count; i++) {
        r.skip(8);   // width, height, colours, reserved, planes/hotspot: the bitmap header is authoritative
        const uint32_t size = r.u32(), offset = r.u32();
        if (!r.ok) break;
        entries.push_back({size, offset});
    }
    std::vector<Rgba> decoded;
    int skipped = 0;
    for (const Entry& e : entries) {
        if (e.offset > bytes.size() || e.size > bytes.size() - e.offset) { skipped++; continue; }
        const uint8_t* payload = bytes.data() + e.offset;
        if (hasPngSignature(payload, e.size)) {
            auto png = decodePngImage(payload, e.size);
            if (!png || png->width() > maxIconSize * 4 || png->height() > maxIconSize * 4) { skipped++; continue; }
            decoded.push_back(fromImage(*png));
        } else {
            Rgba image;
            if (decodeBmpEntry(payload, e.size, image)) decoded.push_back(std::move(image));
            else skipped++;
        }
    }
    if (decoded.empty()) { if (error) *error = "The icon holds no image this reader can use."; return std::nullopt; }

    // Smallest first, so the largest ends on top of the stack; only it is visible.
    std::stable_sort(decoded.begin(), decoded.end(), [](const Rgba& a, const Rgba& b) { return (long long)a.width * a.height < (long long)b.width * b.height; });
    PsdImport result;
    result.document = Document(decoded.back().width, decoded.back().height);
    std::set<std::string> used;
    for (size_t i = 0; i < decoded.size(); i++) {
        std::string name = std::to_string(decoded[i].width) + "x" + std::to_string(decoded[i].height);
        if (!used.insert(name).second) {
            int suffix = 2;
            while (!used.insert(name + " (" + std::to_string(suffix) + ")").second) suffix++;
            name += " (" + std::to_string(suffix) + ")";
        }
        Layer layer(Asset::make(toImage(decoded[i]), name), Point(0, 0));
        layer.visible = i + 1 == decoded.size();
        result.document.layers.push_back(std::move(layer));
    }
    if (skipped) result.notes.push_back(std::to_string(skipped) + " damaged or unsupported icon entr" + (skipped == 1 ? "y was" : "ies were") + " left out.");
    return result;
}

std::optional<PsdImport> importIco(const std::string& path, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!format_io::readFile(path, bytes, error)) return std::nullopt;
    return importIcoBytes(bytes, error);
}

bool encodeIco(const Image& flattened, const std::vector<int>& requested, std::vector<uint8_t>& out, std::string* error, const Document* document) {
    std::vector<int> sizes;
    for (int s : requested) if (s >= 1 && s <= maxIconSize && std::find(sizes.begin(), sizes.end(), s) == sizes.end()) sizes.push_back(s);
    std::sort(sizes.begin(), sizes.end());
    if (sizes.empty()) { if (error) *error = "No icon sizes between 1 and 256 were given."; return false; }
    if (flattened.isEmpty()) { if (error) *error = "Cannot write an empty icon."; return false; }

    std::optional<Rgba> flat;
    auto sourceFor = [&](int size) -> Rgba {
        if (document) {
            const std::string name = std::to_string(size) + "x" + std::to_string(size);
            for (const Layer& l : document->layers)
                if (!l.isGroup && l.name == name && l.asset && l.asset->image && l.asset->image->width() == size && l.asset->image->height() == size)
                    return fromImage(*l.asset->image);
        }
        if (!flat) flat = fromImage(flattened);
        if (flat->width == size && flat->height == size) return *flat;
        return resampleToSquare(*flat, size);
    };
    std::vector<std::pair<int, std::vector<uint8_t>>> payloads;
    for (int size : sizes) {
        const Rgba image = sourceFor(size);
        if (size >= maxIconSize) {
            // Vista and later store the 256 px entry as PNG.
            std::vector<uint8_t> png;
            if (!encodePngImage(*toImage(image), png, 0, error)) return false;
            payloads.emplace_back(size, std::move(png));
        } else {
            payloads.emplace_back(size, encodeBmpEntry(image));
        }
    }
    format_io::Writer w;
    w.u16(0);
    w.u16(typeIcon);
    w.u16(uint32_t(payloads.size()));
    uint32_t offset = uint32_t(dirSize + dirEntrySize * payloads.size());
    for (auto& [size, payload] : payloads) {
        w.u8(size >= maxIconSize ? 0u : uint32_t(size));
        w.u8(size >= maxIconSize ? 0u : uint32_t(size));
        w.u8(0);
        w.u8(0);
        w.u16(1);
        w.u16(32);
        w.u32(uint32_t(payload.size()));
        w.u32(offset);
        offset += uint32_t(payload.size());
    }
    for (auto& entry : payloads) w.append(entry.second);
    out = std::move(w.bytes);
    return true;
}

bool writeIco(const std::string& path, const Image& flattened, const std::vector<int>& sizes, std::string* error, const Document* document) {
    std::vector<uint8_t> bytes;
    return encodeIco(flattened, sizes, bytes, error, document) && format_io::writeFile(path, bytes, error);
}

} // namespace compositor
