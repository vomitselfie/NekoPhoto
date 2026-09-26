// Affinity documents (.afphoto, .afdesign, .afpub, .af) as layered documents. Ported from Patchy (MIT,
// src/third_party/patchy_psd/README.md), src/formats/af_document_io.cpp: Patchy derived the format from files
// authored with a licensed Affinity and the MIT-licensed afread project's notes; the container walk, the tile
// planes, the blend table and the placement rules below are its findings, kept as it pinned them. What differs:
// NekoPhoto has no vector, text, adjustment or layer-effect model for Affinity's, so vector curves and the common
// parametric shapes are drawn as pixels, and text, adjustments, live filters and effects are left out and listed.
//
// Container (all integers little-endian): u32 magic 0x414BFF00, u16 version, u16 flags, u32 class tag, an "#Inf"
// block (stream-table offset, thumbnail offset, sizes, dates), "Prot" + u32 revision (version > 7). The stream
// table ("#FAT".."#FT4" chain, newest link first) names streams (doc.dat, d/<hex> raster tiles, c/<n> placed
// originals, edc/<n> embedded documents); each sits behind a "#Fil" tag, stored raw, zlib or zstd, with optional
// byte or u16 delta predictors and a CRC32 of the decoded bytes. At the thumbnail offset: FFFFFFFF "Thmb", then a
// PNG of the flattened document.
#include "compositor/affinity.h"
#include "affinity_tree.h"
#include "compositor/colour.h"
#include "compositor/png.h"
#include "compositor/render.h"
#include "compositor/vectormask.h"
#include "lcms2.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <tuple>
#include <unordered_map>
#include <zlib.h>

#ifdef COMPOSITOR_HAVE_ZSTD
#include <zstd.h>
#endif

namespace compositor {

using affinity::Class;
using affinity::Reader;
using affinity::tag;
using ClassList = std::vector<std::shared_ptr<Class>>;
using Affine6 = std::array<double, 6>;   // [a, b, tx, c, d, ty]: x' = a x + b y + tx, y' = c x + d y + ty

bool affinityZstdSupported() {
#ifdef COMPOSITOR_HAVE_ZSTD
    return true;
#else
    return false;
#endif
}

bool isAffinityFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char magic[4] = {};
    return in.read(reinterpret_cast<char*>(magic), 4) && magic[0] == 0x00 && magic[1] == 0xFF && magic[2] == 0x4B && magic[3] == 0x41;
}

namespace affinity_detail {

void undoByteDelta(std::vector<uint8_t>& bytes) {
    uint8_t sum = 0;
    for (auto& value : bytes) { sum = uint8_t(sum + value); value = sum; }
}

void undoU16Delta(std::vector<uint8_t>& bytes) {
    uint16_t sum = 0;
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
        sum = uint16_t(sum + uint16_t(bytes[i] | bytes[i + 1] << 8));
        bytes[i] = uint8_t(sum & 0xFF);
        bytes[i + 1] = uint8_t(sum >> 8);
    }
}

void undoTileInterleave(std::vector<uint8_t>& bytes) {
    if (bytes.size() != 0x10000) return;
    std::vector<uint8_t> out(bytes.size());
    for (size_t i = 0; i < 0x4000; i++) {
        out[4 * i + 1] = bytes[2 * i];
        out[4 * i + 3] = bytes[2 * i + 1];
        out[4 * i + 0] = bytes[0x8000 + 2 * i];
        out[4 * i + 2] = bytes[0x8000 + 2 * i + 1];
    }
    bytes = std::move(out);
}

// Layer Blnd and effect BlnM share one versioned enum: a version-0 base table plus modes added later under a
// version bump; version 6 renumbers the space (Pigment inserted at 1). Pinned by Patchy against a document with
// one layer per blend-menu entry.
std::optional<BlendMode> blendMode(uint16_t id, uint16_t version) {
    if (version == 0) {
        static constexpr BlendMode v0[] = {BlendMode::Normal, BlendMode::Darken, BlendMode::Multiply, BlendMode::ColorBurn,
            BlendMode::Lighten, BlendMode::Screen, BlendMode::ColorDodge, BlendMode::LinearDodge, BlendMode::Overlay,
            BlendMode::SoftLight, BlendMode::HardLight, BlendMode::VividLight, BlendMode::PinLight, BlendMode::HardMix,
            BlendMode::Difference, BlendMode::Exclusion, BlendMode::Subtract, BlendMode::Hue, BlendMode::Saturation,
            BlendMode::Luminosity, BlendMode::Color};
        if (id < std::size(v0)) return v0[id];
        return std::nullopt;   // 21 Average, 22 Negation, 23 Reflect, 24 Glow, 25 Erase
    }
    if (version == 1 && id == 2) return BlendMode::DarkerColor;
    if (version == 1 && id == 6) return BlendMode::LighterColor;
    if (version == 1 && id == 15) return BlendMode::LinearLight;
    if (version == 3 && id == 5) return BlendMode::LinearBurn;
    if (version == 4 && id == 21) return BlendMode::Divide;
    if (version >= 6) {
        switch (id) {
        case 0: return BlendMode::Normal;
        case 2: return BlendMode::Darken;
        case 3: return BlendMode::DarkerColor;
        case 4: return BlendMode::Multiply;
        case 5: return BlendMode::ColorBurn;
        case 6: return BlendMode::LinearBurn;
        case 7: return BlendMode::Lighten;
        case 8: return BlendMode::LighterColor;
        case 9: return BlendMode::Screen;
        case 10: return BlendMode::ColorDodge;
        case 11: return BlendMode::LinearDodge;
        case 12: return BlendMode::Overlay;
        case 13: return BlendMode::SoftLight;
        case 14: return BlendMode::HardLight;
        case 15: return BlendMode::VividLight;
        case 16: return BlendMode::PinLight;
        case 17: return BlendMode::LinearLight;
        case 18: return BlendMode::HardMix;
        case 19: return BlendMode::Difference;
        case 20: return BlendMode::Exclusion;
        case 21: return BlendMode::Subtract;
        case 22: return BlendMode::Divide;
        case 23: return BlendMode::Hue;
        case 24: return BlendMode::Saturation;
        case 25: return BlendMode::Luminosity;
        case 26: return BlendMode::Color;
        default: return std::nullopt;   // 1 Pigment, 27 Average ... 32 Erase
        }
    }
    return std::nullopt;
}

std::vector<double> composeTransforms(const std::vector<double>& p, const std::vector<double>& c) {
    if (p.size() != 6 || c.size() != 6) return p.size() == 6 ? p : c;
    return {p[0] * c[0] + p[1] * c[3], p[0] * c[1] + p[1] * c[4], p[0] * c[2] + p[1] * c[5] + p[2],
            p[3] * c[0] + p[4] * c[3], p[3] * c[1] + p[4] * c[4], p[3] * c[2] + p[4] * c[5] + p[5]};
}

} // namespace affinity_detail

namespace {

using affinity_detail::blendMode;

constexpr uint32_t magicTag = 0x414BFF00u;
constexpr uint32_t infTag = 0x666E4923u;    // "#Inf"
constexpr uint32_t protTag = 0x746F7250u;   // "Prot"
constexpr uint32_t filTag = 0x6C694623u;    // "#Fil"
constexpr uint32_t thmbTag = 0x626D6854u;   // "Thmb"
constexpr std::array<uint32_t, 4> fatTags = {0x54414623u, 0x32544623u, 0x33544623u, 0x34544623u};   // #FAT #FT2 #FT3 #FT4
constexpr uint16_t newestVerifiedVersion = 12;
constexpr size_t maxFatChain = 64;
constexpr uint32_t maxStreamsPerFat = 1u << 20;
constexpr uint16_t maxNameLength = 4096;
constexpr uint64_t maxStreamBytes = 512ull << 20;
constexpr int tileSize = 256;
constexpr int maxLayers = 20000;
constexpr int maxEmbedDepth = 3;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

Affine6 identity() { return {1, 0, 0, 0, 1, 0}; }
Affine6 compose(const Affine6& p, const Affine6& c) {
    return {p[0] * c[0] + p[1] * c[3], p[0] * c[1] + p[1] * c[4], p[0] * c[2] + p[1] * c[5] + p[2],
            p[3] * c[0] + p[4] * c[3], p[3] * c[1] + p[4] * c[4], p[3] * c[2] + p[4] * c[5] + p[5]};
}
Point apply(const Affine6& m, double x, double y) { return {m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]}; }
std::optional<Affine6> affineOf(const std::vector<double>& v) {
    if (v.size() != 6) return std::nullopt;
    for (double d : v) if (!std::isfinite(d)) return std::nullopt;
    return Affine6{v[0], v[1], v[2], v[3], v[4], v[5]};
}

// ------------------------------------------------------------------------------------------------ container

struct Stream { uint64_t offset = 0, size = 0, compressed = 0; uint32_t crc = 0; uint8_t compression = 0; };

struct Container {
    uint16_t version = 0;
    uint64_t thumbnailOffset = 0;
    std::unordered_map<std::string, Stream> streams;   // the newest revision of each
};

Container parseContainer(std::span<const uint8_t> bytes) {
    Container container;
    Reader r(bytes);
    if (r.u32() != magicTag) fail("Not an Affinity document.");
    container.version = r.u16();
    (void)r.u16();   // flags
    (void)r.u32();   // class tag
    if (r.u32() != infTag) fail("The Affinity document's info block is missing.");
    const uint64_t fatOffset = r.u64();
    container.thumbnailOffset = r.u64();
    r.skip(8 + 8 + 8 + 4 + 4);
    if (container.version > 7 && r.u32() != protTag) fail("The Affinity document's protocol block is missing.");

    // The #Inf offset names the NEWEST table link; each link's next offset leads to an older save. A name may be
    // declared only in an older link than its newest data record, so collect every link, then apply them oldest
    // first with newer records overwriting.
    std::unordered_map<uint32_t, std::string> names;
    std::vector<std::vector<std::pair<uint32_t, Stream>>> links;
    uint64_t next = fatOffset;
    while (next != 0) {
        if (links.size() >= maxFatChain) fail("The Affinity document's stream table recurses.");
        if (next > bytes.size()) fail("The Affinity document's stream table is out of range.");
        r.seek(size_t(next));
        const uint32_t fat = r.u32();
        if (std::find(fatTags.begin(), fatTags.end(), fat) == fatTags.end()) fail("The Affinity document's stream table is damaged.");
        const bool oldest = fat == fatTags[0], ft4 = fat == fatTags[3];
        next = r.u64();
        r.skip(8 * 4);
        const uint32_t files = r.u32();
        r.skip(8);
        const uint16_t dirs = r.u16();
        r.skip(1);
        if (files > maxStreamsPerFat) fail("The Affinity document's stream table is implausible.");
        auto& link = links.emplace_back();
        for (uint32_t i = 0; i < files; i++) {
            const uint32_t id = r.u32();
            const uint8_t flag = r.u8();
            if (flag > 2) fail("An Affinity stream entry is damaged.");
            Stream s;
            if (flag <= 1) {
                s.offset = r.u64();
                s.size = r.u64();
                s.compressed = r.u64();
                s.crc = r.u32();
                s.compression = r.u8();
                if (!oldest) r.skip(4);
                if (ft4) r.skip(4);
                if (oldest || fat == fatTags[1]) {
                    // The two oldest layouts store an indirect compression code.
                    static constexpr uint8_t codes[] = {0, 0x01, 0x41, 0x81, 0xC1};
                    s.compression = s.compression < 5 ? codes[s.compression] : 0;
                }
            }
            if (flag == 0) {
                const uint16_t length = r.u16();
                if (length > maxNameLength) fail("An Affinity stream name is implausible.");
                std::string name(length, '\0');
                for (auto& c : name) c = char(r.u8());
                names.try_emplace(id, std::move(name));   // walked newest first: the newest name wins
            }
            if (flag <= 1) link.emplace_back(id, s);
        }
        for (uint16_t d = 0; d < dirs; d++) {
            const uint16_t length = r.u16();
            r.skip(2 + 8);
            if (length > maxNameLength) fail("An Affinity directory name is implausible.");
            r.skip(length);
        }
    }
    for (auto link = links.rbegin(); link != links.rend(); ++link)
        for (const auto& [id, s] : *link)
            if (auto name = names.find(id); name != names.end()) container.streams[name->second] = s;
    return container;
}

std::vector<uint8_t> extractStream(std::span<const uint8_t> bytes, const Stream& s, const std::string& name, std::vector<std::string>* notes) {
    if (s.size == 0 || s.size > maxStreamBytes) fail("Affinity stream '" + name + "' has an implausible size.");
    if (s.offset + 4 > bytes.size()) fail("Affinity stream '" + name + "' is out of range.");
    Reader r(bytes);
    r.seek(size_t(s.offset));
    if (r.u32() != filTag) fail("Affinity stream '" + name + "' is damaged.");
    const uint32_t algorithm = s.compression & 0x03u;
    bool alternate = ((s.compression >> 5) & 1u) != 0;
    uint32_t predictor = 0;
    switch (s.compression & 0xC0u) {
    case 0x40: predictor = 1; alternate = false; break;
    case 0x80: predictor = 2; break;
    case 0xC0: predictor = 3; break;
    default: alternate = false; break;
    }
    const uint64_t stored = algorithm == 0 ? s.size : s.compressed;
    if (stored > r.remaining()) fail("Affinity stream '" + name + "' is truncated.");
    const uint8_t* source = bytes.data() + r.position();
    std::vector<uint8_t> out(size_t(s.size));
    if (algorithm == 1) {
        uLongf length = uLongf(s.size);
        if (uncompress(out.data(), &length, source, uLong(stored)) != Z_OK || length != s.size) fail("Affinity stream '" + name + "' failed to decompress.");
    } else if (algorithm == 2) {
#ifdef COMPOSITOR_HAVE_ZSTD
        const size_t produced = ZSTD_decompress(out.data(), out.size(), source, size_t(stored));
        if (ZSTD_isError(produced) || produced != out.size()) fail("Affinity stream '" + name + "' failed to decompress.");
#else
        fail("This build reads no zstd-compressed Affinity streams (it was made without libzstd).");
#endif
    } else if (algorithm == 0) {
        std::memcpy(out.data(), source, size_t(stored));
    } else {
        fail("Affinity stream '" + name + "' uses an unknown compression.");
    }
    if (!alternate && predictor == 1) affinity_detail::undoByteDelta(out);
    else if (!alternate && predictor == 2) affinity_detail::undoU16Delta(out);
    else if (alternate && predictor == 2) { affinity_detail::undoByteDelta(out); affinity_detail::undoTileInterleave(out); }
    const uint32_t crc = uint32_t(crc32(0, out.data(), uInt(out.size())));
    if (crc != s.crc && notes) notes->push_back("Affinity stream '" + name + "' failed its checksum; the file may be damaged.");
    return out;
}

/// The embedded preview (premultiplied), from the PNG at the thumbnail offset.
std::shared_ptr<Image> extractPreview(std::span<const uint8_t> bytes, const Container& container) {
    const uint64_t at = container.thumbnailOffset;
    if (at == 0 || at + 8 > bytes.size()) return nullptr;
    Reader r(bytes);
    r.seek(size_t(at));
    if (r.u32() != 0xFFFFFFFFu || r.u32() != thmbTag) return nullptr;
    static constexpr uint8_t signature[4] = {0x89, 0x50, 0x4E, 0x47};
    const size_t start = r.position(), end = std::min(bytes.size(), start + 256);
    for (size_t i = start; i + 4 <= end; i++)
        if (std::equal(signature, signature + 4, bytes.begin() + std::ptrdiff_t(i)))
            return decodePngImage(bytes.data() + i, bytes.size() - i);
    return nullptr;
}

// ------------------------------------------------------------------------------------------------ images

/// Straight-alpha RGBA through the build; premultiplied only when a layer is made.
using Straight = std::shared_ptr<Image>;
using Plane = std::shared_ptr<GrayImage>;

Straight straightCopy(const Image& premultiplied) {
    auto out = std::make_shared<Image>(premultiplied);
    unpremultiply(*out);
    return out;
}

bool plausibleSize(int64_t w, int64_t h) { return w > 0 && h > 0 && w <= maxImageSide && h <= maxImageSide && w * h <= Document::pixelBudget; }

