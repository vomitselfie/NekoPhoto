#include "compositor/brush.h"
#include "compositor/shape.h"
#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include "HealPixels.h"
}

namespace compositor {

double brushFalloff(double u) {
    const double k = 2.5;
    return std::max(0.0, (std::exp(-k * u * u) - std::exp(-k)) / (1 - std::exp(-k)));
}

namespace {

// Nearest-neighbour stretch of a gray image over a rect of the grid (BrushRaster.draw for masks).
void stretchGray(const GrayImage& source, GrayImage& target, const Rect& rect) {
    int x0 = std::max(0, int(std::floor(rect.minX()))), y0 = std::max(0, int(std::floor(rect.minY())));
    int x1 = std::min(target.width(), int(std::ceil(rect.maxX()))), y1 = std::min(target.height(), int(std::ceil(rect.maxY())));
    for (int y = y0; y < y1; y++) {
        int sy = clamp(int((y - rect.minY()) / rect.height * source.height()), 0, source.height() - 1);
        for (int x = x0; x < x1; x++) {
            int sx = clamp(int((x - rect.minX()) / rect.width * source.width()), 0, source.width() - 1);
            target.at(x, y) = source.at(sx, sy);
        }
    }
}

void copyImage(const Image& source, Image& target, int dx, int dy) {
    for (int y = 0; y < source.height(); y++) {
        int ty = y + dy;
        if (ty < 0 || ty >= target.height()) continue;
        int x0 = std::max(0, -dx), x1 = std::min(source.width(), target.width() - dx);
        if (x1 <= x0) continue;
        std::memcpy(target.pixel(x0 + dx, ty), source.pixel(x0, y), size_t(x1 - x0) * 4);
    }
}

} // namespace

BrushStroke::BrushStroke(const Layer& layer, bool mask, BrushSettings settings, Size canvas, const GrayImage* selection)
    : isMask_(mask), settings_(settings), canvas_(0, 0, canvas.width, canvas.height), name_(layer.name), layerTransform_(layer.transform) {
    const GrayImage* placedMask = nullptr;
    LayerTransform base = layer.transform;
    if (mask && layer.mask && layer.mask->placement && layer.mask->asset.image) {
        placedMask = layer.mask->asset.image.get();
        base = *layer.mask->placement;
    }
    int originalWidth = placedMask ? placedMask->width() : layer.pixelWidth();
    int originalHeight = placedMask ? placedMask->height() : layer.pixelHeight();
    Affine originalMapping = base.pixelToDocument(originalWidth, originalHeight);
    Rect originalBounds(0, 0, originalWidth, originalHeight);
    Rect extent = originalBounds;
    if (!mask) extent = originalBounds.unionWith(originalMapping.inverted().mapBounds(canvas_).integral());
    width_ = int(extent.width);
    height_ = int(extent.height);
    sourceRect_ = originalBounds.offsetBy(-extent.minX(), -extent.minY());
    pixelToDocument_ = originalMapping.translatedBy(extent.minX(), extent.minY());
    documentToPixel_ = pixelToDocument_.inverted();
    LayerTransform expanded = base;
    expanded.size = {double(width_) * base.size.width / originalWidth, double(height_) * base.size.height / originalHeight};
    Point center = originalMapping.apply({extent.midX(), extent.midY()});
    expanded.origin = {center.x - expanded.size.width / 2, center.y - expanded.size.height / 2};
    paintTransform_ = expanded;

    if (width_ < 1 || height_ < 1 || double(width_) * height_ > 100000000.0 || originalWidth > 30000 || originalHeight > 30000
        || !std::isfinite(settings.diameter) || settings.diameter < 1 || settings.diameter > 2000
        || !std::isfinite(settings.hardness) || settings.hardness < 0 || settings.hardness > 1
        || !std::isfinite(settings.opacity) || settings.opacity < 0.01 || settings.opacity > 1) {
        error_ = "This stroke exceeds the supported canvas or brush limits.";
        return;
    }

    if (mask) {
        baseMask_ = std::make_shared<GrayImage>(width_, height_, 255);
        const GrayImage* source = placedMask ? placedMask : (layer.mask && layer.mask->asset.image ? layer.mask->asset.image.get() : nullptr);
        if (source) stretchGray(*source, *baseMask_, sourceRect_);
        workingMask_ = std::make_shared<GrayImage>(*baseMask_);
    } else {
        base_ = std::make_shared<Image>(width_, height_);
        if (layer.asset && layer.asset->image) copyImage(*layer.asset->image, *base_, int(sourceRect_.minX()), int(sourceRect_.minY()));
        working_ = std::make_shared<Image>(*base_);
    }
    coverage_ = std::make_shared<GrayImage>(width_, height_, 0);
    if (selection && !selection->isEmpty()) {
        // Selection coverage resampled into the grid: outside the canvas nothing is selected.
        selection_ = std::make_shared<GrayImage>(width_, height_, 0);
        for (int y = 0; y < height_; y++) {
            Point d = pixelToDocument_.apply({0.5, y + 0.5});
            Point dd = pixelToDocument_.applyVector({1, 0});
            for (int x = 0; x < width_; x++, d = d + dd) {
                int sx = int(std::floor(d.x)), sy = int(std::floor(d.y));
                if (sx < 0 || sy < 0 || sx >= selection->width() || sy >= selection->height()) continue;
                selection_->at(x, y) = selection->at(sx, sy);
            }
        }
    }
    valid_ = true;
}

void BrushStroke::markDirty(const Rect& gridRect) {
    Rect r = gridRect.intersection(Rect(0, 0, width_, height_));
    if (r.isEmpty()) return;
    dirtyGrid_ = dirtyGrid_.unionWith(r);
    touched_ = true;
}

Rect BrushStroke::takeDirtyRect() {
    if (dirtyGrid_.isEmpty()) return {};
    Rect doc = pixelToDocument_.mapBounds(dirtyGrid_).insetBy(-1, -1).intersection(canvas_);
    dirtyGrid_ = {};
    return doc;
}

void BrushStroke::append(Point point) {
    if (!valid_ || !point.isFinite() || std::fabs(point.x) > 1e7 || std::fabs(point.y) > 1e7) return;
    if (!samples_.empty() && samples_.back() == point) return;
    // Undo the provisional tail.
    if (hasTail_) {
        int x0 = int(tailRect_.minX()), y0 = int(tailRect_.minY()), w = int(tailRect_.width), h = int(tailRect_.height);
        for (int y = 0; y < h; y++) std::memcpy(coverage_->row(y0 + y) + x0, &tailBackup_[size_t(y) * w], size_t(w));
        previous_ = tailPrevious_;
        distanceToNext_ = tailDistance_;
        hasTail_ = false;
        markDirty(tailRect_);
    }
    samples_.push_back(point);
    if (samples_.size() > 4) samples_.erase(samples_.begin());
    size_t n = samples_.size();
    if (n == 1) walk(point);
    else if (n >= 3) curve(samples_[n - 3], samples_[n - 2], samples_[n >= 4 ? n - 4 : 0], samples_[n - 1]);
    if (n >= 2) {
        // Draw a straight tail to the cursor, remembering what it covers so it can be taken back.
        Point start = samples_[n - 2];
        double reach = settings_.diameter / 2 + 2;
        Rect box = Rect::fromPoints(start, point).insetBy(-reach, -reach).intersection(canvas_);
        Rect affected = box.isEmpty() ? Rect() : documentToPixel_.mapBounds(box).integral().intersection(Rect(0, 0, width_, height_));
        tailPrevious_ = previous_;
        tailDistance_ = distanceToNext_;
        if (!affected.isEmpty()) {
            tailRect_ = affected;
            int x0 = int(affected.minX()), y0 = int(affected.minY()), w = int(affected.width), h = int(affected.height);
            tailBackup_.resize(size_t(w) * h);
            for (int y = 0; y < h; y++) std::memcpy(&tailBackup_[size_t(y) * w], coverage_->row(y0 + y) + x0, size_t(w));
            hasTail_ = true;
        }
        walk(point);
        previous_ = tailPrevious_;
        distanceToNext_ = tailDistance_;
    }
    if (!dirtyGrid_.isEmpty() && !deferRecompose_) recompose(dirtyGrid_);
}

void BrushStroke::appendAll(const std::vector<Point>& documentPoints) {
    deferRecompose_ = true;
    for (const Point& p : documentPoints) append(p);
    deferRecompose_ = false;
    if (!dirtyGrid_.isEmpty()) recompose(dirtyGrid_);
}

void BrushStroke::flush() {
    if (!valid_) return;
    if (hasTail_) {
        int x0 = int(tailRect_.minX()), y0 = int(tailRect_.minY()), w = int(tailRect_.width), h = int(tailRect_.height);
        for (int y = 0; y < h; y++) std::memcpy(coverage_->row(y0 + y) + x0, &tailBackup_[size_t(y) * w], size_t(w));
        previous_ = tailPrevious_;
        distanceToNext_ = tailDistance_;
        hasTail_ = false;
        markDirty(tailRect_);
    }
    size_t n = samples_.size();
    if (n >= 2) {
        curve(samples_[n - 2], samples_[n - 1], samples_[n >= 3 ? n - 3 : 0], samples_[n - 1]);
        samples_ = {samples_[n - 1]};
    }
    if (!dirtyGrid_.isEmpty()) recompose(dirtyGrid_);
}

void BrushStroke::curve(Point start, Point end, Point before, Point after) {
    auto knot = [](double t, Point a, Point b) { return t + std::max(0.0001, std::sqrt(std::hypot(b.x - a.x, b.y - a.y))); };
    auto mix = [](Point a, Point b, double ta, double tb, double t) {
        double wa = (tb - t) / (tb - ta), wb = (t - ta) / (tb - ta);
        return Point{a.x * wa + b.x * wb, a.y * wa + b.y * wb};
    };
    double t0 = 0, t1 = knot(t0, before, start), t2 = knot(t1, start, end), t3 = knot(t2, end, after);
    int pieces = std::max(1, int(std::ceil(std::hypot(end.x - start.x, end.y - start.y) / 2)));
    for (int index = 1; index <= pieces; index++) {
        double t = t1 + (t2 - t1) * index / pieces;
        Point a1 = mix(before, start, t0, t1, t), a2 = mix(start, end, t1, t2, t), a3 = mix(end, after, t2, t3, t);
        Point b1 = mix(a1, a2, t0, t2, t), b2 = mix(a2, a3, t1, t3, t);
        walk(index == pieces ? end : mix(b1, b2, t1, t2, t));
    }
}

void BrushStroke::walk(Point point) {
    double spacing = std::max(0.25, settings_.diameter * (settings_.hardness >= 1 ? 0.015 : 0.025));
    if (previous_) {
        double dx = point.x - previous_->x, dy = point.y - previous_->y;
        double length = std::hypot(dx, dy);
        if (length > 0) {
            double d = distanceToNext_;
            while (d <= length) {
                dab({previous_->x + dx * d / length, previous_->y + dy * d / length});
                d += spacing;
            }
            distanceToNext_ = d - length;
        }
    } else {
        dab(point);
        distanceToNext_ = spacing;
    }
    previous_ = point;
}

void BrushStroke::dab(Point center) {
    double radius = settings_.diameter / 2;
    Rect circle(center.x - radius, center.y - radius, radius * 2, radius * 2);
    Rect clipped = circle.intersection(canvas_);
    if (clipped.isEmpty()) return;
    Rect affected = documentToPixel_.mapBounds(clipped).integral().intersection(Rect(0, 0, width_, height_));
    if (affected.isEmpty()) return;
    // Document units per grid pixel, for antialiasing the rim.
    double footprint = std::hypot(pixelToDocument_.a, pixelToDocument_.b);
    bool hard = settings_.hardness >= 1;
    double inner = radius * settings_.hardness;
    int x0 = int(affected.minX()), x1 = int(affected.maxX()), y0 = int(affected.minY()), y1 = int(affected.maxY());
    for (int y = y0; y < y1; y++) {
        uint8_t* row = coverage_->row(y);
        Point d = pixelToDocument_.apply({x0 + 0.5, y + 0.5});
        Point dd = pixelToDocument_.applyVector({1, 0});
        for (int x = x0; x < x1; x++, d = d + dd) {
            if (!canvas_.contains(d)) continue;
            double dist = std::hypot(d.x - center.x, d.y - center.y);
            double value;
            if (hard) value = clamp((radius - dist) / std::max(1e-9, footprint) + 0.5, 0.0, 1.0);
            else if (dist <= inner) value = 1;
            else if (dist >= radius) value = 0;
            else value = brushFalloff((dist - inner) / std::max(1e-9, radius - inner));
            if (value <= 0) continue;
            double old = row[x] / 255.0;
            double merged = hard ? std::max(old, value) : (old + value - old * value);
            row[x] = uint8_t(clamp(merged * 255 + 0.5, 0.0, 255.0));
        }
    }
    markDirty(affected);
}

void BrushStroke::recompose(const Rect& gridRect) {
    Rect r = gridRect.intersection(Rect(0, 0, width_, height_));
    if (r.isEmpty()) return;
    int x0 = int(r.minX()), x1 = int(r.maxX()), y0 = int(r.minY()), y1 = int(r.maxY());
    double opacity = settings_.opacity;
    if (isMask_) {
        uint8_t paint = uint8_t(clamp(settings_.maskValue * 255 + 0.5, 0.0, 255.0));
        for (int y = y0; y < y1; y++) {
            const uint8_t* cov = coverage_->row(y);
            const uint8_t* sel = selection_ ? selection_->row(y) : nullptr;
            const uint8_t* base = baseMask_->row(y);
            uint8_t* out = workingMask_->row(y);
            Point d = pixelToDocument_.apply({x0 + 0.5, y + 0.5});
            Point dd = pixelToDocument_.applyVector({1, 0});
            for (int x = x0; x < x1; x++, d = d + dd) {
                double c = cov[x] / 255.0 * opacity * (sel ? sel[x] / 255.0 : 1.0);
                double value = paint;
                if (maskClone_) {
                    // The sample under the document point, bilinear.
                    double sx = d.x - 0.5, sy = d.y - 0.5;
                    int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                    double fx = sx - ix, fy = sy - iy, acc = 0, wsum = 0;
                    for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
                        int px = ix + i, py = iy + j;
                        double w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                        if (w <= 0 || px < 0 || py < 0 || px >= maskClone_->width() || py >= maskClone_->height()) continue;
                        acc += maskClone_->at(px, py) * w; wsum += w;
                    }
                    value = wsum > 0 ? acc / wsum : base[x];
                }
                out[x] = uint8_t(clamp(base[x] * (1 - c) + value * c + 0.5, 0.0, 255.0));
            }
        }
        return;
    }
    double cr = settings_.red * 255, cg = settings_.green * 255, cb = settings_.blue * 255;
    if (settings_.healing) { cr = cg = cb = 0.12 * 255; opacity *= 0.45; } // the wash shown while painting
    for (int y = y0; y < y1; y++) {
        const uint8_t* cov = coverage_->row(y);
        const uint8_t* sel = selection_ ? selection_->row(y) : nullptr;
        const uint8_t* base = base_->row(y) + x0 * 4;
        uint8_t* out = working_->row(y) + x0 * 4;
        Point d = pixelToDocument_.apply({x0 + 0.5, y + 0.5});
        Point dd = pixelToDocument_.applyVector({1, 0});
        for (int x = x0; x < x1; x++, base += 4, out += 4, d = d + dd) {
            double c = cov[x] / 255.0 * opacity * (sel ? sel[x] / 255.0 : 1.0);
            if (c <= 0) { std::memcpy(out, base, 4); continue; }
            if (clone_ && clone_->image) {
                // The sample under the source point, over the original through the tip.
                const Image& src = *clone_->image;
                double sx = d.x + clone_->offset.x - 0.5, sy = d.y + clone_->offset.y - 0.5;
                double s[4] = {0, 0, 0, 0};
                int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                double fx = sx - ix, fy = sy - iy;
                for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
                    int px = ix + i, py = iy + j;
                    double w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                    if (w <= 0 || px < 0 || py < 0 || px >= src.width() || py >= src.height()) continue;
                    const uint8_t* p = src.pixel(px, py);
                    for (int k = 0; k < 4; k++) s[k] += p[k] * w;
                }
                double sa = replacesWithClone_ ? c : s[3] / 255.0 * c;
                for (int k = 0; k < 4; k++) out[k] = uint8_t(clamp(s[k] * c + base[k] * (1 - sa) + 0.5, 0.0, 255.0));
                continue;
            }
            if (settings_.erasing) {
                for (int k = 0; k < 4; k++) out[k] = uint8_t(base[k] * (1 - c) + 0.5);
            } else {
                out[0] = uint8_t(clamp(base[0] * (1 - c) + cr * c + 0.5, 0.0, 255.0));
                out[1] = uint8_t(clamp(base[1] * (1 - c) + cg * c + 0.5, 0.0, 255.0));
                out[2] = uint8_t(clamp(base[2] * (1 - c) + cb * c + 0.5, 0.0, 255.0));
                out[3] = uint8_t(clamp(base[3] * (1 - c) + 255 * c + 0.5, 0.0, 255.0));
            }
        }
    }
}

