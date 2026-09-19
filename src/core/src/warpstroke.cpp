#include "compositor/warpstroke.h"
#include <algorithm>
#include <cmath>

namespace compositor {

WarpStroke::WarpStroke(std::shared_ptr<Image> image, WarpMode mode, double diameter, double hardness, double strength)
    : image_(std::move(image)), mode_(mode), diameter_(std::max(2.0, diameter)), hardness_(std::min(0.98, std::max(0.0, hardness))),
      strength_(std::min(1.0, std::max(0.01, strength))), width_(image_->width()), height_(image_->height()) {}

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

void WarpStroke::push(Point a, Point b) {
    int r = radius();
    float moveX = float((b.x - a.x) * strength_), moveY = float((b.y - a.y) * strength_);
    int margin = int(std::ceil(std::max(std::fabs(moveX), std::fabs(moveY)))) + 2;
    int cx = int(std::lround(b.x)), cy = int(std::lround(b.y));
    int x0 = std::max(0, cx - r - margin), x1 = std::min(width_ - 1, cx + r + margin);
    int y0 = std::max(0, cy - r - margin), y1 = std::min(height_ - 1, cy + r + margin);
    if (x0 > x1 || y0 > y1) return;
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    if (scratch_.size() < size_t(cw) * ch * 4) scratch_.resize(size_t(cw) * ch * 4);
    for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {
        const uint8_t* p = image_->pixel(x + x0, y + y0);
        size_t s = (size_t(y) * cw + x) * 4;
        for (int k = 0; k < 4; k++) scratch_[s + k] = p[k];
    }
    float invR = 1 / float(diameter_ / 2);
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < y0 || y > y1) continue;
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx;
            if (x < x0 || x > x1) continue;
            float w = weight(std::sqrt(float(dx * dx + dy * dy)) * invR);
            if (w <= 0) continue;
            float sx = std::min(float(cw - 1), std::max(0.0f, float(x - x0) - moveX * w));
            float sy = std::min(float(ch - 1), std::max(0.0f, float(y - y0) - moveY * w));
            int ix = std::min(cw - 2, int(sx)), iy = std::min(ch - 2, int(sy));
            if (ix < 0 || iy < 0) continue;
            float fx = sx - ix, fy = sy - iy;
            uint8_t* p = image_->pixel(x, y);
            size_t s00 = (size_t(iy) * cw + ix) * 4, s10 = s00 + 4, s01 = s00 + size_t(cw) * 4, s11 = s01 + 4;
            for (int k = 0; k < 4; k++) {
                float top = scratch_[s00 + k] + (scratch_[s10 + k] - scratch_[s00 + k]) * fx;
                float bottom = scratch_[s01 + k] + (scratch_[s11 + k] - scratch_[s01 + k]) * fx;
                p[k] = uint8_t(std::max(0.0f, std::min(255.0f, std::round(top + (bottom - top) * fy))));
            }
        }
    }
}

} // namespace compositor
