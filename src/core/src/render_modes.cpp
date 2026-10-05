// The primitives of CMYK and Lab rendering (render_modes.h): drawing a layer of 4 or 5 samples a pixel, the
// conversions that bring RGB-made pixels (fill layers, vector paint, rasters from before a conversion) into the
// document's mode, the adjustment layers drawn in these modes, and the entry points: the canvas's display (never
// without a colour transform, since a CMYK or Lab buffer is not RGB), the native render, and RGB for 16-bit exports.
#include "render_modes.h"
#include "edge_interior.h"
#include "compositor/adjustments.h"
#include "compositor/modeedit.h"
#include "compositor/parallel.h"
#include "compositor/render.h"
#include "compositor/resample.h"
#include "compositor/simd.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace compositor {

namespace {

struct MipChoice { int level; double factor; };
MipChoice mipFor(Sampling sampling, double outputWidth, double outputHeight, int pixelWidth, int pixelHeight) {
    if (sampling == Sampling::Nearest) return {0, 1};
    const double fx = outputWidth / std::max(1, pixelWidth), fy = outputHeight / std::max(1, pixelHeight);
    const int level = MipCache::levelFor(std::min(fx, fy), sampling == Sampling::High);
    return {level, std::ldexp(1.0, level)};
}

struct Mapping {
    Affine outputToPixel;
    Rect outputRect;
};

Mapping mappingFor(const LayerTransform& transform, int pixelWidth, int pixelHeight, const Rect& region, double scale, int outWidth, int outHeight, double margin) {
    Mapping m;
    const Affine documentToOutput = Affine::translation(-region.x, -region.y).concatenating(Affine::scaling(scale, scale));
    const Affine pixelToOutput = transform.pixelToDocument(pixelWidth, pixelHeight).concatenating(documentToOutput);
    m.outputToPixel = pixelToOutput.inverted();
    const Rect bounds = pixelToOutput.mapBounds(Rect(0, 0, pixelWidth, pixelHeight)).insetBy(-margin, -margin).integral();
    m.outputRect = bounds.intersection(Rect(0, 0, outWidth, outHeight));
    return m;
}

template <class Img, class T>
inline void sampleNearestN(const Img& image, double x, double y, T* out, int n) {
    const int ix = std::clamp(int(std::floor(x)), 0, image.width() - 1), iy = std::clamp(int(std::floor(y)), 0, image.height() - 1);
    std::memcpy(out, image.pixel(ix, iy), size_t(n) * sizeof(T));
}

/// Four samples as float lanes.
template <class T>
inline simd::f32x4 lanesF(const T* p) {
    T v[4];
    std::memcpy(v, p, sizeof v);
    return simd::f32x4{float(v[0]), float(v[1]), float(v[2]), float(v[3])};
}

/// Bilinear at continuous pixel coordinates (pixel i spans [i, i + 1)), clamped to the edge, for 4 or 5 samples. The
/// RGB samplers' Catmull-Rom (High) is not repeated here: CMYK and Lab layers resample bilinearly at every setting.
template <class Img, class T>
inline void sampleBilinearN(const Img& image, double x, double y, T* out, int n) {
    const double fx = x - 0.5, fy = y - 0.5;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float tx = float(fx - x0), ty = float(fy - y0);
    const int w = image.width(), h = image.height();
    const int xa = std::clamp(x0, 0, w - 1), xb = std::clamp(x0 + 1, 0, w - 1), ya = std::clamp(y0, 0, h - 1), yb = std::clamp(y0 + 1, 0, h - 1);
    const T* p00 = image.pixel(xa, ya); const T* p10 = image.pixel(xb, ya);
    const T* p01 = image.pixel(xa, yb); const T* p11 = image.pixel(xb, yb);
    // The first four samples in float lanes (the same operations in the same order as the scalar tail), the rest one
    // at a time.
    int c = 0;
    if (n >= 4) {
        const simd::f32x4 a = lanesF(p00), b = lanesF(p10), d = lanesF(p01), e = lanesF(p11);
        const simd::f32x4 top = a + (b - a) * tx, bottom = d + (e - d) * tx;
        const simd::i32x4 v = __builtin_convertvector(top + (bottom - top) * ty + 0.5f, simd::i32x4);
        for (; c < 4; c++) out[c] = T(v[c]);
    }
    for (; c < n; c++) {
        const float top = p00[c] + (float(p10[c]) - p00[c]) * tx, bottom = p01[c] + (float(p11[c]) - p01[c]) * tx;
        out[c] = T(top + (bottom - top) * ty + 0.5f);
    }
}

template <class G>
inline int sampleGrayNearestN(const G& image, double x, double y) {
    const int ix = std::clamp(int(std::floor(x)), 0, image.width() - 1), iy = std::clamp(int(std::floor(y)), 0, image.height() - 1);
    return image.at(ix, iy);
}

/// A buffer converted to a document's layout once, kept while the buffer lives (keyed by the buffer and the layout).
std::mutex convertedMutex;
struct Converted { std::weak_ptr<const void> source; const void* identity; int layout; AnyImage converted; };
std::vector<Converted> convertedCache;

/// `any` at depth `S` in mode `M`: shared when it is already there; an RGB buffer (4 samples in a CMYK document) is
/// converted from sRGB into the document's profile; a Lab one is taken as Lab (a Lab and an RGB buffer look alike).
template <SampleType S, ColorMode M>
AnyImage toLayout(const Document& document, const AnyImage& any) {
    if (!any) return {};
    if (any.channels() == colorModeChannels(M)) return imageAtFormat(any, S, M);
    if (M != ColorMode::CMYK || any.channels() != 4 || any.sampleType() == SampleType::F32) return {};
    const AnyImage cmyk = convertImage(any, ColorMode::RGB, ColorProfile(), ColorMode::CMYK, document.profile);
    return cmyk ? imageAtFormat(cmyk, S, M) : AnyImage();
}

template <SampleType S, ColorMode M>
typename ModeOps<S, M>::ImagePtr typed(const AnyImage& any) {
    if constexpr (S == SampleType::U16) return any.u16();
    else if constexpr (M == ColorMode::CMYK) return any.c8();
    else return any.u8();
}

} // namespace

