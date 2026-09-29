// The raster side of a BrushStroke at one pixel layout (docs/high-bit-depth-plan.md, P5.0b, P5c, P7 E): the working
// pixels or mask, the coverage grid, the selection, the dab profile table and the stamps, the build-up (max for hard
// tips, screen for soft), the recompose, clone and processed sources, lifting and moving pixels, gradients and the
// healing commit. BrushStroke keeps the depth-free half (the grid's geometry, the curve through the samples, the tail,
// the dirty area) and drives one StrokeRasterOf<Ops> through it.
//
// What differs between layouts is the arithmetic and the buffers, gathered in a policy: how a sample scales by
// another, how a soft dab screens onto the coverage, how a double is stored back, how many samples a pixel has, which
// buffers and sources of a CloneSource or an AnyImage belong to the layout. StrokeRasterOf<Ops> is written once over
// those operations. Every operation keeps the exact expression the separate 8- and 16-bit code had, so both depths
// paint the same bytes as before and the 8-bit stamp merge still vectorises.
//
// The policies:
// - StrokeOps<U8> and StrokeOps<U16> (brush.cpp, brush_u16.cpp): RGB, and Lab (4 samples; L, a and b as stored, a
//   and b offset, the same arithmetic).
// - StrokeOps<F32> (brush_f32.cpp): 32-bit linear float. Float samples, a float table lerp, `screen` as
//   old + t * (1 - old), products and lerps without a rounding term, stores without rounding; colour is not clamped
//   above (a 32-bit document's colour may exceed 1), coverage stays in 0..1 because every input does. The brush colour
//   arrives linearised (BrushStroke's native colour), so the raster sees linear light. Healing works on a 15-bit
//   encoding of the area (healF); Dodge, Burn and Sponge are greyed at 32 bits, as in Photoshop.
// - CmykStrokeOps<U8> and CmykStrokeOps<U16> (brush_modes.cpp): 5 samples (inverted ink, then alpha) with the
//   depth's integer arithmetic on each. The colour arrives as inks through the document's CMYK profile.
// `channels` is a compile-time constant of each policy, so the RGB loops are the code they were.
#pragma once
#include "compositor/brush.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/heal.h"
#include "compositor/parallel.h"
#include "compositor/render.h"
#include "compositor/shape.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace compositor {

namespace stroke {
/// Pixels in a w x h area, without overflow.
inline long long areaOf(int w, int h) { return (long long)(w) * (long long)(h); }

/// Half-open bounds of the pixels with any alpha (the last of `channels()` samples) within `within`.
template <class Img>
PixelBounds alphaBoundsOf(const Img& image, const PixelBounds& within) {
    const int n = image.channels();
    const int x0 = std::max(0, within.x0), y0 = std::max(0, within.y0), x1 = std::min(image.width(), within.x1), y1 = std::min(image.height(), within.y1);
    if (x0 >= x1 || y0 >= y1) return {};
    std::vector<int> first(size_t(y1 - y0), x1), last(size_t(y1 - y0), x0);
    parallelRows(y0, y1, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const auto* row = image.row(y);
            int a = x0;
            while (a < x1 && !(row[a * n + n - 1] > 0)) a++;
            if (a == x1) continue;
            int b = x1;
            while (b > a && !(row[(b - 1) * n + n - 1] > 0)) b--;
            first[size_t(y - y0)] = a;
            last[size_t(y - y0)] = b;
        }
    }, 64);
    int bx0 = x1, by0 = y1, bx1 = x0, by1 = y0;
    for (int y = y0; y < y1; y++) {
        const int a = first[size_t(y - y0)], b = last[size_t(y - y0)];
        if (a >= b) continue;
        bx0 = std::min(bx0, a); bx1 = std::max(bx1, b); by0 = std::min(by0, y); by1 = std::max(by1, y + 1);
    }
    if (bx0 >= bx1 || by0 >= by1) return {};
    return {bx0, by0, bx1, by1};
}

/// A crop of an interleaved buffer into `out` (sized to the crop, its channels the source's); outside is zero.
template <class Img>
std::shared_ptr<Img> cropInto(const Img& image, int x, int y, std::shared_ptr<Img> out) {
    using Sample = std::remove_cv_t<std::remove_pointer_t<decltype(image.row(0))>>;
    const int n = image.channels();
    const int i0 = std::max(0, -x), i1 = std::min(out->width(), image.width() - x);
    if (i1 <= i0) return out;
    for (int j = 0; j < out->height(); j++) {
        const int sy = y + j;
        if (sy < 0 || sy >= image.height()) continue;
        std::memcpy(out->pixel(i0, j), image.pixel(x + i0, sy), size_t(i1 - i0) * size_t(n) * sizeof(Sample));
    }
    return out;
}
} // namespace stroke

/// The depth-specific operations of a stroke raster. Integer depths share the table interpolation.
template <SampleType S> struct StrokeOps;

struct IntegerStrokeOps {
    /// The dab profile between two table entries, `frac` in 1/256 steps, rounded.
    static unsigned lerpTable(unsigned a, unsigned b, unsigned frac) { return (a * (256 - frac) + b * frac + 128) >> 8; }
};

template <> struct StrokeOps<SampleType::U8> : IntegerStrokeOps {
    static constexpr SampleType type = SampleType::U8;
    static constexpr int channels = 4;
    using Sample = uint8_t;
    using Color = Image;
    using Gray = GrayImage;
    using Tiled = TiledSource;
    using Calc = uint32_t;   // a coverage or a sample in arithmetic
    using Paint = int;       // a colour sample as toward() takes it
    static constexpr uint32_t one = 255;
    static constexpr Sample solidCore = 128;   // the healers' solid core: coverage of half and up

    static const ImagePtr& of(const AnyImage& image) { return image.u8(); }
    static const GrayPtr& of(const AnyGray& image) { return image.u8(); }
    static const Color* cloneImage(const CloneSource& clone) { return clone.image.get(); }
    static Tiled* cloneTiled(const CloneSource& clone) { return clone.tiled.get(); }

    /// v scaled by k (both 0..one), rounded.
    static uint32_t mul(uint32_t v, uint32_t k) { return (v * k + 127) / 255; }
    /// A soft dab `t` screened onto the coverage `old`. In int so the stamp merge vectorises.
    static int screen(int old, int t) { return old + ((t * (255 - old) + 127) / 255); }
    /// base towards colour by k, rounded half away from base.
    static int toward(int base, int colour, int k) { return base + ((colour - base) * k + (colour >= base ? 127 : -127)) / 255; }
    /// a towards b by k.
    static uint32_t mix(uint32_t a, uint32_t b, uint32_t k) { return (a * (255 - k) + b * k + 127) / 255; }
    /// A 0..1 value as a sample, rounded.
    static Sample quantise(double unit) { return Sample(clamp(unit * 255 + 0.5, 0.0, 255.0)); }
    /// A sample-scaled double stored back, rounded and clamped.
    static Sample store(double v) { return Sample(clamp(v + 0.5, 0.0, 255.0)); }
    static Sample storeF(float v) { return Sample(clamp(v + 0.5f, 0.0f, 255.0f)); }
    static Sample nearest(double v) { return Sample(std::lround(clamp(v, 0.0, 255.0))); }
    /// A colour in sample units as the plain paint path's target, rounded and clamped.
    static Paint paintValue(double v) { return Paint(clamp(v + 0.5, 0.0, double(one))); }

    static PixelBounds bounds(const Color& image) { return alphaBounds(image); }
    static PixelBounds bounds(const Color& image, const PixelBounds& within) { return alphaBounds(image, within); }
    /// A working copy of the layer's own pixels.
    static std::shared_ptr<Color> copyOf(const Color& image) { return std::make_shared<Color>(image); }
    static std::shared_ptr<Color> make(int width, int height) { return std::make_shared<Color>(width, height); }
    static std::shared_ptr<Color> crop(const Color& image, int x, int y, int w, int h) { return cropImage(image, x, y, w, h); }
    static long long budget() { return Document::imagePixelBudget(type); }
};

