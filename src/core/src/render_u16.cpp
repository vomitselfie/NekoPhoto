// The renderer's 16-bit primitives (docs/high-bit-depth-plan.md, P2): mask coverage, drawing a layer and resampling
// at 0..32768, and the entry points that render a 16-bit document (written once for the deep depths in
// render_deep.inc). In a file of their own so the 8-bit renderer (render.cpp) compiles as it did.
#include "compositor/render.h"
#include "render_plan.h"
#include "render_deep.inc"
#include "compositor/blend.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/parallel.h"
#include "compositor/resample.h"
#include "compositor/warp.h"
#include "compositor/modetransform.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

namespace compositor {

namespace {

/// The mip level and per-axis scale for drawing `pixels` source pixels across `size` output pixels (as render.cpp).
struct MipChoice { int level; double factor; };
MipChoice mipFor(Sampling sampling, double outputWidth, double outputHeight, int pixelWidth, int pixelHeight) {
    if (sampling == Sampling::Nearest) return {0, 1};
    double fx = outputWidth / std::max(1, pixelWidth), fy = outputHeight / std::max(1, pixelHeight);
    int level = MipCache::levelFor(std::min(fx, fy), sampling == Sampling::High);
    return {level, std::ldexp(1.0, level)};
}

// Per-output-pixel iteration over the region a transform covers (as render.cpp).
struct Mapping {
    Affine documentToOutput;
    Affine outputToPixel;
    Rect outputRect;
};

Mapping mappingFor(const LayerTransform& transform, int pixelWidth, int pixelHeight, const Rect& region, double scale, int outWidth, int outHeight, double margin) {
    Mapping m;
    m.documentToOutput = Affine::translation(-region.x, -region.y).concatenating(Affine::scaling(scale, scale));
    Affine pixelToDocument = transform.pixelToDocument(pixelWidth, pixelHeight);
    Affine pixelToOutput = pixelToDocument.concatenating(m.documentToOutput);
    m.outputToPixel = pixelToOutput.inverted();
    Rect bounds = pixelToOutput.mapBounds(Rect(0, 0, pixelWidth, pixelHeight)).insetBy(-margin, -margin).integral();
    m.outputRect = bounds.intersection(Rect(0, 0, outWidth, outHeight));
    return m;
}

} // namespace

// ---- Mask coverage, drawing a layer, resampling -------------------------------------------------------

namespace {

inline void sampleNearest16(const Image16& image, double x, double y, uint16_t out[4]) {
    const int ix = clamp(int(std::floor(x)), 0, image.width() - 1), iy = clamp(int(std::floor(y)), 0, image.height() - 1);
    std::memcpy(out, image.pixel(ix, iy), 4 * sizeof(uint16_t));
}

inline void samplePixels16(Sampling sampling, const Image16& image, double x, double y, uint16_t out[4]) {
    if (sampling == Sampling::High) sampleBicubic(image, x, y, out);
    else sampleBilinear(image, x, y, out);
}

inline int sampleGrayNearest16(const Gray16& image, double x, double y) {
    const int ix = clamp(int(std::floor(x)), 0, image.width() - 1), iy = clamp(int(std::floor(y)), 0, image.height() - 1);
    return image.at(ix, iy);
}

constexpr float inv16 = 1.0f / 32768.0f;

void sampleMaskCoverage16(const Gray16& mask, const Gray16Ptr& owner, const LayerTransform& transform, const Rect& region, double scale, uint16_t outside, Gray16& out, bool multiply) {
    const int mw = mask.width(), mh = mask.height();
    if (!multiply) out.fill(outside);
    if (mw <= 0 || mh <= 0) return;
    Mapping m = mappingFor(transform, mw, mh, region, scale, out.width(), out.height(), 1);
    const double sx = std::hypot(m.outputToPixel.a, m.outputToPixel.b), sy = std::hypot(m.outputToPixel.c, m.outputToPixel.d);
    MipChoice mip = mipFor(transform.sampling, transform.size.width * scale, transform.size.height * scale, mw, mh);
    std::shared_ptr<const Gray16> reduced;
    if (mip.level > 0) reduced = owner && owner.get() == &mask ? MipCache::shared().level(owner, mip.level) : reduceGray(mask, mip.level);
    const Gray16& src = reduced ? *reduced : mask;
    const double factor = mip.factor;
    const bool nearest = transform.sampling == Sampling::Nearest;
    if (multiply && outside != one16) {
        for (int y = 0; y < out.height(); y++) {
            uint16_t* row = out.row(y);
            for (int x = 0; x < out.width(); x++) {
                const bool inside = x >= m.outputRect.minX() && x < m.outputRect.maxX() && y >= m.outputRect.minY() && y < m.outputRect.maxY();
                if (!inside) row[x] = uint16_t(mul15(row[x], outside));
            }
        }
    }
    parallelRows(int(m.outputRect.minY()), int(m.outputRect.maxY()), [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            uint16_t* row = out.row(y);
            Point p = m.outputToPixel.apply({m.outputRect.minX() + 0.5, y + 0.5});
            const Point dp = m.outputToPixel.applyVector({1, 0});
            for (int x = int(m.outputRect.minX()); x < int(m.outputRect.maxX()); x++, p = p + dp) {
                float value;
                const bool inside = p.x >= 0 && p.x < mw && p.y >= 0 && p.y < mh;
                if (nearest) value = inside ? float(sampleGrayNearest16(src, p.x, p.y)) : float(outside);
                else {
                    const double ex = std::min(p.x, mw - p.x) / std::max(1e-9, sx), ey = std::min(p.y, mh - p.y) / std::max(1e-9, sy);
                    const float edge = float(clamp(std::min(ex, ey) + 0.5, 0.0, 1.0));
                    value = float(sampleGrayBilinear(src, p.x / factor, p.y / factor)) * edge + float(outside) * (1 - edge);
                }
                const uint16_t v = uint16_t(clamp(value + 0.5f, 0.0f, 32768.0f));
                row[x] = multiply ? uint16_t(mul15(row[x], v)) : v;
            }
        }
    });
}

} // namespace