/// EXIF orientation (2..8) applied to a straight image.
Straight oriented(const Straight& src, int orientation) {
    if (orientation < 2 || orientation > 8) return src;
    const int W = src->width(), H = src->height();
    const bool swap = orientation >= 5;
    auto out = std::make_shared<Image>(swap ? H : W, swap ? W : H);
    for (int y = 0; y < out->height(); y++)
        for (int x = 0; x < out->width(); x++) {
            int sx = x, sy = y;
            switch (orientation) {
            case 2: sx = W - 1 - x; break;
            case 3: sx = W - 1 - x; sy = H - 1 - y; break;
            case 4: sy = H - 1 - y; break;
            case 5: sx = y; sy = x; break;
            case 6: sx = y; sy = H - 1 - x; break;
            case 7: sx = W - 1 - y; sy = H - 1 - x; break;
            case 8: sx = W - 1 - y; sy = x; break;
            default: break;
            }
            std::memcpy(out->pixel(x, y), src->pixel(sx, sy), 4);
        }
    return out;
}

/// `source` through the affine into an axis-aligned raster at the returned origin: bilinear, alpha-weighted (the
/// premultiplied accumulation Patchy pinned against Affinity's export of a rotated, scaled raster), with power-of-two
/// box reductions first for strong minification. `Img` is Image (straight RGBA) or GrayImage (a mask, edge-clamped).
template <typename Img>
std::optional<std::tuple<std::shared_ptr<Img>, int, int>> resampleAffine(const Img& source, const Affine6& m) {
    const double a = m[0], b = m[1], tx = m[2], c = m[3], d = m[4], ty = m[5];
    const double det = a * d - b * c;
    if (!std::isfinite(det) || std::abs(det) < 1e-9 || source.isEmpty()) return std::nullopt;
    const double sw = source.width(), sh = source.height();
    double x0 = tx, x1 = tx, y0 = ty, y1 = ty;
    for (auto [sx, sy] : {std::pair{sw, 0.0}, {0.0, sh}, {sw, sh}}) {
        const Point p = apply(m, sx, sy);
        x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
    }
    if (!std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) || !std::isfinite(y1)) return std::nullopt;
    const double ox = std::floor(x0), oy = std::floor(y0);
    const double ew = std::ceil(x1) - ox, eh = std::ceil(y1) - oy;
    if (ew < 1 || eh < 1 || !plausibleSize(int64_t(ew), int64_t(eh))) return std::nullopt;
    const int outW = int(ew), outH = int(eh);
    const double ia = d / det, ib = -b / det, ic = -c / det, id = a / det;
    constexpr bool gray = std::is_same_v<Img, GrayImage>;

    const double scaleX = std::hypot(a, c), scaleY = std::hypot(b, d);
    int hx = 0, hy = 0;
    while (scaleX * double(1 << (hx + 1)) <= 1.0 && (source.width() >> (hx + 1)) >= 1 && hx < 20) hx++;
    while (scaleY * double(1 << (hy + 1)) <= 1.0 && (source.height() >> (hy + 1)) >= 1 && hy < 20) hy++;
    // Box halvings per axis (alpha-weighted for colour; an odd last row or column keeps its pixel).
    std::shared_ptr<Img> reducedStore;
    const Img* sampled = &source;
    const auto halve = [](const Img& s, bool horizontal) {
        const int w = horizontal ? (s.width() + 1) / 2 : s.width(), h = horizontal ? s.height() : (s.height() + 1) / 2;
        auto out = std::make_shared<Img>(w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const int ax = horizontal ? x * 2 : x, ay = horizontal ? y : y * 2;
                const int bx = horizontal ? std::min(ax + 1, s.width() - 1) : ax, by = horizontal ? ay : std::min(ay + 1, s.height() - 1);
                if constexpr (gray) {
                    out->at(x, y) = uint8_t(std::lround((double(s.at(ax, ay)) + s.at(bx, by)) / 2));
                } else {
                    const uint8_t* p0 = s.pixel(ax, ay);
                    const uint8_t* p1 = s.pixel(bx, by);
                    uint8_t* o = out->pixel(x, y);
                    const double a0 = p0[3], a1 = p1[3];
                    o[3] = uint8_t(std::lround((a0 + a1) / 2));
                    for (int k = 0; k < 3; k++) o[k] = a0 + a1 > 0 ? uint8_t(std::lround((p0[k] * a0 + p1[k] * a1) / (a0 + a1))) : 0;
                }
            }
        return out;
    };
    for (int i = 0; i < hx; i++) { reducedStore = halve(*sampled, true); sampled = reducedStore.get(); }
    for (int i = 0; i < hy; i++) { reducedStore = halve(*sampled, false); sampled = reducedStore.get(); }
    const double fx = double(1 << hx), fy = double(1 << hy);

    auto out = std::make_shared<Img>(outW, outH);
    for (int y = 0; y < outH; y++)
        for (int x = 0; x < outW; x++) {
            const double rx = ox + x + 0.5 - tx, ry = oy + y + 0.5 - ty;
            const double sx = (ia * rx + ib * ry) / fx - 0.5, sy = (ic * rx + id * ry) / fy - 0.5;
            const double fx0 = std::floor(sx), fy0 = std::floor(sy);
            const int x0i = int(fx0), y0i = int(fy0);
            const double wx = sx - fx0, wy = sy - fy0;
            const std::array<std::tuple<int, int, double>, 4> taps = {
                std::tuple{x0i, y0i, (1 - wx) * (1 - wy)}, std::tuple{x0i + 1, y0i, wx * (1 - wy)},
                std::tuple{x0i, y0i + 1, (1 - wx) * wy}, std::tuple{x0i + 1, y0i + 1, wx * wy}};
            if constexpr (gray) {
                double acc = 0;
                for (auto [nx, ny, w] : taps) acc += sampled->at(std::clamp(nx, 0, sampled->width() - 1), std::clamp(ny, 0, sampled->height() - 1)) * w;
                out->at(x, y) = uint8_t(std::lround(std::clamp(acc, 0.0, 255.0)));
            } else {
                double r = 0, g = 0, bl = 0, al = 0;
                for (auto [nx, ny, w] : taps) {
                    if (nx < 0 || ny < 0 || nx >= sampled->width() || ny >= sampled->height()) continue;
                    const uint8_t* p = sampled->pixel(nx, ny);
                    const double aw = p[3] / 255.0 * w;
                    r += p[0] * aw; g += p[1] * aw; bl += p[2] * aw; al += p[3] * w;
                }
                uint8_t* o = out->pixel(x, y);
                if (al > 0.5) {
                    const double k = 255.0 / al;
                    o[0] = uint8_t(std::clamp(std::lround(r * k), 0L, 255L));
                    o[1] = uint8_t(std::clamp(std::lround(g * k), 0L, 255L));
                    o[2] = uint8_t(std::clamp(std::lround(bl * k), 0L, 255L));
                    o[3] = uint8_t(std::lround(std::min(255.0, al)));
                }
            }
        }
    return std::tuple{out, int(ox), int(oy)};
}

// ------------------------------------------------------------------------------------------------ tile planes

enum class Format { RGBA8, RGBA16, Gray8, Gray16, CMYKA8, LabA16, Mask8, Mask16, RGBAFloat, Unsupported };

Format formatFor(int64_t id) {
    switch (id) {
    case 0: return Format::RGBA8;
    case 1: return Format::RGBA16;
    case 2: return Format::Gray8;
    case 3: return Format::Gray16;
    case 4: return Format::CMYKA8;
    case 5: return Format::LabA16;
    case 6: return Format::Mask8;
    case 7: return Format::Mask16;
    case 9: return Format::RGBAFloat;
    default: return Format::Unsupported;
    }
}

/// One channel's plane: tiles are 256 BYTES wide by 256 rows whatever the sample depth, so the horizontal axis
/// counts bytes (a 16-bit channel's row spans width * 2).
struct ChannelPlane { std::vector<uint8_t> bytes; size_t widthBytes = 0, rows = 0; };

constexpr int64_t tileFillMax = 2, tileFillFloatOne = 3, tileStored = 4, tileFromOriginal = 5;
constexpr size_t maxPlaneBytes = size_t(1) << 30;

uint32_t tagOf(const std::string& s) {
    if (s.size() != 4) return 0;
    return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint32_t(uint8_t(s[3]));
}

struct PlaneTags { uint32_t tilesW, tilesH, index, status; };
PlaneTags baseTags(int channel) {
    const std::string n = std::to_string(channel);
    return {tagOf("TWi" + n), tagOf("THi" + n), tagOf("Idx" + n), tagOf("Sta" + n)};
}
uint32_t mipTag(char kind, int level, int channel) { return uint32_t('M') << 24 | uint32_t(uint8_t(kind)) << 16 | uint32_t(uint8_t(level)) << 8 | uint32_t(uint8_t('0' + channel)); }
PlaneTags mipTags(int level, int channel) { return {mipTag('W', level, channel), mipTag('H', level, channel), mipTag('I', level, channel), mipTag('T', level, channel)}; }

void fillFull(uint8_t* dst, size_t samples, size_t sampleBytes, bool isFloat) {
    if (!isFloat) { std::memset(dst, 0xFF, samples * sampleBytes); return; }
    const float one = 1.0f;
    for (size_t i = 0; i < samples; i++) std::memcpy(dst + i * 4, &one, 4);
}

enum class PlaneStatus { Ok, NeedsOriginal, UnknownCode, Invalid };

struct Source { std::span<const uint8_t> bytes; const Container& container; };

const std::vector<int64_t>* statusCodes(const Class& dybm, uint32_t t) {
    const affinity::Field* f = dybm.field(t);
    return f ? std::get_if<std::vector<int64_t>>(&f->value) : nullptr;
}

PlaneStatus decodePlane(const Source& src, const Class& dybm, const PlaneTags& tags, size_t sampleBytes, bool isFloat,
                        int64_t planeW, int64_t planeH, const ChannelPlane* original, ChannelPlane& out) {
    int64_t tilesW = dybm.integer(tags.tilesW, 1), tilesH = dybm.integer(tags.tilesH, 1);
    const auto* codes = statusCodes(dybm, tags.status);
    if (!dybm.field(tags.tilesW) && !dybm.field(tags.tilesH)) {
        // Old (1.x) bitmaps store no grid fields: derive it, when the status list has exactly that many codes.
        const int64_t dw = std::max<int64_t>(1, (planeW * int64_t(sampleBytes) + tileSize - 1) / tileSize);
        const int64_t dh = std::max<int64_t>(1, (planeH + tileSize - 1) / tileSize);
        if (codes && int64_t(codes->size()) == dw * dh) { tilesW = dw; tilesH = dh; }
    }
    if (tilesW <= 0 || tilesH <= 0 || tilesW > 8192 || tilesH > 8192 || tilesW * tilesH > (1LL << 20)) return PlaneStatus::Invalid;
    out.widthBytes = size_t(tilesW) * tileSize;
    out.rows = size_t(tilesH) * tileSize;
    if (out.widthBytes * out.rows > maxPlaneBytes) return PlaneStatus::Invalid;
    out.bytes.assign(out.widthBytes * out.rows, 0);
    if (!codes) return PlaneStatus::Ok;
    const affinity::Field* idx = dybm.field(tags.index);
    const ClassList* blocks = idx ? std::get_if<ClassList>(&idx->value) : nullptr;
    size_t block = 0;
    for (size_t t = 0; t < codes->size(); t++) {
        const size_t tx = (t % size_t(tilesW)) * tileSize, ty = (t / size_t(tilesW)) * tileSize;
        if (ty >= out.rows) break;
        switch ((*codes)[t]) {
        case 0: case 1: break;
        case tileFillMax: case tileFillFloatOne:
            for (size_t row = 0; row < tileSize; row++) fillFull(out.bytes.data() + (ty + row) * out.widthBytes + tx, tileSize / sampleBytes, sampleBytes, isFloat);
            break;
        case tileFromOriginal:
            if (!original) return PlaneStatus::NeedsOriginal;
            if (original->widthBytes != out.widthBytes || original->rows != out.rows) return PlaneStatus::Invalid;
            for (size_t row = 0; row < tileSize; row++) {
                const size_t at = (ty + row) * out.widthBytes + tx;
                std::memcpy(out.bytes.data() + at, original->bytes.data() + at, tileSize);
            }
            break;
        case tileStored: {
            if (!blocks || block >= blocks->size() || !(*blocks)[block]) { block++; break; }
            const Class& b = *(*blocks)[block++];
            const affinity::Field* data = b.field(tag("Data"));
            const auto* embedded = data ? std::get_if<affinity::Embedded>(&data->value) : nullptr;
            if (!embedded) break;
            auto stream = src.container.streams.find(embedded->data);
            if (stream == src.container.streams.end()) break;
            std::vector<uint8_t> tile;
            try { tile = extractStream(src.bytes, stream->second, embedded->data, nullptr); } catch (const std::exception&) { break; }
            if (tile.size() == size_t(tileSize) * tileSize) {
                for (size_t row = 0; row < tileSize; row++) std::memcpy(out.bytes.data() + (ty + row) * out.widthBytes + tx, tile.data() + row * tileSize, tileSize);
                break;
            }
            // A partial tile: only a sub-rectangle, [x, y, w, h] or [x0, y0, x1, y1], told apart by the stream size.
            const auto rect = b.vector(tag("Rect"));
            if (rect.size() != 4) break;
            const int64_t rx = int64_t(rect[0]), ry = int64_t(rect[1]);
            int64_t rw = int64_t(rect[2]), rh = int64_t(rect[3]);
            if (rw > rx && rh > ry && uint64_t(rw - rx) * uint64_t(rh - ry) == tile.size()) { rw -= rx; rh -= ry; }
            if (rx < 0 || ry < 0 || rw <= 0 || rh <= 0 || rx + rw > tileSize || ry + rh > tileSize || uint64_t(rw) * uint64_t(rh) != tile.size()) break;
            for (int64_t row = 0; row < rh; row++)
                std::memcpy(out.bytes.data() + (ty + size_t(ry + row)) * out.widthBytes + tx + size_t(rx), tile.data() + size_t(row * rw), size_t(rw));
            break;
        }
        default: return PlaneStatus::UnknownCode;
        }
    }
    return PlaneStatus::Ok;
}

/// A sample on the 0..255 scale (16-bit / 257); float planes give their raw linear value.
float samplePlane(const ChannelPlane& p, size_t sampleBytes, bool isFloat, int64_t x, int64_t y) {
    const uint8_t* s = p.bytes.data() + size_t(y) * p.widthBytes + size_t(x) * sampleBytes;
    if (isFloat) { float v; std::memcpy(&v, s, 4); return std::isfinite(v) ? v : 0.0f; }
    if (sampleBytes == 2) return float(uint16_t(s[0] | s[1] << 8)) / 257.0f;
    return float(s[0]);
}

float srgbToLinear(float v) { v = std::clamp(v, 0.0f, 1.0f); return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
uint8_t linearToSrgb8(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return uint8_t(std::lround(std::clamp(s, 0.0f, 1.0f) * 255));
}
uint8_t toByte(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 255.0f))); }

/// A placed original: the untouched image file a bitmap's Bckg stream holds (a Blck tree whose Data is the file
/// and TifO its EXIF orientation), decoded to straight RGBA.
Straight decodeOriginal(const Source& src, const Class& dybm, const PsdImportOptions& options) {
    const affinity::Field* f = dybm.field(tag("Bckg"));
    const auto* embedded = f ? std::get_if<affinity::Embedded>(&f->value) : nullptr;
    if (!embedded || embedded->data.empty()) return nullptr;
    auto stream = src.container.streams.find(embedded->data);
    if (stream == src.container.streams.end()) return nullptr;
    try {
        const auto blob = extractStream(src.bytes, stream->second, embedded->data, nullptr);
        const affinity::Tree block = affinity::parseTree(blob);
        if (!block.root) return nullptr;
        const affinity::Field* dataField = block.root->field(tag("Data"));
        const auto* data = dataField ? std::get_if<std::vector<uint8_t>>(&dataField->value) : nullptr;
        if (!data || data->empty()) return nullptr;
        ImagePtr decoded;
        static constexpr uint8_t png[4] = {0x89, 'P', 'N', 'G'};
        if (data->size() > 8 && std::equal(png, png + 4, data->begin())) decoded = decodePngImage(data->data(), data->size());
        else if (options.decodeImage) decoded = options.decodeImage(*data, "", "");
        if (!decoded || decoded->isEmpty() || !plausibleSize(decoded->width(), decoded->height())) return nullptr;
        return oriented(straightCopy(*decoded), int(block.root->integer(tag("TifO"), 1)));
    } catch (const std::exception&) {
        return nullptr;
    }
}