template <> struct StrokeOps<SampleType::U16> : IntegerStrokeOps {
    static constexpr SampleType type = SampleType::U16;
    static constexpr int channels = 4;
    using Sample = uint16_t;
    using Color = Image16;
    using Gray = Gray16;
    using Tiled = TiledSource16;
    using Calc = uint32_t;
    using Paint = int;
    static constexpr uint32_t one = one16;
    static constexpr uint32_t half = one16 / 2;
    static constexpr Sample solidCore = widen8(128);

    static const Image16Ptr& of(const AnyImage& image) { return image.u16(); }
    static const Gray16Ptr& of(const AnyGray& image) { return image.u16(); }
    static const Color* cloneImage(const CloneSource& clone) { return clone.image16.get(); }
    static Tiled* cloneTiled(const CloneSource& clone) { return clone.tiled16.get(); }

    static uint32_t mul(uint32_t v, uint32_t k) { return (v * k + half) >> 15; }
    static uint32_t screen(uint32_t old, uint32_t t) { return old + mul(t, one - old); }
    static int toward(int base, int colour, int k) {
        const int delta = colour - base;
        return base + (delta * k + (delta >= 0 ? int(half) : -int(half))) / int(one);
    }
    static uint32_t mix(uint32_t a, uint32_t b, uint32_t k) { return (a * (one - k) + b * k + half) >> 15; }
    static Sample quantise(double unit) { return Sample(clamp(unit * one + 0.5, 0.0, double(one))); }
    static Sample store(double v) { return Sample(clamp(v + 0.5, 0.0, double(one))); }
    static Sample storeF(float v) { return Sample(clamp(v + 0.5f, 0.0f, float(one))); }
    static Sample nearest(double v) { return Sample(std::lround(clamp(v, 0.0, double(one)))); }
    static Paint paintValue(double v) { return Paint(clamp(v + 0.5, 0.0, double(one))); }

    static PixelBounds bounds(const Color& image) { return bounds(image, {0, 0, image.width(), image.height()}); }
    static PixelBounds bounds(const Color& image, const PixelBounds& within);   // brush_u16.cpp
    static std::shared_ptr<Color> copyOf(const Color& image);                    // brush_u16.cpp
    static std::shared_ptr<Color> make(int width, int height) { return std::make_shared<Color>(width, height); }
    static std::shared_ptr<Color> crop(const Color& image, int x, int y, int w, int h) { return cropImage(image, x, y, w, h); }
    static long long budget() { return Document::imagePixelBudget(type); }
};

/// 32-bit float: linear light, colour unbounded above, coverage 0..1.
template <> struct StrokeOps<SampleType::F32> {
    static constexpr SampleType type = SampleType::F32;
    static constexpr int channels = 4;
    using Sample = float;
    using Color = ImageF;
    using Gray = GrayF;
    using Tiled = TiledSourceOf<ImageF>;
    using Calc = float;
    using Paint = float;
    static constexpr float one = 1.0f;
    static constexpr Sample solidCore = 128.0f / 255.0f;   // the same spot as the lower depths heal

    static const ImageFPtr& of(const AnyImage& image) { return image.f32(); }
    static const GrayFPtr& of(const AnyGray& image) { return image.f32(); }
    static const Color* cloneImage(const CloneSource& clone) { return clone.imageF.get(); }
    static Tiled* cloneTiled(const CloneSource& clone) { return clone.tiledF.get(); }

    static float lerpTable(float a, float b, unsigned frac) { return a + (b - a) * (float(frac) * (1.0f / 256)); }
    static float mul(float v, float k) { return v * k; }
    static float screen(float old, float t) { return old + t * (1 - old); }
    static float toward(float base, float colour, float k) { return base + (colour - base) * k; }
    static float mix(float a, float b, float k) { return a * (1 - k) + b * k; }
    static Sample quantise(double unit) { return Sample(clamp(unit, 0.0, 1.0)); }
    /// Stored without rounding; only below zero is cut (colour above 1 is light, not an overflow).
    static Sample store(double v) { return Sample(std::max(0.0, v)); }
    static Sample storeF(float v) { return std::max(0.0f, v); }
    static Sample nearest(double v) { return Sample(std::max(0.0, v)); }
    static Paint paintValue(double v) { return Paint(std::max(0.0, v)); }

    static PixelBounds bounds(const Color& image) { return bounds(image, {0, 0, image.width(), image.height()}); }
    static PixelBounds bounds(const Color& image, const PixelBounds& within) { return stroke::alphaBoundsOf(image, within); }
    static std::shared_ptr<Color> copyOf(const Color& image) { return std::make_shared<Color>(image); }
    static std::shared_ptr<Color> make(int width, int height) { return std::make_shared<Color>(width, height); }
    static std::shared_ptr<Color> crop(const Color& image, int x, int y, int w, int h) { return cropImage(image, x, y, w, h); }
    static long long budget() { return Document::imagePixelBudget(type); }
};

/// CMYK at 8 or 16 bits: 5 samples (inverted ink, then alpha), the depth's integer arithmetic on each.
template <SampleType S> struct CmykStrokeOps;
template <> struct CmykStrokeOps<SampleType::U8> : StrokeOps<SampleType::U8> {
    static constexpr int channels = 5;
    using Color = ImageC8;
    using Tiled = TiledSourceOf<ImageC8>;
    using StrokeOps<SampleType::U8>::of;
    static const ImageC8Ptr& of(const AnyImage& image) { return image.c8(); }
    static const Color* cloneImage(const CloneSource& clone) { return clone.imageC8.get(); }
    static Tiled* cloneTiled(const CloneSource& clone) { return clone.tiledC8.get(); }
    static PixelBounds bounds(const Color& image) { return bounds(image, {0, 0, image.width(), image.height()}); }
    static PixelBounds bounds(const Color& image, const PixelBounds& within) { return stroke::alphaBoundsOf(image, within); }
    static std::shared_ptr<Color> copyOf(const Color& image) { return std::make_shared<Color>(image); }
    static std::shared_ptr<Color> make(int width, int height) { return std::make_shared<Color>(width, height, channels); }
    static std::shared_ptr<Color> crop(const Color& image, int x, int y, int w, int h) { return stroke::cropInto(image, x, y, make(std::max(0, w), std::max(0, h))); }
    static long long budget() { return Document::imagePixelBudget(type, ColorMode::CMYK); }
};
template <> struct CmykStrokeOps<SampleType::U16> : StrokeOps<SampleType::U16> {
    static constexpr int channels = 5;
    static PixelBounds bounds(const Color& image) { return bounds(image, {0, 0, image.width(), image.height()}); }
    static PixelBounds bounds(const Color& image, const PixelBounds& within) { return stroke::alphaBoundsOf(image, within); }
    static std::shared_ptr<Color> copyOf(const Color& image) { return std::make_shared<Color>(image); }
    static std::shared_ptr<Color> make(int width, int height) { return std::make_shared<Color>(width, height, channels); }
    static std::shared_ptr<Color> crop(const Color& image, int x, int y, int w, int h) { return stroke::cropInto(image, x, y, make(std::max(0, w), std::max(0, h))); }
    static long long budget() { return Document::imagePixelBudget(type, ColorMode::CMYK); }
};

/// On a grid aligned with the document, a dab is a precomputed tile at one of 4x4 subpixel phases (2x2 for soft tips
/// over 512 pixels, whose rim hides a quarter pixel), merged row by row (Krita's dab cache). Each phase is built the
/// first time a dab needs it; all are dropped when the tip or the grid scale changes.
template <class T>
struct StampOf {
    int side = 0, steps = 4;   // steps: subpixel positions per axis
    double radius = -1, hardness = -1, scale = 0;
    std::vector<T> tiles[16];
    bool built[16] = {};
};

template <class OpsT>
class StrokeRasterOf {
public:
    using Ops = OpsT;
    static constexpr int N = Ops::channels;   // samples per pixel, the last alpha
    using Sample = typename Ops::Sample;
    using Color = typename Ops::Color;
    using Gray = typename Ops::Gray;
    using Calc = typename Ops::Calc;
    using Paint = typename Ops::Paint;

    explicit StrokeRasterOf(BrushStroke& stroke) : g_(stroke) {}
    /// Sets up the grid and the buffers for `layer` (its pixels, or its mask); false with the stroke's error set.
    bool init(const Layer& layer, bool mask, const Gray* selection);