void sampleMaskCoverage(const Gray16& mask, const LayerTransform& transform, const Rect& region, double scale, uint16_t outside, Gray16& out, bool multiply) {
    sampleMaskCoverage16(mask, nullptr, transform, region, scale, outside, out, multiply);
}
void sampleMaskCoverage(const Gray16Ptr& mask, const LayerTransform& transform, const Rect& region, double scale, uint16_t outside, Gray16& out, bool multiply) {
    if (mask) sampleMaskCoverage16(*mask, mask, transform, region, scale, outside, out, multiply);
}

void drawLayer(const DrawParams16& params, const Rect& region, double scale, const Gray16* coverage, Image16& out) {
    if (!params.image || params.image->isEmpty() || out.isEmpty()) return;
    const Image16& full = *params.image;
    const int pw = full.width(), ph = full.height();
    Mapping m = mappingFor(params.transform, pw, ph, region, scale, out.width(), out.height(), 1);
    if (m.outputRect.isEmpty()) return;
    const bool nearest = params.transform.sampling == Sampling::Nearest;
    MipChoice mip = mipFor(params.transform.sampling, params.transform.size.width * scale, params.transform.size.height * scale, pw, ph);
    Image16Ptr source = MipCache::shared().level(params.image, mip.level);
    const double factor = mip.factor;
    const double sx = std::hypot(m.outputToPixel.a, m.outputToPixel.b), sy = std::hypot(m.outputToPixel.c, m.outputToPixel.d);

    const Gray16* mask = nullptr;
    Gray16Ptr maskHold;
    std::optional<LayerTransform> maskPlacement;
    if (params.mask && params.mask->enabled) {
        maskHold = params.maskImage ? params.maskImage : params.mask->asset.image.u16();
        mask = maskHold.get();
        maskPlacement = params.maskPlacement ? params.maskPlacement : params.mask->placement;
        if (maskPlacement && maskPlacement->samePlacement(params.layerTransformForMask)) maskPlacement.reset();
    }
    std::shared_ptr<Gray16> placedMask;
    if (mask && maskPlacement) {
        const uint16_t background = widen8(params.mask->asset.thumbnail ? LayerMask::background(*params.mask->asset.thumbnail) : 255);
        placedMask = std::make_shared<Gray16>(out.width(), out.height(), background);
        sampleMaskCoverage(*mask, *maskPlacement, region, scale, background, *placedMask, false);
    }
    const double maskScaleX = mask ? double(mask->width()) / pw : 1, maskScaleY = mask ? double(mask->height()) / ph : 1;
    const float opacity = float(clamp(params.opacity, 0.0, 1.0));
    const double invFactor = 1.0 / factor;
    const int xBegin = int(m.outputRect.minX()), xEnd = int(m.outputRect.maxX());
    const Point dp = m.outputToPixel.applyVector({1, 0});
    const BlendMode mode = params.mode;
    const bool dissolve = mode == BlendMode::Dissolve;

    // On the output grid at whole pixels: rows of the layer straight over the destination, one span per row.
    const Affine& o2p = m.outputToPixel;
    const bool onGrid = !dissolve && factor == 1 && std::fabs(o2p.a - 1) < 1e-9 && std::fabs(o2p.b) < 1e-9
        && std::fabs(o2p.c) < 1e-9 && std::fabs(o2p.d - 1) < 1e-9 && std::fabs(o2p.tx - std::round(o2p.tx)) < 1e-9 && std::fabs(o2p.ty - std::round(o2p.ty)) < 1e-9
        && (!mask || placedMask || (maskScaleX == 1 && maskScaleY == 1));
    if (onGrid) {
        const int offsetX = int(std::round(o2p.tx)), offsetY = int(std::round(o2p.ty));
        const int spanBegin = std::max(xBegin, -offsetX), spanEnd = std::min(xEnd, pw - offsetX);
        if (spanEnd <= spanBegin) return;
        parallelRows(int(m.outputRect.minY()), int(m.outputRect.maxY()), [&](int ya, int yb) {
            std::vector<uint32_t> steps(size_t(spanEnd - spanBegin));
            for (int y = ya; y < yb; y++) {
                const int py = y + offsetY;
                if (py < 0 || py >= ph) continue;
                const uint16_t* covRow = coverage ? coverage->row(y) : nullptr;
                const uint16_t* placedRow = placedMask ? placedMask->row(y) : nullptr;
                const uint16_t* maskRow = mask && !placedMask ? mask->row(py) : nullptr;
                for (int x = spanBegin; x < spanEnd; x++) {
                    float cov = opacity;
                    if (covRow) cov *= covRow[x] * inv16;
                    if (placedRow) cov *= placedRow[x] * inv16;
                    else if (maskRow) cov *= maskRow[x + offsetX] * inv16;
                    steps[size_t(x - spanBegin)] = coverageSteps16(cov);
                }
                compositeSpan16(mode, source->pixel(spanBegin + offsetX, py), steps.data(), out.pixel(spanBegin, y), spanEnd - spanBegin);
            }
        });
        return;
    }

    parallelRows(int(m.outputRect.minY()), int(m.outputRect.maxY()), [&](int ya, int yb) {
        std::vector<uint16_t> samples(size_t(xEnd - xBegin) * 4);
        std::vector<uint32_t> steps(size_t(xEnd - xBegin));
        for (int y = ya; y < yb; y++) {
            const uint16_t* covRow = coverage ? coverage->row(y) : nullptr;
            const uint16_t* placedRow = placedMask ? placedMask->row(y) : nullptr;
            Point p = m.outputToPixel.apply({xBegin + 0.5, y + 0.5});
            bool any = false;
            for (int x = xBegin; x < xEnd; x++, p = p + dp) {
                const size_t i = size_t(x - xBegin);
                steps[i] = 0;
                float edge;
                if (nearest) {
                    if (p.x < 0 || p.x >= pw || p.y < 0 || p.y >= ph) continue;
                    edge = 1;
                } else {
                    const double ex = std::min(p.x, pw - p.x) / std::max(1e-9, sx), ey = std::min(p.y, ph - p.y) / std::max(1e-9, sy);
                    edge = float(clamp(std::min(ex, ey) + 0.5, 0.0, 1.0));
                    if (edge <= 0) continue;
                }
                float cov = edge * opacity;
                if (covRow) cov *= covRow[x] * inv16;
                if (mask) {
                    if (placedRow) cov *= placedRow[x] * inv16;
                    else cov *= float(nearest ? sampleGrayNearest16(*mask, p.x * maskScaleX, p.y * maskScaleY) : sampleGrayBilinear(*mask, p.x * maskScaleX, p.y * maskScaleY)) * inv16;
                }
                if (cov <= 0) continue;
                uint16_t* src = &samples[i * 4];
                if (nearest) sampleNearest16(*source, p.x, p.y, src);
                else samplePixels16(params.transform.sampling, *source, p.x * invFactor, p.y * invFactor, src);
                if (!src[3]) continue;
                if (dissolve) { compositePixelAt16(mode, src, cov, out.pixel(x, y), docX(region, scale, x), docY(region, scale, y)); continue; }
                steps[i] = coverageSteps16(cov);
                any = any || steps[i];
            }
            if (any) compositeSpan16(mode, samples.data(), steps.data(), out.pixel(xBegin, y), xEnd - xBegin);
        }
    });
}

