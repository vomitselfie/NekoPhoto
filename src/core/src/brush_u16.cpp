// BrushStroke on a 16-bit document (docs/high-bit-depth-plan.md, P3b): the same stroke as brush.cpp with the working
// pixels, the mask, the coverage and the selection at 0..32768. The dab profile, the stamps, the build-up rules and the
// recompose follow the 8-bit code step for step, with 15-bit samples in place of bytes, so an 8-bit-sourced layer
// painted here and reduced to 8 bits lands within a level of the 8-bit stroke (tests/depth_paint_tests.cpp).
#include "compositor/brush.h"
#include "compositor/depth.h"
#include "compositor/heal.h"
#include "compositor/parallel.h"
#include "compositor/render.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace compositor {

namespace {

constexpr uint32_t one = one16;
constexpr uint32_t half = one16 / 2;

inline long long areaOf(int w, int h) { return (long long)(w) * (long long)(h); }

/// A 15-bit sample scaled by a 15-bit factor, rounded.
inline uint32_t scale15(uint32_t v, uint32_t k) { return (v * k + half) >> 15; }

void stretchGray(const Gray16& source, Gray16& target, const Rect& rect) {
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

void copyImage(const Image16& source, Image16& target, int dx, int dy) {
    const int x0 = std::max(0, -dx), x1 = std::min(source.width(), target.width() - dx);
    if (x1 <= x0) return;
    parallelFor(0, source.height(), 256, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const int ty = y + dy;
            if (ty < 0 || ty >= target.height()) continue;
            std::memcpy(target.pixel(x0 + dx, ty), source.pixel(x0, y), size_t(x1 - x0) * 4 * sizeof(uint16_t));
        }
    });
}

/// Half-open bounds of the pixels with any alpha within `within`: each row scanned in from both ends, rows in parallel.
PixelBounds alphaBoundsWithin(const Image16& image, const PixelBounds& within) {
    const int x0 = std::max(0, within.x0), y0 = std::max(0, within.y0), x1 = std::min(image.width(), within.x1), y1 = std::min(image.height(), within.y1);
    if (x0 >= x1 || y0 >= y1) return {};
    std::vector<int> first(size_t(y1 - y0), x1), last(size_t(y1 - y0), x0);
    parallelRows(y0, y1, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint16_t* row = image.row(y);
            int a = x0;
            while (a < x1 && !row[a * 4 + 3]) a++;
            if (a == x1) continue;
            int b = x1;
            while (b > a && !row[(b - 1) * 4 + 3]) b--;
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

/// A bilinear sample of a 16-bit image (or an ensured tiled one) at a document point, transparent outside.
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
            const uint16_t* p = at(px, py);
            if (!p) continue;
            for (int k = 0; k < 4; k++) s[k] += p[k] * w;
        }
}

} // namespace

BrushStroke::BrushStroke(const Layer& layer, bool mask, BrushSettings settings, Size canvas, SampleType depth, const Gray16* selection)
    : isMask_(mask), settings_(settings), canvas_(0, 0, canvas.width, canvas.height), name_(layer.name), layerTransform_(layer.transform) {
    if (depth != SampleType::U16) { error_ = "A stroke at this depth needs the 8-bit constructor."; return; }
    depth_ = depth;
    initSixteen(layer, mask, selection);
}

