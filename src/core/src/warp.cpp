#include "compositor/warp.h"
#include "compositor/resample.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace compositor {

Corners cornersOf(const LayerTransform& t) { return {t.point({0, 0}), t.point({1, 0}), t.point({1, 1}), t.point({0, 1})}; }

bool cornersUsable(const Corners& c) {
    for (auto& p : c) if (!p.isFinite() || std::fabs(p.x) > 1e6 || std::fabs(p.y) > 1e6) return false;
    double sign = 0;
    for (int i = 0; i < 4; i++) {
        Point a = c[size_t(i)], b = c[size_t((i + 1) % 4)], d = c[size_t((i + 2) % 4)];
        double cross = (b.x - a.x) * (d.y - b.y) - (b.y - a.y) * (d.x - b.x);
        if (std::fabs(cross) <= 0.01) return false;
        if (sign == 0) sign = cross < 0 ? -1 : 1;
        else if ((cross < 0) != (sign < 0)) return false;
    }
    return true;
}

Point Homography::map(Point p) const {
    double w = m[6] * p.x + m[7] * p.y + m[8];
    if (std::fabs(w) < 1e-12) w = 1e-12;
    return {(m[0] * p.x + m[1] * p.y + m[2]) / w, (m[3] * p.x + m[4] * p.y + m[5]) / w};
}

Homography Homography::inverted() const {
    const double* a = m;
    double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
    Homography r;
    if (std::fabs(det) < 1e-300) return r;
    double inv = 1 / det;
    r.m[0] = (a[4] * a[8] - a[5] * a[7]) * inv; r.m[1] = (a[2] * a[7] - a[1] * a[8]) * inv; r.m[2] = (a[1] * a[5] - a[2] * a[4]) * inv;
    r.m[3] = (a[5] * a[6] - a[3] * a[8]) * inv; r.m[4] = (a[0] * a[8] - a[2] * a[6]) * inv; r.m[5] = (a[2] * a[3] - a[0] * a[5]) * inv;
    r.m[6] = (a[3] * a[7] - a[4] * a[6]) * inv; r.m[7] = (a[1] * a[6] - a[0] * a[7]) * inv; r.m[8] = (a[0] * a[4] - a[1] * a[3]) * inv;
    return r;
}

Homography Homography::unitTo(const Corners& c) {
    double sx = c[0].x - c[1].x + c[2].x - c[3].x, sy = c[0].y - c[1].y + c[2].y - c[3].y;
    double g = 0, h = 0;
    if (std::fabs(sx) > 1e-9 || std::fabs(sy) > 1e-9) {
        double dx1 = c[1].x - c[2].x, dx2 = c[3].x - c[2].x, dy1 = c[1].y - c[2].y, dy2 = c[3].y - c[2].y;
        double den = dx1 * dy2 - dx2 * dy1;
        if (std::fabs(den) > 1e-12) { g = (sx * dy2 - dx2 * sy) / den; h = (dx1 * sy - sx * dy1) / den; }
    }
    Homography r;
    r.m[0] = c[1].x - c[0].x + g * c[1].x; r.m[1] = c[3].x - c[0].x + h * c[3].x; r.m[2] = c[0].x;
    r.m[3] = c[1].y - c[0].y + g * c[1].y; r.m[4] = c[3].y - c[0].y + h * c[3].y; r.m[5] = c[0].y;
    r.m[6] = g; r.m[7] = h; r.m[8] = 1;
    return r;
}