/// The code-5 source plane for one channel, from the original's R, G, B, A (re-encoded to the plane's depth).
ChannelPlane planeFromOriginal(const Image& original, const ChannelPlane& layout, int channel, size_t sampleBytes, bool isFloat) {
    ChannelPlane plane;
    plane.widthBytes = layout.widthBytes;
    plane.rows = layout.rows;
    plane.bytes.assign(plane.widthBytes * plane.rows, 0);
    const int component = std::clamp(channel - 1, 0, 3);
    for (int y = 0; y < original.height() && size_t(y) < plane.rows; y++) {
        uint8_t* row = plane.bytes.data() + size_t(y) * plane.widthBytes;
        for (int x = 0; x < original.width(); x++) {
            const size_t at = size_t(x) * sampleBytes;
            if (at + sampleBytes > plane.widthBytes) break;
            const uint8_t v = original.pixel(x, y)[component];
            if (isFloat) { const float f = component < 3 ? srgbToLinear(v / 255.0f) : v / 255.0f; std::memcpy(row + at, &f, 4); }
            else if (sampleBytes == 2) { const uint16_t w = uint16_t(v * 257u); row[at] = uint8_t(w & 0xFF); row[at + 1] = uint8_t(w >> 8); }
            else row[at] = v;
        }
    }
    return plane;
}

/// The half-resolution fallback: each channel's level-1 mip plane, scaled up 2x bilinearly into the base layout.
bool mipSources(const Source& src, const Class& dybm, int channels, size_t sampleBytes, bool isFloat, int64_t width, int64_t height,
                const std::vector<ChannelPlane>& layouts, std::vector<ChannelPlane>& out) {
    const int64_t mw = (width + 1) / 2, mh = (height + 1) / 2;
    for (int channel = 1; channel <= channels; channel++) {
        ChannelPlane mip;
        if (decodePlane(src, dybm, mipTags(1, channel), sampleBytes, isFloat, mw, mh, nullptr, mip) != PlaneStatus::Ok
            || size_t(mw) * sampleBytes > mip.widthBytes || size_t(mh) > mip.rows) return false;
        const ChannelPlane& layout = layouts[size_t(channel - 1)];
        ChannelPlane scaled;
        scaled.widthBytes = layout.widthBytes;
        scaled.rows = layout.rows;
        scaled.bytes.assign(scaled.widthBytes * scaled.rows, 0);
        for (int64_t y = 0; y < height && size_t(y) < scaled.rows; y++)
            for (int64_t x = 0; x < width; x++) {
                const size_t at = size_t(x) * sampleBytes;
                if (at + sampleBytes > scaled.widthBytes) break;
                const float sx = std::clamp((float(x) + 0.5f) * 0.5f - 0.5f, 0.0f, float(mw - 1));
                const float sy = std::clamp((float(y) + 0.5f) * 0.5f - 0.5f, 0.0f, float(mh - 1));
                const int64_t x0 = int64_t(sx), y0 = int64_t(sy), x1 = std::min(x0 + 1, mw - 1), y1 = std::min(y0 + 1, mh - 1);
                const float fx = sx - float(x0), fy = sy - float(y0);
                const float top = samplePlane(mip, sampleBytes, isFloat, x0, y0) * (1 - fx) + samplePlane(mip, sampleBytes, isFloat, x1, y0) * fx;
                const float bottom = samplePlane(mip, sampleBytes, isFloat, x0, y1) * (1 - fx) + samplePlane(mip, sampleBytes, isFloat, x1, y1) * fx;
                const float v = top * (1 - fy) + bottom * fy;
                uint8_t* dst = scaled.bytes.data() + size_t(y) * scaled.widthBytes + at;
                if (isFloat) std::memcpy(dst, &v, 4);
                else if (sampleBytes == 2) { const auto w = uint16_t(std::lround(std::clamp(v, 0.0f, 255.0f) * 257)); dst[0] = uint8_t(w & 0xFF); dst[1] = uint8_t(w >> 8); }
                else dst[0] = toByte(v);
            }
        out.push_back(std::move(scaled));
    }
    return true;
}

struct Decoded {
    Straight rgba;   // colour formats
    Plane mask;      // M8 / M16
    bool approximateColor = false;
};

/// Converts Lab (the ICC v4 Lab16 encoding Affinity stores) to sRGB through Little CMS.
class LabToSrgb {
public:
    LabToSrgb() {
        context_ = cmsCreateContext(nullptr, nullptr);
        if (!context_) return;
        cmsHPROFILE lab = cmsCreateLab4ProfileTHR(context_, nullptr);
        cmsHPROFILE srgb = cmsCreate_sRGBProfileTHR(context_);
        if (lab && srgb) transform_ = cmsCreateTransformTHR(context_, lab, TYPE_Lab_16, srgb, TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOCACHE);
        if (lab) cmsCloseProfile(lab);
        if (srgb) cmsCloseProfile(srgb);
    }
    ~LabToSrgb() { if (transform_) cmsDeleteTransform(transform_); if (context_) cmsDeleteContext(context_); }
    LabToSrgb(const LabToSrgb&) = delete;
    LabToSrgb& operator=(const LabToSrgb&) = delete;
    bool valid() const { return transform_ != nullptr; }
    void convert(const uint16_t* lab, uint8_t* rgb, size_t count) const { cmsDoTransform(transform_, lab, rgb, cmsUInt32Number(count)); }

private:
    cmsContext context_ = nullptr;
    cmsHTRANSFORM transform_ = nullptr;
};

std::optional<Decoded> decodeBitmap(const Source& src, const Class& dybm, const std::string& name, const PsdImportOptions& options,
                                    std::vector<std::string>& notes, std::string* why) {
    const auto reason = [&](const char* r) { if (why) *why = r; };
    int64_t formatId = -1;
    if (const affinity::Field* f = dybm.field(tag("Frmt")))
        if (const auto* e = std::get_if<affinity::Enum>(&f->value)) formatId = e->id;
    const Format kind = formatFor(formatId);
    if (kind == Format::Unsupported) { reason("has an unsupported pixel format"); return std::nullopt; }
    const int64_t width = dybm.integer(tag("BmpW"), 0), height = dybm.integer(tag("BmpH"), 0);
    if (!plausibleSize(width, height)) { reason("has a bitmap larger than a layer may be"); return std::nullopt; }
    const bool is16 = kind == Format::RGBA16 || kind == Format::Gray16 || kind == Format::LabA16 || kind == Format::Mask16;
    const bool isFloat = kind == Format::RGBAFloat;
    const size_t sampleBytes = isFloat ? 4 : is16 ? 2 : 1;
    int channels = 4;
    if (kind == Format::Gray8 || kind == Format::Gray16) channels = 2;
    else if (kind == Format::CMYKA8) channels = 5;
    else if (kind == Format::Mask8 || kind == Format::Mask16) channels = 1;

    // Lazy placed images (3.x saves of untouched placed images) store no base-plane fields at all, only the Bckg
    // original: every base tile is implicitly "from the original".
    bool baseAbsent = dybm.field(tag("Bckg")) != nullptr;
    for (int ch = 1; baseAbsent && ch <= channels; ch++) {
        const PlaneTags t = baseTags(ch);
        if (dybm.field(t.status) || dybm.field(t.tilesW) || dybm.field(t.tilesH) || dybm.field(t.index)) baseAbsent = false;
    }
    std::vector<ChannelPlane> planes(static_cast<size_t>(channels));
    bool needsOriginal = false;
    if (baseAbsent) {
        needsOriginal = true;
        const size_t tw = (size_t(width) * sampleBytes + tileSize - 1) / tileSize, th = (size_t(height) + tileSize - 1) / tileSize;
        for (auto& p : planes) { p.widthBytes = tw * tileSize; p.rows = th * tileSize; }
    }
    for (int ch = 1; !baseAbsent && ch <= channels; ch++) {
        const PlaneStatus s = decodePlane(src, dybm, baseTags(ch), sampleBytes, isFloat, width, height, nullptr, planes[size_t(ch - 1)]);
        if (s == PlaneStatus::NeedsOriginal) { needsOriginal = true; continue; }
        if (s != PlaneStatus::Ok) { reason(s == PlaneStatus::UnknownCode ? "uses an unknown tile encoding" : "has an invalid tile layout"); return std::nullopt; }
    }
    if (needsOriginal) {
        std::vector<ChannelPlane> sources;
        bool have = false;
        if (kind == Format::RGBA8 || kind == Format::RGBA16 || kind == Format::RGBAFloat) {
            if (Straight original = decodeOriginal(src, dybm, options); original && original->width() == width && original->height() == height) {
                for (int ch = 1; ch <= channels; ch++) sources.push_back(planeFromOriginal(*original, planes[size_t(ch - 1)], ch, sampleBytes, isFloat));
                have = true;
            }
        }
        if (!have) {
            sources.clear();
            if (mipSources(src, dybm, channels, sampleBytes, isFloat, width, height, planes, sources)) {
                have = true;
                notes.push_back("Layer \"" + name + "\": its placed image could not be decoded; imported at half resolution from Affinity's stored reduction.");
            }
        }
        if (!have) { reason("places an original image that could not be decoded"); return std::nullopt; }
        if (baseAbsent) planes = std::move(sources);
        else
            for (int ch = 1; ch <= channels; ch++)
                if (decodePlane(src, dybm, baseTags(ch), sampleBytes, isFloat, width, height, &sources[size_t(ch - 1)], planes[size_t(ch - 1)]) != PlaneStatus::Ok) {
                    reason("has an invalid tile layout");
                    return std::nullopt;
                }
    }
    for (const auto& p : planes)
        if (size_t(width) * sampleBytes > p.widthBytes || size_t(height) > p.rows) { reason("has an invalid tile layout"); return std::nullopt; }

    const auto sample = [&](int ch, int64_t x, int64_t y) { return samplePlane(planes[size_t(ch)], sampleBytes, isFloat, x, y); };
    const int w = int(width), h = int(height);
    Decoded decoded;
    if (kind == Format::Mask8 || kind == Format::Mask16) {
        decoded.mask = std::make_shared<GrayImage>(w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) decoded.mask->at(x, y) = toByte(sample(0, x, y));
        return decoded;
    }
    auto image = std::make_shared<Image>(w, h);
    if (kind == Format::LabA16) {
        const LabToSrgb lab;
        if (!lab.valid()) { reason("could not be converted from Lab"); return std::nullopt; }
        std::vector<uint16_t> in(size_t(w) * 3);
        std::vector<uint8_t> rgb(size_t(w) * 3);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++)
                for (int ch = 0; ch < 3; ch++) {
                    const uint8_t* p = planes[size_t(ch)].bytes.data() + size_t(y) * planes[size_t(ch)].widthBytes + size_t(x) * 2;
                    in[size_t(x) * 3 + size_t(ch)] = uint16_t(p[0] | p[1] << 8);
                }
            lab.convert(in.data(), rgb.data(), size_t(w));
            for (int x = 0; x < w; x++) { uint8_t* o = image->pixel(x, y); std::memcpy(o, &rgb[size_t(x) * 3], 3); o[3] = toByte(sample(3, x, y)); }
        }
        decoded.rgba = image;
        return decoded;
    }
    // CMYK through the bitmap's own ICC profile when it has one (Prof > CMYP > Data), else the naive ink mix.
    std::shared_ptr<const CmykToSrgb> cmyk;
    if (kind == Format::CMYKA8)
        if (const Class* profiles = dybm.child(tag("Prof")))
            if (const Class* profile = profiles->child(tag("CMYP")))
                if (const affinity::Field* data = profile->field(tag("Data")))
                    if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&data->value)) cmyk = CmykToSrgb::fromProfile(*bytes);
    if (cmyk) {
        std::vector<uint8_t> inverted(size_t(w) * 4), rgb(size_t(w) * 3);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++)
                for (int ch = 0; ch < 4; ch++) inverted[size_t(x) * 4 + size_t(ch)] = uint8_t(255 - toByte(sample(ch, x, y)));   // .af stores ink; PSD's layout inverts it
            cmyk->convert(inverted.data(), rgb.data(), size_t(w));
            for (int x = 0; x < w; x++) { uint8_t* o = image->pixel(x, y); std::memcpy(o, &rgb[size_t(x) * 3], 3); o[3] = toByte(sample(4, x, y)); }
        }
        decoded.rgba = image;
        return decoded;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* o = image->pixel(x, y);
            switch (kind) {
            case Format::Gray8: case Format::Gray16: o[0] = o[1] = o[2] = toByte(sample(0, x, y)); o[3] = toByte(sample(1, x, y)); break;
            case Format::RGBAFloat:
                o[0] = linearToSrgb8(sample(0, x, y)); o[1] = linearToSrgb8(sample(1, x, y)); o[2] = linearToSrgb8(sample(2, x, y));
                o[3] = toByte(sample(3, x, y) * 255);
                break;
            case Format::CMYKA8: {
                const float c = sample(0, x, y) / 255, m = sample(1, x, y) / 255, ye = sample(2, x, y) / 255, k = sample(3, x, y) / 255;
                o[0] = toByte(255 * (1 - c) * (1 - k)); o[1] = toByte(255 * (1 - m) * (1 - k)); o[2] = toByte(255 * (1 - ye) * (1 - k));
                o[3] = toByte(sample(4, x, y));
                break;
            }
            default: for (int c = 0; c < 4; c++) o[c] = toByte(sample(c, x, y)); break;
            }
        }
    decoded.rgba = image;
    decoded.approximateColor = kind == Format::CMYKA8;
    return decoded;
}

// ------------------------------------------------------------------------------------------------ colours, paths

/// An Affinity colour class (RGBA, HSLA or CMYK floats in `_col`) as straight RGBA 0..1.
std::optional<std::array<float, 4>> readColor(const Class* color) {
    if (!color) return std::nullopt;
    const affinity::Field* f = color->field(tag("_col"));
    const auto* data = f ? std::get_if<std::vector<uint8_t>>(&f->value) : nullptr;
    if (!data || data->size() < 16) return std::nullopt;
    const auto component = [&](size_t i) {
        const uint32_t bits = uint32_t((*data)[i * 4]) | uint32_t((*data)[i * 4 + 1]) << 8 | uint32_t((*data)[i * 4 + 2]) << 16 | uint32_t((*data)[i * 4 + 3]) << 24;
        float v;
        std::memcpy(&v, &bits, 4);
        return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
    };
    std::array<float, 4> c{component(0), component(1), component(2), component(3)};
    if (color->type == tag("CMYK") && data->size() >= 20)
        return std::array<float, 4>{(1 - c[0]) * (1 - c[3]), (1 - c[1]) * (1 - c[3]), (1 - c[2]) * (1 - c[3]), component(4)};
    if (color->type == tag("HSLA")) {
        const float hue = c[0] - std::floor(c[0]), s = c[1], l = c[2];
        const float chroma = (1 - std::abs(2 * l - 1)) * s, seg = hue * 6, x = chroma * (1 - std::abs(std::fmod(seg, 2.0f) - 1));
        float r = 0, g = 0, b = 0;
        switch (int(seg)) {
        case 0: r = chroma; g = x; break;
        case 1: r = x; g = chroma; break;
        case 2: g = chroma; b = x; break;
        case 3: g = x; b = chroma; break;
        case 4: r = x; b = chroma; break;
        default: r = chroma; b = x; break;
        }
        const float m = l - chroma / 2;
        c = {std::clamp(r + m, 0.0f, 1.0f), std::clamp(g + m, 0.0f, 1.0f), std::clamp(b + m, 0.0f, 1.0f), c[3]};
    }
    return c;
}

VectorPath::Knot knot(double ax, double ay, double ix, double iy, double ox, double oy) { return {ix, iy, ax, ay, ox, oy}; }
VectorPath::Knot corner(double x, double y) { return knot(x, y, x, y, x, y); }

const Class* firstClass(const Class& node, uint32_t t) {
    if (const Class* c = node.child(t)) return c;
    if (const ClassList* l = node.list(t); l && !l->empty()) return l->front().get();
    return nullptr;
}

/// A linked symbol instance keeps its geometry on the defining sibling in its SLnk > ILOb ring.
const Class* symbolSiblingWith(const Class& node, uint32_t field) {
    const Class* link = node.child(tag("SLnk"));
    const ClassList* members = link ? link->list(tag("ILOb")) : nullptr;
    if (!members) return nullptr;
    for (const auto& m : *members) if (m && m.get() != &node && m->child(field)) return m.get();
    return nullptr;
}