void BrushStroke::initSixteen(const Layer& layer, bool mask, const Gray16* selection) {
    const Gray16* ownMask = layer.mask ? layer.mask->asset.image.u16().get() : nullptr;
    const Gray16* placedMask = nullptr;
    LayerTransform base = layer.transform;
    if (mask && layer.mask && layer.mask->placement && ownMask) {
        placedMask = ownMask;
        base = *layer.mask->placement;
    }
    const int originalWidth = placedMask ? placedMask->width() : layer.pixelWidth();
    const int originalHeight = placedMask ? placedMask->height() : layer.pixelHeight();
    const Affine originalMapping = base.pixelToDocument(originalWidth, originalHeight);
    const Rect originalBounds(0, 0, originalWidth, originalHeight);
    Rect extent = originalBounds;
    if (!mask) extent = originalBounds.unionWith(originalMapping.inverted().mapBounds(canvas_).integral());
    width_ = int(extent.width);
    height_ = int(extent.height);
    sourceRect_ = originalBounds.offsetBy(-extent.minX(), -extent.minY());
    pixelToDocument_ = originalMapping.translatedBy(extent.minX(), extent.minY());
    documentToPixel_ = pixelToDocument_.inverted();
    LayerTransform expanded = base;
    expanded.size = {double(width_) * base.size.width / originalWidth, double(height_) * base.size.height / originalHeight};
    const Point center = originalMapping.apply({extent.midX(), extent.midY()});
    expanded.origin = {center.x - expanded.size.width / 2, center.y - expanded.size.height / 2};
    paintTransform_ = expanded;

    const Image16* own = layer.asset ? layer.asset->image.u16().get() : nullptr;
    if (width_ < 1 || height_ < 1 || double(width_) * height_ > double(Document::imagePixelBudget(SampleType::U16)) || originalWidth > maxImageSide || originalHeight > maxImageSide
        || !std::isfinite(settings_.diameter) || settings_.diameter < 1 || settings_.diameter > 2000
        || !std::isfinite(settings_.hardness) || settings_.hardness < 0 || settings_.hardness > 1
        || !std::isfinite(settings_.opacity) || settings_.opacity < 0.01 || settings_.opacity > 1) {
        error_ = "This stroke exceeds the supported canvas or brush limits.";
        return;
    }
    if ((!mask && layer.asset && layer.asset->image && !own) || (mask && layer.mask && layer.mask->asset.image && !ownMask)) {
        error_ = "The layer's pixels are not at the document's depth.";
        return;
    }

    if (mask) {
        baseMask16_ = std::make_shared<Gray16>(width_, height_, uint16_t(one));
        if (placedMask || ownMask) stretchGray(placedMask ? *placedMask : *ownMask, *baseMask16_, sourceRect_);
        workingMask16_ = std::make_shared<Gray16>(*baseMask16_);
    } else {
        const bool sameGrid = own && sourceRect_ == Rect(0, 0, width_, height_) && own->width() == width_ && own->height() == height_;
        if (sameGrid) {
            base16_ = layer.asset->image.u16();
            baseBounds_ = alphaBoundsWithin(*base16_, {0, 0, width_, height_});
            auto working = std::make_shared<Image16>(width_, height_);
            copyImage(*base16_, *working, 0, 0);
            working16_ = working;
        } else {
            auto copy = std::make_shared<Image16>(width_, height_);
            auto working = std::make_shared<Image16>(width_, height_);
            if (own) {
                const int dx = int(sourceRect_.minX()), dy = int(sourceRect_.minY());
                copyImage(*own, *copy, dx, dy);
                copyImage(*own, *working, dx, dy);
                PixelBounds b = alphaBoundsWithin(*own, {0, 0, own->width(), own->height()});
                if (!b.isEmpty()) {
                    b = {std::max(0, b.x0 + dx), std::max(0, b.y0 + dy), std::min(width_, b.x1 + dx), std::min(height_, b.y1 + dy)};
                    baseBounds_ = b.isEmpty() ? PixelBounds{} : b;
                }
            }
            base16_ = copy;
            working16_ = working;
        }
        if (!working16_) working16_ = std::make_shared<Image16>(*base16_);
        if (layer.mask && layer.mask->enabled && !layer.mask->placement && ownMask && ownMask->width() == originalWidth && ownMask->height() == originalHeight) {
            visible16_ = std::make_shared<Gray16>(width_, height_, uint16_t(one));
            stretchGray(*ownMask, *visible16_, sourceRect_);
        }
    }
    coverage16_ = std::make_shared<Gray16>(width_, height_, 0);
    if (selection && !selection->isEmpty()) {
        selection16_ = std::make_shared<Gray16>(width_, height_, 0);
        for (int y = 0; y < height_; y++) {
            Point d = pixelToDocument_.apply({0.5, y + 0.5});
            const Point dd = pixelToDocument_.applyVector({1, 0});
            for (int x = 0; x < width_; x++, d = d + dd) {
                const int sx = int(std::floor(d.x)), sy = int(std::floor(d.y));
                if (sx < 0 || sy < 0 || sx >= selection->width() || sy >= selection->height()) continue;
                selection16_->at(x, y) = uint16_t(std::min<uint32_t>(selection->at(sx, sy), one));
            }
        }
    }
    valid_ = true;
}

