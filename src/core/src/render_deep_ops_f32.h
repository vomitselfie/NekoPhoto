// The deep renderer's policy at 32 bits (render_deep_ops.h): premultiplied linear float, coverage in float, nothing
// rounded. render_exec_f32.cpp and render_f32.cpp instantiate the deep templates with it.
//
// Three things depend on the document rather than the depth: the curve its 8-bit colours (a shape's stroke, an
// artboard's background, a layer style's colours) are linearised with, the luminance weights its non-separable
// blend modes use, and the view (exposure, gamma, Highlight Compression) its canvas frame goes through. The policy's
// functions are static, so they read them from the render in progress (`FloatRenderScope`, set by the entry points
// in render_f32.cpp and render.cpp). One 32-bit render runs at a time; renders at other depths are not held up.
//
// Vector coverage, gradient and pattern fills and mask density and feather come from their 16-bit forms (15 bits of
// coverage, colour linearised from the 16-bit ramp): exact float versions are P5e's. Adjustment layers draw through
// adjustments_f32.cpp (P5b); the kinds Photoshop lacks at 32 bits are kept but not drawn.
#pragma once
#include "render_deep_ops.h"
#include "compositor/view32.h"
#include <array>
#include <mutex>

namespace compositor {

/// What a 32-bit render needs to know about its document.
struct FloatRenderContext {
    TransferCurve curve;                 // the document's encoding (encodedTransfer)
    std::array<float, 256> linear8{};    // 8-bit colour levels through it
    float luma[3] = {0.2126f, 0.7152f, 0.0722f};
    View32 view;                         // the canvas frame's view
    float peak = 0;                      // Highlight Compression's white (0: the frame's brightest)
};

/// The context of the 32-bit render in progress on any thread (sRGB and no view when none is).
const FloatRenderContext& floatRenderContext();

/// Makes `document`'s context current for the render it spans; the first on a thread waits for any other thread's
/// 32-bit render to finish. `view` and `peak` set the canvas frame's tone mapping.
class FloatRenderScope {
public:
    explicit FloatRenderScope(const Document& document, const View32& view = {}, float peak = 0);
    ~FloatRenderScope();
    FloatRenderScope(const FloatRenderScope&) = delete;
    FloatRenderScope& operator=(const FloatRenderScope&) = delete;

private:
    FloatRenderContext context_;
    const FloatRenderContext* previous_ = nullptr;
    bool outermost_ = false;
};

template <>
struct DeepOps<SampleType::F32> {
    using Sample = float;
    using Image = ImageF;
    using Gray = GrayF;
    using ImagePtr = ImageFPtr;
    using GrayPtr = GrayFPtr;
    using Params = DrawParamsF;
    using Step = float;

    static constexpr Sample one = 1.0f;
    static constexpr int channels = 4;   // RGB and alpha: 32 bits is RGB only
    static constexpr bool styles = true;

    /// An adjustment layer's blend: straight colour as the RGB policy blends it.
    static void blendStraight(BlendMode mode, const float* cb, float* cs) {
        if (mode == BlendMode::Normal) return;
        const Rgb m = blendColor(mode, {cb[0], cb[1], cb[2]}, {cs[0], cs[1], cs[2]});
        cs[0] = m.r; cs[1] = m.g; cs[2] = m.b;
    }

    static float unit(Sample s) { return s; }
    static Sample mul(Sample a, Sample b) { return a * b; }
    /// Colour over alpha, unbounded (a clipping base's colour may be above 1).
    static Sample unpremultiply(Sample c, Sample a) { return a > 0 ? c / a : 0.0f; }
    /// Blend If reads a 32-bit colour through the document's curve, as its 0..255 sliders show it.
    static int blendIfLevel(Sample c, Sample a) {
        const float v = a > 0 ? std::clamp(c / a, 0.0f, 1.0f) : 0.0f;
        return std::clamp(int(std::lround(floatRenderContext().curve.fromLinear(v) * 255.0f)), 0, 255);
    }
    static Sample coverage(float f) { return cleanCoverage(f); }
    static Sample fade(Sample before, Sample after, float t) { return before + (after - before) * t; }
    static Sample store(float v, Sample) { return cleanColour(v); }
    static Sample from8(uint8_t v) { return floatRenderContext().linear8[v]; }