    void saveTail(const Rect& affected);
    void restoreTail(const Rect& tail);
    void dab(Point center);
    /// Rebuilds the working pixels (or mask) over `r` (inside the grid) from the coverage.
    void recompose(const Rect& r);
    bool liftSelection();
    void moveLifted(Point offset, bool duplicate);
    void fillGradientOver(int shape, Point from, Point to, const GradientStops& stops, double opacity);
    /// A gradient whose stops are already in the layout's samples (fractions of one, straight): `stops` the first three
    /// colour samples (or the gray value), `fourth` the fourth (CMYK's black) in its red.
    void fillGradientNative(int shape, Point from, Point to, const GradientStops& stops, const GradientStops* fourth, double opacity);
    void heal();
    void commit(BrushStroke::Commit& result);
    void refreshLevels(int x0, int y0, int x1, int y1) const;
    /// A 15-bit coverage another engine stamps into (imported tip brushes on a 32-bit stroke), made on first use;
    /// syncCoverage copies it into the float coverage over `r` before a recompose.
    Gray16* proxyCoverage();
    void syncCoverage(const Rect& r);

    std::shared_ptr<const Color> base;     // original pixels in the grid (image strokes); the layer's own image when the grid matches it
    std::shared_ptr<Color> working, lifted;
    std::shared_ptr<Gray> baseMask, workingMask;
    std::shared_ptr<Gray> visible;         // the layer mask's visible pixels on the grid, for the healers
    std::shared_ptr<Gray> coverage;
    std::shared_ptr<Gray> selection;       // selection coverage in grid pixels, if any
    std::shared_ptr<const Gray> maskClone;

private:
    void refreshDabTable(double radius, double hardness, double footprint);
    bool stampDab(Point center, double radius, const Rect& affected);
    void recomposeRows(const Rect& r);
    void healFromClone(const PixelBounds& bounds);
    void healF(const PixelBounds& bounds);
    /// The paint colour in sample units (colour samples, then alpha at one).
    void paintColour(double out[N], bool wash) const;

    BrushStroke& g_;
    /// Dab profile over squared distance (0..one), rebuilt when the tip changes; the dab loop reads it instead of
    /// computing a square root and a falloff per pixel.
    std::vector<Sample> dabTable_;
    double dabTableRadius_ = -1, dabTableHardness_ = -1, dabTableFootprint_ = -1, dabTableScale_ = 0;
    StampOf<Sample> stamp_;
    std::vector<Sample> tailBackup_;
    std::shared_ptr<Gray16> proxy_;
};

template <SampleType S> using StrokeRaster = StrokeRasterOf<StrokeOps<S>>;
template <SampleType S> using CmykStrokeRaster = StrokeRasterOf<CmykStrokeOps<S>>;

namespace stroke {

// Nearest-neighbour stretch of a gray image over a rect of the grid (BrushRaster.draw for masks).
template <class Gray>
void stretchGray(const Gray& source, Gray& target, const Rect& rect) {
    const int x0 = std::max(0, int(std::floor(rect.minX()))), y0 = std::max(0, int(std::floor(rect.minY())));
    const int x1 = std::min(target.width(), int(std::ceil(rect.maxX()))), y1 = std::min(target.height(), int(std::ceil(rect.maxY())));
    for (int y = y0; y < y1; y++) {
        const int sy = clamp(int((y - rect.minY()) / rect.height * source.height()), 0, source.height() - 1);
        for (int x = x0; x < x1; x++) {
            const int sx = clamp(int((x - rect.minX()) / rect.width * source.width()), 0, source.width() - 1);
            target.at(x, y) = source.at(sx, sy);
        }
    }
}

template <int N, class Color>
void copyImage(const Color& source, Color& target, int dx, int dy) {
    using Sample = std::remove_cv_t<std::remove_pointer_t<decltype(source.row(0))>>;
    const int x0 = std::max(0, -dx), x1 = std::min(source.width(), target.width() - dx);
    if (x1 <= x0) return;
    parallelFor(0, source.height(), 256, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const int ty = y + dy;
            if (ty < 0 || ty >= target.height()) continue;
            std::memcpy(target.pixel(x0 + dx, ty), source.pixel(x0, y), size_t(x1 - x0) * N * sizeof(Sample));
        }
    });
}
template <class Color>
void copyImage(const Color& source, Color& target, int dx, int dy) { copyImage<4>(source, target, dx, dy); }

/// A bilinear sample of an image (or an ensured tiled one) at a document point, transparent outside.
template <int N = 4, class Source>
void bilinear(const Source& at, int sw, int sh, double sx, double sy, double s[N]) {
    for (int k = 0; k < N; k++) s[k] = 0;
    const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
    const double fx = sx - ix, fy = sy - iy;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            const int px = ix + i, py = iy + j;
            const double w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            if (w <= 0 || px < 0 || py < 0 || px >= sw || py >= sh) continue;
            const auto* p = at(px, py);
            if (!p) continue;
            for (int k = 0; k < N; k++) s[k] += p[k] * w;
        }
}

} // namespace stroke

template <class OpsT>
bool StrokeRasterOf<OpsT>::init(const Layer& layer, bool mask, const Gray* selectionIn) {
    BrushStroke& g = g_;
    const Gray* ownMask = layer.mask ? Ops::of(layer.mask->asset.image).get() : nullptr;
    const Gray* placedMask = nullptr;
    LayerTransform baseTransform = layer.transform;
    if (mask && layer.mask && layer.mask->placement && ownMask) {
        placedMask = ownMask;
        baseTransform = *layer.mask->placement;
    }
    const int originalWidth = placedMask ? placedMask->width() : layer.pixelWidth();
    const int originalHeight = placedMask ? placedMask->height() : layer.pixelHeight();
    const Affine originalMapping = baseTransform.pixelToDocument(originalWidth, originalHeight);
    const Rect originalBounds(0, 0, originalWidth, originalHeight);
    Rect extent = originalBounds;
    if (!mask) extent = originalBounds.unionWith(originalMapping.inverted().mapBounds(g.canvas_).integral());
    g.width_ = int(extent.width);
    g.height_ = int(extent.height);
    const int width = g.width_, height = g.height_;
    g.sourceRect_ = originalBounds.offsetBy(-extent.minX(), -extent.minY());
    g.pixelToDocument_ = originalMapping.translatedBy(extent.minX(), extent.minY());
    g.documentToPixel_ = g.pixelToDocument_.inverted();
    LayerTransform expanded = baseTransform;
    expanded.size = {double(width) * baseTransform.size.width / originalWidth, double(height) * baseTransform.size.height / originalHeight};
    const Point center = originalMapping.apply({extent.midX(), extent.midY()});
    expanded.origin = {center.x - expanded.size.width / 2, center.y - expanded.size.height / 2};
    g.paintTransform_ = expanded;

    const BrushSettings& settings = g.settings_;
    const Color* own = layer.asset ? Ops::of(layer.asset->image).get() : nullptr;
    if constexpr (requires { own->channels(); }) {
        if (own && own->channels() != N) own = nullptr;   // a buffer of another layout is refused below
    }
    if (width < 1 || height < 1 || double(width) * height > double(Ops::budget()) || originalWidth > maxImageSide || originalHeight > maxImageSide
        || !std::isfinite(settings.diameter) || settings.diameter < 1 || settings.diameter > 2000
        || !std::isfinite(settings.hardness) || settings.hardness < 0 || settings.hardness > 1
        || !std::isfinite(settings.opacity) || settings.opacity < 0.01 || settings.opacity > 1) {
        g.error_ = "This stroke exceeds the supported canvas or brush limits.";
        return false;
    }
    // A deeper stroke refuses pixels of another depth; the 8-bit RGB one has always painted such a layer as blank.
    if constexpr (Ops::type != SampleType::U8 || N != 4) {
        if ((!mask && layer.asset && layer.asset->image && !own) || (mask && layer.mask && layer.mask->asset.image && !ownMask)) {
            g.error_ = "The layer's pixels are not at the document's depth.";
            return false;
        }
    }

    if (mask) {
        baseMask = std::make_shared<Gray>(width, height, Sample(Ops::one));
        if (placedMask || ownMask) stroke::stretchGray(placedMask ? *placedMask : *ownMask, *baseMask, g.sourceRect_);
        workingMask = std::make_shared<Gray>(*baseMask);
    } else {
        // The grid usually is the layer's own pixel grid: then the layer image (immutable, shared) is the base
        // and only the working copy is made.
        const bool sameGrid = own && g.sourceRect_ == Rect(0, 0, width, height) && own->width() == width && own->height() == height;
        if (sameGrid) {
            base = Ops::of(layer.asset->image);
            g.baseBounds_ = Ops::bounds(*base);
            working = Ops::copyOf(*base);
        } else {
            // Base and working copy each get the layer's own pixels; the rest of the grid is zero pages in both,
            // so the working copy is not a copy of the whole grid. A blank layer (no pixels yet) needs no copy and
            // no scan: two images of zero pages, which cost nothing until painted.
            auto copy = Ops::make(width, height);
            auto work = Ops::make(width, height);
            if (own) {
                const int dx = int(g.sourceRect_.minX()), dy = int(g.sourceRect_.minY());
                stroke::copyImage<N>(*own, *copy, dx, dy);
                stroke::copyImage<N>(*own, *work, dx, dy);
                PixelBounds b = Ops::bounds(*own);
                if (!b.isEmpty()) {
                    b = {std::max(0, b.x0 + dx), std::max(0, b.y0 + dy), std::min(width, b.x1 + dx), std::min(height, b.y1 + dy)};
                    g.baseBounds_ = b.isEmpty() ? PixelBounds{} : b;
                }
            }
            base = copy;
            working = work;
        }
        // The healers copy only from what the mask shows: a dab at a cut-out's edge closes with the subject.
        if (layer.mask && layer.mask->enabled && !layer.mask->placement && ownMask && ownMask->width() == originalWidth && ownMask->height() == originalHeight) {
            visible = std::make_shared<Gray>(width, height, Sample(Ops::one));
            stroke::stretchGray(*ownMask, *visible, g.sourceRect_);
        }
    }
    coverage = std::make_shared<Gray>(width, height, Sample(0));
    if (selectionIn && !selectionIn->isEmpty()) {
        // Selection coverage resampled into the grid: outside the canvas nothing is selected.
        selection = std::make_shared<Gray>(width, height, Sample(0));
        for (int y = 0; y < height; y++) {
            Point d = g.pixelToDocument_.apply({0.5, y + 0.5});
            const Point dd = g.pixelToDocument_.applyVector({1, 0});
            for (int x = 0; x < width; x++, d = d + dd) {
                const int sx = int(std::floor(d.x)), sy = int(std::floor(d.y));
                if (sx < 0 || sy < 0 || sx >= selectionIn->width() || sy >= selectionIn->height()) continue;
                selection->at(x, y) = Sample(std::min<Calc>(Calc(selectionIn->at(sx, sy)), Calc(Ops::one)));
            }
        }
    }
    return true;
}

