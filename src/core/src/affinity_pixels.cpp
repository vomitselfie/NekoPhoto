// The Affinity reader's pixels (affinity_read.h): straight images, EXIF orientation, the affine resampler, and a
// bitmap's tile planes decoded at every format (RGBA, grey, CMYK, Lab, masks, float). Ported from Patchy with the rest
// (affinity.cpp).
#include "affinity_read.h"
#include "nekophoto/colour.h"
#include "nekophoto/png.h"
#include "nekophoto/render.h"
#include "nekophoto/vectormask.h"
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

#ifdef NEKOPHOTO_HAVE_ZSTD
#include <zstd.h>
#endif

namespace nekophoto::affinity_read {

// ------------------------------------------------------------------------------------------------ images

Straight straightCopy(const Image& premultiplied) {
    auto out = std::make_shared<Image>(premultiplied);
    unpremultiply(*out);
    return out;
}

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

template std::optional<std::tuple<std::shared_ptr<Image>, int, int>> resampleAffine<Image>(const Image&, const Affine6&);
template std::optional<std::tuple<std::shared_ptr<GrayImage>, int, int>> resampleAffine<GrayImage>(const GrayImage&, const Affine6&);

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
    const uint64_t inflateBudget = uint64_t(out.bytes.size()) + (uint64_t(1) << 20);
    uint64_t inflated = 0;
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
            // A tile's stream holds one tile at most, and the plane decompresses no more than it holds (plus slack):
            // many tiles naming one big stream would otherwise inflate it again for each.
            if (stream->second.size > uint64_t(tileSize) * tileSize) break;
            if (inflated + stream->second.size > inflateBudget) return PlaneStatus::Invalid;
            inflated += stream->second.size;
            std::vector<uint8_t> tile;
            try { tile = extractStream(src.bytes, stream->second, embedded->data, nullptr); } catch (const std::exception&) { break; }
            if (tile.size() == size_t(tileSize) * tileSize) {
                for (size_t row = 0; row < tileSize; row++) std::memcpy(out.bytes.data() + (ty + row) * out.widthBytes + tx, tile.data() + row * tileSize, tileSize);
                break;
            }
            // A partial tile: only a sub-rectangle, [x, y, w, h] or [x0, y0, x1, y1], told apart by the stream size.
            const auto rect = b.vector(tag("Rect"));
            if (rect.size() != 4) break;
            // Clamped before converting: the rectangle's doubles come from the file.
            auto whole = [](double v) { return std::isfinite(v) ? int64_t(std::clamp(v, -1e9, 1e9)) : int64_t(-1); };
            const int64_t rx = whole(rect[0]), ry = whole(rect[1]);
            int64_t rw = whole(rect[2]), rh = whole(rect[3]);
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

} // namespace nekophoto::affinity_read