// ---- The policy's primitives ------------------------------------------------------------------------------------

template <SampleType S, ColorMode M>
void ModeOps<S, M>::drawLayer(const Params& params, const Rect& region, double scale, const Gray* coverage, Image& out) {
    constexpr int N = channels, C = N - 1;
    if (!params.image || params.image->isEmpty() || out.isEmpty()) return;
    const Image& full = *params.image;
    if constexpr (requires { full.channels(); }) if (full.channels() != N) return;
    const int pw = full.width(), ph = full.height();
    const Mapping m = mappingFor(params.transform, pw, ph, region, scale, out.width(), out.height(), 1);
    if (m.outputRect.isEmpty()) return;
    const bool nearest = params.transform.sampling == Sampling::Nearest;
    const MipChoice mip = mipFor(params.transform.sampling, params.transform.size.width * scale, params.transform.size.height * scale, pw, ph);
    const ImagePtr source = MipCache::shared().level(params.image, mip.level);
    const double factor = mip.factor;
    const double sx = std::hypot(m.outputToPixel.a, m.outputToPixel.b), sy = std::hypot(m.outputToPixel.c, m.outputToPixel.d);
    const float inv = 1.0f / float(one);

    const Gray* mask = nullptr;
    GrayPtr maskHold;
    std::optional<LayerTransform> maskPlacement;
    if (params.mask && params.mask->enabled) {
        maskHold = params.maskImage ? params.maskImage : gray(params.mask->asset.image);
        mask = maskHold.get();
        maskPlacement = params.maskPlacement ? params.maskPlacement : params.mask->placement;
        if (maskPlacement && maskPlacement->samePlacement(params.layerTransformForMask)) maskPlacement.reset();
    }
    std::shared_ptr<Gray> placedMask;
    if (mask && maskPlacement) {
        const Sample background = from8(params.mask->asset.thumbnail ? LayerMask::background(*params.mask->asset.thumbnail) : 255);
        placedMask = std::make_shared<Gray>(out.width(), out.height(), background);
        sampleMaskCoverage(*mask, *maskPlacement, region, scale, background, *placedMask, false);
    }
    const double maskScaleX = mask ? double(mask->width()) / pw : 1, maskScaleY = mask ? double(mask->height()) / ph : 1;
    const float opacity = float(std::clamp(params.opacity, 0.0, 1.0));
    const int xBegin = int(m.outputRect.minX()), xEnd = int(m.outputRect.maxX());
    const Point dp = m.outputToPixel.applyVector({1, 0});
    const BlendMode mode = params.mode;
    const bool dissolve = blendModeFor(mode, M) == BlendMode::Dissolve;

    // On the output grid at whole pixels: rows of the layer straight over the destination, one span per row (as the
    // RGB executors do).
    const Affine& o2p = m.outputToPixel;
    const bool onGrid = !dissolve && factor == 1 && std::fabs(o2p.a - 1) < 1e-9 && std::fabs(o2p.b) < 1e-9 && std::fabs(o2p.c) < 1e-9
        && std::fabs(o2p.d - 1) < 1e-9 && std::fabs(o2p.tx - std::round(o2p.tx)) < 1e-9 && std::fabs(o2p.ty - std::round(o2p.ty)) < 1e-9
        && (!mask || placedMask || (maskScaleX == 1 && maskScaleY == 1));
    if (onGrid) {
        const int offsetX = int(std::round(o2p.tx)), offsetY = int(std::round(o2p.ty));
        const int spanBegin = std::max(xBegin, -offsetX), spanEnd = std::min(xEnd, pw - offsetX);
        if (spanEnd <= spanBegin) return;
        parallelRows(int(m.outputRect.minY()), int(m.outputRect.maxY()), [&](int ya, int yb) {
            std::vector<Step> steps(size_t(spanEnd - spanBegin));
            for (int y = ya; y < yb; y++) {
                const int py = y + offsetY;
                if (py < 0 || py >= ph) continue;
                const Sample* covRow = coverage ? coverage->row(y) : nullptr;
                const Sample* placedRow = placedMask ? placedMask->row(y) : nullptr;
                const Sample* maskRow = mask && !placedMask ? mask->row(py) : nullptr;
                for (int x = spanBegin; x < spanEnd; x++) {
                    float cov = opacity;
                    if (covRow) cov *= covRow[x] * inv;
                    if (placedRow) cov *= placedRow[x] * inv;
                    else if (maskRow) cov *= maskRow[x + offsetX] * inv;
                    steps[size_t(x - spanBegin)] = ModeOps::steps(cov);
                }
                span(mode, source->pixel(spanBegin + offsetX, py), steps.data(), out.pixel(spanBegin, y), spanEnd - spanBegin);
            }
        });
        return;
    }

    parallelRows(int(m.outputRect.minY()), int(m.outputRect.maxY()), [&](int ya, int yb) {
        std::vector<Sample> samples(size_t(xEnd - xBegin) * N);
        std::vector<Step> steps(size_t(xEnd - xBegin));
        for (int y = ya; y < yb; y++) {
            const Sample* covRow = coverage ? coverage->row(y) : nullptr;
            const Sample* placedRow = placedMask ? placedMask->row(y) : nullptr;
            Point p = m.outputToPixel.apply({xBegin + 0.5, y + 0.5});
            int interiorFrom = xBegin, interiorTo = xBegin;
            if (!nearest) edgeInterior(p, dp, sx, sy, pw, ph, xBegin, xEnd - xBegin, interiorFrom, interiorTo);
            bool any = false;
            for (int x = xBegin; x < xEnd; x++, p = p + dp) {
                const size_t i = size_t(x - xBegin);
                steps[i] = 0;
                float edge;
                if (nearest) {
                    if (p.x < 0 || p.x >= pw || p.y < 0 || p.y >= ph) continue;
                    edge = 1;
                } else if (x >= interiorFrom && x < interiorTo) {
                    edge = 1;
                } else {
                    const double ex = std::min(p.x, pw - p.x) / std::max(1e-9, sx), ey = std::min(p.y, ph - p.y) / std::max(1e-9, sy);
                    edge = float(std::clamp(std::min(ex, ey) + 0.5, 0.0, 1.0));
                    if (edge <= 0) continue;
                }
                float cov = edge * opacity;
                if (covRow) cov *= covRow[x] * inv;
                if (mask) {
                    if (placedRow) cov *= placedRow[x] * inv;
                    else cov *= float(nearest ? sampleGrayNearestN(*mask, p.x * maskScaleX, p.y * maskScaleY) : sampleGrayBilinear(*mask, p.x * maskScaleX, p.y * maskScaleY)) * inv;
                }
                if (cov <= 0) continue;
                Sample* src = &samples[i * N];
                if (nearest) sampleNearestN(*source, p.x, p.y, src, N);
                else sampleBilinearN(*source, p.x / factor, p.y / factor, src, N);
                if (!src[C]) continue;
                if (dissolve) { pixelAt(mode, src, cov, out.pixel(x, y), docX(region, scale, x), docY(region, scale, y)); continue; }
                steps[i] = ModeOps::steps(cov);
                any = any || steps[i];
            }
            if (any) span(mode, samples.data(), steps.data(), out.pixel(xBegin, y), xEnd - xBegin);
        }
    });
}