/// A PCrv's poly-curve (Crvs > Data, or a compound's baked 'crvs'): per subpath a closed flag and 18-byte records
/// (x, y as f64, u16 flags: 1 corner anchor, 2 smooth anchor, 0x100 the previous anchor's out handle, 0x200 the
/// next anchor's in handle). All subpaths fill even-odd together.
std::optional<VectorPath> curvePath(const Class& node) {
    const Class* curves = node.child(tag("Crvs"));
    if (!curves) curves = node.child(tag("crvs"));
    if (!curves) if (const Class* s = symbolSiblingWith(node, tag("Crvs"))) curves = s->child(tag("Crvs"));
    const Class* data = curves ? curves->child(tag("Data")) : nullptr;
    if (!data) return std::nullopt;
    const auto f64 = [](const uint8_t* b) { uint64_t bits = 0; for (int i = 7; i >= 0; i--) bits = bits << 8 | b[i]; double v; std::memcpy(&v, &bits, 8); return v; };
    VectorPath path;
    bool closed = true;
    for (const auto& field : data->fields) {
        if (const auto* flag = std::get_if<bool>(&field.value)) { closed = *flag; continue; }
        const auto* records = std::get_if<affinity::CurveArray>(&field.value);
        if (!records || records->recordSize < 18) continue;
        VectorPath::Subpath sub;
        sub.closed = closed;
        bool pendingIn = false;
        double inX = 0, inY = 0;
        for (size_t at = 0; at + records->recordSize <= records->bytes.size(); at += records->recordSize) {
            const uint8_t* b = records->bytes.data() + at;
            const double x = f64(b), y = f64(b + 8);
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            const uint16_t flags = uint16_t(b[16] | b[17] << 8);
            if (flags & 0x0003u) {
                auto k = corner(x, y);
                if (pendingIn) { k.inX = inX; k.inY = inY; pendingIn = false; }
                sub.knots.push_back(k);
            } else if (flags & 0x0100u) {
                if (!sub.knots.empty()) { sub.knots.back().outX = x; sub.knots.back().outY = y; }
            } else if (flags & 0x0200u) {
                pendingIn = true; inX = x; inY = y;
            }
        }
        if (sub.closed && sub.knots.size() >= 2) {
            const auto& first = sub.knots.front();
            const auto& last = sub.knots.back();
            if (std::abs(first.x - last.x) < 1e-4 && std::abs(first.y - last.y) < 1e-4) {
                sub.knots.front().inX = last.inX;
                sub.knots.front().inY = last.inY;
                sub.knots.pop_back();
            }
        }
        if (sub.knots.size() >= 2) path.subpaths.push_back(std::move(sub));
        closed = true;
    }
    if (path.subpaths.empty()) return std::nullopt;
    return path;
}

constexpr double quarterArc = 0.5522847498307936;
enum { cornerRound = 0, cornerStraight = 1, cornerRoundInverse = 2, cornerCutout = 3, cornerNone = 4 };

// The parametric shapes below are Patchy's parametric_shape_path, kept close to its text (and its anchor type) so
// updates stay a comparison: an anchor with its in and out handles, and a subpath of them.
struct PA { double anchor_x, anchor_y, in_x, in_y, out_x, out_y; bool smooth; };
struct Sub { std::vector<PA> anchors; bool closed = true; };

VectorPath toPath(const std::vector<Sub>& subs) {
    VectorPath path;
    for (const Sub& s : subs) {
        VectorPath::Subpath out;
        out.closed = s.closed;
        for (const PA& a : s.anchors) out.knots.push_back(knot(a.anchor_x, a.anchor_y, a.in_x, a.in_y, a.out_x, a.out_y));
        path.subpaths.push_back(std::move(out));
    }
    return path;
}

int64_t enumId(const Class& cls, uint32_t t, int64_t fallback) {
    const affinity::Field* f = cls.field(t);
    if (const auto* e = f ? std::get_if<affinity::Enum>(&f->value) : nullptr) return e->id;
    return fallback;
}

// One rectangle corner, walking clockwise: `enter` is the direction along the incoming edge, `exit` the outgoing,
// `radius` the corner size (0: sharp), `type` the wire corner type.
void appendRectCornerPA(Sub& subpath, double px, double py, double enter_x, double enter_y, double exit_x, double exit_y, double radius, int type) {
    const auto add = [&](double x, double y, bool smooth) -> PA& { subpath.anchors.push_back(PA{x, y, x, y, x, y, smooth}); return subpath.anchors.back(); };
    if (radius <= 0.0 || type == cornerNone) { add(px, py, false); return; }
    const double in_x = px - enter_x * radius, in_y = py - enter_y * radius, out_x = px + exit_x * radius, out_y = py + exit_y * radius;
    const double k = quarterArc * radius;
    switch (type) {
    case cornerRound: {
        PA& a = add(in_x, in_y, true); a.out_x = in_x + enter_x * k; a.out_y = in_y + enter_y * k;
        PA& b = add(out_x, out_y, true); b.in_x = out_x - exit_x * k; b.in_y = out_y - exit_y * k;
        break;
    }
    case cornerRoundInverse: {
        PA& a = add(in_x, in_y, false); a.out_x = in_x + exit_x * k; a.out_y = in_y + exit_y * k;
        PA& b = add(out_x, out_y, false); b.in_x = out_x - enter_x * k; b.in_y = out_y - enter_y * k;
        break;
    }
    case cornerCutout:
        add(in_x, in_y, false);
        add(in_x + out_x - px, in_y + out_y - py, false);
        add(out_x, out_y, false);
        break;
    default:
        add(in_x, in_y, false);
        add(out_x, out_y, false);
        break;
    }
}