namespace {

// Bounds and placement shared by the image and mask warps.
struct WarpFrame {
    Rect bounds;
    int width, height;
    double factor;
    Homography docToUnit;
};

std::optional<WarpFrame> frameFor(const LayerTransform& transform, const Corners& corners, int limit) {
    if (!cornersUsable(corners)) return std::nullopt;
    // Corners land a hair off whole pixels after a rotation through sin/cos (300 + 1e-14); snapping keeps floor/ceil
    // from adding a soft extra row or column.
    auto snap = [](double v) { return std::round(v * 1e6) / 1e6; };
    double minX = std::floor(snap(std::min({corners[0].x, corners[1].x, corners[2].x, corners[3].x})));
    double minY = std::floor(snap(std::min({corners[0].y, corners[1].y, corners[2].y, corners[3].y})));
    double maxX = std::ceil(snap(std::max({corners[0].x, corners[1].x, corners[2].x, corners[3].x})));
    double maxY = std::ceil(snap(std::max({corners[0].y, corners[1].y, corners[2].y, corners[3].y})));
    Rect bounds(minX, minY, maxX - minX, maxY - minY);
    if (bounds.width < 1 || bounds.height < 1 || bounds.width > 30000 || bounds.height > 30000 || bounds.width * bounds.height > 100000000.0) return std::nullopt;
    double factor = limit > 0 ? std::min(1.0, limit / std::max(bounds.width, bounds.height)) : 1;
    WarpFrame f;
    f.bounds = bounds;
    f.width = std::max(1, int(std::ceil(bounds.width * factor)));
    f.height = std::max(1, int(std::ceil(bounds.height * factor)));
    f.factor = factor;
    // Flipped layers show their pixels mirrored: the unit square of the pixels maps to swapped corners.
    Corners target = corners;
    if (transform.flipX) { std::swap(target[0], target[1]); std::swap(target[3], target[2]); }
    if (transform.flipY) { std::swap(target[0], target[3]); std::swap(target[1], target[2]); }
    f.docToUnit = Homography::unitTo(target).inverted();
    return f;
}

/// Walks a warp frame's output pixels through the homography incrementally: the homogeneous numerators
/// and denominator are linear along a row, so each pixel costs three adds and one reciprocal instead of a
/// full projective map (and the edge footprint comes from the derivatives instead of two more maps).
struct HomographyStepper {
    const double* h;
    double step;      // document units per output pixel
    double x0, y0;    // document position of output pixel (0, 0)'s centre
    explicit HomographyStepper(const WarpFrame& f) : h(f.docToUnit.m), step(1.0 / f.factor), x0(f.bounds.x + 0.5 / f.factor), y0(f.bounds.y + 0.5 / f.factor) {}

    struct Row {
        const double* h;
        double step;
        double nu, nv, den;             // numerators and denominator at the current pixel
        double dnu, dnv, dden;          // their change per pixel along the row
        void advance() { nu += dnu; nv += dnv; den += dden; }
        Point unit() const { double inv = 1.0 / den; return {nu * inv, nv * inv}; }
        /// Source-pixel extent of one output pixel, the larger of the two axes.
        double footprint(Point u, int pw, int ph) const {
            double inv = 1.0 / den;
            double dudx = (h[0] - u.x * h[6]) * inv, dvdx = (h[3] - u.y * h[6]) * inv;
            double dudy = (h[1] - u.x * h[7]) * inv, dvdy = (h[4] - u.y * h[7]) * inv;
            return step * std::max(std::hypot(dudx * pw, dvdx * ph), std::hypot(dudy * pw, dvdy * ph));
        }
    };
    Row row(int y) const {
        double Y = y0 + y * step;
        Row r;
        r.h = h; r.step = step;
        r.nu = h[0] * x0 + h[1] * Y + h[2];
        r.nv = h[3] * x0 + h[4] * Y + h[5];
        r.den = h[6] * x0 + h[7] * Y + h[8];
        r.dnu = h[0] * step; r.dnv = h[3] * step; r.dden = h[6] * step;
        return r;
    }
};

/// When the warp is a pure scale and translation (axis-aligned corners, in order), the separable resample
/// applies: output pixel x's centre in source pixels is origin + x * step along each axis.
struct AxisAlignedWarp { double originX, stepX, originY, stepY; };
std::optional<AxisAlignedWarp> axisAlignedWarp(const WarpFrame& f, const Corners& corners, int pw, int ph) {
    auto same = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    if (!(same(corners[0].y, corners[1].y) && same(corners[2].y, corners[3].y) && same(corners[0].x, corners[3].x) && same(corners[1].x, corners[2].x))) return std::nullopt;
    if (corners[1].x <= corners[0].x || corners[3].y <= corners[0].y) return std::nullopt;
    const HomographyStepper stepper(f);
    HomographyStepper::Row r0 = stepper.row(0), r1 = stepper.row(1);
    Point u00 = r0.unit();
    r0.advance();
    Point u10 = r0.unit(), u01 = r1.unit();
    return AxisAlignedWarp{u00.x * pw, (u10.x - u00.x) * pw, u00.y * ph, (u01.y - u00.y) * ph};
}

/// The pixels a warp samples from: the image itself, or a reduction (cached when the image is shared).
struct MipSource {
    const Image* image;
    ImagePtr holder;
};

MipSource mipSourceFor(const Image& image, const ImagePtr& owner, int level) {
    if (level <= 0) return {&image, nullptr};
    ImagePtr reduced = owner && owner.get() == &image ? MipCache::shared().level(owner, level) : reduceImage(image, level);
    return {reduced.get(), reduced};
}

std::optional<WarpedImage> warpImageImpl(const Image& image, const ImagePtr& owner, const LayerTransform& transform, const Corners& corners, int limit) {
    auto frame = frameFor(transform, corners, limit);
    if (!frame || image.isEmpty()) return std::nullopt;
    const WarpFrame& f = *frame;
    int pw = image.width(), ph = image.height();
    bool nearest = transform.sampling == Sampling::Nearest;
    LayerTransform placed(f.bounds.origin(), f.bounds.size());
    placed.sampling = transform.sampling;
    if (!nearest)
        if (auto axis = axisAlignedWarp(f, corners, pw, ph))
            return WarpedImage{resampleAxisAligned(image, f.width, f.height, axis->originX, axis->stepX, axis->originY, axis->stepY, filterFor(transform.sampling)), placed};
    auto out = std::make_shared<Image>(f.width, f.height);
    // Mip level from the average reduction across the shape.
    double areaOut = f.bounds.width * f.bounds.height * f.factor * f.factor;
    int level = nearest ? 0 : MipCache::levelFor(std::sqrt(areaOut / std::max(1.0, double(pw) * ph)), transform.sampling == Sampling::High);
    MipSource mipSource = mipSourceFor(image, owner, level);
    const Image* source = mipSource.image;
    double mip = std::ldexp(1.0, level);
    const HomographyStepper stepper(f);
    parallelRows(0, f.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* row = out->row(y);
            HomographyStepper::Row r = stepper.row(y);
            for (int x = 0; x < f.width; x++, row += 4, r.advance()) {
                Point u = r.unit();
                Point px{u.x * pw, u.y * ph};
                // Local pixel footprint for the edge antialias, from the mapping's derivatives.
                double footprint = std::max(1e-6, r.footprint(u, pw, ph));
                float edge;
                if (nearest) { if (px.x < 0 || px.x >= pw || px.y < 0 || px.y >= ph) continue; edge = 1; }
                else {
                    double e = std::min({px.x, pw - px.x, px.y, ph - px.y}) / footprint;
                    edge = float(clamp(e + 0.5, 0.0, 1.0));
                    if (edge <= 0) continue;
                }
                if (nearest) {
                    std::memcpy(row, source->pixel(clamp(int(std::floor(px.x)), 0, pw - 1), clamp(int(std::floor(px.y)), 0, ph - 1)), 4);
                    continue;
                }
                uint8_t s[4];
                if (transform.sampling == Sampling::High) sampleBicubic(*source, px.x / mip, px.y / mip, s);
                else sampleBilinear(*source, px.x / mip, px.y / mip, s);
                const int e = int(edge * 256 + 0.5f);
                for (int c = 0; c < 4; c++) row[c] = uint8_t((s[c] * e + 128) >> 8);
            }
        }
    });
    return WarpedImage{out, placed};
}