std::shared_ptr<Image16> resampleLayer(const Image16Ptr& image, const LayerTransform& transform, const LayerTransform& target, int width, int height) {
    auto out = std::make_shared<Image16>(std::max(0, width), std::max(0, height));
    if (!image || image->isEmpty() || width <= 0 || height <= 0) return out;
    const Affine map = target.pixelToDocument(width, height).concatenating(transform.pixelToDocument(image->width(), image->height()).inverted());
    const bool nearest = transform.sampling == Sampling::Nearest;
    const double sx = std::hypot(map.a, map.b), sy = std::hypot(map.c, map.d);
    const int level = nearest ? 0 : MipCache::levelFor(1.0 / std::max(sx, sy), transform.sampling == Sampling::High);
    Image16Ptr source = MipCache::shared().level(image, level);
    const double factor = std::ldexp(1.0, level);
    const int pw = image->width(), ph = image->height();
    parallelRows(0, height, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            uint16_t* row = out->row(y);
            Point p = map.apply({0.5, y + 0.5});
            const Point dp = map.applyVector({1, 0});
            for (int x = 0; x < width; x++, p = p + dp, row += 4) {
                if (nearest) {
                    if (p.x < 0 || p.x >= pw || p.y < 0 || p.y >= ph) continue;
                    sampleNearest16(*source, p.x, p.y, row);
                } else {
                    const double ex = std::min(p.x, pw - p.x) / std::max(1e-9, sx), ey = std::min(p.y, ph - p.y) / std::max(1e-9, sy);
                    const uint32_t edge = uint32_t(clamp(std::min(ex, ey) + 0.5, 0.0, 1.0) * 32768 + 0.5);
                    if (edge == 0) continue;
                    uint16_t s[4];
                    samplePixels16(transform.sampling, *source, p.x / factor, p.y / factor, s);
                    for (int c = 0; c < 4; c++) row[c] = uint16_t(mul15(s[c], edge));
                }
            }
        }
    });
    return out;
}

