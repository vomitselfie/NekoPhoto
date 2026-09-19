#include "compositor/transform.h"
#include <vector>

namespace compositor {

const char* samplingName(Sampling s) {
    switch (s) {
    case Sampling::Nearest: return "Nearest";
    case Sampling::Smooth: return "Smooth";
    case Sampling::High: return "High quality";
    }
    return "High quality";
}

bool parseSampling(const std::string& name, Sampling& out) {
    if (name == "Nearest") { out = Sampling::Nearest; return true; }
    if (name == "Smooth") { out = Sampling::Smooth; return true; }
    if (name == "High quality") { out = Sampling::High; return true; }
    return false;
}

const std::array<Point, 8> LayerTransform::handles = {Point{0, 0}, Point{0.5, 0}, Point{1, 0}, Point{1, 0.5},
                                                      Point{1, 1}, Point{0.5, 1}, Point{0, 1}, Point{0, 0.5}};

bool LayerTransform::isValid() const {
    return origin.isFinite() && size.isFinite() && std::isfinite(rotation)
        && size.width >= 1 && size.width <= 300000 && size.height >= 1 && size.height <= 300000
        && std::fabs(origin.x) <= 1000000 && std::fabs(origin.y) <= 1000000;
}

Point LayerTransform::point(Point unit) const {
    double x = (unit.x - 0.5) * size.width, y = (unit.y - 0.5) * size.height;
    double r = radians();
    Point c = center();
    return {c.x + x * std::cos(r) - y * std::sin(r), c.y + x * std::sin(r) + y * std::cos(r)};
}

bool LayerTransform::contains(Point p) const {
    Point c = center();
    double x = p.x - c.x, y = p.y - c.y, r = radians();
    return std::fabs(x * std::cos(r) + y * std::sin(r)) <= size.width / 2
        && std::fabs(-x * std::sin(r) + y * std::cos(r)) <= size.height / 2;
}

LayerTransform LayerTransform::scaledToPercent(double percent, Size pixelSize) const {
    LayerTransform result = *this;
    Point c = center();
    result.size = {pixelSize.width * percent / 100, pixelSize.height * percent / 100};
    result.origin = {c.x - result.size.width / 2, c.y - result.size.height / 2};
    return result;
}

LayerTransform LayerTransform::rounded() const {
    LayerTransform result = *this;
    result.origin = {std::round(origin.x), std::round(origin.y)};
    result.size = {std::max(1.0, std::round(size.width)), std::max(1.0, std::round(size.height))};
    result.rotation = std::round(rotation);
    return result;
}

Affine LayerTransform::pixelToDocument(int width, int height) const {
    Point c = center();
    return Affine::translation(c.x, c.y)
        .rotated(radians())
        .scaledBy(size.width / std::max(1, width) * (flipX ? -1 : 1), size.height / std::max(1, height) * (flipY ? -1 : 1))
        .translatedBy(-double(width) / 2, -double(height) / 2);
}

LayerTransform LayerTransform::placing(const Affine& map) const {
    double sign = flipX ? -1 : 1;
    double angle = std::atan2(map.b * sign, map.a * sign);
    double along = -map.c * std::sin(angle) + map.d * std::cos(angle);
    Point middle = map.apply({0.5, 0.5});
    LayerTransform result = *this;
    result.size = {std::hypot(map.a, map.b), std::fabs(along)};
    double degrees = angle * 180 / M_PI;
    result.rotation = degrees + std::round((rotation - degrees) / 360) * 360;
    result.flipY = along < 0;
    result.origin = {middle.x - result.size.width / 2, middle.y - result.size.height / 2};
    return result;
}

LayerTransform LayerTransform::following(const LayerTransform& from, const LayerTransform& to) const {
    if (from == to) return *this;
    if (from.size == to.size && from.rotation == to.rotation && from.flipX == to.flipX && from.flipY == to.flipY) {
        LayerTransform moved = *this;
        moved.origin.x += to.origin.x - from.origin.x;
        moved.origin.y += to.origin.y - from.origin.y;
        return moved;
    }
    return placing(unitToDocument().concatenating(from.unitToDocument().inverted()).concatenating(to.unitToDocument()));
}

bool LayerTransform::samePlacement(const LayerTransform& other) const {
    LayerTransform copy = *this;
    copy.sampling = other.sampling;
    return copy == other;
}

std::array<Point, 4> LayerTransform::corners() const {
    return {point({0, 0}), point({1, 0}), point({1, 1}), point({0, 1})};
}

Rect LayerTransform::bounds() const {
    auto c = corners();
    double x0 = c[0].x, y0 = c[0].y, x1 = c[0].x, y1 = c[0].y;
    for (auto& p : c) { x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y); }
    return {x0, y0, x1 - x0, y1 - y0};
}