// Wire semantics pinned by Patchy's one-toggle probes and its shape-curves sweep: the record's class tag is the
// kind; ShpB [x0, y0, x1, y1] is the local box; radial kinds work in the box-inscribed ellipse's unit space.
std::optional<VectorPath> shapePath(const Class& node, std::string* why) {
  // A linked symbol instance keeps its geometry (Shpe record AND local box)
  // on the defining sibling; the instance's own transform still places it.
  const Class* geometry_node = &node;
  if (node.child(tag("Shpe")) == nullptr) {
    if (const Class* sibling = symbolSiblingWith(node, tag("Shpe"))) {
      geometry_node = sibling;
    }
  }
  const Class* shape = geometry_node->child(tag("Shpe"));
  if (shape == nullptr) {
    *why = "has no shape record";
    return std::nullopt;
  }
  const auto box = geometry_node->vector(tag("ShpB"));
  if (box.size() != 4 || !(box[2] > box[0]) || !(box[3] > box[1])) {
    *why = "has a degenerate shape box";
    return std::nullopt;
  }
  const double x0 = box[0];
  const double y0 = box[1];
  const double x1 = box[2];
  const double y1 = box[3];
  const double w = x1 - x0;
  const double h = y1 - y0;

  std::vector<Sub> subs;
  Sub subpath;
  subpath.closed = true;

  // Shared helpers for the long-tail kinds: unit space is the box-inscribed
  // ellipse's coordinate system (centre (0,0), radius 1, y down); mapping to
  // the document is an anisotropic scale, which keeps beziers beziers.
  constexpr double kPiSh = 3.14159265358979323846;
  constexpr double kCircleK = 0.5522847498307934;
  const double ucx = (x0 + x1) / 2.0;
  const double ucy = (y0 + y1) / 2.0;
  const double hw = w / 2.0;
  const double hh = h / 2.0;
  const auto unit_x = [&](double ux) { return ucx + hw * ux; };
  const auto unit_y = [&](double uy) { return ucy + hh * uy; };
  const auto push_corner = [](Sub& sp, double px, double py) {
    sp.anchors.push_back(PA{px, py, px, py, px, py, false});
  };
  const auto push_corner_unit = [&](Sub& sp, double ux, double uy) {
    push_corner(sp, unit_x(ux), unit_y(uy));
  };
  // Append a circular arc (unit space: centre (ax,ay), radius r) from angle
  // a0 to a1, either direction, split into <=90-degree bezier segments. When
  // the subpath already ends at the arc's start point only its out handle is
  // set (so lines and arcs chain); interior anchors are smooth, the arc's
  // endpoints stay corners for the caller to join.
  const auto append_unit_arc = [&](Sub& sp, double ax, double ay, double r, double a0,
                                   double a1) {
    const int segments =
        std::max(1, static_cast<int>(std::ceil(std::abs(a1 - a0) / (kPiSh / 3.0) - 1e-9)));
    const double step = (a1 - a0) / segments;
    const double k = 4.0 / 3.0 * std::tan(step / 4.0) * r;
    for (int i = 0; i <= segments; ++i) {
      const double ang = a0 + step * static_cast<double>(i);
      const double px = unit_x(ax + r * std::cos(ang));
      const double py = unit_y(ay + r * std::sin(ang));
      const double tx = -std::sin(ang) * k * hw;
      const double ty = std::cos(ang) * k * hh;
      if (i == 0) {
        if (!sp.anchors.empty() && std::abs(sp.anchors.back().anchor_x - px) < 1e-6 &&
            std::abs(sp.anchors.back().anchor_y - py) < 1e-6) {
          sp.anchors.back().out_x = px + tx;
          sp.anchors.back().out_y = py + ty;
        } else {
          sp.anchors.push_back(PA{px, py, px, py, px + tx, py + ty, false});
        }
        continue;
      }
      PA anchor{px, py, px - tx, py - ty, px, py, i < segments};
      if (i < segments) {
        anchor.out_x = px + tx;
        anchor.out_y = py + ty;
      }
      sp.anchors.push_back(anchor);
    }
  };
  // A closed subpath built from chained arcs ends where it began; fold the
  // duplicate final anchor's incoming handle onto the first anchor.
  const auto close_fold = [](Sub& sp) {
    if (sp.anchors.size() >= 2) {
      const auto& first = sp.anchors.front();
      const auto& last = sp.anchors.back();
      if (std::abs(first.anchor_x - last.anchor_x) < 1e-6 &&
          std::abs(first.anchor_y - last.anchor_y) < 1e-6) {
        sp.anchors.front().in_x = last.in_x;
        sp.anchors.front().in_y = last.in_y;
        sp.anchors.pop_back();
      }
    }
  };

  if (shape->type == tag("ShNR")) {
    auto radii = shape->vector(tag("ShCR"));
    radii.resize(4, 0.0);
    auto types_raw = shape->vector(tag("CTyp"));
    types_raw.resize(4, static_cast<double>(cornerNone));
    std::array<int, 4> types{};
    for (std::size_t i = 0; i < 4; ++i) {
      types[i] = static_cast<int>(types_raw[i]);
    }
    if (shape->boolean(tag("Lock"), true)) {
      // Single-radius mode: corner 0 defines every corner.
      radii[1] = radii[2] = radii[3] = radii[0];
      types[1] = types[2] = types[3] = types[0];
    }
    const bool absolute = shape->boolean(tag("AbSz"), false);
    const double reference = std::min(w, h);
    std::array<double, 4> r{};  // TL, TR, BR, BL in local pixels
    for (std::size_t i = 0; i < 4; ++i) {
      const double value = absolute ? radii[i] : radii[i] * reference;
      r[i] = (types[i] == cornerNone) ? 0.0 : std::max(0.0, value);
    }
    // Adjacent radii cannot exceed their shared edge (the CSS overlap rule;
    // Affinity's exact overflow behavior is unprobed).
    double scale = 1.0;
    const auto limit = [&](double a, double b, double edge) {
      if (a + b > edge && a + b > 0.0) {
        scale = std::min(scale, edge / (a + b));
      }
    };
    limit(r[0], r[1], w);
    limit(r[1], r[2], h);
    limit(r[2], r[3], w);
    limit(r[3], r[0], h);
    for (double& value : r) {
      value *= scale;
    }
    appendRectCornerPA(subpath, x0, y0, 0.0, -1.0, 1.0, 0.0, r[0], types[0]);
    appendRectCornerPA(subpath, x1, y0, 1.0, 0.0, 0.0, 1.0, r[1], types[1]);
    appendRectCornerPA(subpath, x1, y1, 0.0, 1.0, -1.0, 0.0, r[2], types[2]);
    appendRectCornerPA(subpath, x0, y1, -1.0, 0.0, 0.0, -1.0, r[3], types[3]);
  } else if (shape->type == tag("ShpE")) {
    const double cx = (x0 + x1) / 2.0;
    const double cy = (y0 + y1) / 2.0;
    const double kx = quarterArc * (w / 2.0);
    const double ky = quarterArc * (h / 2.0);
    const auto add = [&](double ax, double ay, double ix, double iy, double ox, double oy) {
      subpath.anchors.push_back(PA{ax, ay, ix, iy, ox, oy, true});
    };
    add(cx, y0, cx - kx, y0, cx + kx, y0);
    add(x1, cy, x1, cy - ky, x1, cy + ky);
    add(cx, y1, cx + kx, y1, cx - kx, y1);
    add(x0, cy, x0, cy + ky, x0, cy - ky);
  } else if (shape->type == tag("ShPy")) {
    for (const auto& field : shape->fields) {
      if (field.tag != tag("Side") && field.tag != tag("Smth") &&
          field.tag != tag("Curv")) {
        *why = "is a non-regular Affinity polygon";
        return std::nullopt;
      }
    }
    const auto sides = std::clamp<std::int64_t>(shape->integer(tag("Side"), 5), 3, 256);
    const bool smooth = shape->boolean(tag("Smth"), false);
    const double cx = (x0 + x1) / 2.0;
    const double cy = (y0 + y1) / 2.0;
    constexpr double kPi = 3.14159265358979323846;
    // Smoothed polygons replace every corner with a symmetric smooth anchor
    // whose tangent length scales with Curv: the shp-polygon6-smooth probe
    // pins Smth=true/Curv=0 as rendering EXACTLY like the plain polygon, so
    // Curv is the rounding amount. Curv=1 is mapped to the circle through
    // the vertices ((4/3)tan(pi/2n) in unit-circle space) - plausible but
    // unpinned, no Curv>0 ground truth exists yet.
    const double curv = std::clamp(shape->number(tag("Curv"), 0.0), 0.0, 1.0);
    const double tangent =
        smooth ? curv * (4.0 / 3.0) * std::tan(kPi / (2.0 * static_cast<double>(sides))) : 0.0;
    for (std::int64_t i = 0; i < sides; ++i) {
      const double angle = -kPi / 2.0 + 2.0 * kPi * static_cast<double>(i) /
                                            static_cast<double>(sides);
      const double ux = std::cos(angle);
      const double uy = std::sin(angle);
      const double x = cx + (w / 2.0) * ux;
      const double y = cy + (h / 2.0) * uy;
      if (!smooth) {
        subpath.anchors.push_back(PA{x, y, x, y, x, y, false});
        continue;
      }
      // The tangent runs perpendicular to the radius (unit space), scaled per
      // axis by the box's half sizes.
      const double tx = -uy * tangent * (w / 2.0);
      const double ty = ux * tangent * (h / 2.0);
      subpath.anchors.push_back(PA{x, y, x - tx, y - ty, x + tx, y + ty, true});
    }
  } else if (shape->type == tag("ShSt")) {
    // Star: Pnts outer vertices on the box-inscribed ellipse (first vertex
    // up, like ShPy), alternating with inner vertices at the IRad fraction,
    // offset by half a step. CrvL/CrvR only take effect when Lgcy
    // (curvedEdges) is true - the shape-curves sweep pins both a plain-star
    // conversion for Lgcy alone or CrvL/CrvR alone, and tangential tip
    // handles for the combination (star-lgcy-crv, CrvL=CrvR=0.4 -> unit
    // handle 0.18665). Circle-rounded stars (CrcI/CrcO) keep the
    // placeholder until their tangent construction is pinned.
    if (shape->number(tag("CrcI"), 0.0) > 1e-6 ||
        shape->number(tag("CrcO"), 0.0) > 1e-6) {
      *why = "is a circle-rounded Affinity star";
      return std::nullopt;
    }
    const auto points = std::clamp<std::int64_t>(shape->integer(tag("Pnts"), 5), 3, 256);
    const double inner = std::clamp(shape->number(tag("IRad"), 0.5), 0.0, 1.0);
    const bool curved = shape->boolean(tag("Lgcy"), false);
    const double curve_left =
        curved ? std::clamp(shape->number(tag("CrvL"), 0.0), 0.0, 1.0) : 0.0;
    const double curve_right =
        curved ? std::clamp(shape->number(tag("CrvR"), 0.0), 0.0, 1.0) : 0.0;
    // The pinned 0.4 -> 0.18665 sample gives the per-unit-curve handle scale
    // at five points; scale other point counts by their half-step tangent.
    const double handle_scale = 0.46662 * std::tan(kPiSh / static_cast<double>(points)) /
                                std::tan(kPiSh / 5.0);
    const double cx = (x0 + x1) / 2.0;
    const double cy = (y0 + y1) / 2.0;
    constexpr double kPi = 3.14159265358979323846;
    for (std::int64_t i = 0; i < points; ++i) {
      const double outer_angle = -kPi / 2.0 + 2.0 * kPi * static_cast<double>(i) /
                                                  static_cast<double>(points);
      const double inner_angle = outer_angle + kPi / static_cast<double>(points);
      const double ox = cx + (w / 2.0) * std::cos(outer_angle);
      const double oy = cy + (h / 2.0) * std::sin(outer_angle);
      if (curve_left > 0.0 || curve_right > 0.0) {
        // Tangential handles round the tip; CrvL feeds the incoming side.
        const double tx = -std::sin(outer_angle) * hw;
        const double ty = std::cos(outer_angle) * hh;
        subpath.anchors.push_back(PA{
            ox, oy, ox - tx * curve_left * handle_scale, oy - ty * curve_left * handle_scale,
            ox + tx * curve_right * handle_scale, oy + ty * curve_right * handle_scale, false});
      } else {
        subpath.anchors.push_back(PA{ox, oy, ox, oy, ox, oy, false});
      }
      const double ix = cx + inner * (w / 2.0) * std::cos(inner_angle);
      const double iy = cy + inner * (h / 2.0) * std::sin(inner_angle);
      subpath.anchors.push_back(PA{ix, iy, ix, iy, ix, iy, false});
    }
  } else if (shape->type == tag("ShpD")) {
    // Diamond: side vertices sit Pos of the way up the box (default 0.5).
    const double pos = std::clamp(shape->number(tag("Pos "), 0.5), 0.0, 1.0);
    const double side_y = y1 - pos * h;
    push_corner(subpath, ucx, y0);
    push_corner(subpath, x1, side_y);
    push_corner(subpath, ucx, y1);
    push_corner(subpath, x0, side_y);
  } else if (shape->type == tag("ShTz")) {
    // Trapezoid: the top edge runs from PosL to PosR across the box.
    const double pos_l = std::clamp(shape->number(tag("PosL"), 0.25), 0.0, 1.0);
    const double pos_r = std::clamp(shape->number(tag("PosR"), 0.75), 0.0, 1.0);
    push_corner(subpath, x1, y1);
    push_corner(subpath, x0, y1);
    push_corner(subpath, x0 + pos_l * w, y0);
    push_corner(subpath, x0 + pos_r * w, y0);
  } else if (shape->type == tag("ShHt")) {
    // Heart: a fixed six-anchor template in box fractions; only the top
    // cleft anchor moves - its y is Sprd of the way down the box.
    const double spread = std::clamp(shape->number(tag("Sprd"), 0.2), 0.0, 1.0);
    struct HeartAnchor {
      double ax, ay, inx, iny, outx, outy;
      bool smooth;
    };
    const HeartAnchor kHeart[6] = {
        {0.5, 0.0, 0.3947370, 0.0, 0.6052630, 0.0, false},
        {0.9210530, 0.1, 0.8157890, 0.0, 1.0263160, 0.2, true},
        {0.9210530, 0.6, 1.0263160, 0.4, 0.8473680, 0.75, true},
        {0.5, 1.0, 0.6578950, 0.9, 0.3421050, 0.9, false},
        {0.0789474, 0.6, 0.1526320, 0.75, -0.0263158, 0.4, true},
        {0.0789474, 0.1, -0.0263158, 0.2, 0.1842110, 0.0, true},
    };
    for (const auto& a : kHeart) {
      const double ay = a.ax == 0.5 && a.ay == 0.0 ? spread : a.ay;
      subpath.anchors.push_back(PA{x0 + a.ax * w, y0 + ay * h, x0 + a.inx * w,
                                           y0 + a.iny * h, x0 + a.outx * w, y0 + a.outy * h,
                                           a.smooth});
    }
  } else if (shape->type == tag("ShTr")) {
    // Tear: an ellipse whose top anchor collapses into a corner at Tail
    // across the top edge; Curv scales the upper side handles (default 0.3),
    // Bend curls the tail tip (handle template pinned at Tail 0.5). The Fixd
    // and Ball fields do not alter 3.x geometry (sweep-pinned) and are
    // ignored.
    const double tail = std::clamp(shape->number(tag("Tail"), 0.5), 0.0, 1.0);
    const double curve = std::clamp(shape->number(tag("Curv"), 0.3), 0.0, 1.0);
    const double bend = std::clamp(shape->number(tag("Bend"), 0.0), -1.0, 1.0);
    const double tip_x = x0 + tail * w;
    // The bottom bulb is a half-ellipse whose vertical radius caps at half
    // the WIDTH (the tall-box probe pins side anchors at y1 - w/2 there);
    // the side anchors' upper handles are Curv of the way back up to the top.
    const double bulb = std::min(hw, hh);
    const double side_y = y1 - bulb;
    subpath.anchors.push_back(PA{tip_x, y0, tip_x + bend * w * 0.1,
                                         y0 + bend * h * 0.25, tip_x + bend * w * 0.25, y0,
                                         false});
    subpath.anchors.push_back(PA{x1, side_y, x1, side_y - curve * (side_y - y0), x1,
                                         side_y + kCircleK * bulb, true});
    subpath.anchors.push_back(PA{ucx, y1, ucx + kCircleK * hw, y1, ucx - kCircleK * hw,
                                         y1, true});
    subpath.anchors.push_back(PA{x0, side_y, x0, side_y + kCircleK * bulb, x0,
                                         side_y - curve * (side_y - y0), true});
  } else if (shape->type == tag("ShDA")) {
    // Arrow: a straight-line polygon. Head length is LPr1/RPr1 of the BOX
    // HEIGHT (wide-box pinned), the shaft is Thck of the height, and
    // LPr2/RPr2 offset the shaft junction from the barb base. Only end
    // styles 0 (flat) and 1 (the plain arrowhead) are modeled.
    const auto left_style = enumId(*shape, tag("LSty"), 1);
    const auto right_style = enumId(*shape, tag("RSty"), 1);
    if (left_style > 1 || right_style > 1) {
      *why = "uses an Affinity arrow end style NekoPhoto does not read yet";
      return std::nullopt;
    }
    const double thick = std::clamp(shape->number(tag("Thck"), 0.35), 0.0, 1.0);
    const double shaft_top = ucy - thick * hh;
    const double shaft_bottom = ucy + thick * hh;
    double left_len = std::max(0.0, shape->number(tag("LPr1"), 0.5)) * h;
    const double left_inner = shape->number(tag("LPr2"), 0.0) * h;
    double right_len = std::max(0.0, shape->number(tag("RPr1"), 0.5)) * h;
    const double right_inner = shape->number(tag("RPr2"), 0.0) * h;
    // Heads that would overlap scale down proportionally to meet (the
    // tall-box probe: two 30px heads in a 44px box become 22px each).
    const double head_total = (left_style == 1 ? left_len : 0.0) +
                              (right_style == 1 ? right_len : 0.0);
    if (head_total > w && head_total > 0.0) {
      left_len *= w / head_total;
      right_len *= w / head_total;
    }
    const double barb_l = x0 + left_len;
    const double shaft_l = left_style == 1 ? barb_l + left_inner : x0;
    const double barb_r = x1 - right_len;
    const double shaft_r = right_style == 1 ? barb_r - right_inner : x1;
    if (left_style == 1) {
      push_corner(subpath, shaft_l, shaft_bottom);
      push_corner(subpath, barb_l, y1);
      push_corner(subpath, x0, ucy);
      push_corner(subpath, barb_l, y0);
      push_corner(subpath, shaft_l, shaft_top);
    } else {
      push_corner(subpath, x0, shaft_bottom);
      push_corner(subpath, x0, shaft_top);
    }
    if (right_style == 1) {
      push_corner(subpath, shaft_r, shaft_top);
      push_corner(subpath, barb_r, y0);
      push_corner(subpath, x1, ucy);
      push_corner(subpath, barb_r, y1);
      push_corner(subpath, shaft_r, shaft_bottom);
    } else {
      push_corner(subpath, x1, shaft_top);
      push_corner(subpath, x1, shaft_bottom);
    }
  } else if (shape->type == tag("ShPi")) {
    // Pie: the wire angles are radians in y-UP math orientation (AngS
    // default pi/2 = top, AngE default 0 = right); the filled sector runs
    // from AngE to AngS clockwise on screen. IRad > 0 cuts an annulus.
    const double phi_s = -shape->number(tag("AngS"), kPiSh / 2.0);
    double phi_e = -shape->number(tag("AngE"), 0.0);
    const double inner = std::clamp(shape->number(tag("IRad"), 0.0), 0.0, 1.0);
    double sweep = phi_s - phi_e;
    sweep -= std::floor(sweep / (2.0 * kPiSh)) * 2.0 * kPiSh;  // wrap into (0, 2*pi]
    if (sweep < 1e-9) {
      sweep = 2.0 * kPiSh;
    }
    const double phi_end = phi_e + sweep;  // == phi_s modulo 2*pi
    if (inner <= 1e-6) {
      push_corner_unit(subpath, std::cos(phi_end), std::sin(phi_end));
      push_corner_unit(subpath, 0.0, 0.0);
      append_unit_arc(subpath, 0.0, 0.0, 1.0, phi_e, phi_end);
    } else {
      push_corner_unit(subpath, std::cos(phi_end), std::sin(phi_end));
      append_unit_arc(subpath, 0.0, 0.0, inner, phi_end, phi_e);
      append_unit_arc(subpath, 0.0, 0.0, 1.0, phi_e, phi_end);
    }
    close_fold(subpath);
  } else if (shape->type == tag("ShSg")) {
    // Segment: the box ellipse clipped to the band between two chords
    // perpendicular to the Angl direction (unit space): Pos0/Pos1 map to
    // signed offsets 2*Pos-1 along the direction.
    const double angle = shape->number(tag("Angl"), kPiSh / 2.0);
    double c0 = std::clamp(2.0 * shape->number(tag("Pos0"), 0.25) - 1.0, -1.0, 1.0);
    double c1 = std::clamp(2.0 * shape->number(tag("Pos1"), 1.0) - 1.0, -1.0, 1.0);
    if (c0 > c1) {
      std::swap(c0, c1);
    }
    const double alpha0 = std::acos(std::clamp(c0, -1.0, 1.0));
    const double alpha1 = std::acos(std::clamp(c1, -1.0, 1.0));
    if (alpha0 - alpha1 < 1e-6) {
      *why = "has a degenerate shape outline";
      return std::nullopt;
    }
    const double delta = -angle;  // screen angle of the cut direction
    push_corner_unit(subpath, std::cos(delta + alpha0), std::sin(delta + alpha0));
    push_corner_unit(subpath, std::cos(delta - alpha0), std::sin(delta - alpha0));
    if (alpha1 > 1e-6) {
      append_unit_arc(subpath, 0.0, 0.0, 1.0, delta - alpha0, delta - alpha1);
      push_corner_unit(subpath, std::cos(delta + alpha1), std::sin(delta + alpha1));
      append_unit_arc(subpath, 0.0, 0.0, 1.0, delta + alpha1, delta + alpha0);
    } else {
      append_unit_arc(subpath, 0.0, 0.0, 1.0, delta - alpha0, delta + alpha0);
    }
    close_fold(subpath);
  } else if (shape->type == tag("ShCr")) {
    // Crescent: the region between two boundary curves (unit space), each
    // from (0,-1) through (Arc, 0) to (0,1) - ArcL/ArcR are the signed bulge
    // fractions (the JS setters negate leftArc on the wire; the wire values
    // place both boundaries directly). Affinity's boundary is two cubics
    // whose handles the shape-curves sweep pins exactly across four bulges:
    // the endpoint handle is (K*m, (1-|m|)/3) and the mid handle is vertical
    // with length K*|m| + (1-|m|)/3 - the ellipse half at |m|=1, the
    // straight line at m=0, blended linearly between.
    const double arc_l = std::clamp(shape->number(tag("ArcL"), -1.0), -1.0, 1.0);
    const double arc_r = std::clamp(shape->number(tag("ArcR"), -0.3), -1.0, 1.0);
    if (std::abs(arc_r - arc_l) < 1e-6) {
      *why = "has a degenerate shape outline";
      return std::nullopt;
    }
    // Emits the boundary for bulge m from (0,-1) to (0,1) when down is true,
    // or the reverse; the subpath's last anchor is the start point.
    const auto boundary = [&](double m, bool down) {
      const double s = down ? 1.0 : -1.0;
      if (std::abs(m) < 1e-6) {
        push_corner_unit(subpath, 0.0, s);
        return;
      }
      const double end_off = (1.0 - std::abs(m)) / 3.0;
      const double mid_len = kCircleK * std::abs(m) + end_off;
      auto& start = subpath.anchors.back();
      start.out_x = unit_x(kCircleK * m);
      start.out_y = unit_y(-s + s * end_off);
      subpath.anchors.push_back(PA{unit_x(m), unit_y(0.0), unit_x(m),
                                           unit_y(-s * mid_len), unit_x(m), unit_y(s * mid_len),
                                           true});
      subpath.anchors.push_back(PA{unit_x(0.0), unit_y(s), unit_x(kCircleK * m),
                                           unit_y(s - s * end_off), unit_x(0.0), unit_y(s),
                                           false});
    };
    push_corner_unit(subpath, 0.0, -1.0);
    boundary(arc_r, true);
    boundary(arc_l, false);
    close_fold(subpath);
  } else if (shape->type == tag("ShDS")) {
    // Double star: 4*Pnts straight-line vertices at uniform angle steps from
    // the top, radii cycling [1, IRad, PRad, IRad].
    const auto points = std::clamp<std::int64_t>(shape->integer(tag("Pnts"), 5), 2, 128);
    const double inner = std::clamp(shape->number(tag("IRad"), 0.525731), 0.0, 1.0);
    const double point_r = std::clamp(shape->number(tag("PRad"), 0.809017), 0.0, 1.0);
    const double radii[4] = {1.0, inner, point_r, inner};
    const std::int64_t count = points * 4;
    for (std::int64_t i = 0; i < count; ++i) {
      const double ang = -kPiSh / 2.0 +
                         2.0 * kPiSh * static_cast<double>(i) / static_cast<double>(count);
      const double r = radii[i % 4];
      push_corner_unit(subpath, r * std::cos(ang), r * std::sin(ang));
    }
  } else if (shape->type == tag("ShSS")) {
    // Square star: Side arms whose tips are flattened; per arm the two outer
    // corners sit at (cos h, +/- (1-COut)*sin h) in tip-local unit space
    // (h = pi/Side), with the inner corner between arms at radius 1-COut.
    const auto sides = std::clamp<std::int64_t>(shape->integer(tag("Side"), 5), 3, 128);
    const double cutout = std::clamp(shape->number(tag("COut"), 0.5), 0.0, 1.0);
    const double inner_r = 1.0 - cutout;
    const double half = kPiSh / static_cast<double>(sides);
    for (std::int64_t k = 0; k < sides; ++k) {
      const double tip = -kPiSh / 2.0 + half +
                         2.0 * kPiSh * static_cast<double>(k) / static_cast<double>(sides);
      const double ct = std::cos(tip);
      const double st = std::sin(tip);
      const double px = std::cos(half);
      const double py = inner_r * std::sin(half);
      push_corner_unit(subpath, ct * px - st * -py, st * px + ct * -py);
      push_corner_unit(subpath, ct * px - st * py, st * px + ct * py);
      push_corner_unit(subpath, inner_r * std::cos(tip + half), inner_r * std::sin(tip + half));
    }
  } else if (shape->type == tag("ShCg")) {
    // Cog: alternating tooth-top arcs (radius 1, width TtSz of the tooth
    // period) and root arcs (radius IRad, width NtSz), joined by straight
    // flanks; Curv bows the flanks (handles at 2/3*Curv along the chord,
    // approximate); Hole cuts a centre ellipse.
    const auto teeth = std::clamp<std::int64_t>(shape->integer(tag("Teth"), 12), 3, 256);
    const double root_r = std::clamp(shape->number(tag("IRad"), 0.85), 0.0, 1.0);
    const double hole = std::clamp(shape->number(tag("Hole"), 0.2), 0.0, 1.0);
    const double tooth_size = std::clamp(shape->number(tag("TtSz"), 0.37), 0.0, 1.0);
    const double notch_size = std::clamp(shape->number(tag("NtSz"), 0.42), 0.0, 1.0);
    const double curve = std::clamp(shape->number(tag("Curv"), 0.0), 0.0, 1.0);
    const double period = 2.0 * kPiSh / static_cast<double>(teeth);
    const double half_top = tooth_size * period / 2.0;
    const double half_notch = notch_size * period / 2.0;
    for (std::int64_t k = 0; k < teeth; ++k) {
      const double centre = -kPiSh / 2.0 + period * static_cast<double>(k);
      append_unit_arc(subpath, 0.0, 0.0, 1.0, centre - half_top, centre + half_top);
      append_unit_arc(subpath, 0.0, 0.0, root_r, centre + period / 2.0 - half_notch,
                      centre + period / 2.0 + half_notch);
    }
    close_fold(subpath);
    if (curve > 0.0) {
      // Bow every straight flank: a flank runs between an arc-end anchor
      // (degenerate out handle) and the next arc-start anchor.
      const std::size_t n = subpath.anchors.size();
      for (std::size_t i = 0; i < n; ++i) {
        auto& a = subpath.anchors[i];
        auto& b = subpath.anchors[(i + 1) % n];
        const bool a_flat = a.out_x == a.anchor_x && a.out_y == a.anchor_y;
        const bool b_flat = b.in_x == b.anchor_x && b.in_y == b.anchor_y;
        if (a_flat && b_flat) {
          const double dx = (b.anchor_x - a.anchor_x) * (2.0 / 3.0) * curve;
          const double dy = (b.anchor_y - a.anchor_y) * (2.0 / 3.0) * curve;
          a.out_x = a.anchor_x + dx;
          a.out_y = a.anchor_y + dy;
          b.in_x = b.anchor_x - dx;
          b.in_y = b.anchor_y - dy;
        }
      }
    }
    if (subpath.anchors.size() < 2) {
      *why = "has a degenerate shape outline";
      return std::nullopt;
    }
    subs.push_back(std::move(subpath));
    if (hole > 1e-6) {
      Sub hole_path;
      hole_path.closed = true;
      append_unit_arc(hole_path, 0.0, 0.0, hole, -kPiSh / 2.0, 3.0 * kPiSh / 2.0);
      close_fold(hole_path);
      subs.push_back(std::move(hole_path));
    }
    return toPath(subs);
  } else if (shape->type == tag("ShCl")) {
    // Cloud: Bubl bubbles; notches at radius IRad between bumps at radius 1.
    // Each bubble half is a quarter-ellipse in the bump-rotated frame with
    // radial semi-axis 1 - IRad*cos(P/2) and tangential semi-axis
    // IRad*sin(P/2) (wide- and default-probe pinned).
    const auto bubbles = std::clamp<std::int64_t>(shape->integer(tag("Bubl"), 12), 3, 256);
    const double inner = std::clamp(shape->number(tag("IRad"), 0.8164966), 0.01, 1.0);
    const double period = 2.0 * kPiSh / static_cast<double>(bubbles);
    const double radial = std::max(0.0, 1.0 - inner * std::cos(period / 2.0));
    const double tangential = inner * std::sin(period / 2.0);
    for (std::int64_t k = 0; k < bubbles; ++k) {
      const double bump = -kPiSh / 2.0 + period * static_cast<double>(k);
      const double dirx = std::cos(bump);
      const double diry = std::sin(bump);
      const double tanx = -std::sin(bump);
      const double tany = std::cos(bump);
      const double n1x = inner * std::cos(bump - period / 2.0);
      const double n1y = inner * std::sin(bump - period / 2.0);
      const double n2x = inner * std::cos(bump + period / 2.0);
      const double n2y = inner * std::sin(bump + period / 2.0);
      const auto push_or_patch = [&](double ux, double uy, double outx, double outy) {
        const double px = unit_x(ux);
        const double py = unit_y(uy);
        if (!subpath.anchors.empty() && std::abs(subpath.anchors.back().anchor_x - px) < 1e-6 &&
            std::abs(subpath.anchors.back().anchor_y - py) < 1e-6) {
          subpath.anchors.back().out_x = unit_x(outx);
          subpath.anchors.back().out_y = unit_y(outy);
        } else {
          subpath.anchors.push_back(PA{px, py, px, py, unit_x(outx), unit_y(outy),
                                               false});
        }
      };
      push_or_patch(n1x, n1y, n1x + kCircleK * radial * dirx, n1y + kCircleK * radial * diry);
      subpath.anchors.push_back(PA{
          unit_x(dirx), unit_y(diry), unit_x(dirx - kCircleK * tangential * tanx),
          unit_y(diry - kCircleK * tangential * tany),
          unit_x(dirx + kCircleK * tangential * tanx),
          unit_y(diry + kCircleK * tangential * tany), true});
      subpath.anchors.push_back(PA{unit_x(n2x), unit_y(n2y),
                                           unit_x(n2x + kCircleK * radial * dirx),
                                           unit_y(n2y + kCircleK * radial * diry), unit_x(n2x),
                                           unit_y(n2y), false});
    }
    close_fold(subpath);
  } else if (shape->type == tag("ShpT")) {
    // Triangle: apex at the "Pos " fraction across the top edge (default
    // centered), base along the bottom of the box.
    for (const auto& field : shape->fields) {
      if (field.tag != tag("Pos ")) {
        *why = "is an Affinity triangle variant NekoPhoto does not read yet";
        return std::nullopt;
      }
    }
    const double pos = std::clamp(shape->number(tag("Pos "), 0.5), 0.0, 1.0);
    const double ax = x0 + pos * w;
    subpath.anchors.push_back(PA{ax, y0, ax, y0, ax, y0, false});
    subpath.anchors.push_back(PA{x1, y1, x1, y1, x1, y1, false});
    subpath.anchors.push_back(PA{x0, y1, x0, y1, x0, y1, false});
  } else {
    *why = "is an Affinity shape kind NekoPhoto does not read yet";
    return std::nullopt;
  }

  if (subpath.anchors.size() < 2) {
    *why = "has a degenerate shape outline";
    return std::nullopt;
  }
  subs.push_back(std::move(subpath));
  return toPath(subs);
}

