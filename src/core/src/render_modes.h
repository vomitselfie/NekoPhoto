// The deep executor's policy for CMYK and Lab documents (docs/high-bit-depth-plan.md, P7 step C): `ModeOps<S, M>`,
// which render_exec_deep.inc runs with as its second template argument. One policy serves 8-bit CMYK (ImageC8,
// render_exec_c8.cpp), 8-bit Lab (Image, render_exec_lab8.cpp) and 16-bit CMYK and Lab (Image16 with 5 or 4
// channels, render_exec_modes16.cpp), so the RGB executors (render_exec_u8.cpp, the U16 instance) are untouched.
//
// What a CMYK or Lab document draws, and what it does not yet:
// - pixel layers in every blend mode Photoshop offers there (blend.h's blendModeFor: CMYK's non-separable modes draw
//   as Normal until they are calibrated), opacity, pixel and vector masks, clipping stacks, folders, artboards,
//   vector strokes and gradient or pattern fill layers (drawn in RGB and converted);
// - adjustment layers: every kind Photoshop offers in the mode but Color Lookup, on the document's own samples
//   (adjustments_modes.cpp, modeedit.h);
// - layer styles: every effect, its masks as in RGB, its colours through the document's profile and its blending in
//   the document's channels (layerstyle_render.cpp).
//
// A buffer held in another layout (an 8-bit RGB raster in a CMYK document, say, text rendered before the document's
// conversion reached it) is converted to the document's mode from sRGB once and kept while it lives.
#pragma once
#include "render_deep_ops.h"
#include "render_plan.h"
#include "layerstyle_render.h"
#include "compositor/blend.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/vectormask.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>

namespace compositor {

template <SampleType S, ColorMode M>
struct ModeOps {
    static_assert(M != ColorMode::RGB && S != SampleType::F32, "CMYK and Lab exist at 8 and 16 bits");
    static constexpr bool deep = S == SampleType::U16;
    using Sample = SampleOf<S>;
    using Image = std::conditional_t<deep, Image16, std::conditional_t<M == ColorMode::CMYK, ImageC8, compositor::Image>>;
    using Gray = GrayOf<S>;
    using ImagePtr = std::shared_ptr<const Image>;
    using GrayPtr = std::shared_ptr<const Gray>;
    using Step = std::conditional_t<deep, uint32_t, uint16_t>;
    struct Params {
        ImagePtr image;
        LayerTransform transform;
        double opacity = 1;
        BlendMode mode = BlendMode::Normal;
        const LayerMask* mask = nullptr;
        std::optional<LayerTransform> maskPlacement;
        GrayPtr maskImage;
        LayerTransform layerTransformForMask;
    };

    static constexpr ColorMode colorMode = M;
    static constexpr Sample one = SampleTraits<S>::one;
    static constexpr int channels = colorModeChannels(M);
    static constexpr bool styles = true;

    static float unit(Sample s) { return float(s) / float(one); }
    static Sample mul(Sample a, Sample b) {
        if constexpr (deep) return Sample(mul15(a, b));
        else return Sample((uint32_t(a) * b + 127) / 255);
    }
    static Sample unpremultiply(Sample c, Sample a) { return Sample(std::min<uint32_t>(one, (uint32_t(c) * one + a / 2) / a)); }
    static Sample coverage(float f) { return Sample(std::lround(f * float(one))); }
    static Sample fade(Sample before, Sample after, float t) { return Sample(std::lround(before + (float(after) - float(before)) * t)); }
    static Sample store(float v, Sample alpha) { return Sample(std::clamp(v * float(one) + 0.5f, 0.0f, float(alpha))); }
    static Sample from8(uint8_t v) {
        if constexpr (deep) return widen8(v);
        else return v;
    }

    static Step steps(float c) {
        if constexpr (deep) return coverageSteps16(c);
        else return Step(coverageSteps(c));
    }
    static void span(BlendMode mode, const Sample* src, const Step* steps, Sample* dst, int count) {
        if constexpr (deep) compositeSpanMode16(mode, M, src, steps, dst, count);
        else compositeSpanMode8(mode, M, src, steps, dst, count);
    }
    static void pixelAt(BlendMode mode, const Sample* src, float c, Sample* dst, int x, int y) {
        if constexpr (deep) compositePixelAtMode16(mode, M, src, c, dst, x, y);
        else compositePixelAtMode8(mode, M, src, c, dst, x, y);
    }
    static void pixelCovered(BlendMode mode, const Sample* src, Sample c, Sample* dst) {
        const Step k = deep ? Step(c) : Step(coverageSteps(unit(c)));
        span(mode, src, &k, dst, 1);
    }
    static void blendStraight(BlendMode mode, const float* cb, float* cs) { blendStraightMode(mode, M, cb, cs); }