std::shared_ptr<Gray16> resampleMask(const Gray16& mask, const LayerTransform& transform, const LayerTransform& target, int width, int height, uint16_t outside) {
    auto out = std::make_shared<Gray16>(std::max(0, width), std::max(0, height), outside);
    if (mask.isEmpty() || width <= 0 || height <= 0) return out;
    const Affine map = target.pixelToDocument(width, height).concatenating(transform.pixelToDocument(mask.width(), mask.height()).inverted());
    const double sx = std::hypot(map.a, map.b), sy = std::hypot(map.c, map.d);
    const int level = MipCache::levelFor(1.0 / std::max(sx, sy));
    std::shared_ptr<const Gray16> reduced = level > 0 ? reduceGray(mask, level) : nullptr;
    const Gray16& src = reduced ? *reduced : mask;
    const double factor = std::ldexp(1.0, level);
    const int mw = mask.width(), mh = mask.height();
    parallelRows(0, height, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            uint16_t* row = out->row(y);
            Point p = map.apply({0.5, y + 0.5});
            const Point dp = map.applyVector({1, 0});
            for (int x = 0; x < width; x++, p = p + dp) {
                const double ex = std::min(p.x, mw - p.x) / std::max(1e-9, sx), ey = std::min(p.y, mh - p.y) / std::max(1e-9, sy);
                const float edge = float(clamp(std::min(ex, ey) + 0.5, 0.0, 1.0));
                const float in = float(sampleGrayBilinear(src, p.x / factor, p.y / factor));
                row[x] = uint16_t(clamp(in * edge + float(outside) * (1 - edge) + 0.5f, 0.0f, 32768.0f));
            }
        }
    });
    return out;
}


