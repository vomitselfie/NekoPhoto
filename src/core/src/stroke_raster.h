// The raster side of a BrushStroke at one sample depth (docs/high-bit-depth-plan.md, P5.0b): the working pixels or
// mask, the coverage grid, the selection, the dab profile table and the stamps, the build-up (max for hard tips,
// screen for soft), the recompose, clone and processed sources, lifting and moving pixels, gradients and the healing
// commit. BrushStroke keeps the depth-free half (the grid's geometry, the curve through the samples, the tail, the
// dirty area) and drives one StrokeRaster<S> through it.
//
// What differs between depths is the arithmetic, gathered in StrokeOps<S>: how a sample scales by another, how a soft
// dab screens onto the coverage, how a double is stored back, which buffers and sources of a CloneSource or an
// AnyImage belong to the depth. StrokeRaster<S> is written once over those operations; brush.cpp instantiates it for
// U8 and brush_u16.cpp for U16. Every operation keeps the exact expression the separate 8- and 16-bit code had, so both
// depths paint the same bytes as before and the 8-bit stamp merge still vectorises.
//
// F32 (P5c) needs a StrokeOps<SampleType::F32> and not a new raster:
// - Sample float, Color ImageF, Gray GrayF, a TiledSourceOf<ImageF>, and CloneSource and the mask clone sources at F32;
//   `of` reading AnyImage::f32() / AnyGray::f32().
// - lerpTable as a float lerp (a + (b - a) * frac / 256) instead of the rounded shift; quantise without the + 0.5 and
//   the truncation; screen as old + t * (1 - old); mul, mix and toward as plain float products and lerps (no rounding
//   term); solidCore 0.5 (or 128 / 255, to heal the same spot as the lower depths).
// - store, storeF and nearest without rounding. Coverage, alpha, masks and selections are clamped to 0..1; colour is
//   not (a 32-bit document's colour may exceed 1), so store needs a colour and a coverage form, and `one` becomes 1.
// - The brush colour (settings red, green, blue) is display-referred: the stroke linearises it through the document's
//   TransferCurve before painting, so the raster sees linear light.
// - bounds (alpha bounds of an ImageF), copyOf, and the float overloads the raster calls: cropImage, cropGray,
//   nonzeroBounds, fillGradient, MipCache::refresh, Asset::make and MaskAsset::make. Healing decides on the 8-bit
//   decisionImage (P5c) rather than spotHeal and healFrom at float; Dodge, Burn and Sponge are greyed at 32 bits.
// - A float stamp tile costs four times the 8-bit one; the merge vectorises as it is (no integer division).
#pragma once
#include "compositor/brush.h"
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
} // namespace stroke

/// The depth-specific operations of a stroke raster. Integer depths share the table interpolation.
template <SampleType S> struct StrokeOps;

struct IntegerStrokeOps {
    /// The dab profile between two table entries, `frac` in 1/256 steps, rounded.
    static unsigned lerpTable(unsigned a, unsigned b, unsigned frac) { return (a * (256 - frac) + b * frac + 128) >> 8; }
};

template <> struct StrokeOps<SampleType::U8> : IntegerStrokeOps {
    using Sample = uint8_t;
    using Color = Image;
    using Gray = GrayImage;
    using Tiled = TiledSource;
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

    static PixelBounds bounds(const Color& image) { return alphaBounds(image); }
    static PixelBounds bounds(const Color& image, const PixelBounds& within) { return alphaBounds(image, within); }
    /// A working copy of the layer's own pixels.
    static std::shared_ptr<Color> copyOf(const Color& image) { return std::make_shared<Color>(image); }
};

