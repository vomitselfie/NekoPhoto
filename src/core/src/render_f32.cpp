// The renderer's 32-bit primitives (docs/bit-depth.md, "32 bits"): mask coverage, drawing a layer and resampling in
// premultiplied linear float, and the entry points that render a 32-bit document (the deep depths' render_deep.inc).
// The 16-bit forms (render_u16.cpp) with nothing quantised: coverage and colour stay float from the source to the
// blend.
#include "compositor/render.h"
#include "render_plan.h"
#include "render_deep_ops_f32.h"
#include "render_deep.inc"
#include "compositor/blend.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/parallel.h"
#include "compositor/warp.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

namespace compositor {

namespace {

struct MipChoice { int level; double factor; };
MipChoice mipFor(Sampling sampling, double outputWidth, double outputHeight, int pixelWidth, int pixelHeight) {
    if (sampling == Sampling::Nearest) return {0, 1};
    double fx = outputWidth / std::max(1, pixelWidth), fy = outputHeight / std::max(1, pixelHeight);
    int level = MipCache::levelFor(std::min(fx, fy), sampling == Sampling::High);
    return {level, std::ldexp(1.0, level)};
}

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

inline void sampleNearestF(const ImageF& image, double x, double y, float out[4]) {
    const int ix = clamp(int(std::floor(x)), 0, image.width() - 1), iy = clamp(int(std::floor(y)), 0, image.height() - 1);
    std::memcpy(out, image.pixel(ix, iy), 4 * sizeof(float));
}

inline void samplePixelsF(Sampling sampling, const ImageF& image, double x, double y, float out[4]) {
    if (sampling == Sampling::High) sampleBicubic(image, x, y, out);
    else sampleBilinear(image, x, y, out);
}

inline float sampleGrayNearestF(const GrayF& image, double x, double y) {
    const int ix = clamp(int(std::floor(x)), 0, image.width() - 1), iy = clamp(int(std::floor(y)), 0, image.height() - 1);
    return image.at(ix, iy);
}

void sampleMaskCoverageF(const GrayF& mask, const GrayFPtr& owner, const LayerTransform& transform, const Rect& region, double scale, float outside, GrayF& out, bool multiply) {
    const int mw = mask.width(), mh = mask.height();
    if (!multiply) out.fill(outside);
    if (mw <= 0 || mh <= 0) return;
    Mapping m = mappingFor(transform, mw, mh, region, scale, out.width(), out.height(), 1);
    const double sx = std::hypot(m.outputToPixel.a, m.outputToPixel.b), sy = std::hypot(m.outputToPixel.c, m.outputToPixel.d);
    MipChoice mip = mipFor(transform.sampling, transform.size.width * scale, transform.size.height * scale, mw, mh);
    std::shared_ptr<const GrayF> reduced;
    if (mip.level > 0) reduced = owner && owner.get() == &mask ? MipCache::shared().level(owner, mip.level) : reduceGray(mask, mip.level);
    const GrayF& src = reduced ? *reduced : mask;
    const double factor = mip.factor;
    const bool nearest = transform.sampling == Sampling::Nearest;
    if (multiply && outside != 1.0f) {
        for (int y = 0; y < out.height(); y++) {
            float* row = out.row(y);
            for (int x = 0; x < out.width(); x++) {
                const bool inside = x >= m.outputRect.minX() && x < m.outputRect.maxX() && y >= m.outputRect.minY() && y < m.outputRect.maxY();
                if (!inside) row[x] *= outside;
            }
        }
    }
    parallelRows(int(m.outputRect.minY()), int(m.outputRect.maxY()), [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            float* row = out.row(y);
            Point p = m.outputToPixel.apply({m.outputRect.minX() + 0.5, y + 0.5});
            const Point dp = m.outputToPixel.applyVector({1, 0});
            for (int x = int(m.outputRect.minX()); x < int(m.outputRect.maxX()); x++, p = p + dp) {
                float value;
                const bool inside = p.x >= 0 && p.x < mw && p.y >= 0 && p.y < mh;
                if (nearest) value = inside ? sampleGrayNearestF(src, p.x, p.y) : outside;
                else {
                    const double ex = std::min(p.x, mw - p.x) / std::max(1e-9, sx), ey = std::min(p.y, mh - p.y) / std::max(1e-9, sy);
                    const float edge = float(clamp(std::min(ex, ey) + 0.5, 0.0, 1.0));
                    value = sampleGrayBilinear(src, p.x / factor, p.y / factor) * edge + outside * (1 - edge);
                }
                const float v = cleanCoverage(value);
                row[x] = multiply ? row[x] * v : v;
            }
        }
    });
}

} // namespace