void BrushStroke::saveTail(const Rect& affected) {
    const int x0 = int(affected.minX()), y0 = int(affected.minY()), w = int(affected.width), h = int(affected.height);
    tailBackup16_.resize(size_t(w) * size_t(h));
    for (int y = 0; y < h; y++) std::memcpy(&tailBackup16_[size_t(y) * size_t(w)], coverage16_->row(y0 + y) + x0, size_t(w) * sizeof(uint16_t));
}

void BrushStroke::restoreTail() {
    const int x0 = int(tailRect_.minX()), y0 = int(tailRect_.minY()), w = int(tailRect_.width), h = int(tailRect_.height);
    for (int y = 0; y < h; y++) std::memcpy(coverage16_->row(y0 + y) + x0, &tailBackup16_[size_t(y) * size_t(w)], size_t(w) * sizeof(uint16_t));
}

void BrushStroke::refreshDabTable16(double radius, double hardness, double footprint) {
    if (dabTableRadius_ == radius && dabTableHardness_ == hardness && dabTableFootprint_ == footprint) return;
    dabTableRadius_ = radius; dabTableHardness_ = hardness; dabTableFootprint_ = footprint;
    const bool hard = hardness >= 1;
    const double inner = radius * hardness;
    const double reach = radius + footprint;
    const int entries = 8192;
    dabTableScale_ = entries / (reach * reach);
    dabTable16_.assign(size_t(entries) + 2, 0);
    for (int i = 0; i <= entries; i++) {
        const double dist = std::sqrt(i / dabTableScale_);
        double value;
        if (hard) value = clamp((radius - dist) / std::max(1e-9, footprint) + 0.5, 0.0, 1.0);
        else if (dist <= inner) value = 1;
        else if (dist >= radius) value = 0;
        else value = brushFalloff((dist - inner) / std::max(1e-9, radius - inner));
        dabTable16_[size_t(i)] = uint16_t(clamp(value * one + 0.5, 0.0, double(one)));
    }
}

bool BrushStroke::stampDab16(Point center, double radius, const Rect& affected) {
    const Affine& g = pixelToDocument_;
    if (!settings_.stampedDabs || g.b != 0 || g.c != 0 || g.a != g.d || !(g.a > 0)) return false;
    const double scale = g.a;
    const double gridRadius = radius / scale;
    const int side = 2 * int(std::ceil(gridRadius + 1)) + 2;
    if (side > 2600) return false;
    const bool hard = settings_.hardness >= 1;
    StampOf<uint16_t>& stamp = stamp16_;
    if (stamp.side != side || stamp.radius != radius || stamp.hardness != settings_.hardness || stamp.scale != scale) {
        stamp.side = side; stamp.radius = radius; stamp.hardness = settings_.hardness; stamp.scale = scale;
        stamp.steps = side <= 512 || (hard && side <= 2048) ? 4 : 2;
        for (int phase = 0; phase < 16; phase++) { stamp.tiles[phase].clear(); stamp.tiles[phase].shrink_to_fit(); stamp.built[phase] = false; }
    }
    const int steps = stamp.steps, shift = steps == 4 ? 2 : 1;
    const Point gc = documentToPixel_.apply(center);
    const int qx = int(std::floor(gc.x * steps + 0.5)), qy = int(std::floor(gc.y * steps + 0.5));
    const int phase = (qx & (steps - 1)) + (qy & (steps - 1)) * steps;
    const int ox = (qx >> shift) - side / 2, oy = (qy >> shift) - side / 2;
    if (!stamp.built[phase]) {
        const double reach2 = (radius + scale) * (radius + scale);
        const double cx = side / 2 + double(phase % steps) / steps, cy = side / 2 + double(phase / steps) / steps;
        std::vector<uint16_t>& tile = stamp.tiles[phase];
        tile.assign(size_t(side) * size_t(side), 0);
        parallelRows(0, side, [&](int ya, int yb) {
            for (int j = ya; j < yb; j++)
                for (int i = 0; i < side; i++) {
                    const double dx = (i + 0.5 - cx) * scale, dy = (j + 0.5 - cy) * scale, q = dx * dx + dy * dy;
                    if (q >= reach2) continue;
                    const double index = q * dabTableScale_;
                    const int k = int(index);
                    const unsigned frac = unsigned((index - k) * 256);
                    tile[size_t(j) * side + size_t(i)] = uint16_t((dabTable16_[size_t(k)] * (256 - frac) + dabTable16_[size_t(k) + 1] * frac + 128) >> 8);
                }
        }, 64);
        stamp.built[phase] = true;
    }
    const Rect canvasGrid = documentToPixel_.mapBounds(canvas_);
    const int x0 = std::max({int(affected.minX()), ox, int(std::ceil(canvasGrid.minX() - 0.5))}), x1 = std::min({int(affected.maxX()), ox + side, int(std::ceil(canvasGrid.maxX() - 0.5))});
    const int y0 = std::max({int(affected.minY()), oy, int(std::ceil(canvasGrid.minY() - 0.5))}), y1 = std::min({int(affected.maxY()), oy + side, int(std::ceil(canvasGrid.maxY() - 0.5))});
    if (x0 >= x1 || y0 >= y1) return true;
    const std::vector<uint16_t>& tile = stamp.tiles[phase];
    const int n = x1 - x0;
    auto merge = [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            uint16_t* row = coverage16_->row(y) + x0;
            const uint16_t* t = &tile[size_t(y - oy) * side + size_t(x0 - ox)];
            if (hard) for (int i = 0; i < n; i++) row[i] = std::max(row[i], t[i]);
            else for (int i = 0; i < n; i++) row[i] = uint16_t(row[i] + scale15(t[i], one - row[i]));
        }
    };
    if (areaOf(n, y1 - y0) >= 1 << 20) parallelRows(y0, y1, merge, 32); else merge(y0, y1);
    return true;
}