template <class OpsT>
void StrokeRasterOf<OpsT>::refreshLevels(int x0, int y0, int x1, int y1) const {
    if (working) MipCache::shared().refresh(working.get(), x0, y0, x1, y1);
    if (workingMask) MipCache::shared().refresh(workingMask.get(), x0, y0, x1, y1);
}

template <class OpsT>
Gray16* StrokeRasterOf<OpsT>::proxyCoverage() {
    if constexpr (Ops::type != SampleType::F32) return nullptr;
    else {
        if (!proxy_ && coverage) proxy_ = std::make_shared<Gray16>(coverage->width(), coverage->height());
        return proxy_.get();
    }
}

template <class OpsT>
void StrokeRasterOf<OpsT>::syncCoverage(const Rect& r) {
    if constexpr (Ops::type == SampleType::F32) {
        if (!proxy_ || !coverage) return;
        const int x0 = std::max(0, int(r.minX())), y0 = std::max(0, int(r.minY()));
        const int x1 = std::min(coverage->width(), int(std::ceil(r.maxX()))), y1 = std::min(coverage->height(), int(std::ceil(r.maxY())));
        for (int y = y0; y < y1; y++) {
            const uint16_t* in = proxy_->row(y);
            float* out = coverage->row(y);
            for (int x = x0; x < x1; x++) out[x] = std::min<uint32_t>(in[x], one16) * (1.0f / float(one16));
        }
    } else {
        (void)r;
    }
}

template <class OpsT>
void StrokeRasterOf<OpsT>::saveTail(const Rect& affected) {
    const int x0 = int(affected.minX()), y0 = int(affected.minY()), w = int(affected.width), h = int(affected.height);
    tailBackup_.resize(size_t(w) * size_t(h));
    for (int y = 0; y < h; y++) std::memcpy(&tailBackup_[size_t(y) * size_t(w)], coverage->row(y0 + y) + x0, size_t(w) * sizeof(Sample));
}

template <class OpsT>
void StrokeRasterOf<OpsT>::restoreTail(const Rect& tail) {
    const int x0 = int(tail.minX()), y0 = int(tail.minY()), w = int(tail.width), h = int(tail.height);
    for (int y = 0; y < h; y++) std::memcpy(coverage->row(y0 + y) + x0, &tailBackup_[size_t(y) * size_t(w)], size_t(w) * sizeof(Sample));
}

template <class OpsT>
void StrokeRasterOf<OpsT>::refreshDabTable(double radius, double hardness, double footprint) {
    if (dabTableRadius_ == radius && dabTableHardness_ == hardness && dabTableFootprint_ == footprint) return;
    dabTableRadius_ = radius; dabTableHardness_ = hardness; dabTableFootprint_ = footprint;
    const bool hard = hardness >= 1;
    const double inner = radius * hardness;
    const double reach = radius + footprint;   // nothing beyond
    const int entries = 8192;
    dabTableScale_ = entries / (reach * reach);
    dabTable_.assign(size_t(entries) + 2, 0);
    for (int i = 0; i <= entries; i++) {
        const double dist = std::sqrt(i / dabTableScale_);
        double value;
        if (hard) value = clamp((radius - dist) / std::max(1e-9, footprint) + 0.5, 0.0, 1.0);
        else if (dist <= inner) value = 1;
        else if (dist >= radius) value = 0;
        else value = brushFalloff((dist - inner) / std::max(1e-9, radius - inner));
        dabTable_[size_t(i)] = Ops::quantise(value);
    }
}