template <SampleType S, ColorMode M>
typename ModeOps<S, M>::ImagePtr ModeOps<S, M>::image(const Document& document, const AnyImage& any) {
    if (!any) return nullptr;
    if (any.channels() == channels && any.sampleType() == S) return typed<S, M>(any);
    const int layout = int(S) * 4 + int(M);
    {
        std::lock_guard<std::mutex> lock(convertedMutex);
        for (auto it = convertedCache.begin(); it != convertedCache.end();) {
            if (it->source.expired()) { it = convertedCache.erase(it); continue; }
            if (it->identity == any.identity() && it->layout == layout) return typed<S, M>(it->converted);
            ++it;
        }
    }
    const AnyImage converted = toLayout<S, M>(document, any);
    if (!converted) return nullptr;
    std::shared_ptr<const void> owner = any.visit([](const auto& p) -> std::shared_ptr<const void> { return p; });
    std::lock_guard<std::mutex> lock(convertedMutex);
    if (convertedCache.size() > 128) convertedCache.erase(convertedCache.begin());
    convertedCache.push_back({owner, any.identity(), layout, converted});
    return typed<S, M>(converted);
}

template <SampleType S, ColorMode M>
void ModeOps<S, M>::solid(const Document& document, const uint8_t rgba[4], Sample* out) {
    // The colour (an sRGB value, as colours are stored in every mode) through the document's profile, opaque, at 8
    // bits; then widened and premultiplied by its alpha.
    const PixelFormat layout8 = pixelFormatFor(SampleType::U8, M);
    const ColorTransformPtr t = transformBetween(ColorProfile(), document.profile, ConvertOptions(), PixelFormat::RGBA8, layout8);
    const uint8_t in[4] = {rgba[0], rgba[1], rgba[2], 255};
    uint8_t px[5] = {0, 0, 0, 0, 255};
    if (t) t->apply(in, px, 1);
    const Sample a = from8(rgba[3]);
    for (int k = 0; k < channels - 1; k++) out[k] = mul(from8(px[k]), a);
    out[channels - 1] = a;
}