void BrushStroke::dab16(Point center) {
    const double radius = settings_.diameter / 2;
    const Rect circle(center.x - radius, center.y - radius, radius * 2, radius * 2);
    const Rect clipped = circle.intersection(canvas_);
    if (clipped.isEmpty()) return;
    const Rect affected = documentToPixel_.mapBounds(clipped).integral().intersection(Rect(0, 0, width_, height_));
    if (affected.isEmpty()) return;
    const double footprint = std::hypot(pixelToDocument_.a, pixelToDocument_.b);
    refreshDabTable16(radius, settings_.hardness, footprint);
    if (stampDab16(center, radius, affected)) { markDirty(affected); return; }
    const bool hard = settings_.hardness >= 1;
    const bool whollyInside = clipped == circle;
    const double reach2 = (radius + footprint) * (radius + footprint);
    const int x0 = int(affected.minX()), x1 = int(affected.maxX()), y0 = int(affected.minY()), y1 = int(affected.maxY());
    const Point dd = pixelToDocument_.applyVector({1, 0});
    const double dd2 = dd.x * dd.x + dd.y * dd.y;
    auto rows = [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            uint16_t* row = coverage16_->row(y);
            Point d = pixelToDocument_.apply({x0 + 0.5, y + 0.5});
            double rx = d.x - center.x, ry = d.y - center.y;
            double q = rx * rx + ry * ry;
            double dq = 2 * (rx * dd.x + ry * dd.y) + dd2;
            for (int x = x0; x < x1; x++, q += dq, dq += 2 * dd2, d = d + dd) {
                if (q >= reach2) continue;
                if (!whollyInside && !canvas_.contains(d)) continue;
                const double index = q * dabTableScale_;
                const int i = int(index);
                const unsigned frac = unsigned((index - i) * 256);
                const uint32_t value = (dabTable16_[size_t(i)] * (256 - frac) + dabTable16_[size_t(i) + 1] * frac + 128) >> 8;
                if (value == 0) continue;
                const uint32_t old = row[x];
                row[x] = uint16_t(hard ? std::max(old, value) : old + scale15(value, one - old));
            }
        }
    };
    if (areaOf(x1 - x0, y1 - y0) >= 65536) parallelRows(y0, y1, rows, 32); else rows(y0, y1);
    markDirty(affected);
}