std::optional<WarpedImage> trimWarped(std::optional<WarpedImage> warped, Rect* crop) {
    if (!warped) return std::nullopt;
    PixelBounds b = alphaBounds(*warped->image);
    Rect full(0, 0, warped->image->width(), warped->image->height());
    if (crop) *crop = full;
    if (b.isEmpty() || (b.x0 == 0 && b.y0 == 0 && b.x1 == warped->image->width() && b.y1 == warped->image->height())) return warped;
    Rect c(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
    if (crop) *crop = c;
    WarpedImage result;
    result.image = cropImage(*warped->image, int(c.x), int(c.y), int(c.width), int(c.height));
    result.transform = warped->transform;
    result.transform.origin = {warped->transform.origin.x + c.x, warped->transform.origin.y + c.y};
    result.transform.size = c.size();
    return result;
}

} // namespace

std::optional<WarpedImage> warpImage(const ImagePtr& image, const LayerTransform& transform, const Corners& corners, int limit) {
    if (!image) return std::nullopt;
    return warpImageImpl(*image, image, transform, corners, limit);
}
std::optional<WarpedImage> warpImage(const Image& image, const LayerTransform& transform, const Corners& corners, int limit) {
    return warpImageImpl(image, nullptr, transform, corners, limit);
}
std::optional<WarpedImage> warpImageTrimmed(const ImagePtr& image, const LayerTransform& transform, const Corners& corners, Rect* crop) {
    return trimWarped(warpImage(image, transform, corners, 0), crop);
}
std::optional<WarpedImage> warpImageTrimmed(const Image& image, const LayerTransform& transform, const Corners& corners, Rect* crop) {
    return trimWarped(warpImage(image, transform, corners, 0), crop);
}

std::optional<WarpedMask> warpMask(const GrayPtr& mask, const LayerTransform& transform, const Corners& corners, uint8_t background, int limit) {
    if (!mask) return std::nullopt;
    return warpMask(*mask, transform, corners, background, limit);
}