template <> struct StrokeOps<SampleType::U16> : IntegerStrokeOps {
    using Sample = uint16_t;
    using Color = Image16;
    using Gray = Gray16;
    using Tiled = TiledSource16;
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

    static PixelBounds bounds(const Color& image) { return bounds(image, {0, 0, image.width(), image.height()}); }
    static PixelBounds bounds(const Color& image, const PixelBounds& within);   // brush_u16.cpp
    static std::shared_ptr<Color> copyOf(const Color& image);                    // brush_u16.cpp
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

template <SampleType S>
class StrokeRaster {
public:
    using Ops = StrokeOps<S>;
    using Sample = typename Ops::Sample;
    using Color = typename Ops::Color;
    using Gray = typename Ops::Gray;

    explicit StrokeRaster(BrushStroke& stroke) : g_(stroke) {}
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
    void heal();
    void commit(BrushStroke::Commit& result);
    void refreshLevels(int x0, int y0, int x1, int y1) const;

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

    BrushStroke& g_;
    /// Dab profile over squared distance (0..one), rebuilt when the tip changes; the dab loop reads it instead of
    /// computing a square root and a falloff per pixel.
    std::vector<Sample> dabTable_;
    double dabTableRadius_ = -1, dabTableHardness_ = -1, dabTableFootprint_ = -1, dabTableScale_ = 0;
    StampOf<Sample> stamp_;
    std::vector<Sample> tailBackup_;
};

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

template <class Color>
void copyImage(const Color& source, Color& target, int dx, int dy) {
    using Sample = std::remove_cv_t<std::remove_pointer_t<decltype(source.row(0))>>;
    const int x0 = std::max(0, -dx), x1 = std::min(source.width(), target.width() - dx);
    if (x1 <= x0) return;
    parallelFor(0, source.height(), 256, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const int ty = y + dy;
            if (ty < 0 || ty >= target.height()) continue;
            std::memcpy(target.pixel(x0 + dx, ty), source.pixel(x0, y), size_t(x1 - x0) * 4 * sizeof(Sample));
        }
    });
}

/// A bilinear sample of an image (or an ensured tiled one) at a document point, transparent outside.
template <class Source>
void bilinear(const Source& at, int sw, int sh, double sx, double sy, double s[4]) {
    s[0] = s[1] = s[2] = s[3] = 0;
    const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
    const double fx = sx - ix, fy = sy - iy;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            const int px = ix + i, py = iy + j;
            const double w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            if (w <= 0 || px < 0 || py < 0 || px >= sw || py >= sh) continue;
            const auto* p = at(px, py);
            if (!p) continue;
            for (int k = 0; k < 4; k++) s[k] += p[k] * w;
        }
}

} // namespace stroke

template <SampleType S>
bool StrokeRaster<S>::init(const Layer& layer, bool mask, const Gray* selectionIn) {
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
    if (width < 1 || height < 1 || double(width) * height > double(Document::imagePixelBudget(S)) || originalWidth > maxImageSide || originalHeight > maxImageSide
        || !std::isfinite(settings.diameter) || settings.diameter < 1 || settings.diameter > 2000
        || !std::isfinite(settings.hardness) || settings.hardness < 0 || settings.hardness > 1
        || !std::isfinite(settings.opacity) || settings.opacity < 0.01 || settings.opacity > 1) {
        g.error_ = "This stroke exceeds the supported canvas or brush limits.";
        return false;
    }
    // A deeper stroke refuses pixels of another depth; the 8-bit one has always painted such a layer as blank.
    if constexpr (S != SampleType::U8) {
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
            auto copy = std::make_shared<Color>(width, height);
            auto work = std::make_shared<Color>(width, height);
            if (own) {
                const int dx = int(g.sourceRect_.minX()), dy = int(g.sourceRect_.minY());
                stroke::copyImage(*own, *copy, dx, dy);
                stroke::copyImage(*own, *work, dx, dy);
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
                selection->at(x, y) = Sample(std::min<uint32_t>(selectionIn->at(sx, sy), Ops::one));
            }
        }
    }
    return true;
}

template <SampleType S>
void StrokeRaster<S>::refreshLevels(int x0, int y0, int x1, int y1) const {
    if (working) MipCache::shared().refresh(working.get(), x0, y0, x1, y1);
    if (workingMask) MipCache::shared().refresh(workingMask.get(), x0, y0, x1, y1);
}

template <SampleType S>
void StrokeRaster<S>::saveTail(const Rect& affected) {
    const int x0 = int(affected.minX()), y0 = int(affected.minY()), w = int(affected.width), h = int(affected.height);
    tailBackup_.resize(size_t(w) * size_t(h));
    for (int y = 0; y < h; y++) std::memcpy(&tailBackup_[size_t(y) * size_t(w)], coverage->row(y0 + y) + x0, size_t(w) * sizeof(Sample));
}

template <SampleType S>
void StrokeRaster<S>::restoreTail(const Rect& tail) {
    const int x0 = int(tail.minX()), y0 = int(tail.minY()), w = int(tail.width), h = int(tail.height);
    for (int y = 0; y < h; y++) std::memcpy(coverage->row(y0 + y) + x0, &tailBackup_[size_t(y) * size_t(w)], size_t(w) * sizeof(Sample));
}