template <class OpsT>
bool StrokeRasterOf<OpsT>::stampDab(Point center, double radius, const Rect& affected) {
    const BrushStroke& g = g_;
    const Affine& m = g.pixelToDocument_;
    if (!g.settings_.stampedDabs || m.b != 0 || m.c != 0 || m.a != m.d || !(m.a > 0)) return false;
    const double scale = m.a;                      // document units per grid pixel
    const double gridRadius = radius / scale;
    const int side = 2 * int(std::ceil(gridRadius + 1)) + 2;
    if (side > 2600) return false;   // beyond, the general path
    const bool hard = g.settings_.hardness >= 1;
    StampOf<Sample>& stamp = stamp_;
    if (stamp.side != side || stamp.radius != radius || stamp.hardness != g.settings_.hardness || stamp.scale != scale) {
        stamp.side = side; stamp.radius = radius; stamp.hardness = g.settings_.hardness; stamp.scale = scale;
        // A hard rim shows a quarter-pixel shift; a soft one does not, so soft tips over 512 pixels keep 4 tiles.
        stamp.steps = side <= 512 || (hard && side <= 2048) ? 4 : 2;
        for (int phase = 0; phase < 16; phase++) { stamp.tiles[phase].clear(); stamp.tiles[phase].shrink_to_fit(); stamp.built[phase] = false; }
    }
    // Where the tile lands: its centre pixel on the grid pixel under the dab, at the nearest subpixel phase
    // (an eighth of a pixel off at most with quarter steps, a quarter with half steps).
    const int steps = stamp.steps, shift = steps == 4 ? 2 : 1;
    const Point gc = g.documentToPixel_.apply(center);
    const int qx = int(std::floor(gc.x * steps + 0.5)), qy = int(std::floor(gc.y * steps + 0.5));
    const int phase = (qx & (steps - 1)) + (qy & (steps - 1)) * steps;
    const int ox = (qx >> shift) - side / 2, oy = (qy >> shift) - side / 2;
    if (!stamp.built[phase]) {
        // The tip at this phase, from the same profile table the general path reads.
        const double reach2 = (radius + scale) * (radius + scale);
        const double cx = side / 2 + double(phase % steps) / steps, cy = side / 2 + double(phase / steps) / steps;
        std::vector<Sample>& tile = stamp.tiles[phase];
        tile.assign(size_t(side) * size_t(side), 0);
        parallelRows(0, side, [&](int ya, int yb) {
            for (int j = ya; j < yb; j++)
                for (int i = 0; i < side; i++) {
                    const double dx = (i + 0.5 - cx) * scale, dy = (j + 0.5 - cy) * scale, q = dx * dx + dy * dy;
                    if (q >= reach2) continue;
                    const double index = q * dabTableScale_;
                    const int k = int(index);
                    const unsigned frac = unsigned((index - k) * 256);
                    tile[size_t(j) * side + size_t(i)] = Sample(Ops::lerpTable(dabTable_[size_t(k)], dabTable_[size_t(k) + 1], frac));
                }
        }, 64);
        stamp.built[phase] = true;
    }
    // Rows and columns whose pixel centres lie on the canvas and in the affected rect.
    const Rect canvasGrid = g.documentToPixel_.mapBounds(g.canvas_);
    const int x0 = std::max({int(affected.minX()), ox, int(std::ceil(canvasGrid.minX() - 0.5))}), x1 = std::min({int(affected.maxX()), ox + side, int(std::ceil(canvasGrid.maxX() - 0.5))});
    const int y0 = std::max({int(affected.minY()), oy, int(std::ceil(canvasGrid.minY() - 0.5))}), y1 = std::min({int(affected.maxY()), oy + side, int(std::ceil(canvasGrid.maxY() - 0.5))});
    if (x0 >= x1 || y0 >= y1) return true;
    const std::vector<Sample>& tile = stamp.tiles[phase];
    const int n = x1 - x0;
    Gray& cov = *coverage;
    auto merge = [&, n](int ya, int yb) {
        // `n` by value: a byte store may alias anything captured by reference, which would keep the loops scalar.
        for (int y = ya; y < yb; y++) {
            Sample* row = cov.row(y) + x0;
            const Sample* t = &tile[size_t(y - oy) * side + size_t(x0 - ox)];
            if (hard) for (int i = 0; i < n; i++) row[i] = std::max(row[i], t[i]);
            else for (int i = 0; i < n; i++) row[i] = Sample(Ops::screen(row[i], t[i]));
        }
    };
    // A large dab is merged on every core; a small one is quicker than handing it out.
    if (stroke::areaOf(n, y1 - y0) >= 1 << 20) parallelRows(y0, y1, merge, 32); else merge(y0, y1);
    return true;
}

template <class OpsT>
void StrokeRasterOf<OpsT>::dab(Point center) {
    BrushStroke& g = g_;
    const double radius = g.settings_.diameter / 2;
    const Rect circle(center.x - radius, center.y - radius, radius * 2, radius * 2);
    const Rect clipped = circle.intersection(g.canvas_);
    if (clipped.isEmpty()) return;
    const Rect affected = g.documentToPixel_.mapBounds(clipped).integral().intersection(Rect(0, 0, g.width_, g.height_));
    if (affected.isEmpty()) return;
    // Document units per grid pixel, for antialiasing the rim.
    const double footprint = std::hypot(g.pixelToDocument_.a, g.pixelToDocument_.b);
    refreshDabTable(radius, g.settings_.hardness, footprint);
    if (stampDab(center, radius, affected)) { g.markDirty(affected); return; }
    const bool hard = g.settings_.hardness >= 1;
    const bool whollyInside = clipped == circle;
    const double reach2 = (radius + footprint) * (radius + footprint);
    const int x0 = int(affected.minX()), x1 = int(affected.maxX()), y0 = int(affected.minY()), y1 = int(affected.maxY());
    const Affine& toDocument = g.pixelToDocument_;
    const Rect& canvas = g.canvas_;
    const Point dd = toDocument.applyVector({1, 0});
    const double dd2 = dd.x * dd.x + dd.y * dd.y;
    auto rows = [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            Sample* row = coverage->row(y);
            Point d = toDocument.apply({x0 + 0.5, y + 0.5});
            // Squared distance to the centre is a quadratic along the row: step it with first and second differences.
            const double rx = d.x - center.x, ry = d.y - center.y;
            double q = rx * rx + ry * ry;
            double dq = 2 * (rx * dd.x + ry * dd.y) + dd2;
            for (int x = x0; x < x1; x++, q += dq, dq += 2 * dd2, d = d + dd) {
                if (q >= reach2) continue;
                if (!whollyInside && !canvas.contains(d)) continue;
                const double index = q * dabTableScale_;
                const int i = int(index);
                const unsigned frac = unsigned((index - i) * 256);
                const Calc value = Ops::lerpTable(dabTable_[size_t(i)], dabTable_[size_t(i) + 1], frac);
                if (value == 0) continue;
                const Calc old = row[x];
                row[x] = Sample(hard ? std::max(old, value) : Calc(Ops::screen(old, value)));
            }
        }
    };
    if (stroke::areaOf(x1 - x0, y1 - y0) >= 65536) parallelRows(y0, y1, rows, 32); else rows(y0, y1);
    g.markDirty(affected);
}

template <class OpsT>
void StrokeRasterOf<OpsT>::recompose(const Rect& r) {
    const BrushStroke& g = g_;
    typename Ops::Tiled* tiled = g.clone_ ? Ops::cloneTiled(*g.clone_) : nullptr;
    if (tiled) {
        // The document pixels the bilinear samples under this area read, made before the rows run in parallel.
        const Rect grid = r.integral();
        double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
        for (double gx : {grid.minX(), grid.maxX()})
            for (double gy : {grid.minY(), grid.maxY()}) {
                const Point d = g.pixelToDocument_.apply({gx, gy});
                minX = std::min(minX, d.x); maxX = std::max(maxX, d.x); minY = std::min(minY, d.y); maxY = std::max(maxY, d.y);
            }
        const double ox = g.clone_->offset.x - 0.5, oy = g.clone_->offset.y - 0.5;
        const int ex0 = int(std::floor(minX + ox)) - 2, ey0 = int(std::floor(minY + oy)) - 2, ex1 = int(std::ceil(maxX + ox)) + 3, ey1 = int(std::ceil(maxY + oy)) + 3;
        tiled->ensure(ex0, ey0, ex1, ey1);
    }
    // Rows are independent: a large area (a big brush's dab) is recomposed on every core.
    if (stroke::areaOf(int(r.width), int(r.height)) < 65536) { recomposeRows(r); return; }
    parallelRows(int(r.minY()), int(r.maxY()), [&](int ya, int yb) { recomposeRows(Rect(r.minX(), ya, r.width, yb - ya)); }, 32);
}

template <class OpsT>
void StrokeRasterOf<OpsT>::paintColour(double out[N], bool wash) const {
    const BrushStroke& g = g_;
    constexpr double one = Ops::one;
    const BrushSettings& settings = g.settings_;
    const float rgb[3] = {wash ? 0.12f : float(settings.red), wash ? 0.12f : float(settings.green), wash ? 0.12f : float(settings.blue)};
    if (g.toNative_) {
        double native[4] = {0, 0, 0, 0};
        g.toNative_(rgb, native);
        for (int c = 0; c < N - 1; c++) out[c] = native[c] * one;
    } else if (wash) {
        for (int c = 0; c < N - 1; c++) out[c] = 0.12 * one;
    } else {
        out[0] = settings.red * one; out[1] = settings.green * one; out[2] = settings.blue * one;
    }
    out[N - 1] = one;
}