template <SampleType S, ColorMode M>
typename ModeOps<S, M>::ImagePtr ModeOps<S, M>::vectorPaint(const VectorPaint& paint, const Document& document, const Rect& bounds, const Rect& area, double scale, int w, int h) {
    if constexpr (M == ColorMode::CMYK)
        if (AnyImage inks = renderVectorPaintInks(paint, document, bounds, area, scale, w, h, deep)) return typed<S, M>(inks);
    AnyImage rgb;
    if constexpr (deep) rgb = renderVectorPaint16(paint, document, bounds, area, scale, w, h);
    else rgb = renderVectorPaint(paint, document, bounds, area, scale, w, h);
    return typed<S, M>(toLayout<S, M>(document, rgb));
}

template <SampleType S, ColorMode M>
typename ModeOps<S, M>::ImagePtr ModeOps<S, M>::fillLayer(const Layer& layer, const Document& document) {
    if constexpr (M == ColorMode::CMYK)
        if (AnyImage inks = renderFillLayerInks(layer, document, deep)) return typed<S, M>(inks);
    AnyImage rgb;
    if constexpr (deep) rgb = renderFillLayer16(layer, document);
    else rgb = renderFillLayer(layer, document);
    return typed<S, M>(toLayout<S, M>(document, rgb));
}

template <SampleType S, ColorMode M>
bool ModeOps<S, M>::adjust(const Document& document, const LayerAdjustment& adjustment, Image& image, const Rect&, double) {
    AdjustmentSettings settings;
    if (!AdjustmentSettings::parse(adjustment.json, settings)) return false;
    // The document's own samples (adjustments_modes.cpp); a kind the mode does not offer is not drawn.
    if constexpr (deep) return applyAdjustmentMode(settings, image, M, document.profile);
    else if constexpr (M == ColorMode::CMYK) return applyAdjustmentMode(settings, image, document.profile);
    else return applyAdjustmentLab(settings, image);
}