LayerTransform TransformDrag::updated(Point point, bool lockRatio, bool shift, bool option) const {
    LayerTransform result = original;
    switch (mode) {
    case Mode::Move: {
        double dx = point.x - start.x, dy = point.y - start.y;
        if (shift) { if (std::fabs(dx) >= std::fabs(dy)) dy = 0; else dx = 0; }
        result.origin.x += dx;
        result.origin.y += dy;
        break;
    }
    case Mode::Rotate: {
        Point c = original.center();
        double delta = std::atan2(point.y - c.y, point.x - c.x) - std::atan2(start.y - c.y, start.x - c.x);
        result.rotation += delta * 180 / M_PI;
        if (shift) result.rotation = std::round(result.rotation / 15) * 15;
        break;
    }
    case Mode::Resize: {
        Point h = LayerTransform::handles[size_t(clamp(handle, 0, 7))];
        Point anchorUnit = option ? Point{0.5, 0.5} : Point{1 - h.x, 1 - h.y};
        Point anchor = original.point(anchorUnit);
        Point initialHandle = original.point(h);
        double dx = initialHandle.x + point.x - start.x - anchor.x;
        double dy = initialHandle.y + point.y - start.y - anchor.y;
        double span = option ? 2 : 1;
        double r = original.radians();
        double localX = (dx * std::cos(r) + dy * std::sin(r)) * span;
        double localY = (-dx * std::sin(r) + dy * std::cos(r)) * span;
        double sx = h.x * 2 - 1, sy = h.y * 2 - 1;
        double width = sx == 0 ? original.size.width : std::max(1.0, localX * sx);
        double height = sy == 0 ? original.size.height : std::max(1.0, localY * sy);
        if (lockRatio != shift) {
            double factor;
            if (sx == 0) factor = height / original.size.height;
            else if (sy == 0) factor = width / original.size.width;
            else {
                factor = std::max(1 / std::min(original.size.width, original.size.height),
                    (localX * sx * original.size.width + localY * sy * original.size.height)
                    / (original.size.width * original.size.width + original.size.height * original.size.height));
            }
            width = original.size.width * factor;
            height = original.size.height * factor;
        }
        result.size = {width, height};
        double offsetX = (0.5 - anchorUnit.x) * width, offsetY = (0.5 - anchorUnit.y) * height;
        Point c{anchor.x + offsetX * std::cos(r) - offsetY * std::sin(r), anchor.y + offsetX * std::sin(r) + offsetY * std::cos(r)};
        result.origin = {c.x - width / 2, c.y - height / 2};
        break;
    }
    }
    return result.isValid() ? result : original;
}

namespace {
struct Shift { double move = 0; bool found = false; double target = 0; };
Shift shiftGuides(const std::vector<double>& guides, const std::vector<double>& targets, double tolerance) {
    Shift best;
    for (double g : guides)
        for (double t : targets) {
            double move = t - g;
            if (std::fabs(move) > tolerance) continue;
            if (best.found && std::fabs(best.move) <= std::fabs(move)) continue;
            best = {move, true, t};
        }
    return best;
}
} // namespace

SnapResult snapOffset(const Rect& box, const std::vector<double>& xs, const std::vector<double>& ys, double tolerance) {
    Shift h = shiftGuides({box.minX(), box.midX(), box.maxX()}, xs, tolerance);
    Shift v = shiftGuides({box.minY(), box.midY(), box.maxY()}, ys, tolerance);
    return {h.move, v.move, h.found, v.found, h.target, v.target};
}

} // namespace compositor