void sampleMaskCoverage(const GrayF& mask, const LayerTransform& transform, const Rect& region, double scale, float outside, GrayF& out, bool multiply) {
    sampleMaskCoverageF(mask, nullptr, transform, region, scale, outside, out, multiply);
}
void sampleMaskCoverage(const GrayFPtr& mask, const LayerTransform& transform, const Rect& region, double scale, float outside, GrayF& out, bool multiply) {
    if (mask) sampleMaskCoverageF(*mask, mask, transform, region, scale, outside, out, multiply);
}

void drawLayer(const DrawParamsF& params, const Rect& region, double scale, const GrayF* coverage, ImageF& out) {
    if (!params.image || params.image->isEmpty() || out.isEmpty()) return;
    const ImageF& full = *params.image;
    const int pw = full.width(), ph = full.height();
    Mapping m = mappingFor(params.transform, pw, ph, region, scale, out.width(), out.height(), 1);
    if (m.outputRect.isEmpty()) return;
    const bool nearest = params.transform.sampling == Sampling::Nearest;
    MipChoice mip = mipFor(params.transform.sampling, params.transform.size.width * scale, params.transform.size.height * scale, pw, ph);
    ImageFPtr source = MipCache::shared().level(params.image, mip.level);
    const double factor = mip.factor;
    const double sx = std::hypot(m.outputToPixel.a, m.outputToPixel.b), sy = std::hypot(m.outputToPixel.c, m.outputToPixel.d);
    const float* luma = floatRenderContext().luma;

    const GrayF* mask = nullptr;
    GrayFPtr maskHold;
    std::optional<LayerTransform> maskPlacement;
    if (params.mask && params.mask->enabled) {
        maskHold = params.maskImage ? params.maskImage : params.mask->asset.image.f32();
        mask = maskHold.get();
        maskPlacement = params.maskPlacement ? params.maskPlacement : params.mask->placement;
        if (maskPlacement && maskPlacement->samePlacement(params.layerTransformForMask)) maskPlacement.reset();
    }
    std::shared_ptr<GrayF> placedMask;
    if (mask && maskPlacement) {
        const float background = params.mask->asset.thumbnail ? float(LayerMask::background(*params.mask->asset.thumbnail)) / 255.0f : 1.0f;
        placedMask = std::make_shared<GrayF>(out.width(), out.height(), background);
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
            std::vector<float> steps(size_t(spanEnd - spanBegin));
            for (int y = ya; y < yb; y++) {
                const int py = y + offsetY;
                if (py < 0 || py >= ph) continue;
                const float* covRow = coverage ? coverage->row(y) : nullptr;
                const float* placedRow = placedMask ? placedMask->row(y) : nullptr;
                const float* maskRow = mask && !placedMask ? mask->row(py) : nullptr;
                for (int x = spanBegin; x < spanEnd; x++) {
                    float cov = opacity;
                    if (covRow) cov *= covRow[x];
                    if (placedRow) cov *= placedRow[x];
                    else if (maskRow) cov *= maskRow[x + offsetX];
                    steps[size_t(x - spanBegin)] = cleanCoverage(cov);
                }
                compositeSpanF(mode, source->pixel(spanBegin + offsetX, py), steps.data(), out.pixel(spanBegin, y), spanEnd - spanBegin, luma);
            }
        });
        return;
    }

    parallelRows(int(m.outputRect.minY()), int(m.outputRect.maxY()), [&](int ya, int yb) {
        std::vector<float> samples(size_t(xEnd - xBegin) * 4);
        std::vector<float> steps(size_t(xEnd - xBegin));
        for (int y = ya; y < yb; y++) {
            const float* covRow = coverage ? coverage->row(y) : nullptr;
            const float* placedRow = placedMask ? placedMask->row(y) : nullptr;
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
                if (covRow) cov *= covRow[x];
                if (mask) {
                    if (placedRow) cov *= placedRow[x];
                    else cov *= nearest ? sampleGrayNearestF(*mask, p.x * maskScaleX, p.y * maskScaleY) : sampleGrayBilinear(*mask, p.x * maskScaleX, p.y * maskScaleY);
                }
                if (!(cov > 0)) continue;
                float* src = &samples[i * 4];
                if (nearest) sampleNearestF(*source, p.x, p.y, src);
                else samplePixelsF(params.transform.sampling, *source, p.x * invFactor, p.y * invFactor, src);
                if (!(src[3] > 0)) continue;
                if (dissolve) { compositePixelAtF(mode, src, cov, out.pixel(x, y), docX(region, scale, x), docY(region, scale, y), luma); continue; }
                steps[i] = cleanCoverage(cov);
                any = true;
            }
            if (any) compositeSpanF(mode, samples.data(), steps.data(), out.pixel(xBegin, y), xEnd - xBegin, luma);
        }
    });
}