void BrushStroke::recomposeRows16(const Rect& r) {
    const int x0 = int(r.minX()), x1 = int(r.maxX()), y0 = int(r.minY()), y1 = int(r.maxY());
    double opacity = settings_.opacity;
    if (isMask_) {
        const double paint = clamp(settings_.maskValue * one + 0.5, 0.0, double(one));
        const Gray16* sample = maskClone16_.get();
        for (int y = y0; y < y1; y++) {
            const uint16_t* cov = coverage16_->row(y);
            const uint16_t* sel = selection16_ ? selection16_->row(y) : nullptr;
            const uint16_t* base = baseMask16_->row(y);
            uint16_t* out = workingMask16_->row(y);
            Point d = pixelToDocument_.apply({x0 + 0.5, y + 0.5});
            const Point dd = pixelToDocument_.applyVector({1, 0});
            for (int x = x0; x < x1; x++, d = d + dd) {
                const double c = cov[x] / double(one) * opacity * (sel ? sel[x] / double(one) : 1.0);
                double value = std::floor(paint);
                if (sample) {
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
                    value = wsum > 0 ? acc / wsum : base[x];
                }
                out[x] = uint16_t(clamp(base[x] * (1 - c) + value * c + 0.5, 0.0, double(one)));
            }
        }
        return;
    }
    double cr = settings_.red * one, cg = settings_.green * one, cb = settings_.blue * one;
    if (settings_.healing && !clone_) { cr = cg = cb = 0.12 * one; opacity *= 0.45; }
    if (!clone_) {
        const uint32_t op = uint32_t(clamp(opacity * one + 0.5, 0.0, double(one)));
        const int colour[4] = {int(clamp(cr + 0.5, 0.0, double(one))), int(clamp(cg + 0.5, 0.0, double(one))), int(clamp(cb + 0.5, 0.0, double(one))), int(one)};
        const bool erasing = settings_.erasing;
        for (int y = y0; y < y1; y++) {
            const uint16_t* cov = coverage16_->row(y);
            const uint16_t* sel = selection16_ ? selection16_->row(y) : nullptr;
            const uint16_t* base = base16_->row(y) + x0 * 4;
            uint16_t* out = working16_->row(y) + x0 * 4;
            for (int x = x0; x < x1; x++, base += 4, out += 4) {
                uint32_t k = cov[x];
                if (sel) k = scale15(k, sel[x]);
                k = scale15(k, op);
                if (k == 0) { std::memcpy(out, base, 4 * sizeof(uint16_t)); continue; }
                if (erasing) { for (int c = 0; c < 4; c++) out[c] = uint16_t(scale15(base[c], one - k)); continue; }
                for (int c = 0; c < 4; c++) {
                    const int delta = colour[c] - int(base[c]);
                    out[c] = uint16_t(int(base[c]) + (delta * int(k) + (delta >= 0 ? int(half) : -int(half))) / int(one));
                }
            }
        }
        return;
    }
    const Image16* src = clone_->image16.get();
    const TiledSource16* tiled = clone_->tiled16.get();
    const int sw = src ? src->width() : tiled ? tiled->width() : 0, sh = src ? src->height() : tiled ? tiled->height() : 0;
    auto at = [&](int px, int py) -> const uint16_t* { return src ? src->pixel(px, py) : tiled->pixel(px, py); };
    for (int y = y0; y < y1; y++) {
        const uint16_t* cov = coverage16_->row(y);
        const uint16_t* sel = selection16_ ? selection16_->row(y) : nullptr;
        const uint16_t* base = base16_->row(y) + x0 * 4;
        uint16_t* out = working16_->row(y) + x0 * 4;
        Point d = pixelToDocument_.apply({x0 + 0.5, y + 0.5});
        const Point dd = pixelToDocument_.applyVector({1, 0});
        for (int x = x0; x < x1; x++, base += 4, out += 4, d = d + dd) {
            const double c = cov[x] / double(one) * opacity * (sel ? sel[x] / double(one) : 1.0);
            if (c <= 0) { std::memcpy(out, base, 4 * sizeof(uint16_t)); continue; }
            if (src || tiled) {
                double s[4];
                bilinear(at, sw, sh, d.x + clone_->offset.x - 0.5, d.y + clone_->offset.y - 0.5, s);
                const double sa = replacesWithClone_ ? c : s[3] / one * c;
                for (int k = 0; k < 4; k++) out[k] = uint16_t(clamp(s[k] * c + base[k] * (1 - sa) + 0.5, 0.0, double(one)));
                continue;
            }
            if (settings_.erasing) {
                for (int k = 0; k < 4; k++) out[k] = uint16_t(base[k] * (1 - c) + 0.5);
            } else {
                out[0] = uint16_t(clamp(base[0] * (1 - c) + cr * c + 0.5, 0.0, double(one)));
                out[1] = uint16_t(clamp(base[1] * (1 - c) + cg * c + 0.5, 0.0, double(one)));
                out[2] = uint16_t(clamp(base[2] * (1 - c) + cb * c + 0.5, 0.0, double(one)));
                out[3] = uint16_t(clamp(base[3] * (1 - c) + one * c + 0.5, 0.0, double(one)));
            }
        }
    }
}