bool BrushStroke::liftSelection() {
    if (isMask_ || !selection_ || !base_) return false;
    PixelBounds b = nonzeroBounds(*selection_);
    Rect region = b.isEmpty() ? Rect() : Rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0).intersection(sourceRect_).integral();
    if (region.isEmpty()) return false;
    liftedRect_ = region;
    lifted_ = std::make_shared<Image>(int(region.width), int(region.height));
    bool any = false;
    for (int y = 0; y < lifted_->height(); y++) for (int x = 0; x < lifted_->width(); x++) {
        int sx = x + int(region.x), sy = y + int(region.y);
        unsigned k = selection_->at(sx, sy);
        const uint8_t* s = base_->pixel(sx, sy);
        uint8_t* d = lifted_->pixel(x, y);
        for (int c = 0; c < 4; c++) d[c] = uint8_t((s[c] * k + 127) / 255);
        if (d[3]) any = true;
    }
    return any;
}

void BrushStroke::moveLifted(Point offset, bool duplicate) {
    if (!lifted_ || !selection_) return;
    // The offset in grid pixels (whole document pixels may land between grid pixels on a scaled layer).
    Point zero = documentToPixel_.apply({0, 0}), moved = documentToPixel_.apply(offset);
    double gx = moved.x - zero.x, gy = moved.y - zero.y;
    working_ = std::make_shared<Image>(*base_);
    if (!duplicate) {
        for (int y = 0; y < height_; y++) for (int x = 0; x < width_; x++) {
            unsigned k = selection_->at(x, y);
            if (!k) continue;
            uint8_t* p = working_->pixel(x, y);
            for (int c = 0; c < 4; c++) p[c] = uint8_t((p[c] * (255 - k) + 127) / 255);
        }
    }
    bool whole = std::fabs(gx - std::round(gx)) < 1e-6 && std::fabs(gy - std::round(gy)) < 1e-6;
    double tx = liftedRect_.x + gx, ty = liftedRect_.y + gy;
    int lw = lifted_->width(), lh = lifted_->height();
    Rect target = Rect(tx, ty, lw, lh).insetBy(-1, -1).integral().intersection(Rect(0, 0, width_, height_));
    for (int y = int(target.minY()); y < int(target.maxY()); y++) for (int x = int(target.minX()); x < int(target.maxX()); x++) {
        double sx = x - tx, sy = y - ty;
        float s[4];
        if (whole) {
            int ix = int(std::lround(sx)), iy = int(std::lround(sy));
            if (ix < 0 || iy < 0 || ix >= lw || iy >= lh) continue;
            const uint8_t* p = lifted_->pixel(ix, iy);
            for (int c = 0; c < 4; c++) s[c] = p[c];
        } else {
            double bx = sx - 0.5 + 0.5, by = sy - 0.5 + 0.5; // sample centre-aligned
            int x0 = int(std::floor(bx)), y0 = int(std::floor(by));
            float fx = float(bx - x0), fy = float(by - y0);
            for (int c = 0; c < 4; c++) s[c] = 0;
            for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
                int px = x0 + i, py = y0 + j;
                float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                if (w <= 0 || px < 0 || py < 0 || px >= lw || py >= lh) continue;
                const uint8_t* p = lifted_->pixel(px, py);
                for (int c = 0; c < 4; c++) s[c] += p[c] * w;
            }
        }
        if (s[3] <= 0) continue;
        uint8_t* d = working_->pixel(x, y);
        float a = s[3] / 255.0f;
        for (int c = 0; c < 4; c++) d[c] = uint8_t(clamp(s[c] + d[c] * (1 - a) + 0.5f, 0.0f, 255.0f));
    }
    touched_ = true;
    dirtyGrid_ = {};
}