std::shared_ptr<ImageF> resampleLayer(const ImageFPtr& image, const LayerTransform& transform, const LayerTransform& target, int width, int height) {
    auto out = std::make_shared<ImageF>(std::max(0, width), std::max(0, height));
    if (!image || image->isEmpty() || width <= 0 || height <= 0) return out;
    const Affine map = target.pixelToDocument(width, height).concatenating(transform.pixelToDocument(image->width(), image->height()).inverted());
    const bool nearest = transform.sampling == Sampling::Nearest;
    const double sx = std::hypot(map.a, map.b), sy = std::hypot(map.c, map.d);
    const int level = nearest ? 0 : MipCache::levelFor(1.0 / std::max(sx, sy), transform.sampling == Sampling::High);
    ImageFPtr source = MipCache::shared().level(image, level);
    const double factor = std::ldexp(1.0, level);
    const int pw = image->width(), ph = image->height();
    parallelRows(0, height, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            float* row = out->row(y);
            Point p = map.apply({0.5, y + 0.5});
            const Point dp = map.applyVector({1, 0});
            for (int x = 0; x < width; x++, p = p + dp, row += 4) {
                if (nearest) {
                    if (p.x < 0 || p.x >= pw || p.y < 0 || p.y >= ph) continue;
                    sampleNearestF(*source, p.x, p.y, row);
                } else {
                    const double ex = std::min(p.x, pw - p.x) / std::max(1e-9, sx), ey = std::min(p.y, ph - p.y) / std::max(1e-9, sy);
                    const float edge = float(clamp(std::min(ex, ey) + 0.5, 0.0, 1.0));
                    if (!(edge > 0)) continue;
                    float s[4];
                    samplePixelsF(transform.sampling, *source, p.x / factor, p.y / factor, s);
                    for (int c = 0; c < 4; c++) row[c] = s[c] * edge;
                }
            }
        }
    });
    return out;
}

std::shared_ptr<GrayF> resampleMask(const GrayF& mask, const LayerTransform& transform, const LayerTransform& target, int width, int height, float outside) {
    auto out = std::make_shared<GrayF>(std::max(0, width), std::max(0, height), outside);
    if (mask.isEmpty() || width <= 0 || height <= 0) return out;
    const Affine map = target.pixelToDocument(width, height).concatenating(transform.pixelToDocument(mask.width(), mask.height()).inverted());
    const double sx = std::hypot(map.a, map.b), sy = std::hypot(map.c, map.d);
    const int level = MipCache::levelFor(1.0 / std::max(sx, sy));
    std::shared_ptr<const GrayF> reduced = level > 0 ? reduceGray(mask, level) : nullptr;
    const GrayF& src = reduced ? *reduced : mask;
    const double factor = std::ldexp(1.0, level);
    const int mw = mask.width(), mh = mask.height();
    parallelRows(0, height, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            float* row = out->row(y);
            Point p = map.apply({0.5, y + 0.5});
            const Point dp = map.applyVector({1, 0});
            for (int x = 0; x < width; x++, p = p + dp) {
                const double ex = std::min(p.x, mw - p.x) / std::max(1e-9, sx), ey = std::min(p.y, mh - p.y) / std::max(1e-9, sy);
                const float edge = float(clamp(std::min(ex, ey) + 0.5, 0.0, 1.0));
                row[x] = cleanCoverage(sampleGrayBilinear(src, p.x / factor, p.y / factor) * edge + outside * (1 - edge));
            }
        }
    });
    return out;
}