bool BrushStroke::liftSelection16() {
    if (isMask_ || !selection16_ || !base16_) return false;
    const PixelBounds b = nonzeroBounds(*selection16_);
    const Rect region = b.isEmpty() ? Rect() : Rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0).intersection(sourceRect_).integral();
    if (region.isEmpty()) return false;
    liftedRect_ = region;
    lifted16_ = std::make_shared<Image16>(int(region.width), int(region.height));
    bool any = false;
    for (int y = 0; y < lifted16_->height(); y++)
        for (int x = 0; x < lifted16_->width(); x++) {
            const int sx = x + int(region.x), sy = y + int(region.y);
            const uint32_t k = selection16_->at(sx, sy);
            const uint16_t* s = base16_->pixel(sx, sy);
            uint16_t* d = lifted16_->pixel(x, y);
            for (int c = 0; c < 4; c++) d[c] = uint16_t(scale15(s[c], k));
            if (d[3]) any = true;
        }
    return any;
}

void BrushStroke::moveLifted16(Point offset, bool duplicate) {
    if (!lifted16_ || !selection16_) return;
    const Point zero = documentToPixel_.apply({0, 0}), moved = documentToPixel_.apply(offset);
    const double gx = moved.x - zero.x, gy = moved.y - zero.y;
    working16_ = std::make_shared<Image16>(*base16_);
    if (!duplicate)
        for (int y = 0; y < height_; y++)
            for (int x = 0; x < width_; x++) {
                const uint32_t k = selection16_->at(x, y);
                if (!k) continue;
                uint16_t* p = working16_->pixel(x, y);
                for (int c = 0; c < 4; c++) p[c] = uint16_t(scale15(p[c], one - std::min(k, one)));
            }
    const bool whole = std::fabs(gx - std::round(gx)) < 1e-6 && std::fabs(gy - std::round(gy)) < 1e-6;
    const double tx = liftedRect_.x + gx, ty = liftedRect_.y + gy;
    const int lw = lifted16_->width(), lh = lifted16_->height();
    const Rect target = Rect(tx, ty, lw, lh).insetBy(-1, -1).integral().intersection(Rect(0, 0, width_, height_));
    for (int y = int(target.minY()); y < int(target.maxY()); y++)
        for (int x = int(target.minX()); x < int(target.maxX()); x++) {
            const double sx = x - tx, sy = y - ty;
            float s[4] = {0, 0, 0, 0};
            if (whole) {
                const int ix = int(std::lround(sx)), iy = int(std::lround(sy));
                if (ix < 0 || iy < 0 || ix >= lw || iy >= lh) continue;
                const uint16_t* p = lifted16_->pixel(ix, iy);
                for (int c = 0; c < 4; c++) s[c] = p[c];
            } else {
                const int bx = int(std::floor(sx)), by = int(std::floor(sy));
                const float fx = float(sx - bx), fy = float(sy - by);
                for (int j = 0; j < 2; j++)
                    for (int i = 0; i < 2; i++) {
                        const int px = bx + i, py = by + j;
                        const float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                        if (w <= 0 || px < 0 || py < 0 || px >= lw || py >= lh) continue;
                        const uint16_t* p = lifted16_->pixel(px, py);
                        for (int c = 0; c < 4; c++) s[c] += p[c] * w;
                    }
            }
            if (s[3] <= 0) continue;
            uint16_t* d = working16_->pixel(x, y);
            const float a = s[3] / float(one);
            for (int c = 0; c < 4; c++) d[c] = uint16_t(clamp(s[c] + d[c] * (1 - a) + 0.5f, 0.0f, float(one)));
        }
    touched_ = true;
    dirtyGrid_ = {};
    refreshLevels(Rect(0, 0, width_, height_));
}