void BrushStroke::fillGradientOver(int shape, Point from, Point to, const float startColor[4], const float endColor[4], double opacity) {
    if (!valid_) return;
    touched_ = true;
    dirtyGrid_ = {}; // the working image is composed here, not from the coverage
    // Pixels outside the canvas are left alone: the canvas as grid coverage, times the selection.
    GrayImage inside(width_, height_, 0);
    for (int y = 0; y < height_; y++) {
        Point d = pixelToDocument_.apply({0.5, y + 0.5});
        Point dd = pixelToDocument_.applyVector({1, 0});
        for (int x = 0; x < width_; x++, d = d + dd)
            if (canvas_.contains(d)) inside.at(x, y) = selection_ ? selection_->at(x, y) : 255;
    }
    GradientStops stops;
    for (int c = 0; c < 4; c++) { stops.start[c] = startColor[c]; stops.end[c] = endColor[c]; }
    if (isMask_) fillGradient(*baseMask_, *workingMask_, pixelToDocument_, GradientShape(shape), from, to, stops, opacity, &inside);
    else fillGradient(*base_, *working_, pixelToDocument_, GradientShape(shape), from, to, stops, opacity, &inside);
}

void BrushStroke::fillColor(double red, double green, double blue) {
    float color[4] = {float(red), float(green), float(blue), 1.0f};
    fillGradientOver(0, {0, 0}, {0, 0}, color, color, 1);
}