template <SampleType S>
void StrokeRaster<S>::refreshDabTable(double radius, double hardness, double footprint) {
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

template <SampleType S>
bool StrokeRaster<S>::stampDab(Point center, double radius, const Rect& affected) {
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

template <SampleType S>
void StrokeRaster<S>::dab(Point center) {
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
                const unsigned value = Ops::lerpTable(dabTable_[size_t(i)], dabTable_[size_t(i) + 1], frac);
                if (value == 0) continue;
                const unsigned old = row[x];
                row[x] = Sample(hard ? std::max(old, value) : unsigned(Ops::screen(old, value)));
            }
        }
    };
    if (stroke::areaOf(x1 - x0, y1 - y0) >= 65536) parallelRows(y0, y1, rows, 32); else rows(y0, y1);
    g.markDirty(affected);
}

template <SampleType S>
void StrokeRaster<S>::recompose(const Rect& r) {
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

template <SampleType S>
void StrokeRaster<S>::recomposeRows(const Rect& r) {
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
    double cr = settings.red * one, cg = settings.green * one, cb = settings.blue * one;
    if (settings.healing && !g.clone_) { cr = cg = cb = 0.12 * one; opacity *= 0.45; } // the wash shown while painting (the Healing Brush shows its source)
    if (!g.clone_) {
        // Plain paint or erase: integer lerps, one coverage step per pixel.
        const uint32_t op = Ops::quantise(opacity);
        const int colour[4] = {int(clamp(cr + 0.5, 0.0, one)), int(clamp(cg + 0.5, 0.0, one)), int(clamp(cb + 0.5, 0.0, one)), int(Ops::one)};
        const bool erasing = settings.erasing;
        for (int y = y0; y < y1; y++) {
            const Sample* cov = coverage->row(y);
            const Sample* selRow = sel ? sel->row(y) : nullptr;
            const Sample* baseRow = base->row(y) + x0 * 4;
            Sample* out = working->row(y) + x0 * 4;
            for (int x = x0; x < x1; x++, baseRow += 4, out += 4) {
                uint32_t k = cov[x];
                if (selRow) k = Ops::mul(k, selRow[x]);
                k = Ops::mul(k, op);
                if (k == 0) { std::memcpy(out, baseRow, 4 * sizeof(Sample)); continue; }
                if (erasing) { for (int c = 0; c < 4; c++) out[c] = Sample(Ops::mul(baseRow[c], Ops::one - k)); continue; }
                for (int c = 0; c < 4; c++) out[c] = Sample(Ops::toward(int(baseRow[c]), colour[c], int(k)));
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
        const Sample* baseRow = base->row(y) + x0 * 4;
        Sample* out = working->row(y) + x0 * 4;
        Point d = g.pixelToDocument_.apply({x0 + 0.5, y + 0.5});
        const Point dd = g.pixelToDocument_.applyVector({1, 0});
        for (int x = x0; x < x1; x++, baseRow += 4, out += 4, d = d + dd) {
            const double c = cov[x] / one * opacity * (selRow ? selRow[x] / one : 1.0);
            if (c <= 0) { std::memcpy(out, baseRow, 4 * sizeof(Sample)); continue; }
            if (src || tiled) {
                // The sample under the source point, over the original through the tip.
                double s[4];
                stroke::bilinear(at, sw, sh, d.x + g.clone_->offset.x - 0.5, d.y + g.clone_->offset.y - 0.5, s);
                const double sa = g.replacesWithClone_ ? c : s[3] / one * c;
                for (int k = 0; k < 4; k++) out[k] = Ops::store(s[k] * c + baseRow[k] * (1 - sa));
                continue;
            }
            if (settings.erasing) {
                for (int k = 0; k < 4; k++) out[k] = Ops::store(baseRow[k] * (1 - c));
            } else {
                out[0] = Ops::store(baseRow[0] * (1 - c) + cr * c);
                out[1] = Ops::store(baseRow[1] * (1 - c) + cg * c);
                out[2] = Ops::store(baseRow[2] * (1 - c) + cb * c);
                out[3] = Ops::store(baseRow[3] * (1 - c) + one * c);
            }
        }
    }
}

template <SampleType S>
bool StrokeRaster<S>::liftSelection() {
    BrushStroke& g = g_;
    if (g.isMask_ || !selection || !base) return false;
    const PixelBounds b = nonzeroBounds(*selection);
    const Rect region = b.isEmpty() ? Rect() : Rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0).intersection(g.sourceRect_).integral();
    if (region.isEmpty()) return false;
    g.liftedRect_ = region;
    lifted = std::make_shared<Color>(int(region.width), int(region.height));
    bool any = false;
    for (int y = 0; y < lifted->height(); y++)
        for (int x = 0; x < lifted->width(); x++) {
            const int sx = x + int(region.x), sy = y + int(region.y);
            const uint32_t k = selection->at(sx, sy);
            const Sample* s = base->pixel(sx, sy);
            Sample* d = lifted->pixel(x, y);
            for (int c = 0; c < 4; c++) d[c] = Sample(Ops::mul(s[c], k));
            if (d[3]) any = true;
        }
    return any;
}

template <SampleType S>
void StrokeRaster<S>::moveLifted(Point offset, bool duplicate) {
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
                const uint32_t k = selection->at(x, y);
                if (!k) continue;
                Sample* p = working->pixel(x, y);
                for (int c = 0; c < 4; c++) p[c] = Sample(Ops::mul(p[c], Ops::one - std::min(k, Ops::one)));
            }
    const bool whole = std::fabs(gx - std::round(gx)) < 1e-6 && std::fabs(gy - std::round(gy)) < 1e-6;
    const double tx = g.liftedRect_.x + gx, ty = g.liftedRect_.y + gy;
    const int lw = lifted->width(), lh = lifted->height();
    const Rect target = Rect(tx, ty, lw, lh).insetBy(-1, -1).integral().intersection(Rect(0, 0, width, height));
    for (int y = int(target.minY()); y < int(target.maxY()); y++)
        for (int x = int(target.minX()); x < int(target.maxX()); x++) {
            const double sx = x - tx, sy = y - ty;
            float s[4] = {0, 0, 0, 0};
            if (whole) {
                const int ix = int(std::lround(sx)), iy = int(std::lround(sy));
                if (ix < 0 || iy < 0 || ix >= lw || iy >= lh) continue;
                const Sample* p = lifted->pixel(ix, iy);
                for (int c = 0; c < 4; c++) s[c] = p[c];
            } else {
                // The 8-bit path has always centred the sample this way (its rounding kept for identical pixels).
                double bxf = sx, byf = sy;
                if constexpr (S == SampleType::U8) { bxf = sx - 0.5 + 0.5; byf = sy - 0.5 + 0.5; }
                const int bx = int(std::floor(bxf)), by = int(std::floor(byf));
                const float fx = float(bxf - bx), fy = float(byf - by);
                for (int j = 0; j < 2; j++)
                    for (int i = 0; i < 2; i++) {
                        const int px = bx + i, py = by + j;
                        const float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                        if (w <= 0 || px < 0 || py < 0 || px >= lw || py >= lh) continue;
                        const Sample* p = lifted->pixel(px, py);
                        for (int c = 0; c < 4; c++) s[c] += p[c] * w;
                    }
            }
            if (s[3] <= 0) continue;
            Sample* d = working->pixel(x, y);
            const float a = s[3] / float(Ops::one);
            for (int c = 0; c < 4; c++) d[c] = Ops::storeF(s[c] + d[c] * (1 - a));
        }
    g.touched_ = true;
    g.dirtyGrid_ = {};
    refreshLevels(0, 0, width, height);
}

template <SampleType S>
void StrokeRaster<S>::fillGradientOver(int shape, Point from, Point to, const GradientStops& stops, double opacity) {
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
    if (g.isMask_) fillGradient(*baseMask, *workingMask, g.pixelToDocument_, GradientShape(shape), from, to, stops, opacity, &inside);
    else fillGradient(*base, *working, g.pixelToDocument_, GradientShape(shape), from, to, stops, opacity, &inside);
    refreshLevels(0, 0, g.width_, g.height_);
}

template <SampleType S>
void StrokeRaster<S>::heal() {
    const BrushStroke& g = g_;
    if (!g.settings_.healing || g.isMask_ || !coverage) return;
    const PixelBounds b = nonzeroBounds(*coverage);
    if (b.isEmpty()) return;
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

template <SampleType S>
void StrokeRaster<S>::healFromClone(const PixelBounds& b) {
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
}

template <SampleType S>
void StrokeRaster<S>::commit(BrushStroke::Commit& result) {
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
    std::shared_ptr<Color> image = (crop == Rect(0, 0, width, height)) ? working : cropImage(*working, int(crop.minX()), int(crop.minY()), int(crop.width), int(crop.height));
    result.asset = Asset::make(std::shared_ptr<const Color>(image), g.name_);
    const Point center = g.pixelToDocument_.apply({crop.midX(), crop.midY()});
    LayerTransform t = g.paintTransform_;
    t.size = {crop.width * g.paintTransform_.size.width / width, crop.height * g.paintTransform_.size.height / height};
    t.origin = {center.x - t.size.width / 2, center.y - t.size.height / 2};
    result.transform = t;
}

} // namespace compositor
