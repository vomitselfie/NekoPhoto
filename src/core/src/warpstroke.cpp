#include "compositor/warpstroke.h"
#include "compositor/resample.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>

namespace compositor {

WarpStroke::WarpStroke(std::shared_ptr<Image> image, WarpMode mode, double diameter, double hardness, double strength)
    : image_(std::move(image)), mode_(mode), diameter_(std::max(2.0, diameter)), hardness_(std::min(0.98, std::max(0.0, hardness))),
      strength_(std::min(1.0, std::max(0.01, strength))), width_(image_->width()), height_(image_->height()) {
    if (mode_ == WarpMode::Liquify) original_ = std::make_shared<Image>(*image_);
}

float WarpStroke::weight(float u) const {
    if (u >= 1) return 0;
    float h = float(hardness_);
    if (u <= h) return 1;
    float t = (1 - u) / (1 - h);
    return t * t * (3 - 2 * t);
}

void WarpStroke::append(Point point) {
    if (!hasLast_) {
        last_ = point;
        hasLast_ = true;
        if (mode_ == WarpMode::Smudge) pickUp(point);
        return;
    }
    double distance = std::hypot(point.x - last_.x, point.y - last_.y);
    double spacing = std::max(1.0, diameter_ * (mode_ == WarpMode::Smudge ? 0.08 : 0.025));
    if (distance < spacing) return;
    int steps = int(std::ceil(distance / spacing));
    Point previous = last_;
    for (int step = 1; step <= steps; step++) {
        double t = double(step) / steps;
        Point next{last_.x + (point.x - last_.x) * t, last_.y + (point.y - last_.y) * t};
        if (mode_ == WarpMode::Smudge) smudge(next); else push(previous, next);
        points_.push_back(next);
        previous = next;
    }
    last_ = point;
}

void WarpStroke::markDirty(int x0, int y0, int x1, int y1) {
    x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(width_, x1); y1 = std::min(height_, y1);
    if (x0 >= x1 || y0 >= y1) return;
    if (dirtyX0_ >= dirtyX1_) { dirtyX0_ = x0; dirtyY0_ = y0; dirtyX1_ = x1; dirtyY1_ = y1; return; }
    dirtyX0_ = std::min(dirtyX0_, x0); dirtyY0_ = std::min(dirtyY0_, y0); dirtyX1_ = std::max(dirtyX1_, x1); dirtyY1_ = std::max(dirtyY1_, y1);
}

Rect WarpStroke::takeDirtyRect() {
    if (dirtyX0_ >= dirtyX1_) return {};
    // The renderer draws zoomed-out layers from reduced copies cached by image; this image changes in place.
    MipCache::shared().refresh(image_.get(), dirtyX0_, dirtyY0_, dirtyX1_, dirtyY1_);
    Rect r(dirtyX0_, dirtyY0_, dirtyX1_ - dirtyX0_, dirtyY1_ - dirtyY0_);
    dirtyX0_ = dirtyX1_ = 0;
    return r;
}

void WarpStroke::pickUp(Point center) {
    int r = radius(), side = 2 * r + 1;
    carried_.assign(size_t(side) * side * 4, 0);
    int cx = int(std::lround(center.x)), cy = int(std::lround(center.y));
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= height_) continue;
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx;
            if (x < 0 || x >= width_) continue;
            const uint8_t* p = image_->pixel(x, y);
            size_t c = (size_t(dy + r) * side + size_t(dx + r)) * 4;
            for (int k = 0; k < 4; k++) carried_[c + k] = p[k];
        }
    }
}

void WarpStroke::smudge(Point center) {
    int r = radius(), side = 2 * r + 1;
    int cx = int(std::lround(center.x)), cy = int(std::lround(center.y));
    markDirty(cx - r, cy - r, cx + r + 1, cy + r + 1);
    float keep = float(strength_), invR = 1 / float(diameter_ / 2);
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= height_) continue;
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx;
            if (x < 0 || x >= width_) continue;
            float w = weight(std::sqrt(float(dx * dx + dy * dy)) * invR);
            if (w <= 0) continue;
            uint8_t* p = image_->pixel(x, y);
            size_t c = (size_t(dy + r) * side + size_t(dx + r)) * 4;
            for (int k = 0; k < 4; k++) {
                float under = p[k];
                float painted = under + (carried_[c + k] - under) * w;
                p[k] = uint8_t(std::max(0.0f, std::min(255.0f, std::round(painted))));
                carried_[c + k] = painted + (carried_[c + k] - painted) * keep;
            }
        }
    }
}