std::optional<WarpedMask> warpMask(const GrayImage& mask, const LayerTransform& transform, const Corners& corners, uint8_t background, int limit) {
    auto frame = frameFor(transform, corners, limit);
    if (!frame || mask.isEmpty()) return std::nullopt;
    const WarpFrame& f = *frame;
    LayerTransform placed(f.bounds.origin(), f.bounds.size());
    placed.sampling = transform.sampling;
    // A uniform 1x1 mask already covers any shape.
    if (mask.width() == 1 && mask.height() == 1) return WarpedMask{std::make_shared<GrayImage>(mask), placed};
    int pw = mask.width(), ph = mask.height();
    if (transform.sampling != Sampling::Nearest)
        if (auto axis = axisAlignedWarp(f, corners, pw, ph))
            return WarpedMask{resampleAxisAligned(mask, f.width, f.height, axis->originX, axis->stepX, axis->originY, axis->stepY, filterFor(transform.sampling), background), placed};
    auto out = std::make_shared<GrayImage>(f.width, f.height, background);
    const HomographyStepper stepper(f);
    parallelRows(0, f.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* row = out->row(y);
            HomographyStepper::Row r = stepper.row(y);
            for (int x = 0; x < f.width; x++, r.advance()) {
                Point u = r.unit();
                Point px{u.x * pw, u.y * ph};
                double footprint = std::max(1e-6, r.footprint(u, pw, ph));
                double e = std::min({px.x, pw - px.x, px.y, ph - px.y}) / footprint;
                float edge = float(clamp(e + 0.5, 0.0, 1.0));
                if (edge <= 0) continue;
                float v = float(sampleGrayBilinear(mask, px.x, px.y));
                row[x] = uint8_t(clamp(v * edge + background * (1 - edge) + 0.5f, 0.0f, 255.0f));
            }
        }
    });
    return WarpedMask{out, placed};
}

Corners carriedCorners(const LayerTransform& placement, const LayerTransform& by, const Corners& corners) {
    Affine toUnit = by.unitToDocument().inverted();
    Homography map = Homography::unitTo(corners);
    Corners result;
    Corners own = cornersOf(placement);
    for (size_t i = 0; i < 4; i++) result[i] = map.map(toUnit.apply(own[i]));
    return result;
}

std::shared_ptr<GrayImage> warpCoverage(const GrayImage& coverage, const LayerTransform& original, int pixelWidth, int pixelHeight, const Corners& corners) {
    auto out = std::make_shared<GrayImage>(coverage.width(), coverage.height(), 0);
    if (!cornersUsable(corners)) return out;
    Corners target = corners;
    if (original.flipX) { std::swap(target[0], target[1]); std::swap(target[3], target[2]); }
    if (original.flipY) { std::swap(target[0], target[3]); std::swap(target[1], target[2]); }
    Homography docToUnit = Homography::unitTo(target).inverted();
    Affine pixelToDoc = original.pixelToDocument(pixelWidth, pixelHeight);
    int w = coverage.width(), h = coverage.height();
    const double* hm = docToUnit.m;
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const double Y = y + 0.5;
            double nu = hm[0] * 0.5 + hm[1] * Y + hm[2], nv = hm[3] * 0.5 + hm[4] * Y + hm[5], den = hm[6] * 0.5 + hm[7] * Y + hm[8];
            for (int x = 0; x < w; x++, nu += hm[0], nv += hm[3], den += hm[6]) {
                const double inv = 1.0 / den;
                Point u{nu * inv, nv * inv};
                if (u.x < 0 || u.x > 1 || u.y < 0 || u.y > 1) continue;
                Point src = pixelToDoc.apply({u.x * pixelWidth, u.y * pixelHeight});
                double bx = clamp(src.x - 0.5, 0.0, double(w - 1)), by = clamp(src.y - 0.5, 0.0, double(h - 1));
                int x0 = int(bx), yy0 = int(by), x1 = std::min(x0 + 1, w - 1), yy1 = std::min(yy0 + 1, h - 1);
                float fx = float(bx - x0), fy = float(by - yy0);
                float v = coverage.at(x0, yy0) * (1 - fx) * (1 - fy) + coverage.at(x1, yy0) * fx * (1 - fy) + coverage.at(x0, yy1) * (1 - fx) * fy + coverage.at(x1, yy1) * fx * fy;
                out->at(x, y) = uint8_t(clamp(v + 0.5f, 0.0f, 255.0f));
            }
        }
    });
    return out;
}

} // namespace compositor