template <class OpsT>
void StrokeRasterOf<OpsT>::recomposeRows(const Rect& r) {
    const BrushStroke& g = g_;
    const BrushSettings& settings = g.settings_;
    const int x0 = int(r.minX()), x1 = int(r.maxX()), y0 = int(r.minY()), y1 = int(r.maxY());
    constexpr double one = Ops::one;
    double opacity = settings.opacity;
    const Gray* sel = selection.get();
    if (g.isMask_) {
        const double paint = Ops::quantise(settings.maskValue);
        const Gray* sample = maskClone.get();
        for (int y = y0; y < y1; y++) {
            const Sample* cov = coverage->row(y);
            const Sample* selRow = sel ? sel->row(y) : nullptr;
            const Sample* baseRow = baseMask->row(y);
            Sample* out = workingMask->row(y);
            Point d = g.pixelToDocument_.apply({x0 + 0.5, y + 0.5});
            const Point dd = g.pixelToDocument_.applyVector({1, 0});
            for (int x = x0; x < x1; x++, d = d + dd) {
                const double c = cov[x] / one * opacity * (selRow ? selRow[x] / one : 1.0);
                double value = paint;
                if (sample) {
                    // The sample under the document point, bilinear.
                    const double sx = d.x - 0.5, sy = d.y - 0.5;
                    const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                    const double fx = sx - ix, fy = sy - iy;
                    double acc = 0, wsum = 0;
                    for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
                        const int px = ix + i, py = iy + j;
                        const double w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                        if (w <= 0 || px < 0 || py < 0 || px >= sample->width() || py >= sample->height()) continue;
                        acc += sample->at(px, py) * w; wsum += w;
                    }
                    value = wsum > 0 ? acc / wsum : baseRow[x];
                }
                out[x] = Ops::store(baseRow[x] * (1 - c) + value * c);
            }
        }
        return;
    }
    const bool wash = settings.healing && !g.clone_;   // the wash shown while painting (the Healing Brush shows its source)
    double paint[N];
    paintColour(paint, wash);
    if (wash) opacity *= 0.45;
    if (!g.clone_) {
        // Plain paint or erase: integer lerps, one coverage step per pixel.
        const Calc op = Ops::quantise(opacity);
        Paint colour[N];
        for (int c = 0; c < N - 1; c++) colour[c] = Ops::paintValue(paint[c]);
        colour[N - 1] = Paint(Ops::one);
        const bool erasing = settings.erasing;
        for (int y = y0; y < y1; y++) {
            const Sample* cov = coverage->row(y);
            const Sample* selRow = sel ? sel->row(y) : nullptr;
            const Sample* baseRow = base->row(y) + x0 * N;
            Sample* out = working->row(y) + x0 * N;
            for (int x = x0; x < x1; x++, baseRow += N, out += N) {
                Calc k = cov[x];
                if (selRow) k = Ops::mul(k, selRow[x]);
                k = Ops::mul(k, op);
                if (k == 0) { std::memcpy(out, baseRow, N * sizeof(Sample)); continue; }
                if (erasing) { for (int c = 0; c < N; c++) out[c] = Sample(Ops::mul(baseRow[c], Ops::one - k)); continue; }
                for (int c = 0; c < N; c++) out[c] = Sample(Ops::toward(Paint(baseRow[c]), colour[c], Paint(k)));
            }
        }
        return;
    }
    const Color* src = Ops::cloneImage(*g.clone_);
    const typename Ops::Tiled* tiled = Ops::cloneTiled(*g.clone_);
    const int sw = src ? src->width() : tiled ? tiled->width() : 0, sh = src ? src->height() : tiled ? tiled->height() : 0;
    auto at = [&](int px, int py) -> const Sample* { return src ? src->pixel(px, py) : tiled->pixel(px, py); };
    for (int y = y0; y < y1; y++) {
        const Sample* cov = coverage->row(y);
        const Sample* selRow = sel ? sel->row(y) : nullptr;
        const Sample* baseRow = base->row(y) + x0 * N;
        Sample* out = working->row(y) + x0 * N;
        Point d = g.pixelToDocument_.apply({x0 + 0.5, y + 0.5});
        const Point dd = g.pixelToDocument_.applyVector({1, 0});
        for (int x = x0; x < x1; x++, baseRow += N, out += N, d = d + dd) {
            const double c = cov[x] / one * opacity * (selRow ? selRow[x] / one : 1.0);
            if (c <= 0) { std::memcpy(out, baseRow, N * sizeof(Sample)); continue; }
            if (src || tiled) {
                // The sample under the source point, over the original through the tip.
                double s[N];
                stroke::bilinear<N>(at, sw, sh, d.x + g.clone_->offset.x - 0.5, d.y + g.clone_->offset.y - 0.5, s);
                const double sa = g.replacesWithClone_ ? c : s[N - 1] / one * c;
                for (int k = 0; k < N; k++) out[k] = Ops::store(s[k] * c + baseRow[k] * (1 - sa));
                continue;
            }
            if (settings.erasing) {
                for (int k = 0; k < N; k++) out[k] = Ops::store(baseRow[k] * (1 - c));
            } else {
                for (int k = 0; k < N; k++) out[k] = Ops::store(baseRow[k] * (1 - c) + paint[k] * c);
            }
        }
    }
}

template <class OpsT>
bool StrokeRasterOf<OpsT>::liftSelection() {
    BrushStroke& g = g_;
    if (g.isMask_ || !selection || !base) return false;
    const PixelBounds b = nonzeroBounds(*selection);
    const Rect region = b.isEmpty() ? Rect() : Rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0).intersection(g.sourceRect_).integral();
    if (region.isEmpty()) return false;
    g.liftedRect_ = region;
    lifted = Ops::make(int(region.width), int(region.height));
    bool any = false;
    for (int y = 0; y < lifted->height(); y++)
        for (int x = 0; x < lifted->width(); x++) {
            const int sx = x + int(region.x), sy = y + int(region.y);
            const Calc k = selection->at(sx, sy);
            const Sample* s = base->pixel(sx, sy);
            Sample* d = lifted->pixel(x, y);
            for (int c = 0; c < N; c++) d[c] = Sample(Ops::mul(s[c], k));
            if (d[N - 1]) any = true;
        }
    return any;
}

template <class OpsT>
void StrokeRasterOf<OpsT>::moveLifted(Point offset, bool duplicate) {
    BrushStroke& g = g_;
    if (!lifted || !selection) return;
    const int width = g.width_, height = g.height_;
    // The offset in grid pixels (whole document pixels may land between grid pixels on a scaled layer).
    const Point zero = g.documentToPixel_.apply({0, 0}), moved = g.documentToPixel_.apply(offset);
    const double gx = moved.x - zero.x, gy = moved.y - zero.y;
    working = std::make_shared<Color>(*base);
    if (!duplicate)
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                const Calc k = selection->at(x, y);
                if (!k) continue;
                Sample* p = working->pixel(x, y);
                for (int c = 0; c < N; c++) p[c] = Sample(Ops::mul(p[c], Ops::one - std::min(k, Calc(Ops::one))));
            }
    const bool whole = std::fabs(gx - std::round(gx)) < 1e-6 && std::fabs(gy - std::round(gy)) < 1e-6;
    const double tx = g.liftedRect_.x + gx, ty = g.liftedRect_.y + gy;
    const int lw = lifted->width(), lh = lifted->height();
    const Rect target = Rect(tx, ty, lw, lh).insetBy(-1, -1).integral().intersection(Rect(0, 0, width, height));
    for (int y = int(target.minY()); y < int(target.maxY()); y++)
        for (int x = int(target.minX()); x < int(target.maxX()); x++) {
            const double sx = x - tx, sy = y - ty;
            float s[N] = {};
            if (whole) {
                const int ix = int(std::lround(sx)), iy = int(std::lround(sy));
                if (ix < 0 || iy < 0 || ix >= lw || iy >= lh) continue;
                const Sample* p = lifted->pixel(ix, iy);
                for (int c = 0; c < N; c++) s[c] = p[c];
            } else {
                // The 8-bit path has always centred the sample this way (its rounding kept for identical pixels).
                double bxf = sx, byf = sy;
                if constexpr (Ops::type == SampleType::U8) { bxf = sx - 0.5 + 0.5; byf = sy - 0.5 + 0.5; }
                const int bx = int(std::floor(bxf)), by = int(std::floor(byf));
                const float fx = float(bxf - bx), fy = float(byf - by);
                for (int j = 0; j < 2; j++)
                    for (int i = 0; i < 2; i++) {
                        const int px = bx + i, py = by + j;
                        const float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                        if (w <= 0 || px < 0 || py < 0 || px >= lw || py >= lh) continue;
                        const Sample* p = lifted->pixel(px, py);
                        for (int c = 0; c < N; c++) s[c] += p[c] * w;
                    }
            }
            if (s[N - 1] <= 0) continue;
            Sample* d = working->pixel(x, y);
            const float a = s[N - 1] / float(Ops::one);
            for (int c = 0; c < N; c++) d[c] = Ops::storeF(s[c] + d[c] * (1 - a));
        }
    g.touched_ = true;
    g.dirtyGrid_ = {};
    refreshLevels(0, 0, width, height);
}

