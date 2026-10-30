// What the parts of the Affinity reader share: the container (affinity_container.cpp: the stream table, a stream's
// bytes, the preview), the pixels (affinity_pixels.cpp: tile planes and placed originals decoded, the affine
// resampler), the vector shapes (affinity_shapes.cpp: colours, curves and parametric shapes as paths) and the build
// (affinity.cpp: the object tree into layers, text and the document). Internal to the core.
#pragma once
#include "nekophoto/affinity.h"
#include "affinity_tree.h"
#include "nekophoto/document.h"
#include "nekophoto/psd.h"
#include "nekophoto/vectormask.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace nekophoto::affinity_read {

using affinity::Class;
using affinity::Reader;
using affinity::tag;
using ClassList = std::vector<std::shared_ptr<Class>>;
using Affine6 = std::array<double, 6>;   // [a, b, tx, c, d, ty]: x' = a x + b y + tx, y' = c x + d y + ty

inline constexpr int tileSize = 256;

[[noreturn]] inline void fail(const std::string& what) { throw std::runtime_error(what); }

inline Affine6 identity() { return {1, 0, 0, 0, 1, 0}; }
inline Affine6 compose(const Affine6& p, const Affine6& c) {
    return {p[0] * c[0] + p[1] * c[3], p[0] * c[1] + p[1] * c[4], p[0] * c[2] + p[1] * c[5] + p[2],
            p[3] * c[0] + p[4] * c[3], p[3] * c[1] + p[4] * c[4], p[3] * c[2] + p[4] * c[5] + p[5]};
}
inline Point apply(const Affine6& m, double x, double y) { return {m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]}; }
inline std::optional<Affine6> affineOf(const std::vector<double>& v) {
    if (v.size() != 6) return std::nullopt;
    for (double d : v) if (!std::isfinite(d)) return std::nullopt;
    return Affine6{v[0], v[1], v[2], v[3], v[4], v[5]};
}

// ---- the container (affinity_container.cpp) ------------------------------------------------------------------

struct Stream { uint64_t offset = 0, size = 0, compressed = 0; uint32_t crc = 0; uint8_t compression = 0; };

struct Container {
    uint16_t version = 0;
    uint64_t thumbnailOffset = 0;
    std::unordered_map<std::string, Stream> streams;   // the newest revision of each
};

Container parseContainer(std::span<const uint8_t> bytes);
/// A stream's bytes, decompressed and its predictors undone; a failed checksum is noted.
std::vector<uint8_t> extractStream(std::span<const uint8_t> bytes, const Stream& s, const std::string& name, std::vector<std::string>* notes);
/// The embedded preview (premultiplied), from the PNG at the thumbnail offset.
std::shared_ptr<Image> extractPreview(std::span<const uint8_t> bytes, const Container& container);

// ---- the pixels (affinity_pixels.cpp) ------------------------------------------------------------------------

/// Straight-alpha RGBA through the build; premultiplied only when a layer is made.
using Straight = std::shared_ptr<Image>;
using Plane = std::shared_ptr<GrayImage>;

inline bool plausibleSize(int64_t w, int64_t h) { return w > 0 && h > 0 && w <= maxImageSide && h <= maxImageSide && w * h <= Document::pixelBudget; }
Straight straightCopy(const Image& premultiplied);
/// `source` through the affine into an axis-aligned raster at the returned origin (bilinear, alpha-weighted, with
/// box reductions first for strong minification). `Img` is Image (straight RGBA) or GrayImage (a mask, edge-clamped).
template <typename Img>
std::optional<std::tuple<std::shared_ptr<Img>, int, int>> resampleAffine(const Img& source, const Affine6& m);

inline uint32_t tagOf(const std::string& s) {
    if (s.size() != 4) return 0;
    return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint32_t(uint8_t(s[3]));
}

struct Source { std::span<const uint8_t> bytes; const Container& container; };

struct Decoded {
    Straight rgba;   // colour formats
    Plane mask;      // M8 / M16
    bool approximateColor = false;
};

/// A placed original: the untouched image file a bitmap's Bckg stream holds, decoded to straight RGBA.
Straight decodeOriginal(const Source& src, const Class& dybm, const PsdImportOptions& options);
/// A bitmap (DyBm) decoded from its tile planes; none, with `why`, when it cannot be.
std::optional<Decoded> decodeBitmap(const Source& src, const Class& dybm, const std::string& name, const PsdImportOptions& options,
                                    std::vector<std::string>& notes, std::string* why);

// ---- colours and shapes (affinity_shapes.cpp) ----------------------------------------------------------------

/// An Affinity colour class (RGBA, HSLA or CMYK floats in `_col`) as straight RGBA 0..1.
std::optional<std::array<float, 4>> readColor(const Class* color);
const Class* firstClass(const Class& node, uint32_t t);
/// A PCrv's poly-curve as a path.
std::optional<VectorPath> curvePath(const Class& node);
/// A parametric shape (the ShpN's Shpe record) as a path; none, with `why`, for a kind not drawn here.
std::optional<VectorPath> shapePath(const Class& node, std::string* why);
void transformPath(VectorPath& path, const Affine6& m);
bool isArtboard(const Class& node);
/// Adjustments (the *RA family) and live filters (FlRN): their bitmap is a mask, not content.
bool isAdjustmentOrFilter(uint32_t type);

} // namespace nekophoto::affinity_read