// ---- Rendering a 16-bit document ------------------------------------------------------------------------------

void renderForDisplay16(const RenderPlan& plan, const Rect& region, double scale, Image& out, RenderCache* cache, uint64_t version, bool clear, const ColorTransform* display) {
    renderForDisplayDeep<SampleType::U16>(plan, region, scale, out, cache, version, clear, display);
}

void render16(const Document& document, const RenderOptions& options, Image16& out, const Overrides* overrides, RenderCache* cache) {
    // A CMYK or Lab document renders at its own layout, then converts to sRGB (render_modes.h).
    if (document.colorMode != ColorMode::RGB) { renderModeAsRgb16(document, options, out, overrides, cache); return; }
    renderDeep<SampleType::U16>(document, options, out, overrides, cache);
}

std::shared_ptr<Image16> renderFlattened16(const Document& document) {
    auto out = std::make_shared<Image16>(document.width, document.height);
    render16(document, RenderOptions(), *out);
    return out;
}


/// resizeDocument for a 16-bit document (render.cpp hands it over): the same steps, 16-bit layers and masks resampled
/// at 16 bits, and byte budgets.
bool resizeDocument16(Document& document, int width, int height, double resolution, Sampling sampling) {
    if (!Document::canCreate(width, height, document.sampleType)) return false;
    double sx = double(width) / document.width, sy = double(height) / document.height;
    if (document.width == width && document.height == height) { document.resolution = resolution; return true; }
    Document out = document;
    out.width = width; out.height = height; out.resolution = resolution;
    out.selection.reset();
    long long used = 0, usedMask = 0;
    Affine scale = Affine::scaling(sx, sy);
    for (auto& layer : out.layers) {
        // A smart object keeps its source: its placement scales instead (where no shear arises), so resizing
        // never resamples it into pixels.
        if (layer.isLiveSmartObject() && (layer.transform.rotation == 0 || std::abs(sx - sy) < 1e-9)) {
            const LayerTransform old = layer.transform;
            layer.transform = old.placing(old.unitToDocument().concatenating(scale));
            layer.transform.sampling = old.sampling;
            if (layer.mask && layer.mask->asset.image) {
                const LayerTransform placement = layer.mask->placement.value_or(old);
                layer.mask->placement = placement.placing(placement.unitToDocument().concatenating(scale));
            }
            continue;
        }
        // The scaled corners' axis-aligned box: nonuniform scaling of a rotated rectangle adds shear that
        // width/height/angle cannot hold, so each layer is rasterised into its box.
        auto c = layer.transform.corners();
        double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
        for (auto& p : c) { minX = std::min(minX, p.x * sx); minY = std::min(minY, p.y * sy); maxX = std::max(maxX, p.x * sx); maxY = std::max(maxY, p.y * sy); }
        double left = std::floor(minX), top = std::floor(minY);
        int w = std::max(1, int(std::ceil(maxX) - left)), h = std::max(1, int(std::ceil(maxY) - top));
        LayerTransform box(Point(left, top), Size(w, h));
        box.sampling = sampling;
        if (!box.isValid()) return false;
        if (layer.asset && layer.asset->image.u8()) {
            if (w > 30000 || h > 30000 || (long long)w * h > Document::pixelBudget || (long long)w * h > Document::projectPixelBudget - used) return false;
            used += (long long)w * h;
            // Shear can't be expressed as a LayerTransform, so resample through the scaled corner mapping directly.
            Corners corners;
            for (size_t i = 0; i < 4; i++) corners[i] = {c[i].x * sx, c[i].y * sy};
            LayerTransform sampled = layer.transform;
            sampled.sampling = sampling;   // the dialog's choice, not the layer's own
            auto warped = warpImage(layer.asset->image.u8(), sampled, corners, 0);
            if (!warped) return false;
            layer.asset = Asset::make(warped->image, layer.name);
            layer.shapeImage.reset();
            box = warped->transform;
            box.sampling = sampling;
        } else if (layer.asset && layer.asset->image.u16() && layer.asset->image.channels() == 5) {
            // 16-bit CMYK: the same warp over all five samples (modetransform.h).
            if (w > 30000 || h > 30000 || (long long)w * h > document.imagePixelBudget() || (long long)w * h > document.projectPixelBudgetAt() - used) return false;
            used += (long long)w * h;
            Corners corners;
            for (size_t i = 0; i < 4; i++) corners[i] = {c[i].x * sx, c[i].y * sy};
            LayerTransform sampled = layer.transform;
            sampled.sampling = sampling;
            auto warped = warpImageAny(layer.asset->image, sampled, corners, 0);
            if (!warped) return false;
            layer.asset = Asset::makeAny(warped->image, layer.name);
            layer.shapeImage.reset();
            box = warped->transform;
            box.sampling = sampling;
        } else if (layer.asset && layer.asset->image.u16()) {
            // At 16 bits: the same warp through the scaled corners, and a budget in bytes.
            if (w > 30000 || h > 30000 || (long long)w * h > document.imagePixelBudget() || (long long)w * h > document.projectPixelBudgetAt() - used) return false;
            used += (long long)w * h;
            Corners corners;
            for (size_t i = 0; i < 4; i++) corners[i] = {c[i].x * sx, c[i].y * sy};
            LayerTransform sampled = layer.transform;
            sampled.sampling = sampling;
            auto warped = warpImage(layer.asset->image.u16(), sampled, corners, 0);
            if (!warped) return false;
            layer.asset = Asset::make(Image16Ptr(warped->image), layer.name);
            layer.shapeImage.reset();
            box = warped->transform;
            box.sampling = sampling;
        }
        if (layer.mask && layer.mask->asset.image.u16()) {
            const Gray16& mask = *layer.mask->asset.image.u16();
            if (layer.mask->placement) layer.mask->placement = layer.mask->placement->placing(layer.mask->placement->unitToDocument().concatenating(scale));
            else if (mask.width() > 1 || mask.height() > 1) {
                if ((long long)w * h > document.imagePixelBudget() || (long long)w * h > document.projectPixelBudgetAt() - usedMask) return false;
                usedMask += (long long)w * h;
                Corners corners;
                for (size_t i = 0; i < 4; i++) corners[i] = {c[i].x * sx, c[i].y * sy};
                LayerTransform sampled = layer.transform;
                sampled.sampling = sampling;
                auto warped = warpMask(mask, sampled, corners, 0, 0);
                if (!warped) return false;
                layer.mask->asset = MaskAsset::make(Gray16Ptr(warped->image));
                if (!layer.asset) box = warped->transform;
            }
        } else if (layer.mask && layer.mask->asset.image.u8()) {
            const GrayImage& mask = *layer.mask->asset.image.u8();
            if (layer.mask->placement) layer.mask->placement = layer.mask->placement->placing(layer.mask->placement->unitToDocument().concatenating(scale));
            else if (mask.width() > 1 || mask.height() > 1) {
                if ((long long)w * h > Document::pixelBudget || (long long)w * h > Document::projectPixelBudget - usedMask) return false;
                usedMask += (long long)w * h;
                Corners corners;
                for (size_t i = 0; i < 4; i++) corners[i] = {c[i].x * sx, c[i].y * sy};
                LayerTransform sampled = layer.transform;
                sampled.sampling = sampling;
                auto warped = warpMask(layer.mask->asset.image.u8(), sampled, corners, 0, 0);
                if (!warped) return false;
                // The warp's bounds equal the box; the pixel grid now matches the layer's.
                layer.mask->asset = MaskAsset::make(warped->image);
                if (!layer.asset) box = warped->transform;
            }
        }
        layer.transform = box;
    }
    document = out;
    return true;
}

} // namespace compositor