    static Step steps(float coverage) { return cleanCoverage(coverage); }
    static void span(BlendMode mode, const Sample* src, const Step* steps, Sample* dst, int count) {
        compositeSpanF(mode, src, steps, dst, count, floatRenderContext().luma);
    }
    static void pixelAt(BlendMode mode, const Sample* src, float coverage, Sample* dst, int x, int y) {
        compositePixelAtF(mode, src, coverage, dst, x, y, floatRenderContext().luma);
    }
    static void pixelCovered(BlendMode mode, const Sample* src, Sample coverage, Sample* dst) {
        compositePixelF(mode, src, coverage, dst, floatRenderContext().luma);
    }

    static void drawLayer(const Params& params, const Rect& region, double scale, const Gray* coverage, Image& out) {
        compositor::drawLayer(params, region, scale, coverage, out);
    }
    static void sampleMask(const GrayPtr& mask, const LayerTransform& transform, const Rect& region, double scale, Gray& out) {
        sampleMaskCoverage(mask, transform, region, scale, 0.0f, out, false);
    }
    static std::shared_ptr<Gray> vectorMask(const VectorPath& path, const Rect& region, double scale, int w, int h) {
        return widenGrayF(*rasterizeVectorMask16(path, region, scale, w, h));
    }
    static std::shared_ptr<Gray> vectorStroke(const VectorPath& path, const VectorStroke& stroke, const Rect& region, double scale, int w, int h) {
        return widenGrayF(*rasterizeVectorStroke16(path, stroke, region, scale, w, h));
    }
    static ImagePtr vectorPaint(const VectorPaint& paint, const Document& document, const Rect& bounds, const Rect& area, double scale, int w, int h) {
        Image16Ptr deep = renderVectorPaint16(paint, document, bounds, area, scale, w, h);
        return deep ? ImagePtr(lineariseImage(*deep, floatRenderContext().curve)) : nullptr;
    }
    static void maskParameters(Gray& coverage, std::optional<int> density, std::optional<double> feather, double scale, bool clampEdges = false) {
        auto deep = narrowGrayF16(coverage);
        applyMaskParameters(*deep, density, feather, scale, clampEdges);
        coverage = *widenGrayF(*deep);
    }
    static ImagePtr fillLayer(const Layer& layer, const Document& document) {
        Image16Ptr deep = renderFillLayer16(layer, document);
        return deep ? ImagePtr(lineariseImage(*deep, floatRenderContext().curve)) : nullptr;
    }
    /// Photoshop's 32-bit adjustments (adjustments_f32.cpp) through the document's encoding; a kind it lacks at 32 bits
    /// (Brightness/Contrast, Posterize, Threshold, Selective Color, Grain) is kept but not drawn.
    static bool adjust(const LayerAdjustment& adjustment, Image& image, const Rect& region, double scale) {
        return applyAdjustment(adjustment, image, region, scale, floatRenderContext().curve);
    }
    static void drawStyled(StyledDraw draw, Image& target) {
        draw.linear = &floatRenderContext().curve;
        drawStyledLayer(draw, target);
    }
    static void setStyleSource(StyledDraw& draw, std::function<void(Image&, const Rect&)> source) { draw.drawSourceF = std::move(source); }
    static void setStyleCoverage(StyledDraw& draw, const Gray* coverage) { draw.coverageF = coverage; }

    // Defined in render_exec_f32.cpp: a buffer at another depth in a 32-bit document is converted once and kept.
    static ImagePtr image(const AnyImage& image);
    static GrayPtr gray(const AnyGray& gray);
    static std::optional<ImagePtr> overrideImage(const LayerOverride& o);
    static std::optional<GrayPtr> overrideMask(const LayerOverride& o);

    static std::shared_ptr<Image>& backdrop(RenderCache& cache) { return cache.backdropF; }
    static std::shared_ptr<Image>& above(RenderCache& cache) { return cache.aboveF; }
    static void resetOtherDepths(RenderCache& cache) {
        cache.backdrop.reset(); cache.above.reset(); cache.backdrop16.reset(); cache.above16.reset(); cache.backdropC8.reset(); cache.aboveC8.reset();
    }

    static std::shared_ptr<Image> widenFrom8(const compositor::Image& image) { return lineariseImage(image, floatRenderContext().curve); }
    static std::shared_ptr<compositor::Image> narrowTo8(const Image& image) { return encodeImage8(image, floatRenderContext().curve); }
    static void toDisplay(const Image& deep, compositor::Image& out, const ColorTransform* display) {
        const FloatRenderContext& c = floatRenderContext();
        float peak = c.peak;
        if (c.view.method == ToneMethod::HighlightCompression && !(peak > 0)) peak = peakLuminance(deep, {c.luma[0], c.luma[1], c.luma[2]});
        toDisplayF(deep, out, ToneMap::of(c.view, c.luma, peak), display, c.curve);
    }
};

} // namespace compositor
