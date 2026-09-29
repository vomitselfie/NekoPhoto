// What the deep renderer (render_exec_deep.inc, render_deep.inc) needs from a sample type, as one policy:
// `DeepOps<S>`. The executor and the entry points are written once against it; each deep depth supplies a
// specialisation and instantiates the templates in its own file (render_exec_u16.cpp, render_u16.cpp).
//
// Colour samples and coverage samples share a type per depth (`Sample`): coverage is what masks, folder masks,
// artboards and clipping bases hold. Everything below is small and inline where it runs per pixel, so the
// instantiated executor compiles to the code it replaced.
//
// A specialisation provides:
// - types: `Sample`, `Image`, `Gray`, `ImagePtr`, `GrayPtr`, `Params` (the layer-drawing parameters), `Step`
//   (the per-pixel coverage a blend span takes);
// - samples: `one` (full alpha and full coverage), `unit(s)` (a sample as 0..1 float), `mul(a, b)` (a colour or
//   coverage sample scaled by a coverage sample), `unpremultiply(c, a)` (a colour sample over its alpha, clamped
//   to `one`), `coverage(f)` (0..1 float to a coverage sample, rounded), `fade(before, after, t)` (Pass Through
//   fading), `store(v, alpha)` (a premultiplied 0..1 float back to a sample, clamped to `alpha`), `from8(v)`;
// - blending: `steps(f)` (coverage to a span's `Step`), `span(...)`, `pixelAt(...)` (Dissolve, and paint with a
//   per-pixel float coverage), `pixelCovered(...)` (one pixel at a coverage sample);
// - the renderer's primitives at this depth: `drawLayer`, `sampleMask`, `vectorMask`, `vectorStroke`,
//   `vectorPaint`, `maskParameters`, `fillLayer`, `adjust`, `drawStyled`, and the `StyledDraw` fields
//   (`setStyleSource`, `setStyleCoverage`);
// - where a document's buffers come from: `image(AnyImage)`, `gray(AnyGray)`, the edit overrides
//   (`overrideImage`, `overrideMask`), and the frame cache's slots (`backdrop`, `above`, `resetOtherDepths`);
// - the entry points' conversions: `widenFrom8`, `narrowTo8`, `toDisplay`.
#pragma once
#include "render_plan.h"
#include "layerstyle_render.h"
#include "compositor/adjustments.h"
#include "compositor/blend.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/vectormask.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>

namespace compositor {

template <SampleType S> struct DeepOps;

template <>
struct DeepOps<SampleType::U16> {
    using Sample = uint16_t;
    using Image = Image16;
    using Gray = Gray16;
    using ImagePtr = Image16Ptr;
    using GrayPtr = Gray16Ptr;
    using Params = DrawParams16;
    using Step = uint32_t;

    static constexpr Sample one = Sample(one16);

    static float unit(Sample s) { return s / 32768.0f; }
    static Sample mul(Sample a, Sample b) { return Sample(mul15(a, b)); }
    static Sample unpremultiply(Sample c, Sample a) { return Sample(std::min<uint32_t>(one16, (uint32_t(c) * one16 + a / 2) / a)); }
    static Sample coverage(float f) { return Sample(std::lround(f * 32768)); }
    static Sample fade(Sample before, Sample after, float t) { return Sample(std::lround(before + (float(after) - float(before)) * t)); }
    static Sample store(float v, Sample alpha) { return Sample(clamp(v * 32768.0f + 0.5f, 0.0f, float(alpha))); }
    static Sample from8(uint8_t v) { return widen8(v); }

    static Step steps(float coverage) { return coverageSteps16(coverage); }
    static void span(BlendMode mode, const Sample* src, const Step* steps, Sample* dst, int count) { compositeSpan16(mode, src, steps, dst, count); }
    static void pixelAt(BlendMode mode, const Sample* src, float coverage, Sample* dst, int x, int y) { compositePixelAt16(mode, src, coverage, dst, x, y); }
    static void pixelCovered(BlendMode mode, const Sample* src, Sample coverage, Sample* dst) { compositePixelSteps16(mode, src, coverage, dst); }

    static void drawLayer(const Params& params, const Rect& region, double scale, const Gray* coverage, Image& out) {
        compositor::drawLayer(params, region, scale, coverage, out);
    }
    static void sampleMask(const GrayPtr& mask, const LayerTransform& transform, const Rect& region, double scale, Gray& out) {
        sampleMaskCoverage(mask, transform, region, scale, 0, out, false);
    }
    static std::shared_ptr<Gray> vectorMask(const VectorPath& path, const Rect& region, double scale, int w, int h) {
        return rasterizeVectorMask16(path, region, scale, w, h);
    }
    static std::shared_ptr<Gray> vectorStroke(const VectorPath& path, const VectorStroke& stroke, const Rect& region, double scale, int w, int h) {
        return rasterizeVectorStroke16(path, stroke, region, scale, w, h);
    }
    static ImagePtr vectorPaint(const VectorPaint& paint, const Document& document, const Rect& bounds, const Rect& area, double scale, int w, int h) {
        return renderVectorPaint16(paint, document, bounds, area, scale, w, h);
    }
    static void maskParameters(Gray& coverage, std::optional<int> density, std::optional<double> feather, double scale, bool clampEdges = false) {
        applyMaskParameters(coverage, density, feather, scale, clampEdges);
    }
    static ImagePtr fillLayer(const Layer& layer, const Document& document) { return renderFillLayer16(layer, document); }
    static bool adjust(const LayerAdjustment& adjustment, Image& image, const Rect& region, double scale) {
        return applyAdjustment(adjustment, image, region, scale);
    }
    static void drawStyled(const StyledDraw& draw, Image& target) { drawStyledLayer(draw, target); }
    static void setStyleSource(StyledDraw& draw, std::function<void(Image&, const Rect&)> source) { draw.drawSource16 = std::move(source); }
    static void setStyleCoverage(StyledDraw& draw, const Gray* coverage) { draw.coverage16 = coverage; }

    // Defined in render_exec_u16.cpp: an 8-bit buffer in a 16-bit document is widened once and kept.
    static ImagePtr image(const AnyImage& image);
    static GrayPtr gray(const AnyGray& gray);
    static std::optional<ImagePtr> overrideImage(const LayerOverride& o);
    static std::optional<GrayPtr> overrideMask(const LayerOverride& o);

    static std::shared_ptr<Image>& backdrop(RenderCache& cache) { return cache.backdrop16; }
    static std::shared_ptr<Image>& above(RenderCache& cache) { return cache.above16; }
    static void resetOtherDepths(RenderCache& cache) { cache.backdrop.reset(); cache.above.reset(); }

    static std::shared_ptr<Image> widenFrom8(const compositor::Image& image) { return widenImage(image); }
    static std::shared_ptr<compositor::Image> narrowTo8(const Image& image) { return narrowImage(image); }
    static void toDisplay(const Image& deep, compositor::Image& out, const ColorTransform* display) {
        if (display) convertImage16To8(deep, out, *display);
        else narrowInto(deep, out);
    }
};

} // namespace compositor