template <class OpsT>
void StrokeRasterOf<OpsT>::fillGradientOver(int shape, Point from, Point to, const GradientStops& stops, double opacity) {
    BrushStroke& g = g_;
    g.touched_ = true;
    g.dirtyGrid_ = {}; // the working image is composed here, not from the coverage
    // Pixels outside the canvas are left alone: the canvas as grid coverage, times the selection.
    Gray inside(g.width_, g.height_, Sample(0));
    for (int y = 0; y < g.height_; y++) {
        Point d = g.pixelToDocument_.apply({0.5, y + 0.5});
        const Point dd = g.pixelToDocument_.applyVector({1, 0});
        for (int x = 0; x < g.width_; x++, d = d + dd)
            if (g.canvas_.contains(d)) inside.at(x, y) = selection ? selection->at(x, y) : Sample(Ops::one);
    }
    if constexpr (Ops::type == SampleType::F32) {
        (void)stops; (void)opacity; (void)shape; (void)from; (void)to;   // the float gradient goes through fillGradientNative
    } else {
        if (g.isMask_) fillGradient(*baseMask, *workingMask, g.pixelToDocument_, GradientShape(shape), from, to, stops, opacity, &inside);
        else if constexpr (N == 4) fillGradient(*base, *working, g.pixelToDocument_, GradientShape(shape), from, to, stops, opacity, &inside);
    }
    refreshLevels(0, 0, g.width_, g.height_);
}

template <class OpsT>
void StrokeRasterOf<OpsT>::fillGradientNative(int shape, Point from, Point to, const GradientStops& stops, const GradientStops* fourth, double opacity) {
    BrushStroke& g = g_;
    g.touched_ = true;
    g.dirtyGrid_ = {};
    constexpr double one = Ops::one;
    const GradientShape kind = GradientShape(shape);
    // Each pixel's colour from the stops at its position, over the original by the stop's alpha, the opacity, the
    // canvas and the selection; in the layout's own samples (linear light at 32 bits, inks in CMYK, L a b in Lab).
    parallelRows(0, g.height_, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            Point d = g.pixelToDocument_.apply({0.5, y + 0.5});
            const Point dd = g.pixelToDocument_.applyVector({1, 0});
            for (int x = 0; x < g.width_; x++, d = d + dd) {
                if (!g.canvas_.contains(d)) continue;
                const double t = gradientPositionAt(kind, from, to, d);
                float col[4], extra[4] = {0, 0, 0, 0};
                stops.sample(float(t), col);
                if (fourth) fourth->sample(float(t), extra);
                const double cov = opacity * (selection ? selection->at(x, y) / one : 1.0);
                if (g.isMask_) {
                    const double k = cov * col[3];
                    workingMask->at(x, y) = Ops::store(baseMask->at(x, y) * (1 - k) + col[0] * one * k);
                    continue;
                }
                const double a = col[3] * cov;
                const Sample* b = base->pixel(x, y);
                Sample* o = working->pixel(x, y);
                if (a <= 0) { std::memcpy(o, b, N * sizeof(Sample)); continue; }
                double value[N];
                for (int c = 0; c < N - 1; c++) value[c] = c < 3 ? col[c] : extra[0];
                value[N - 1] = 1;
                for (int c = 0; c < N; c++) o[c] = Ops::store(value[c] * a * one + b[c] * (1 - a));
            }
        }
    }, 16);
    refreshLevels(0, 0, g.width_, g.height_);
}

template <class OpsT>
void StrokeRasterOf<OpsT>::heal() {
    const BrushStroke& g = g_;
    if (!g.settings_.healing || g.isMask_ || !coverage) return;
    const PixelBounds b = nonzeroBounds(*coverage);
    if (b.isEmpty()) return;
    if constexpr (N != 4) {
        return;   // CMYK healing is not ported yet (greyed in supports())
    } else if constexpr (Ops::type == SampleType::F32) {
        healF(b);
    } else {
        if (g.clone_ && Ops::cloneImage(*g.clone_)) { healFromClone(b); return; }
        // Room for the kernel's patch search, which looks up to about three spot-widths away.
        const double reach = (std::max(b.x1 - b.x0, b.y1 - b.y0) + 32) * 3.2;
        const Rect region = Rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0).insetBy(-reach, -reach).intersection(Rect(0, 0, g.width_, g.height_)).integral();
        const int rx = int(region.x), ry = int(region.y), rw = int(region.width), rh = int(region.height);
        if (rw <= 0 || rh <= 0) return;
        auto pixels = cropImage(*base, rx, ry, rw, rh);
        auto painting = cropGray(*coverage, rx, ry, rw, rh);
        if (selection)
            for (int y = 0; y < rh; y++)
                for (int x = 0; x < rw; x++) painting->at(x, y) = Sample(Ops::mul(painting->at(x, y), selection->at(x + rx, y + ry)));
        // The kernel treats any touched pixel as the hole, so a soft tip would make its own faint rim part of
        // the hole and leave it half healed. Heal the solid core only, then feather the result in by coverage.
        auto core = std::make_shared<Gray>(rw, rh);
        for (int y = 0; y < rh; y++)
            for (int x = 0; x < rw; x++) core->at(x, y) = painting->at(x, y) >= Ops::solidCore ? Sample(Ops::one) : Sample(0);
        std::shared_ptr<Gray> visibleCrop = visible ? cropGray(*visible, rx, ry, rw, rh) : nullptr;
        spotHeal(*pixels, *core, float(g.settings_.opacity), g.settings_.healingMode, g.settings_.healingSeed, visibleCrop.get());
        // The healed pixels replace the wash: the working image becomes the original with the healed region.
        working = std::make_shared<Color>(*base);
        for (int y = 0; y < rh; y++) {
            Sample* dst = working->pixel(rx, y + ry);
            const Sample* healed = pixels->row(y);
            const Sample* orig = base->pixel(rx, y + ry);
            for (int x = 0; x < rw; x++, dst += 4, healed += 4, orig += 4) {
                const uint32_t k = std::min<uint32_t>(painting->at(x, y), Ops::one);
                if (k == 0) continue;
                if (k >= Ops::one) { std::memcpy(dst, healed, 4 * sizeof(Sample)); continue; }
                for (int c = 0; c < 4; c++) dst[c] = Sample(Ops::mix(orig[c], healed[c], k));
            }
        }
        refreshLevels(0, 0, g.width_, g.height_);
    }
}

template <class OpsT>
void StrokeRasterOf<OpsT>::healFromClone(const PixelBounds& b) {
    if constexpr (N == 4 && Ops::type != SampleType::F32) {
        // The Healing Brush: the source under the stroke (as Clone Stamp samples it), its tone matched to the edge.
        const BrushStroke& g = g_;
        const Rect region = Rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0).insetBy(-2, -2).intersection(Rect(0, 0, g.width_, g.height_)).integral();
        const int rx = int(region.x), ry = int(region.y), rw = int(region.width), rh = int(region.height);
        if (rw <= 0 || rh <= 0) return;
        auto pixels = cropImage(*base, rx, ry, rw, rh);
        auto painting = cropGray(*coverage, rx, ry, rw, rh);
        if (selection)
            for (int y = 0; y < rh; y++)
                for (int x = 0; x < rw; x++) painting->at(x, y) = Sample(Ops::mul(painting->at(x, y), selection->at(x + rx, y + ry)));
        Color source(rw, rh);
        const Color& src = *Ops::cloneImage(*g.clone_);
        auto at = [&](int px, int py) -> const Sample* { return src.pixel(px, py); };
        for (int y = 0; y < rh; y++) {
            Point d = g.pixelToDocument_.apply({rx + 0.5, y + ry + 0.5});
            const Point dd = g.pixelToDocument_.applyVector({1, 0});
            for (int x = 0; x < rw; x++, d = d + dd) {
                double s[4];
                stroke::bilinear(at, src.width(), src.height(), d.x + g.clone_->offset.x - 0.5, d.y + g.clone_->offset.y - 0.5, s);
                Sample* out = source.pixel(x, y);
                for (int k = 0; k < 4; k++) out[k] = Ops::nearest(s[k]);
            }
        }
        std::shared_ptr<Gray> visibleCrop = visible ? cropGray(*visible, rx, ry, rw, rh) : nullptr;
        healFrom(*pixels, source, *painting, float(g.settings_.opacity), visibleCrop.get());
        working = std::make_shared<Color>(*base);
        for (int y = 0; y < rh; y++) std::memcpy(working->pixel(rx, y + ry), pixels->row(y), size_t(rw) * 4 * sizeof(Sample));
        refreshLevels(0, 0, g.width_, g.height_);
    } else {
        (void)b;
    }
}