void WarpStroke::growField(int x0, int y0, int x1, int y1) {
    if (field_.width > 0 && x0 >= field_.x0 && y0 >= field_.y0 && x1 <= field_.x0 + field_.width && y1 <= field_.y0 + field_.height) return;
    constexpr int pad = 32;
    const int nx0 = std::max(0, std::min(field_.width > 0 ? field_.x0 : x0, x0 - pad)), ny0 = std::max(0, std::min(field_.width > 0 ? field_.y0 : y0, y0 - pad));
    const int nx1 = std::min(width_, std::max(field_.width > 0 ? field_.x0 + field_.width : x1, x1 + pad));
    const int ny1 = std::min(height_, std::max(field_.width > 0 ? field_.y0 + field_.height : y1, y1 + pad));
    Field grown;
    grown.x0 = nx0; grown.y0 = ny0; grown.width = nx1 - nx0; grown.height = ny1 - ny0;
    grown.offsets.assign(size_t(grown.width) * grown.height * 2, 0.0f);
    for (int y = 0; y < field_.height; y++)
        std::copy_n(&field_.offsets[size_t(y) * field_.width * 2], size_t(field_.width) * 2, &grown.offsets[(size_t(y + field_.y0 - ny0) * grown.width + size_t(field_.x0 - nx0)) * 2]);
    field_ = std::move(grown);
}

void WarpStroke::fieldAt(double x, double y, float& dx, float& dy) const {
    // Bilinear over the field's pixel centres; zero beyond the field.
    dx = dy = 0;
    if (field_.width == 0) return;
    const double fx = x - 0.5 - field_.x0, fy = y - 0.5 - field_.y0;
    if (fx <= -1 || fy <= -1 || fx >= field_.width || fy >= field_.height) return;
    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
    const float tx = float(fx - ix), ty = float(fy - iy);
    auto at = [&](int px, int py, int c) -> float {
        if (px < 0 || py < 0 || px >= field_.width || py >= field_.height) return 0;
        return field_.offsets[(size_t(py) * field_.width + size_t(px)) * 2 + size_t(c)];
    };
    dx = (at(ix, iy, 0) * (1 - tx) + at(ix + 1, iy, 0) * tx) * (1 - ty) + (at(ix, iy + 1, 0) * (1 - tx) + at(ix + 1, iy + 1, 0) * tx) * ty;
    dy = (at(ix, iy, 1) * (1 - tx) + at(ix + 1, iy, 1) * tx) * (1 - ty) + (at(ix, iy + 1, 1) * (1 - tx) + at(ix + 1, iy + 1, 1) * tx) * ty;
}

void WarpStroke::push(Point a, Point b) {
    const int r = radius();
    const double moveX = (b.x - a.x) * strength_, moveY = (b.y - a.y) * strength_;
    const int cx = int(std::lround(b.x)), cy = int(std::lround(b.y));
    const int x0 = std::max(0, cx - r), x1 = std::min(width_ - 1, cx + r);
    const int y0 = std::max(0, cy - r), y1 = std::min(height_ - 1, cy + r);
    if (x0 > x1 || y0 > y1) return;
    growField(x0, y0, x1 + 1, y1 + 1);
    markDirty(x0, y0, x1 + 1, y1 + 1);
    // Each pixel under the brush now shows what the result so far showed a little behind it, so it takes
    // that point's displacement plus the move (Gustafsson's forward warp: the falloff, shaped by the
    // hardness, shrinks with the length of the drag).
    const double R = diameter_ / 2, drag2 = moveX * moveX + moveY * moveY;
    const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    std::vector<float> updated(size_t(bw) * bh * 2);
    parallelRows(y0, y1 + 1, [&](int ya, int yb) {
    for (int y = ya; y < yb; y++)
        for (int x = x0; x <= x1; x++) {
            float* u = &updated[(size_t(y - y0) * bw + size_t(x - x0)) * 2];
            const double u0 = std::hypot(x + 0.5 - b.x, y + 0.5 - b.y) / R;
            fieldAt(x + 0.5, y + 0.5, u[0], u[1]);
            if (u0 >= 1) continue;
            const double s = hardness_ >= 1 ? 0 : std::max(0.0, (u0 - hardness_) / (1 - hardness_));
            const double band = R * R * (1 - s * s);
            const double w = (band / (band + drag2)) * (band / (band + drag2));
            const double vx = moveX * w, vy = moveY * w;
            float px, py;
            fieldAt(x + 0.5 - vx, y + 0.5 - vy, px, py);
            u[0] = float(px - vx); u[1] = float(py - vy);
        }
    }, 16);
    for (int y = y0; y <= y1; y++)
        std::copy_n(&updated[size_t(y - y0) * bw * 2], size_t(bw) * 2, &field_.offsets[(size_t(y - field_.y0) * field_.width + size_t(x0 - field_.x0)) * 2]);
    // The result under the brush, resampled from the untouched original through the field.
    parallelRows(y0, y1 + 1, [&](int ya, int yb) {
    for (int y = ya; y < yb; y++)
        for (int x = x0; x <= x1; x++) {
            const float* d = &field_.offsets[(size_t(y - field_.y0) * field_.width + size_t(x - field_.x0)) * 2];
            sampleBicubic(*original_, x + 0.5 + d[0], y + 0.5 + d[1], image_->pixel(x, y));
        }
    }, 16);
}

} // namespace compositor