void BrushStroke::heal() {
    if (!settings_.healing || isMask_ || !coverage_) return;
    PixelBounds b = nonzeroBounds(*coverage_);
    if (b.isEmpty()) return;
    // Room for the kernel's patch search, which looks up to about three spot-widths away.
    double reach = (std::max(b.x1 - b.x0, b.y1 - b.y0) + 32) * 3.2;
    Rect region = Rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0).insetBy(-reach, -reach).intersection(Rect(0, 0, width_, height_)).integral();
    int rx = int(region.x), ry = int(region.y), rw = int(region.width), rh = int(region.height);
    if (rw <= 0 || rh <= 0) return;
    auto pixels = cropImage(*base_, rx, ry, rw, rh);
    auto painting = cropGray(*coverage_, rx, ry, rw, rh);
    if (selection_) for (int y = 0; y < rh; y++) for (int x = 0; x < rw; x++) painting->at(x, y) = uint8_t((painting->at(x, y) * selection_->at(x + rx, y + ry) + 127) / 255);
    // The kernel treats any touched pixel as the hole, so a soft tip would make its own faint rim part of
    // the hole and leave it half healed. Heal the solid core only, then feather the result in by coverage.
    auto core = std::make_shared<GrayImage>(rw, rh);
    for (int y = 0; y < rh; y++) for (int x = 0; x < rw; x++) core->at(x, y) = painting->at(x, y) >= 128 ? 255 : 0;
    if (spot_heal(pixels->data(), core->data(), size_t(rw), size_t(rh), size_t(pixels->stride()), float(settings_.opacity), settings_.healingMode, settings_.healingSeed) != 0) return;
    // The healed pixels replace the wash: the working image becomes the original with the healed region.
    working_ = std::make_shared<Image>(*base_);
    for (int y = 0; y < rh; y++) {
        uint8_t* dst = working_->pixel(rx, y + ry);
        const uint8_t* healed = pixels->row(y);
        const uint8_t* orig = base_->pixel(rx, y + ry);
        for (int x = 0; x < rw; x++, dst += 4, healed += 4, orig += 4) {
            unsigned k = painting->at(x, y);
            if (k == 0) continue;
            if (k >= 255) { std::memcpy(dst, healed, 4); continue; }
            for (int c = 0; c < 4; c++) dst[c] = uint8_t((orig[c] * (255 - k) + healed[c] * k + 127) / 255);
        }
    }
}