template <class OpsT>
void StrokeRasterOf<OpsT>::healF(const PixelBounds& b) {
    if constexpr (Ops::type == SampleType::F32) {
        // At 32 bits the healers work on the area encoded through the document's curve at 15 bits (what decides the
        // patch is the picture as exposure 0 shows it, as the wand's decisionImage), scaled first under its
        // brightest straight colour when that is above 1, so light above white heals as light and not as a clipped
        // white. The healed pixels are decoded back and blended in by coverage; pixels the stroke did not cover keep
        // their exact floats.
        const BrushStroke& g = g_;
        const TransferCurve curve = g.curve_ ? *g.curve_ : TransferCurve::srgb();
        const bool fromClone = g.clone_ && g.clone_->imageF;
        const double reach = fromClone ? 2 : (std::max(b.x1 - b.x0, b.y1 - b.y0) + 32) * 3.2;
        const Rect region = Rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0).insetBy(-reach, -reach).intersection(Rect(0, 0, g.width_, g.height_)).integral();
        const int rx = int(region.x), ry = int(region.y), rw = int(region.width), rh = int(region.height);
        if (rw <= 0 || rh <= 0) return;
        auto pixels = cropImage(*base, rx, ry, rw, rh);
        std::shared_ptr<ImageF> source;
        if (fromClone) {
            source = std::make_shared<ImageF>(rw, rh);
            const ImageF& src = *g.clone_->imageF;
            auto at = [&](int px, int py) -> const float* { return src.pixel(px, py); };
            for (int y = 0; y < rh; y++) {
                Point d = g.pixelToDocument_.apply({rx + 0.5, y + ry + 0.5});
                const Point dd = g.pixelToDocument_.applyVector({1, 0});
                for (int x = 0; x < rw; x++, d = d + dd) {
                    double s[4];
                    stroke::bilinear(at, src.width(), src.height(), d.x + g.clone_->offset.x - 0.5, d.y + g.clone_->offset.y - 0.5, s);
                    float* out = source->pixel(x, y);
                    for (int k = 0; k < 4; k++) out[k] = float(std::max(0.0, s[k]));
                }
            }
        }
        // The brightest straight colour of what the healers read.
        float peak = 1;
        for (const ImageF* image : {pixels.get(), source.get()}) {
            if (!image) continue;
            for (int y = 0; y < rh; y++) {
                const float* p = image->row(y);
                for (int x = 0; x < rw; x++, p += 4)
                    if (p[3] > 0) for (int c = 0; c < 3; c++) peak = std::max(peak, p[c] / p[3]);
            }
        }
        auto scaled = [&](const ImageF& image) {
            if (peak <= 1) return encodeImage16(image, curve);
            ImageF down(image);
            for (int y = 0; y < rh; y++) {
                float* p = down.row(y);
                for (int x = 0; x < rw; x++, p += 4) for (int c = 0; c < 3; c++) p[c] /= peak;
            }
            return encodeImage16(down, curve);
        };
        std::shared_ptr<Image16> encoded = scaled(*pixels);
        Gray16 painting(rw, rh);
        for (int y = 0; y < rh; y++)
            for (int x = 0; x < rw; x++) {
                float k = coverage->at(x + rx, y + ry);
                if (selection) k *= selection->at(x + rx, y + ry);
                painting.at(x, y) = uint16_t(std::lround(clamp(k, 0.0f, 1.0f) * float(one16)));
            }
        std::shared_ptr<Gray16> visibleCrop;
        if (visible) {
            visibleCrop = std::make_shared<Gray16>(rw, rh);
            for (int y = 0; y < rh; y++)
                for (int x = 0; x < rw; x++) visibleCrop->at(x, y) = uint16_t(std::lround(clamp(visible->at(x + rx, y + ry), 0.0f, 1.0f) * float(one16)));
        }
        if (fromClone) healFrom(*encoded, *scaled(*source), painting, float(g.settings_.opacity), visibleCrop.get());
        else {
            Gray16 core(rw, rh);
            for (int y = 0; y < rh; y++)
                for (int x = 0; x < rw; x++) core.at(x, y) = painting.at(x, y) >= StrokeOps<SampleType::U16>::solidCore ? uint16_t(one16) : uint16_t(0);
            spotHeal(*encoded, core, float(g.settings_.opacity), g.settings_.healingMode, g.settings_.healingSeed, visibleCrop.get());
        }
        std::shared_ptr<ImageF> healed = lineariseImage(*encoded, curve);
        working = std::make_shared<Color>(*base);
        for (int y = 0; y < rh; y++) {
            float* dst = working->pixel(rx, y + ry);
            const float* h = healed->row(y);
            const float* orig = base->pixel(rx, y + ry);
            for (int x = 0; x < rw; x++, dst += 4, h += 4, orig += 4) {
                const float k = painting.at(x, y) / float(one16);
                if (k <= 0) continue;
                float value[4] = {h[0] * peak, h[1] * peak, h[2] * peak, h[3]};
                // The Healing Brush replaces the area (healFrom blends by coverage itself); spot healing's core is
                // feathered in by coverage.
                const float w = fromClone ? 1.0f : k;
                for (int c = 0; c < 4; c++) dst[c] = std::max(0.0f, orig[c] * (1 - w) + value[c] * w);
            }
        }
        refreshLevels(0, 0, g.width_, g.height_);
    } else {
        (void)b;
    }
}

template <class OpsT>
void StrokeRasterOf<OpsT>::commit(BrushStroke::Commit& result) {
    const BrushStroke& g = g_;
    if (g.isMask_) {
        result.mask = MaskAsset::make(std::shared_ptr<const Gray>(workingMask));
        result.maskPlacement = g.paintTransform_.samePlacement(g.layerTransform_) ? std::nullopt : std::optional<LayerTransform>(g.paintTransform_);
        return;
    }
    const int width = g.width_, height = g.height_;
    const PixelBounds& baseBounds = g.baseBounds_;
    // Keep every nonzero-alpha pixel; an existing layer keeps at least its old bounds. Outside the base's
    // own alpha and the touched area nothing changed, so only that region needs scanning.
    const Rect scan = Rect(baseBounds.x0, baseBounds.y0, baseBounds.x1 - baseBounds.x0, baseBounds.y1 - baseBounds.y0).unionWith(g.touchedGrid_).integral();
    const PixelBounds b = scan.isEmpty() ? PixelBounds{} : Ops::bounds(*working, PixelBounds{int(scan.minX()), int(scan.minY()), int(scan.maxX()), int(scan.maxY())});
    Rect crop = b.isEmpty() ? Rect() : Rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
    const bool hadSource = base && !baseBounds.isEmpty();
    if (hadSource) crop = crop.unionWith(g.sourceRect_);
    if (crop.isEmpty()) crop = hadSource ? g.sourceRect_ : Rect(0, 0, width, height);
    crop = crop.integral();
    std::shared_ptr<Color> image = (crop == Rect(0, 0, width, height)) ? working : Ops::crop(*working, int(crop.minX()), int(crop.minY()), int(crop.width), int(crop.height));
    result.asset = Asset::make(std::shared_ptr<const Color>(image), g.name_);
    const Point center = g.pixelToDocument_.apply({crop.midX(), crop.midY()});
    LayerTransform t = g.paintTransform_;
    t.size = {crop.width * g.paintTransform_.size.width / width, crop.height * g.paintTransform_.size.height / height};
    t.origin = {center.x - t.size.width / 2, center.y - t.size.height / 2};
    result.transform = t;
}

} // namespace compositor
