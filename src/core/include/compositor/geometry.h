// Points, sizes, rectangles and affine transforms in document pixels, with the
// same conventions as Core Graphics on the Mac (y down for documents; an affine
// maps x' = a·x + c·y + tx, y' = b·x + d·y + ty).
#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

namespace compositor {

struct Point {
    double x = 0, y = 0;
    constexpr Point() = default;
    constexpr Point(double x_, double y_) : x(x_), y(y_) {}
    bool operator==(const Point&) const = default;
    Point operator+(Point o) const { return {x + o.x, y + o.y}; }
    Point operator-(Point o) const { return {x - o.x, y - o.y}; }
    Point operator*(double s) const { return {x * s, y * s}; }
    bool isFinite() const { return std::isfinite(x) && std::isfinite(y); }
};

struct Size {
    double width = 0, height = 0;
    constexpr Size() = default;
    constexpr Size(double w, double h) : width(w), height(h) {}
    bool operator==(const Size&) const = default;
    bool isFinite() const { return std::isfinite(width) && std::isfinite(height); }
};

inline double distance(Point a, Point b) { return std::hypot(b.x - a.x, b.y - a.y); }

struct Rect {
    double x = 0, y = 0, width = 0, height = 0;
    constexpr Rect() = default;
    constexpr Rect(double x_, double y_, double w, double h) : x(x_), y(y_), width(w), height(h) {}
    static Rect fromPoints(Point a, Point b) {
        return {std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(b.x - a.x), std::fabs(b.y - a.y)};
    }
    bool operator==(const Rect&) const = default;
    double minX() const { return x; }
    double minY() const { return y; }
    double maxX() const { return x + width; }
    double maxY() const { return y + height; }
    double midX() const { return x + width / 2; }
    double midY() const { return y + height / 2; }
    Point origin() const { return {x, y}; }
    Size size() const { return {width, height}; }
    Point center() const { return {midX(), midY()}; }
    bool isEmpty() const { return !(width > 0 && height > 0); }
    bool contains(Point p) const { return p.x >= x && p.x < maxX() && p.y >= y && p.y < maxY(); }
    bool contains(const Rect& r) const { return r.minX() >= minX() && r.maxX() <= maxX() && r.minY() >= minY() && r.maxY() <= maxY(); }
    bool intersects(const Rect& r) const {
        return !isEmpty() && !r.isEmpty() && r.minX() < maxX() && r.maxX() > minX() && r.minY() < maxY() && r.maxY() > minY();
    }
    Rect intersection(const Rect& r) const {
        double x0 = std::max(minX(), r.minX()), y0 = std::max(minY(), r.minY());
        double x1 = std::min(maxX(), r.maxX()), y1 = std::min(maxY(), r.maxY());
        if (x1 <= x0 || y1 <= y0) return {};
        return {x0, y0, x1 - x0, y1 - y0};
    }
    Rect unionWith(const Rect& r) const {
        if (isEmpty()) return r;
        if (r.isEmpty()) return *this;
        double x0 = std::min(minX(), r.minX()), y0 = std::min(minY(), r.minY());
        double x1 = std::max(maxX(), r.maxX()), y1 = std::max(maxY(), r.maxY());
        return {x0, y0, x1 - x0, y1 - y0};
    }
    Rect insetBy(double dx, double dy) const { return {x + dx, y + dy, width - 2 * dx, height - 2 * dy}; }
    Rect offsetBy(double dx, double dy) const { return {x + dx, y + dy, width, height}; }
    /// The smallest whole-pixel rectangle containing this one (CGRect.integral).
    Rect integral() const {
        double x0 = std::floor(x), y0 = std::floor(y), x1 = std::ceil(maxX()), y1 = std::ceil(maxY());
        return {x0, y0, x1 - x0, y1 - y0};
    }
};

/// CGAffineTransform: [a b c d tx ty].
struct Affine {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    constexpr Affine() = default;
    constexpr Affine(double a_, double b_, double c_, double d_, double tx_, double ty_) : a(a_), b(b_), c(c_), d(d_), tx(tx_), ty(ty_) {}
    static Affine identity() { return {}; }
    static Affine translation(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static Affine scaling(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }
    static Affine rotation(double radians) { double s = std::sin(radians), c_ = std::cos(radians); return {c_, s, -s, c_, 0, 0}; }
    bool operator==(const Affine&) const = default;

    Point apply(Point p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    /// A vector (no translation).
    Point applyVector(Point v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }

    /// This transform followed by `next` (CGAffineTransform.concatenating).
    Affine concatenating(const Affine& n) const {
        return {a * n.a + b * n.c, a * n.b + b * n.d,
                c * n.a + d * n.c, c * n.b + d * n.d,
                tx * n.a + ty * n.c + n.tx, tx * n.b + ty * n.d + n.ty};
    }
    /// CGAffineTransform.translatedBy / rotated / scaledBy: the operation is applied before this transform.
    Affine translatedBy(double x, double y) const { return translation(x, y).concatenating(*this); }
    Affine rotated(double radians) const { return rotation(radians).concatenating(*this); }
    Affine scaledBy(double sx, double sy) const { return scaling(sx, sy).concatenating(*this); }

    double determinant() const { return a * d - b * c; }
    bool isInvertible() const { return std::fabs(determinant()) > 1e-12; }
    Affine inverted() const {
        double det = determinant();
        if (std::fabs(det) < 1e-300) return *this;
        double ia = d / det, ib = -b / det, ic = -c / det, id = a / det;
        return {ia, ib, ic, id, -(tx * ia + ty * ic), -(tx * ib + ty * id)};
    }
    /// The axis-aligned bounds of `r` after mapping its corners.
    Rect mapBounds(const Rect& r) const {
        Point p[4] = {apply({r.minX(), r.minY()}), apply({r.maxX(), r.minY()}), apply({r.maxX(), r.maxY()}), apply({r.minX(), r.maxY()})};
        double x0 = p[0].x, y0 = p[0].y, x1 = p[0].x, y1 = p[0].y;
        for (auto& q : p) { x0 = std::min(x0, q.x); y0 = std::min(y0, q.y); x1 = std::max(x1, q.x); y1 = std::max(y1, q.y); }
        return {x0, y0, x1 - x0, y1 - y0};
    }
};

template <typename T> T clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace compositor