BrushStroke::Commit BrushStroke::commit() {
    Commit result;
    result.transform = layerTransform_;
    if (!valid_) return result;
    flush();
    if (settings_.healing) heal();
    if (isMask_) {
        result.mask = MaskAsset::make(workingMask_);
        result.maskPlacement = paintTransform_.samePlacement(layerTransform_) ? std::nullopt : std::optional<LayerTransform>(paintTransform_);
        return result;
    }
    // Keep every nonzero-alpha pixel; an existing layer keeps at least its old bounds.
    PixelBounds b = alphaBounds(*working_);
    Rect crop = b.isEmpty() ? Rect() : Rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
    bool hadSource = base_ && alphaBounds(*base_).x1 > 0;
    if (hadSource) crop = crop.unionWith(sourceRect_);
    if (crop.isEmpty()) crop = hadSource ? sourceRect_ : Rect(0, 0, width_, height_);
    crop = crop.integral();
    std::shared_ptr<Image> image = (crop == Rect(0, 0, width_, height_)) ? working_ : cropImage(*working_, int(crop.minX()), int(crop.minY()), int(crop.width), int(crop.height));
    result.asset = Asset::make(image, name_);
    Point center = pixelToDocument_.apply({crop.midX(), crop.midY()});
    LayerTransform t = paintTransform_;
    t.size = {crop.width * paintTransform_.size.width / width_, crop.height * paintTransform_.size.height / height_};
    t.origin = {center.x - t.size.width / 2, center.y - t.size.height / 2};
    result.transform = t;
    return result;
}

} // namespace compositor