    // Layer styles in the document's channels (layerstyle_render.cpp): the effects' colours through its profile.
    static void drawStyled(const StyledDraw& draw, Image& target) { drawStyledLayer(draw, target); }
    static void setStyleSource(StyledDraw& draw, std::function<void(Image&, const Rect&)> source) {
        if constexpr (deep) draw.drawSource16 = std::move(source);
        else if constexpr (M == ColorMode::CMYK) draw.drawSourceC8 = std::move(source);
        else draw.drawSource = std::move(source);
    }
    static void setStyleCoverage(StyledDraw& draw, const Gray* coverage) {
        if constexpr (deep) draw.coverage16 = coverage;
        else draw.coverage = coverage;
    }
    static void setStyleMode(StyledDraw& draw, const Document& document) {
        draw.colorMode = M;
        draw.profile = &document.profile;
    }

    // The primitives (render_modes.cpp).
    static void drawLayer(const Params& params, const Rect& region, double scale, const Gray* coverage, Image& out);
    static void sampleMask(const GrayPtr& mask, const LayerTransform& transform, const Rect& region, double scale, Gray& out) {
        sampleMaskCoverage(mask, transform, region, scale, 0, out, false);
    }
    static std::shared_ptr<Gray> vectorMask(const VectorPath& path, const Rect& region, double scale, int w, int h) {
        if constexpr (deep) return rasterizeVectorMask16(path, region, scale, w, h);
        else return rasterizeVectorMask(path, region, scale, w, h);
    }
    static std::shared_ptr<Gray> vectorStroke(const VectorPath& path, const VectorStroke& stroke, const Rect& region, double scale, int w, int h) {
        if constexpr (deep) return rasterizeVectorStroke16(path, stroke, region, scale, w, h);
        else return rasterizeVectorStroke(path, stroke, region, scale, w, h);
    }
    static void maskParameters(Gray& coverage, std::optional<int> density, std::optional<double> feather, double scale, bool clampEdges = false) {
        applyMaskParameters(coverage, density, feather, scale, clampEdges);
    }
    static ImagePtr vectorPaint(const VectorPaint& paint, const Document& document, const Rect& bounds, const Rect& area, double scale, int w, int h);
    static ImagePtr fillLayer(const Layer& layer, const Document& document);
    static bool adjust(const Document& document, const LayerAdjustment& adjustment, Image& image, const Rect& region, double scale);

    /// The document's buffers at this layout (converted once when they are held in another).
    static ImagePtr image(const Document& document, const AnyImage& image);
    static void solid(const Document& document, const uint8_t rgba[4], Sample* out);
    static GrayPtr gray(const AnyGray& gray) {
        if constexpr (deep) return DeepOps<SampleType::U16>::gray(gray);
        else return gray.u8() ? gray.u8() : (gray.u16() ? GrayPtr(narrowGray(*gray.u16())) : nullptr);
    }
    static std::optional<ImagePtr> overrideImage(const LayerOverride& o) {
        if constexpr (deep) { if (o.image16) return *o.image16; }
        else if constexpr (M == ColorMode::CMYK) { if (o.imageC8) return *o.imageC8; }
        else { if (o.image) return *o.image; }
        return std::nullopt;
    }
    static std::optional<GrayPtr> overrideMask(const LayerOverride& o) {
        if constexpr (deep) return DeepOps<SampleType::U16>::overrideMask(o);
        else { if (o.maskImage) return *o.maskImage; return std::nullopt; }
    }

    static std::shared_ptr<Image>& backdrop(RenderCache& cache) {
        if constexpr (deep) return cache.backdrop16;
        else if constexpr (M == ColorMode::CMYK) return cache.backdropC8;
        else return cache.backdrop;
    }
    static std::shared_ptr<Image>& above(RenderCache& cache) {
        if constexpr (deep) return cache.above16;
        else if constexpr (M == ColorMode::CMYK) return cache.aboveC8;
        else return cache.above;
    }
    static void resetOtherDepths(RenderCache& cache) {
        const void* mine = &backdrop(cache);
        if (mine != &cache.backdrop) { cache.backdrop.reset(); cache.above.reset(); }
        if (mine != &cache.backdrop16) { cache.backdrop16.reset(); cache.above16.reset(); }
        if (mine != &cache.backdropC8) { cache.backdropC8.reset(); cache.aboveC8.reset(); }
    }
};

/// Renders a CMYK or Lab document's plan into `out` at its layout (render_exec_c8.cpp, render_exec_lab8.cpp,
/// render_exec_modes16.cpp).
void executeRenderMode(const RenderPlan& plan, const Rect& region, double scale, ImageC8& out, RenderCache* cache, uint64_t version);
void executeRenderLab8(const RenderPlan& plan, const Rect& region, double scale, Image& out, RenderCache* cache, uint64_t version);
void executeRenderMode16(const RenderPlan& plan, const Rect& region, double scale, Image16& out, RenderCache* cache, uint64_t version);

} // namespace compositor