// ---- Rendering a 32-bit document ------------------------------------------------------------------------------

void renderForDisplayF(const RenderPlan& plan, const Rect& region, double scale, Image& out, RenderCache* cache, uint64_t version, bool clear,
                       const ColorTransform* display, const View32& view, float peak) {
    FloatRenderScope scope(plan.document, view, peak);
    renderForDisplayDeep<SampleType::F32>(plan, region, scale, out, cache, version, clear, display);
}

void renderF(const Document& document, const RenderOptions& options, ImageF& out, const Overrides* overrides, RenderCache* cache) {
    FloatRenderScope scope(document, options.view32, options.peak);
    renderDeep<SampleType::F32>(document, options, out, overrides, cache);
}

std::shared_ptr<ImageF> renderFlattenedF(const Document& document) {
    auto out = std::make_shared<ImageF>(document.width, document.height);
    renderF(document, RenderOptions(), *out);
    return out;
}

std::shared_ptr<Image> decisionImage(const Document& document) {
    if (document.sampleType != SampleType::F32) return renderFlattened(document);
    return encodeImage8(*renderFlattenedF(document), encodedTransfer(document));
}

bool resizeDocumentF(Document& document, int width, int height, double resolution, Sampling sampling) {
    // resizeDocument16's steps at 32 bits: each layer through its scaled corners with the float warp and resampler,
    // light above 1 kept; masks and budgets as at 16 bits.
    if (!Document::canCreate(width, height, document.sampleType)) return false;
    const double sx = double(width) / document.width, sy = double(height) / document.height;
    if (document.width == width && document.height == height) { document.resolution = resolution; return true; }
    Document out = document;
    out.width = width; out.height = height; out.resolution = resolution;
    out.selection.reset();
    long long used = 0, usedMask = 0;
    const Affine scale = Affine::scaling(sx, sy);
    for (auto& layer : out.layers) {
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
        const auto c = layer.transform.corners();
        double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
        for (auto& p : c) { minX = std::min(minX, p.x * sx); minY = std::min(minY, p.y * sy); maxX = std::max(maxX, p.x * sx); maxY = std::max(maxY, p.y * sy); }
        const double left = std::floor(minX), top = std::floor(minY);
        const int w = std::max(1, int(std::ceil(maxX) - left)), h = std::max(1, int(std::ceil(maxY) - top));
        LayerTransform box(Point(left, top), Size(w, h));
        box.sampling = sampling;
        if (!box.isValid()) return false;
        Corners corners;
        for (size_t i = 0; i < 4; i++) corners[i] = {c[i].x * sx, c[i].y * sy};
        LayerTransform sampled = layer.transform;
        sampled.sampling = sampling;   // the dialog's choice, not the layer's own
        if (layer.asset && layer.asset->image.f32()) {
            if (w > 30000 || h > 30000 || (long long)w * h > document.imagePixelBudget() || (long long)w * h > document.projectPixelBudgetAt() - used) return false;
            used += (long long)w * h;
            auto warped = warpImage(layer.asset->image.f32(), sampled, corners, 0);
            if (!warped) return false;
            layer.asset = Asset::make(ImageFPtr(warped->image), layer.name);
            layer.shapeImage.reset();
            box = warped->transform;
            box.sampling = sampling;
        }
        if (layer.mask && layer.mask->asset.image.f32()) {
            const GrayF& mask = *layer.mask->asset.image.f32();
            if (layer.mask->placement) layer.mask->placement = layer.mask->placement->placing(layer.mask->placement->unitToDocument().concatenating(scale));
            else if (mask.width() > 1 || mask.height() > 1) {
                if ((long long)w * h > document.imagePixelBudget() || (long long)w * h > document.projectPixelBudgetAt() - usedMask) return false;
                usedMask += (long long)w * h;
                auto warped = warpMask(mask, sampled, corners, 0.0f, 0);
                if (!warped) return false;
                layer.mask->asset = MaskAsset::make(GrayFPtr(warped->image));
                if (!layer.asset) box = warped->transform;
            }
        }
        layer.transform = box;
    }
    document = out;
    return true;
}

} // namespace compositor