void BrushStroke::fillGradientOver16(int shape, Point from, Point to, const GradientStops& stops, double opacity) {
    touched_ = true;
    dirtyGrid_ = {};
    Gray16 inside(width_, height_, 0);
    for (int y = 0; y < height_; y++) {
        Point d = pixelToDocument_.apply({0.5, y + 0.5});
        const Point dd = pixelToDocument_.applyVector({1, 0});
        for (int x = 0; x < width_; x++, d = d + dd)
            if (canvas_.contains(d)) inside.at(x, y) = selection16_ ? selection16_->at(x, y) : uint16_t(one);
    }
    if (isMask_) fillGradient(*baseMask16_, *workingMask16_, pixelToDocument_, GradientShape(shape), from, to, stops, opacity, &inside);
    else fillGradient(*base16_, *working16_, pixelToDocument_, GradientShape(shape), from, to, stops, opacity, &inside);
    refreshLevels(Rect(0, 0, width_, height_));
}

void BrushStroke::heal16() {
    if (!settings_.healing || isMask_ || !coverage16_) return;
    const PixelBounds b = nonzeroBounds(*coverage16_);
    if (b.isEmpty()) return;
    if (clone_ && clone_->image16) { healFromClone16(b); return; }
    const double reach = (std::max(b.x1 - b.x0, b.y1 - b.y0) + 32) * 3.2;
    const Rect region = Rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0).insetBy(-reach, -reach).intersection(Rect(0, 0, width_, height_)).integral();
    const int rx = int(region.x), ry = int(region.y), rw = int(region.width), rh = int(region.height);
    if (rw <= 0 || rh <= 0) return;
    auto pixels = cropImage(*base16_, rx, ry, rw, rh);
    auto painting = cropGray(*coverage16_, rx, ry, rw, rh);
    if (selection16_)
        for (int y = 0; y < rh; y++)
            for (int x = 0; x < rw; x++) painting->at(x, y) = uint16_t(scale15(painting->at(x, y), selection16_->at(x + rx, y + ry)));
    // The solid core, as at 8 bits (128 of 255 and up, the same level at 16 bits).
    auto core = std::make_shared<Gray16>(rw, rh);
    const uint16_t solid = widen8(128);
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) core->at(x, y) = painting->at(x, y) >= solid ? uint16_t(one) : 0;
    std::shared_ptr<Gray16> visible = visible16_ ? cropGray(*visible16_, rx, ry, rw, rh) : nullptr;
    spotHeal(*pixels, *core, float(settings_.opacity), settings_.healingMode, settings_.healingSeed, visible.get());
    working16_ = std::make_shared<Image16>(*base16_);
    for (int y = 0; y < rh; y++) {
        uint16_t* dst = working16_->pixel(rx, y + ry);
        const uint16_t* healed = pixels->row(y);
        const uint16_t* orig = base16_->pixel(rx, y + ry);
        for (int x = 0; x < rw; x++, dst += 4, healed += 4, orig += 4) {
            const uint32_t k = std::min<uint32_t>(painting->at(x, y), one);
            if (k == 0) continue;
            if (k >= one) { std::memcpy(dst, healed, 4 * sizeof(uint16_t)); continue; }
            for (int c = 0; c < 4; c++) dst[c] = uint16_t((orig[c] * (one - k) + healed[c] * k + half) >> 15);
        }
    }
    refreshLevels(Rect(0, 0, width_, height_));
}