void transformPath(VectorPath& path, const Affine6& m) {
    for (auto& s : path.subpaths)
        for (auto& k : s.knots) {
            const Point a = apply(m, k.x, k.y), i = apply(m, k.inX, k.inY), o = apply(m, k.outX, k.outY);
            k = {i.x, i.y, a.x, a.y, o.x, o.y};
        }
}

bool isArtboard(const Class& node) { return node.type == tag("ShpN") && (node.boolean(tag("ABEn"), false) || node.child(tag("phrp"))); }

/// Adjustments (the *RA family) and live filters (FlRN): their bitmap is a mask, not content.
bool isAdjustmentOrFilter(uint32_t type) { return (type & 0xFFFFu) == (uint32_t('R') << 8 | 'A') || type == tag("FlRN"); }

// ------------------------------------------------------------------------------------------------ building

/// A layer as the build makes it, straight alpha, before it becomes a Document entry.
struct Built {
    std::string name;
    bool group = false, passThrough = true, visible = true, clipped = false, erase = false;
    double opacity = 1;
    BlendMode blend = BlendMode::Normal;
    Straight image;
    int x = 0, y = 0;
    Plane mask;
    int maskX = 0, maskY = 0;
    uint8_t maskOutside = 255;   // what the mask is beyond its plane
    std::vector<Built> children;
};

struct Context {
    Source src;
    const PsdImportOptions& options;
    std::vector<std::string>& notes;
    int width = 0, height = 0;
    int embedDepth = 0;
    int layerCount = 0;
    uint32_t documentVersion = 0;
    Affine6 transform = identity();
    int leftOut = 0;   // visible content not carried
};

Document readContainer(std::span<const uint8_t> bytes, std::vector<std::string>& notes, int embedDepth, const PsdImportOptions& options, ImagePtr* preview);
void buildLayers(Context& ctx, const ClassList& children, std::vector<Built>& out, bool foldErase = true);

std::string layerName(const Class& node) {
    std::string name = node.string(tag("Desc"));
    if (name.empty()) name = node.string(tag("IRFN"));   // placed images show their file name
    return name;
}

/// The node's transform composed under its ancestors'; identity when both are.
Affine6 nodeTransform(const Context& ctx, const Class& node) {
    const auto own = affineOf(node.vector(tag("Xfrm")));
    return own ? compose(ctx.transform, *own) : ctx.transform;
}

std::optional<std::pair<int, int>> translationOf(const Affine6& m) {
    if (std::abs(m[0] - 1) < 1e-6 && std::abs(m[4] - 1) < 1e-6 && std::abs(m[1]) < 1e-6 && std::abs(m[3]) < 1e-6)
        return std::pair{int(std::lround(m[2])), int(std::lround(m[5]))};
    return std::nullopt;
}

void applyBlend(Context& ctx, const Class& node, Built& b, const std::string& name) {
    const affinity::Field* f = node.field(tag("Blnd"));
    const auto* e = f ? std::get_if<affinity::Enum>(&f->value) : nullptr;
    if (!e) return;
    if (b.group) b.passThrough = false;   // an explicit mode isolates a group
    if (auto mode = blendMode(e->id, e->version)) { b.blend = *mode; return; }
    const bool v6 = e->version >= 6;
    const auto approximate = [&](BlendMode mode, double scale, const char* affinityName, const char* ours) {
        b.blend = mode;
        b.opacity *= scale;
        ctx.notes.push_back("Layer \"" + name + "\": Affinity's " + affinityName + " blend mode was approximated as " + ours + ".");
    };
    if ((e->version == 0 && e->id == 21) || (v6 && e->id == 27)) approximate(BlendMode::Normal, 0.5, "Average", "Normal at half opacity");
    else if ((e->version == 0 && e->id == 22) || (v6 && e->id == 28)) approximate(BlendMode::Exclusion, 1, "Negation", "Exclusion");
    else if ((e->version == 0 && e->id == 23) || (v6 && e->id == 29)) approximate(BlendMode::Overlay, 1, "Reflect", "Overlay");
    else if ((e->version == 0 && e->id == 24) || (v6 && e->id == 30)) approximate(BlendMode::LinearLight, 1, "Glow", "Linear Light");
    else if (v6 && e->id == 1) approximate(BlendMode::Overlay, 1, "Pigment", "Overlay");
    else if ((e->version == 0 && e->id == 25) || (v6 && e->id == 32)) b.erase = !b.group;   // folded later; a group carrier stays Normal
    else ctx.notes.push_back("Layer \"" + name + "\": its Affinity blend mode has no counterpart; Normal was used.");
}

void applyCommon(Context& ctx, const Class& node, Built& b, const std::string& name) {
    b.visible = node.boolean(tag("Visi"), true);
    b.opacity = std::clamp(node.number(tag("Opac"), 1.0), 0.0, 1.0);
    // Fill opacity equals opacity for a layer without effects, and effects are not carried.
    if (!b.group) b.opacity *= std::clamp(node.number(tag("FOpc"), 1.0), 0.0, 1.0);
    applyBlend(ctx, node, b, name);
    if (const ClassList* effects = node.list(tag("FiEf"))) {
        int enabled = 0;
        for (const auto& fx : *effects) if (fx && fx->boolean(tag("Enab"), false)) enabled++;
        if (enabled) ctx.notes.push_back("Layer \"" + name + "\": " + std::to_string(enabled) + " layer effect(s) were left out.");
    }
}

/// Every vector outline under `node` (itself, or its children's) in document space, one shape group each.
void appendMaskPaths(const Class& node, const Affine6& base, int depth, VectorPath& into, int& group, int& appended, int& unsupported) {
    if (depth > 16) { unsupported++; return; }
    if (!node.boolean(tag("Visi"), true)) return;
    Affine6 composed = base;
    if (auto own = affineOf(node.vector(tag("Xfrm")))) composed = compose(base, *own);
    auto path = curvePath(node);
    std::string why;
    if (!path && node.type == tag("ShpN")) path = shapePath(node, &why);
    if (path) {
        transformPath(*path, composed);
        for (auto& s : path->subpaths) { s.group = group; s.op = VectorPath::Op::Add; into.subpaths.push_back(std::move(s)); }
        group++;
        appended++;
        return;
    }
    if (const ClassList* kids = node.list(tag("Chld")); kids && !kids->empty()) {
        for (const auto& k : *kids) if (k) appendMaskPaths(*k, composed, depth + 1, into, group, appended, unsupported);
        return;
    }
    unsupported++;
}

/// Coverage of `path` over the canvas.
Plane canvasCoverage(const Context& ctx, const VectorPath& path) {
    return rasterizeVectorMask(path, Rect(0, 0, ctx.width, ctx.height), 1, ctx.width, ctx.height);
}

/// The node's mask: an M8/M16 plane in its AdCh list, or a vector subtree acting as its clip, drawn as a raster.
void applyMasks(Context& ctx, const Class& node, Built& b, const std::string& name) {
    const ClassList* adjuncts = node.list(tag("AdCh"));
    if (!adjuncts) return;
    Affine6 ownerSpace = ctx.transform;
    if (auto own = affineOf(node.vector(tag("Xfrm")))) ownerSpace = compose(ctx.transform, *own);
    for (const auto& adjunct : *adjuncts) {
        if (!adjunct) continue;
        if (isAdjustmentOrFilter(adjunct->type)) {
            ctx.notes.push_back("Layer \"" + name + "\": an adjustment or live filter attached to it was left out.");
            ctx.leftOut++;
            continue;
        }
        if (b.mask) { ctx.notes.push_back("Layer \"" + name + "\": it has more than one mask; only the first was imported."); break; }
        const Class* dybm = adjunct->child(tag("Bitm"));
        if (!dybm) {
            VectorPath path;
            int group = 0, appended = 0, unsupported = 0;
            appendMaskPaths(*adjunct, ownerSpace, 0, path, group, appended, unsupported);
            if (appended == 0 && unsupported == 0) continue;
            if (unsupported > 0 || path.subpaths.empty()) { ctx.notes.push_back("Layer \"" + name + "\": a vector mask could not be read and was left out."); continue; }
            b.mask = canvasCoverage(ctx, path);
            b.maskX = b.maskY = 0;
            b.maskOutside = 0;
            continue;
        }
        std::string why;
        auto decoded = decodeBitmap(ctx.src, *dybm, name, ctx.options, ctx.notes, &why);
        if (!decoded || !decoded->mask) { ctx.notes.push_back("Layer \"" + name + "\": a mask could not be read and was left out."); continue; }
        // The adjunct lives in its owner's space: the owner's transform composes before its own.
        Affine6 m = ownerSpace;
        if (auto own = affineOf(adjunct->vector(tag("Xfrm")))) m = compose(ownerSpace, *own);
        if (auto t = translationOf(m)) {
            b.mask = decoded->mask;
            b.maskX = t->first;
            b.maskY = t->second;
        } else if (auto placed = resampleAffine(*decoded->mask, m)) {
            std::tie(b.mask, b.maskX, b.maskY) = *placed;
        } else {
            ctx.notes.push_back("Layer \"" + name + "\": its mask has a degenerate transform; placed approximately.");
            b.mask = decoded->mask;
            b.maskX = int(std::lround(m[2]));
            b.maskY = int(std::lround(m[5]));
        }
        b.maskOutside = 255;
    }
}

/// Paint of a PCrv or ShpN: solid fill (BFFl, or the oldest files' BFil) and solid stroke (LILn line style, LIFl
/// paint), drawn over the canvas as a straight-alpha layer. None when nothing visible is drawn.
struct Paint {
    std::optional<std::array<float, 4>> fill, stroke;
    VectorStroke line;
    bool gradient = false, texturedStroke = false;
};

std::optional<std::array<float, 4>> solidOf(const Class* descriptor, bool& gradient) {
    if (!descriptor) return std::nullopt;
    const Class* fill = descriptor->child(tag("FDeF"));
    if (!fill && (descriptor->type == tag("FilS") || descriptor->type == tag("FilN") || descriptor->type == tag("FilG"))) fill = descriptor;
    if (!fill || fill->type == tag("FilN")) return std::nullopt;
    if (fill->type == tag("FilG")) { gradient = true; return std::nullopt; }
    return readColor(fill->child(tag("Colr")));
}