template struct ModeOps<SampleType::U8, ColorMode::CMYK>;
template struct ModeOps<SampleType::U8, ColorMode::Lab>;
template struct ModeOps<SampleType::U16, ColorMode::CMYK>;
template struct ModeOps<SampleType::U16, ColorMode::Lab>;

// ---- Entry points -----------------------------------------------------------------------------------------------

namespace {

/// The plan at the document's layout, `w` x `h`.
AnyImage renderPlanNative(const Document& document, const RenderPlan& plan, const Rect& region, double scale, int w, int h, RenderCache* cache, uint64_t version) {
    const int n = colorModeChannels(document.colorMode);
    if (document.sampleType == SampleType::U16) {
        auto out = std::make_shared<Image16>(w, h, n);
        executeRenderMode16(plan, region, scale, *out, cache, version);
        return Image16Ptr(out);
    }
    if (document.colorMode == ColorMode::CMYK) {
        auto out = std::make_shared<ImageC8>(w, h, 5);
        executeRenderMode(plan, region, scale, *out, cache, version);
        return ImageC8Ptr(out);
    }
    auto out = std::make_shared<Image>(w, h);
    executeRenderLab8(plan, region, scale, *out, cache, version);
    return ImagePtr(out);
}

void outputSize(const Document& document, const RenderOptions& options, Rect& region, double& scale, int& w, int& h) {
    region = options.region.isEmpty() ? document.rect() : options.region;
    scale = options.scale > 0 ? options.scale : 1;
    w = std::max(1, int(std::ceil(region.width * scale - 1e-9)));
    h = std::max(1, int(std::ceil(region.height * scale - 1e-9)));
}

} // namespace

void renderForDisplayMode(const Document& document, const RenderPlan& plan, const Rect& region, double scale, Image& out, RenderCache* cache,
                          uint64_t version, bool, const ColorTransform* display) {
    // A CMYK or Lab frame replaces what `out` held: its pixels are the display's RGB, not the document's layout.
    const AnyImage native = renderPlanNative(document, plan, region, scale, out.width(), out.height(), cache, version);
    const PixelFormat layout = pixelFormatFor(document.sampleType, document.colorMode);
    ColorTransformPtr own;
    const ColorTransform* t = display && display->input() == layout && display->output() == PixelFormat::RGBA8 ? display : nullptr;
    if (!t) {
        own = transformBetween(document.profile, srgbProfile(), ConvertOptions(), layout, PixelFormat::RGBA8);
        t = own.get();
    }
    out.clear();
    if (t) convertImageTo8(native, out, *t);
}

AnyImage renderNative(const Document& document, const RenderOptions& options, const Overrides* overrides, RenderCache* cache) {
    Rect region;
    double scale;
    int w, h;
    outputSize(document, options, region, scale, w, h);
    if (document.colorMode == ColorMode::RGB) {
        if (document.sampleType == SampleType::U16) {
            auto out = std::make_shared<Image16>(w, h);
            render16(document, options, *out, overrides, cache);
            return Image16Ptr(out);
        }
        auto out = std::make_shared<Image>(w, h);
        RenderOptions plain = options;
        plain.display = nullptr;
        render(document, plain, *out, overrides, cache);
        return ImagePtr(out);
    }
    RenderPlan plan(document, overrides);
    plan.build();
    return renderPlanNative(document, plan, region, scale, w, h, options.clear ? cache : nullptr, options.version);
}

void renderModeAsRgb16(const Document& document, const RenderOptions& options, Image16& out, const Overrides* overrides, RenderCache* cache) {
    const AnyImage native = renderNative(document, options, overrides, cache);
    // The document's colours in sRGB, the RGB counterpart exports take (docs/color-modes.md).
    AnyImage rgb = convertImage(native, document.colorMode, document.profile, ColorMode::RGB, ColorProfile());
    rgb = rgb ? imageAtDepth(rgb, SampleType::U16) : AnyImage();
    if (rgb.u16()) out = *rgb.u16();
    else out = Image16(native.width(), native.height());
}

} // namespace compositor