void BrushStroke::healFromClone16(const PixelBounds& b) {
    const Rect region = Rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0).insetBy(-2, -2).intersection(Rect(0, 0, width_, height_)).integral();
    const int rx = int(region.x), ry = int(region.y), rw = int(region.width), rh = int(region.height);
    if (rw <= 0 || rh <= 0) return;
    auto pixels = cropImage(*base16_, rx, ry, rw, rh);
    auto painting = cropGray(*coverage16_, rx, ry, rw, rh);
    if (selection16_)
        for (int y = 0; y < rh; y++)
            for (int x = 0; x < rw; x++) painting->at(x, y) = uint16_t(scale15(painting->at(x, y), selection16_->at(x + rx, y + ry)));
    Image16 source(rw, rh);
    const Image16& src = *clone_->image16;
    auto at = [&](int px, int py) -> const uint16_t* { return src.pixel(px, py); };
    for (int y = 0; y < rh; y++) {
        Point d = pixelToDocument_.apply({rx + 0.5, y + ry + 0.5});
        const Point dd = pixelToDocument_.applyVector({1, 0});
        for (int x = 0; x < rw; x++, d = d + dd) {
            double s[4];
            bilinear(at, src.width(), src.height(), d.x + clone_->offset.x - 0.5, d.y + clone_->offset.y - 0.5, s);
            uint16_t* out = source.pixel(x, y);
            for (int k = 0; k < 4; k++) out[k] = uint16_t(std::lround(clamp(s[k], 0.0, double(one))));
        }
    }
    std::shared_ptr<Gray16> visible = visible16_ ? cropGray(*visible16_, rx, ry, rw, rh) : nullptr;
    healFrom(*pixels, source, *painting, float(settings_.opacity), visible.get());
    working16_ = std::make_shared<Image16>(*base16_);
    for (int y = 0; y < rh; y++) std::memcpy(working16_->pixel(rx, y + ry), pixels->row(y), size_t(rw) * 4 * sizeof(uint16_t));
    refreshLevels(Rect(0, 0, width_, height_));
}

BrushStroke::Commit BrushStroke::commit16() {
    Commit result;
    result.transform = layerTransform_;
    flush();
    if (settings_.healing) heal16();
    if (isMask_) {
        result.mask = MaskAsset::make(Gray16Ptr(workingMask16_));
        result.maskPlacement = paintTransform_.samePlacement(layerTransform_) ? std::nullopt : std::optional<LayerTransform>(paintTransform_);
        return result;
    }
    const Rect scan = Rect(baseBounds_.x0, baseBounds_.y0, baseBounds_.x1 - baseBounds_.x0, baseBounds_.y1 - baseBounds_.y0).unionWith(touchedGrid_).integral();
    const PixelBounds b = scan.isEmpty() ? PixelBounds{} : alphaBoundsWithin(*working16_, PixelBounds{int(scan.minX()), int(scan.minY()), int(scan.maxX()), int(scan.maxY())});
    Rect crop = b.isEmpty() ? Rect() : Rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
    const bool hadSource = base16_ && !baseBounds_.isEmpty();
    if (hadSource) crop = crop.unionWith(sourceRect_);
    if (crop.isEmpty()) crop = hadSource ? sourceRect_ : Rect(0, 0, width_, height_);
    crop = crop.integral();
    std::shared_ptr<Image16> image = (crop == Rect(0, 0, width_, height_)) ? working16_ : cropImage(*working16_, int(crop.minX()), int(crop.minY()), int(crop.width), int(crop.height));
    result.asset = Asset::make(Image16Ptr(image), name_);
    const Point center = pixelToDocument_.apply({crop.midX(), crop.midY()});
    LayerTransform t = paintTransform_;
    t.size = {crop.width * paintTransform_.size.width / width_, crop.height * paintTransform_.size.height / height_};
    t.origin = {center.x - t.size.width / 2, center.y - t.size.height / 2};
    result.transform = t;
    return result;
}

std::shared_ptr<TiledSource16> tiledProcessedDocument16(Document document, std::function<void(Image16&)> process, int margin) {
    const int w = document.width, h = document.height;
    margin = std::max(0, margin);
    auto shared = std::make_shared<Document>(std::move(document));
    return std::make_shared<TiledSource16>(w, h, [shared, process = std::move(process), margin, w, h](int x, int y, int tw, int th, Image16& out) {
        const int px0 = std::max(0, x - margin), py0 = std::max(0, y - margin);
        const int px1 = std::min(w, x + tw + margin), py1 = std::min(h, y + th + margin);
        RenderOptions options;
        options.region = Rect(px0, py0, px1 - px0, py1 - py0);
        Image16 padded;
        render16(*shared, options, padded);
        process(padded);
        for (int row = 0; row < th; row++) std::memcpy(out.row(row), padded.pixel(x - px0, y + row - py0), size_t(tw) * 4 * sizeof(uint16_t));
    });
}

} // namespace compositor