bool readPaint(const Class& source, double transformScale, Paint& paint) {
    const Class* fillDescriptor = firstClass(source, tag("BFFl"));
    if (!fillDescriptor) fillDescriptor = firstClass(source, tag("BFil"));
    paint.fill = solidOf(fillDescriptor, paint.gradient);
    const Class* lineDescriptor = firstClass(source, tag("LILn"));
    if (!lineDescriptor || !lineDescriptor->child(tag("LDeL"))) lineDescriptor = firstClass(source, tag("LSty"));
    if (const Class* style = lineDescriptor ? lineDescriptor->child(tag("LDeL")) : nullptr) {
        const double weight = style->number(tag("Wght"), 0.0);
        uint8_t lineStyle = 1;
        if (const affinity::Field* d = style->field(tag("Data")))
            if (const auto* blob = std::get_if<affinity::CurveArray>(&d->value); blob && blob->bytes.size() >= 12) lineStyle = blob->bytes[10];
        if (weight > 0 && lineStyle != 0) {
            bool strokeGradient = false;
            auto color = solidOf(firstClass(source, tag("LIFl")), strokeGradient);
            if (!color) color = solidOf(firstClass(source, tag("PFil")), strokeGradient);
            if (strokeGradient) paint.gradient = true;
            if (color) {
                paint.stroke = color;
                paint.line.enabled = true;
                paint.line.width = lineDescriptor->boolean(tag("LDSc"), false) ? weight * transformScale : weight;
                switch (lineDescriptor->integer(tag("LDSa"), 0)) {
                case 1: paint.line.align = VectorStroke::Align::Inside; break;
                case 2: paint.line.align = VectorStroke::Align::Outside; break;
                default: paint.line.align = VectorStroke::Align::Center; break;
                }
                if (lineStyle == 2) { paint.line.dashes = style->vector(tag("Patn")); paint.line.dashOffset = style->number(tag("Phse"), 0.0); }
                if (lineStyle == 3) paint.texturedStroke = true;
                paint.line.miterLimit = std::max(1.0, source.number(tag("CnML"), paint.line.miterLimit));
            }
        }
    }
    return paint.fill || paint.stroke;
}

/// Draws `path` (document space) with the node's paint; none when nothing shows.
std::optional<Built> drawVector(Context& ctx, const Class& node, const std::string& name, VectorPath path, const Affine6& m, std::string* why) {
    transformPath(path, m);
    const double det = m[0] * m[4] - m[1] * m[3];
    const double scale = std::isfinite(det) && det != 0 ? std::sqrt(std::abs(det)) : 1.0;
    Paint paint;
    bool painted = readPaint(node, scale, paint);
    if (!painted) {
        // Symbol instances may keep their paint on the defining sibling.
        for (const char* ring : {"SLnk", "GLnk", "DLnk", "CLnk"}) {
            const Class* link = node.child(tagOf(ring));
            const ClassList* members = link ? link->list(tag("ILOb")) : nullptr;
            if (!members) continue;
            for (const auto& member : *members)
                if (member && member.get() != &node && readPaint(*member, scale, paint)) { painted = true; break; }
            if (painted) break;
        }
    }
    if (!painted) { *why = paint.gradient ? "is filled with a gradient, which is not read yet" : "has no visible fill or stroke"; return std::nullopt; }
    if (paint.gradient) ctx.notes.push_back("Layer \"" + name + "\": a gradient fill or stroke was left out.");
    if (paint.texturedStroke) ctx.notes.push_back("Layer \"" + name + "\": Affinity's textured brush stroke was drawn as a solid stroke.");

    // Draw within the canvas: the path's bounds (handles included) grown by the stroke.
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const auto& s : path.subpaths)
        for (const auto& k : s.knots)
            for (auto [px, py] : {std::pair{k.x, k.y}, {k.inX, k.inY}, {k.outX, k.outY}}) { x0 = std::min(x0, px); y0 = std::min(y0, py); x1 = std::max(x1, px); y1 = std::max(y1, py); }
    const double grow = (paint.stroke ? paint.line.width * 2 : 0) + 2;
    const int left = std::max(0, int(std::floor(x0 - grow))), top = std::max(0, int(std::floor(y0 - grow)));
    const int right = std::min(ctx.width, int(std::ceil(x1 + grow))), bottom = std::min(ctx.height, int(std::ceil(y1 + grow)));
    if (right <= left || bottom <= top) { *why = "lies outside the canvas"; return std::nullopt; }
    const int w = right - left, h = bottom - top;
    const Rect region(left, top, w, h);
    auto image = std::make_shared<Image>(w, h);
    const auto paintOver = [&](const GrayImage& coverage, const std::array<float, 4>& c) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const double sa = coverage.at(x, y) / 255.0 * c[3];
                if (sa <= 0) continue;
                uint8_t* o = image->pixel(x, y);
                const double da = o[3] / 255.0, oa = sa + da * (1 - sa);
                for (int k = 0; k < 3; k++) o[k] = uint8_t(std::lround((c[size_t(k)] * 255 * sa + o[k] * da * (1 - sa)) / oa));
                o[3] = uint8_t(std::lround(oa * 255));
            }
    };
    if (paint.fill) {
        VectorPath closed = path;
        for (auto& s : closed.subpaths) s.closed = true;
        paintOver(*rasterizeVectorMask(closed, region, 1, w, h), *paint.fill);
    }
    if (paint.stroke) paintOver(*rasterizeVectorStroke(path, paint.line, region, 1, w, h), *paint.stroke);
    if (alphaBounds(*image).isEmpty()) { *why = "draws nothing visible"; return std::nullopt; }
    Built b;
    b.name = name;
    b.image = image;
    b.x = left;
    b.y = top;
    applyCommon(ctx, node, b, name);
    applyMasks(ctx, node, b, name);
    return b;
}

std::optional<Built> buildShapeNode(Context& ctx, const Class& node, const std::string& name, std::string* why) {
    auto path = node.type == tag("ShpN") ? shapePath(node, why) : curvePath(node);
    if (!path) { if (why->empty()) *why = "has no readable outline"; return std::nullopt; }
    return drawVector(ctx, node, name, std::move(*path), nodeTransform(ctx, node), why);
}

Built makeGroup(const std::string& name) { Built g; g.name = name; g.group = true; return g; }

/// A canvas mask clipping to the axis-aligned box.
void clipToBox(Context& ctx, Built& group, double x0, double y0, double x1, double y1) {
    const int mx = int(std::lround(x0)), my = int(std::lround(y0)), mw = int(std::lround(x1 - x0)), mh = int(std::lround(y1 - y0));
    if (mw <= 0 || mh <= 0 || !plausibleSize(mw, mh)) return;
    group.mask = std::make_shared<GrayImage>(mw, mh, 255);
    group.maskX = mx;
    group.maskY = my;
    group.maskOutside = 0;
    (void)ctx;
}

Built buildGroup(Context& ctx, const Class& node, const std::string& name) {
    Built group = makeGroup(name.empty() ? "Group" : name);
    applyCommon(ctx, node, group, group.name);
    if (const ClassList* kids = node.list(tag("Chld"))) {
        const Affine6 saved = ctx.transform;
        ctx.transform = nodeTransform(ctx, node);
        buildLayers(ctx, *kids, group.children);
        ctx.transform = saved;
    }
    applyMasks(ctx, node, group, group.name);
    return group;
}

/// An artboard: a group at its layout position, its own rectangle as the bottom layer, clipped to its bounds.
Built buildArtboard(Context& ctx, const Class& node, const std::string& name) {
    Built group = makeGroup(name);
    group.visible = node.boolean(tag("Visi"), true);
    group.opacity = std::clamp(node.number(tag("Opac"), 1.0), 0.0, 1.0);
    std::string why;
    if (auto background = buildShapeNode(ctx, node, name + " Background", &why)) {
        background->visible = true;
        background->opacity = 1;
        background->blend = BlendMode::Normal;
        background->mask = nullptr;
        group.children.push_back(std::move(*background));
    }
    const Affine6 saved = ctx.transform;
    ctx.transform = nodeTransform(ctx, node);
    const Affine6 m = ctx.transform;
    if (const ClassList* kids = node.list(tag("Chld"))) buildLayers(ctx, *kids, group.children);
    ctx.transform = saved;
    const auto box = node.vector(tag("ShpB"));
    if (box.size() == 4 && box[2] > box[0] && box[3] > box[1]) {
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (auto [sx, sy] : {std::pair{box[0], box[1]}, {box[2], box[1]}, {box[0], box[3]}, {box[2], box[3]}}) {
            const Point p = apply(m, sx, sy);
            x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
        }
        clipToBox(ctx, group, x0, y0, x1, y1);
    }
    return group;
}

/// A placed embedded document (EmbR > EmCn > EmbC names its edc/<n> stream): read and flattened.
Straight flattenEmbedded(Context& ctx, const Class& embr, const std::string& name) {
    if (ctx.embedDepth >= maxEmbedDepth) { ctx.notes.push_back("Layer \"" + name + "\": embedded documents nest too deeply; left out."); return nullptr; }
    const Class* container = embr.child(tag("EmCn"));
    const affinity::Field* f = container ? container->field(tag("EmbC")) : nullptr;
    const auto* ref = f ? std::get_if<affinity::Embedded>(&f->value) : nullptr;
    if (!ref || ref->data.empty()) return nullptr;
    auto stream = ctx.src.container.streams.find(ref->data);
    if (stream == ctx.src.container.streams.end()) return nullptr;
    try {
        const auto nested = extractStream(ctx.src.bytes, stream->second, ref->data, nullptr);
        std::span<const uint8_t> span(nested);
        if (span.size() > 8 && span[0] == 'E' && span[1] == 'm' && span[2] == 'D' && span[3] == 'c') span = span.subspan(8);   // "EmDc" + u32
        std::vector<std::string> inner;
        const Document doc = readContainer(span, inner, ctx.embedDepth + 1, ctx.options, nullptr);
        ctx.notes.push_back("Layer \"" + name + "\": an embedded document was flattened to pixels.");
        return straightCopy(*renderFlattened(doc));
    } catch (const std::exception& e) {
        ctx.notes.push_back("Layer \"" + name + "\": its embedded document could not be read (" + e.what() + ").");
        return nullptr;
    }
}

/// Erase carriers become a mask on a new isolated group holding the layers beneath them: 255 minus the coverage
/// the carrier would paint. Carriers the construction cannot express stay Normal, with a note.
void foldErase(Context& ctx, std::vector<Built>& out, bool allowed) {
    for (size_t i = 0; i < out.size(); i++) {
        Built& carrier = out[i];
        if (!carrier.erase) continue;
        carrier.erase = false;
        const std::string name = carrier.name;
        const auto decline = [&](const std::string& why) { ctx.notes.push_back("Layer \"" + name + "\": Affinity's Erase blend mode " + why + "; shown as Normal."); };
        if (!carrier.visible || carrier.opacity <= 0) continue;
        if (!allowed) { decline("is not supported inside a clipping group"); continue; }
        if (carrier.clipped) { decline("is not supported on a clipped layer"); continue; }
        if (i + 1 < out.size() && out[i + 1].clipped) { decline("is not supported on the base of a clipping group"); continue; }
        if (i == 0) { decline("has no layers beneath it"); continue; }
        if (!carrier.image) { decline("is empty"); continue; }
        auto keep = std::make_shared<GrayImage>(ctx.width, ctx.height, 255);
        bool erases = false;
        const Image& px = *carrier.image;
        for (int y = std::max(0, carrier.y); y < std::min(ctx.height, carrier.y + px.height()); y++)
            for (int x = std::max(0, carrier.x); x < std::min(ctx.width, carrier.x + px.width()); x++) {
                double coverage = px.pixel(x - carrier.x, y - carrier.y)[3] / 255.0 * carrier.opacity;
                if (carrier.mask) {
                    const int mx = x - carrier.maskX, my = y - carrier.maskY;
                    coverage *= (mx >= 0 && my >= 0 && mx < carrier.mask->width() && my < carrier.mask->height() ? carrier.mask->at(mx, my) : carrier.maskOutside) / 255.0;
                }
                const auto v = uint8_t(std::lround(std::clamp(1 - coverage, 0.0, 1.0) * 255));
                keep->at(x, y) = v;
                erases = erases || v < 255;
            }
        if (!erases) { decline("is empty"); continue; }
        Built wrapper = makeGroup(name + " (Erase)");
        wrapper.passThrough = false;
        for (size_t k = 0; k < i; k++) wrapper.children.push_back(std::move(out[k]));
        wrapper.mask = keep;
        out.erase(out.begin(), out.begin() + std::ptrdiff_t(i) + 1);
        out.insert(out.begin(), std::move(wrapper));
        i = 0;
        ctx.notes.push_back("Layer \"" + name + "\": Affinity's Erase blend mode became a mask on a new group holding the layers beneath it.");
    }
}

void buildLayers(Context& ctx, const ClassList& children, std::vector<Built>& out, bool foldEraseHere) {
    for (const auto& child : children) {
        if (!child) continue;
        if (++ctx.layerCount > maxLayers) fail("The Affinity document has an implausible number of layers.");
        const Class& node = *child;
        const std::string name = layerName(node);
        const std::string display = name.empty() ? "Layer" : name;
        const uint32_t type = node.type;
        const Class* bitmap = node.child(tag("Bitm"));

        const auto leaveOut = [&](const std::string& why) {
            ctx.notes.push_back("Layer \"" + display + "\" " + why + "; it was left out.");
            if (node.boolean(tag("Visi"), true)) ctx.leftOut++;
        };
        // Children of a content node live in its local space and clip to it (Affinity nests clipped layers inside
        // their base; NekoPhoto keeps them as clipped siblings above it). The 1.x generation (document versions
        // below 10) does not clip children to a base that painted nothing.
        const auto emitClipped = [&](bool baseMissing) {
            const ClassList* kids = node.list(tag("Chld"));
            if (!kids || kids->empty()) return;
            if (baseMissing && ctx.documentVersion >= 10) {
                ctx.notes.push_back("Layer \"" + display + "\": " + std::to_string(kids->size()) + " layer(s) clipped to it were left out with it.");
                return;
            }
            const Affine6 saved = ctx.transform;
            ctx.transform = nodeTransform(ctx, node);
            std::vector<Built> clipped;
            buildLayers(ctx, *kids, clipped, baseMissing);
            ctx.transform = saved;
            for (auto& layer : clipped) {
                if (!baseMissing) {
                    if (layer.group) ctx.notes.push_back("Layer \"" + layer.name + "\": a group clipped to a layer is shown unclipped.");
                    else layer.clipped = true;
                }
                out.push_back(std::move(layer));
            }
        };

        if (isArtboard(node)) { out.push_back(buildArtboard(ctx, node, display)); continue; }
        if (isAdjustmentOrFilter(type)) {
            leaveOut("is an Affinity adjustment or live filter, which is not read yet");
            emitClipped(true);
            continue;
        }
        if (type == tag("Comp")) {
            // A compound shape's boolean result is baked in its own 'crvs'; without it, its operands as a group.
            std::string why;
            if (auto path = curvePath(node))
                if (auto layer = drawVector(ctx, node, display, std::move(*path), nodeTransform(ctx, node), &why)) { out.push_back(std::move(*layer)); continue; }
            out.push_back(buildGroup(ctx, node, name));
            continue;
        }
        if (type == tag("Grup") || (node.field(tag("Chld")) && !bitmap && type != tag("TxtA") && type != tag("TxtF") && type != tag("PCrv") && type != tag("ShpN"))) {
            out.push_back(buildGroup(ctx, node, name));
            continue;
        }
        if (bitmap) {
            Straight pixels;
            bool approximate = false, maskPlane = false;
            std::string why;
            if (bitmap->type == tag("EmbR")) pixels = flattenEmbedded(ctx, *bitmap, display);
            else if (auto decoded = decodeBitmap(ctx.src, *bitmap, display, ctx.options, ctx.notes, &why)) {
                if (decoded->rgba) { pixels = decoded->rgba; approximate = decoded->approximateColor; }
                else maskPlane = true;
            }
            Affine6 m = nodeTransform(ctx, node);
            // A modern embedded document is centre-anchored: the translation places the middle of its canvas.
            if (bitmap->type == tag("EmbR") && pixels && ctx.documentVersion >= 20) {
                const double hw = pixels->width() / 2.0, hh = pixels->height() / 2.0;
                m[2] -= m[0] * hw + m[1] * hh;
                m[5] -= m[3] * hw + m[4] * hh;
            }
            std::optional<std::tuple<Straight, int, int>> placed;
            if (pixels) {
                if (auto t = translationOf(m)) placed = std::tuple{pixels, t->first, t->second};
                else placed = resampleAffine(*pixels, m);
            }
            if (!placed) {
                leaveOut(maskPlane ? "is an Affinity adjustment or live filter, which is not read yet"
                                   : pixels ? "has a degenerate transform" : why.empty() ? "could not be read" : why);
                emitClipped(true);
                continue;
            }
            if (approximate) ctx.notes.push_back("Layer \"" + display + "\": CMYK converted without a colour profile (approximate).");
            Built b;
            b.name = display;
            std::tie(b.image, b.x, b.y) = *placed;
            applyCommon(ctx, node, b, display);
            applyMasks(ctx, node, b, display);
            out.push_back(std::move(b));
            emitClipped(false);
            continue;
        }
        if (type == tag("TxtA") || type == tag("TxtF")) {
            leaveOut("is text, which is not read yet");
            emitClipped(true);
            continue;
        }
        if (type == tag("PCrv") || type == tag("ShpN")) {
            std::string why;
            auto base = buildShapeNode(ctx, node, display, &why);
            if (!base) { leaveOut(why); emitClipped(true); continue; }
            const ClassList* kids = node.list(tag("Chld"));
            if (!kids || kids->empty()) { out.push_back(std::move(*base)); continue; }
            // Children clip to the shape; when a group is among them (groups cannot clip), the shape and its children
            // go in an isolated group masked by the shape's outline instead.
            const Affine6 saved = ctx.transform;
            ctx.transform = nodeTransform(ctx, node);
            std::vector<Built> inner;
            buildLayers(ctx, *kids, inner, false);
            ctx.transform = saved;
            if (std::none_of(inner.begin(), inner.end(), [](const Built& l) { return l.group; })) {
                out.push_back(std::move(*base));
                for (auto& l : inner) { l.clipped = true; out.push_back(std::move(l)); }
                continue;
            }
            Built wrapper = makeGroup(display);
            wrapper.passThrough = false;
            wrapper.visible = base->visible;
            wrapper.opacity = base->opacity;
            wrapper.blend = base->blend;
            wrapper.mask = base->mask;
            wrapper.maskX = base->maskX;
            wrapper.maskY = base->maskY;
            wrapper.maskOutside = base->maskOutside;
            if (!wrapper.mask) {
                auto outline = node.type == tag("ShpN") ? shapePath(node, &why) : curvePath(node);
                if (outline) {
                    transformPath(*outline, nodeTransform(ctx, node));
                    for (auto& s : outline->subpaths) { s.group = 0; s.op = VectorPath::Op::Add; s.closed = true; }
                    wrapper.mask = canvasCoverage(ctx, *outline);
                    wrapper.maskOutside = 0;
                }
            }
            base->visible = true;
            base->opacity = 1;
            base->blend = BlendMode::Normal;
            base->mask = nullptr;
            wrapper.children.push_back(std::move(*base));
            for (auto& l : inner) wrapper.children.push_back(std::move(l));
            out.push_back(std::move(wrapper));
            continue;
        }
        leaveOut("is Affinity content that is not read yet");
    }
    if (std::any_of(out.begin(), out.end(), [](const Built& b) { return b.erase; })) foldErase(ctx, out, foldEraseHere);
}

bool anyPixels(const std::vector<Built>& layers) {
    for (const auto& l : layers) if ((l.image && !l.image->isEmpty()) || anyPixels(l.children)) return true;
    return false;
}

/// The Built tree into the document's flat, bottom-to-top list.
void emit(std::vector<Built>& layers, std::optional<Uuid> parent, Document& doc, std::vector<std::string>& notes, long long& total) {
    std::optional<Uuid> base;   // the nearest unclipped pixel layer below, for clipping
    for (auto& b : layers) {
        Layer layer;
        int lx = 0, ly = 0, lw = doc.width, lh = doc.height;
        if (b.group) {
            layer = Layer(b.name, doc.size());
            layer.isGroup = true;
            layer.passThrough = b.passThrough;
        } else {
            if (!b.image || b.image->isEmpty()) continue;
            premultiply(*b.image);
            const PixelBounds bounds = alphaBounds(*b.image);
            if (bounds.isEmpty()) { layer = Layer(b.name, doc.size()); }
            else {
                auto image = (bounds.x0 == 0 && bounds.y0 == 0 && bounds.x1 == b.image->width() && bounds.y1 == b.image->height())
                    ? b.image : cropImage(*b.image, bounds.x0, bounds.y0, bounds.x1 - bounds.x0, bounds.y1 - bounds.y0);
                total += (long long)image->width() * image->height();
                if (total > Document::projectPixelBudget) fail("The layers exceed the gigapixel a project may hold.");
                lx = b.x + bounds.x0;
                ly = b.y + bounds.y0;
                lw = image->width();
                lh = image->height();
                layer = Layer(Asset::make(image, b.name), Point(lx, ly));
            }
        }
        layer.name = b.name;
        layer.visible = b.visible;
        layer.opacity = b.opacity;
        layer.blendMode = b.blend;
        layer.parentId = parent;
        if (b.mask && plausibleSize(lw, lh)) {
            // Over the layer's own grid (a group's is the canvas); beyond the stored plane, its outside value.
            auto mask = std::make_shared<GrayImage>(lw, lh, b.maskOutside);
            for (int y = 0; y < lh; y++) {
                const int sy = ly + y - b.maskY;
                if (sy < 0 || sy >= b.mask->height()) continue;
                for (int x = 0; x < lw; x++) {
                    const int sx = lx + x - b.maskX;
                    if (sx >= 0 && sx < b.mask->width()) mask->at(x, y) = b.mask->at(sx, sy);
                }
            }
            LayerMask lm;
            lm.asset = MaskAsset::make(mask);
            layer.mask = lm;
        }
        if (b.clipped && !b.group) {
            if (base) layer.maskSourceId = base;
            else notes.push_back("Layer \"" + b.name + "\": clipped to a layer that was left out; shown unclipped.");
        } else if (!b.group) {
            base = layer.id;
        } else {
            base.reset();
        }
        const Uuid id = layer.id;
        doc.layers.push_back(std::move(layer));
        if (b.group) emit(b.children, id, doc, notes, total);
    }
}

/// Old files save a snapshot cache: a DyBm under the root's Snap subtree names a full-size PNG render.
std::shared_ptr<Image> snapshotRender(const Source& src, const affinity::Tree& tree, const PsdImportOptions& options) {
    const affinity::Field* snap = tree.root ? tree.root->field(tag("Snap")) : nullptr;
    if (!snap) return nullptr;
    std::vector<const Class*> pending;
    if (const auto* one = std::get_if<std::shared_ptr<Class>>(&snap->value)) pending.push_back(one->get());
    else if (const auto* list = std::get_if<ClassList>(&snap->value)) for (auto& c : *list) pending.push_back(c.get());
    Straight best;
    for (int guard = 0; !pending.empty() && guard < 4096; guard++) {
        const Class* node = pending.back();
        pending.pop_back();
        if (!node) continue;
        if (node->type == tag("DyBm")) {
            if (Straight s = decodeOriginal(src, *node, options); s && (!best || (long long)s->width() * s->height() > (long long)best->width() * best->height())) best = s;
            continue;
        }
        for (const auto& f : node->fields) {
            if (const auto* c = std::get_if<std::shared_ptr<Class>>(&f.value)) pending.push_back(c->get());
            else if (const auto* l = std::get_if<ClassList>(&f.value)) for (auto& c : *l) pending.push_back(c.get());
        }
    }
    return best;
}

/// A document of one layer: `image` (premultiplied) stretched over a `width` x `height` canvas.
Document singleLayer(const ImagePtr& image, int width, int height, const std::string& name) {
    Document doc(width, height);
    Layer layer(Asset::make(image, name), Point(0, 0));
    layer.transform.size = Size(width, height);
    doc.layers.push_back(std::move(layer));
    return doc;
}

Document buildDocument(const Source& src, const affinity::Tree& tree, std::vector<std::string>& notes, int embedDepth,
                       const PsdImportOptions& options, const ImagePtr& preview, bool* fellBack) {
    if (!tree.root) fail("The Affinity document tree is empty.");
    const Class* docNode = tree.root->child(tag("DocR"));
    if (!docNode) fail("The Affinity document has no document node.");
    const auto size = docNode->vector(tag("DfSz"));
    if (size.size() != 2) fail("The Affinity document has no canvas size.");
    int width = int(std::lround(size[0])), height = int(std::lround(size[1]));
    const ClassList* spreads = docNode->list(tag("Chld"));
    if (!spreads || spreads->empty() || !spreads->front()) fail("The Affinity document has no spread.");
    if (spreads->size() > 1) notes.push_back("The document has " + std::to_string(spreads->size()) + " pages or spreads; only the first was imported.");
    const Class& spread = *spreads->front();

    // The canvas: the spread's bounds (SprB) when sane; else the union of its artboards; else the page rectangle
    // (SpMd > PagR[0] > rctp), which follows canvas resizes where DfSz keeps the creation size.
    double originX = 0, originY = 0;
    {
        auto bounds = spread.vector(tag("SprB"));
        if (bounds.size() != 4 || !(bounds[2] > bounds[0]) || !(bounds[3] > bounds[1])) {
            bounds.clear();
            bool any = false;
            std::array<double, 4> box{};
            std::function<void(const Class&, const Affine6&, int)> walk = [&](const Class& n, const Affine6& parent, int depth) {
                const ClassList* kids = depth <= 64 ? n.list(tag("Chld")) : nullptr;
                if (!kids) return;
                for (const auto& k : *kids) {
                    if (!k) continue;
                    Affine6 m = parent;
                    if (auto own = affineOf(k->vector(tag("Xfrm")))) m = compose(parent, *own);
                    if (isArtboard(*k)) {
                        const auto b = k->vector(tag("ShpB"));
                        if (b.size() == 4)
                            for (auto [sx, sy] : {std::pair{b[0], b[1]}, {b[2], b[1]}, {b[0], b[3]}, {b[2], b[3]}}) {
                                const Point p = apply(m, sx, sy);
                                box = any ? std::array<double, 4>{std::min(box[0], p.x), std::min(box[1], p.y), std::max(box[2], p.x), std::max(box[3], p.y)}
                                          : std::array<double, 4>{p.x, p.y, p.x, p.y};
                                any = true;
                            }
                        continue;
                    }
                    walk(*k, m, depth + 1);
                }
            };
            walk(spread, identity(), 0);
            if (any) bounds.assign(box.begin(), box.end());
        }
        if (bounds.size() != 4)
            if (const Class* meta = spread.child(tag("SpMd")))
                if (const ClassList* pages = meta->list(tag("PagR")); pages && !pages->empty() && pages->front()) {
                    auto page = pages->front()->vector(tag("rctp"));
                    if (page.size() == 4 && page[2] > page[0] && page[3] > page[1]) bounds = page;
                }
        if (bounds.size() == 4) {
            const int w = int(std::lround(bounds[2] - bounds[0])), h = int(std::lround(bounds[3] - bounds[1]));
            if (plausibleSize(w, h)) { width = w; height = h; originX = bounds[0]; originY = bounds[1]; }
        }
    }
    if (!plausibleSize(width, height)) fail("The canvas exceeds the 100-megapixel budget.");

    Context ctx{src, options, notes, width, height, embedDepth};
    ctx.documentVersion = tree.documentVersion;
    ctx.transform[2] = -originX;
    ctx.transform[5] = -originY;
    std::vector<Built> built;
    if (const ClassList* layers = spread.list(tag("Chld"))) buildLayers(ctx, *layers, built);

    Document doc(width, height);
    if (const Class* units = tree.root->child(tag("UVCn"))) {
        const double ppi = units->number(tag("UPPI"), 0.0);
        if (ppi >= 1 && ppi <= 10000) doc.resolution = ppi;
    }
    if (!anyPixels(built)) {
        // Nothing decoded: a flat render beats a blank canvas. Old files' saved snapshot first, then the preview.
        if (fellBack) *fellBack = true;
        if (Straight snapshot = snapshotRender(src, tree, options)) {
            premultiply(*snapshot);
            notes.push_back("None of the layers could be read; the document's saved snapshot render was imported as one layer.");
            return singleLayer(snapshot, width, height, "Affinity snapshot");
        }
        if (preview) {
            notes.push_back("None of the layers could be read; the embedded preview (" + std::to_string(preview->width()) + " x " +
                            std::to_string(preview->height()) + ") was imported as one layer.");
            return singleLayer(preview, width, height, "Affinity preview");
        }
        fail("The Affinity document has no layers this reader can use.");
    }
    // Unless the spread is transparent (SprT), Affinity paints its background colour (BgrC, white by default)
    // behind every layer.
    if (!spread.boolean(tag("SprT"), false)) {
        std::array<float, 4> c{1, 1, 1, 1};
        if (auto stored = readColor(spread.child(tag("BgrC")))) c = *stored;
        if (c[3] > 0) {
            auto fill = std::make_shared<Image>(width, height);
            const uint8_t a = uint8_t(std::lround(c[3] * 255));
            fill->fill(uint8_t(std::lround(c[0] * c[3] * 255)), uint8_t(std::lround(c[1] * c[3] * 255)), uint8_t(std::lround(c[2] * c[3] * 255)), a);
            doc.layers.emplace_back(Asset::make(fill, "Background"), Point(0, 0));
        }
    }
    long long total = (long long)width * height;
    emit(built, std::nullopt, doc, notes, total);
    if (ctx.leftOut > 0 && preview && embedDepth == 0) {
        // What was left out still shows in Affinity's own preview: kept hidden on top, for reference.
        Layer ref(Asset::make(preview, "Affinity preview (reference)"), Point(0, 0));
        ref.transform.size = Size(width, height);
        ref.visible = false;
        doc.layers.push_back(std::move(ref));
        notes.push_back("Affinity's own preview of the whole document is the hidden top layer, for comparing what was left out.");
    }
    return doc;
}

Document readContainer(std::span<const uint8_t> bytes, std::vector<std::string>& notes, int embedDepth, const PsdImportOptions& options, ImagePtr* previewOut) {
    if (bytes.size() < 4 || bytes[0] != 0x00 || bytes[1] != 0xFF || bytes[2] != 0x4B || bytes[3] != 0x41) fail("Not an Affinity document.");
    const Container container = parseContainer(bytes);
    if (container.version > newestVerifiedVersion)
        notes.push_back("The file was saved by a newer Affinity (container version " + std::to_string(container.version) + "); the import may be incomplete.");
    ImagePtr preview;
    try { preview = extractPreview(bytes, container); } catch (const std::exception&) {}
    if (previewOut) *previewOut = preview;
    const Source src{bytes, container};
    auto docStream = container.streams.find("doc.dat");
    if (docStream != container.streams.end()) {
        std::string why;
        try {
            const auto treeBytes = extractStream(bytes, docStream->second, "doc.dat", &notes);
            const affinity::Tree tree = affinity::parseTree(treeBytes);
            return buildDocument(src, tree, notes, embedDepth, options, preview, nullptr);
        } catch (const std::exception& e) {
            why = e.what();
        }
        if (!preview || embedDepth > 0) fail(why);
        notes.push_back("The layers could not be read (" + why + "); the embedded preview was imported instead.");
    } else if (!preview) {
        fail("The Affinity document has no document stream.");
    }
    return singleLayer(preview, preview->width(), preview->height(), "Affinity preview");
}

} // namespace

std::optional<PsdImport> importAffinityBytes(const std::vector<uint8_t>& file, std::string* error, const PsdImportOptions& options) {
    try {
        PsdImport result;
        ImagePtr preview;
        result.document = readContainer(file, result.notes, 0, options, &preview);
        result.composite = preview;
        result.realComposite = false;
        if (result.document.layers.empty()) { if (error) *error = "The file holds no layers this reader can use."; return std::nullopt; }
        return result;
    } catch (const std::bad_alloc&) {
        if (error) *error = "The Affinity document needs more memory than is available.";
    } catch (const std::exception& e) {
        if (error) *error = e.what();
    }
    return std::nullopt;
}

std::optional<PsdImport> importAffinity(const std::string& path, std::string* error, const PsdImportOptions& options) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { if (error) *error = "The file could not be opened."; return std::nullopt; }
    const std::streamoff size = in.tellg();
    if (size < 0 || uint64_t(size) > (uint64_t(4) << 30)) { if (error) *error = "The file is too large."; return std::nullopt; }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) { if (error) *error = "The file could not be read."; return std::nullopt; }
    return importAffinityBytes(bytes, error, options);
}

} // namespace compositor
