#include "compositor/shape.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>

namespace compositor {

std::shared_ptr<Image> shapeImage(ShapeKind kind, int width, int height, double red, double green, double blue, double cornerRadius) {
    auto out = std::make_shared<Image>(std::max(1, width), std::max(1, height));
    int w = out->width(), h = out->height();
    double radius = kind == ShapeKind::Ellipse ? 0 : std::min({std::max(0.0, cornerRadius), w / 2.0, h / 2.0});
    uint8_t r = uint8_t(clamp(red, 0.0, 1.0) * 255 + 0.5), g = uint8_t(clamp(green, 0.0, 1.0) * 255 + 0.5), b = uint8_t(clamp(blue, 0.0, 1.0) * 255 + 0.5);
    // Signed distance to the shape's edge in pixels; coverage from it (an analytic antialias).
    auto inside = [&](double x, double y) -> double {
        if (kind == ShapeKind::Ellipse) {
            double a = w / 2.0, bb = h / 2.0, dx = (x - a) / a, dy = (y - bb) / bb;
            double f = dx * dx + dy * dy;
            // Distance along the gradient of the implicit ellipse, approximately.
            double gx = 2 * dx / a, gy = 2 * dy / bb;
            double gn = std::hypot(gx, gy);
            return gn > 1e-9 ? (1 - f) / gn : 1e9;
        }
        double dx = std::max(radius - x, x - (w - radius)), dy = std::max(radius - y, y - (h - radius));
        if (radius <= 0) return std::min({x, w - x, y, h - y});
        if (dx > 0 && dy > 0) return radius - std::hypot(dx, dy);
        return std::min({x, w - x, y, h - y});
    };
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* p = out->row(y);
            for (int x = 0; x < w; x++, p += 4) {
                double d = inside(x + 0.5, y + 0.5);
                double cov = clamp(d + 0.5, 0.0, 1.0);
                if (cov <= 0) continue;
                uint8_t a = uint8_t(cov * 255 + 0.5);
                p[0] = uint8_t((r * a + 127) / 255); p[1] = uint8_t((g * a + 127) / 255); p[2] = uint8_t((b * a + 127) / 255); p[3] = a;
            }
        }
    });
    return out;
}

namespace {

inline double gradientPosition(GradientShape shape, Point from, Point to, Point p) {
    double dx = to.x - from.x, dy = to.y - from.y;
    double len2 = dx * dx + dy * dy;
    if (len2 < 1e-12) return 1;
    if (shape == GradientShape::Radial) return clamp(std::hypot(p.x - from.x, p.y - from.y) / std::sqrt(len2), 0.0, 1.0);
    return clamp(((p.x - from.x) * dx + (p.y - from.y) * dy) / len2, 0.0, 1.0);
}

/// `t` within a run whose half-way point sits at `midpoint` (Photoshop's gradient midpoints).
inline float midpointRemap(float t, float midpoint) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float m = std::clamp(midpoint, 0.001f, 0.999f);
    if (std::abs(m - 0.5f) < 1e-6f) return t;
    return t <= m ? 0.5f * t / m : 0.5f + 0.5f * (t - m) / (1 - m);
}

/// The run of `stops` (sorted by location) holding `t`: the index of its end stop (0: before the first stop, size():
/// past the last) and the remapped fraction along it in `u`.
template <typename Stop>
size_t runAt(const std::vector<Stop>& stops, float t, float& u) {
    u = 0;
    if (t <= stops.front().location) return 0;
    if (t >= stops.back().location) return stops.size();
    for (size_t i = 1; i < stops.size(); i++) {
        if (t > stops[i].location) continue;
        const float span = stops[i].location - stops[i - 1].location;
        u = span > 1e-6f ? midpointRemap((t - stops[i - 1].location) / span, stops[i - 1].midpoint) : 1.0f;
        return i;
    }
    return stops.size();
}

} // namespace

void GradientStops::sample(float t, float out[4]) const {
    if (colors.empty()) {
        for (int c = 0; c < 4; c++) out[c] = start[c] + (end[c] - start[c]) * t;
        return;
    }
    float u = 0;
    const size_t i = runAt(colors, t, u);
    if (i == 0) for (int c = 0; c < 3; c++) out[c] = colors.front().rgb[c];
    else if (i >= colors.size()) for (int c = 0; c < 3; c++) out[c] = colors.back().rgb[c];
    else for (int c = 0; c < 3; c++) out[c] = colors[i - 1].rgb[c] + (colors[i].rgb[c] - colors[i - 1].rgb[c]) * u;
    if (alphas.empty()) { out[3] = 1; return; }
    const size_t j = runAt(alphas, t, u);
    out[3] = j == 0 ? alphas.front().opacity : j >= alphas.size() ? alphas.back().opacity
           : alphas[j - 1].opacity + (alphas[j].opacity - alphas[j - 1].opacity) * u;
}

void GradientStops::reverse() {
    for (int c = 0; c < 4; c++) std::swap(start[c], end[c]);
    // A stop's midpoint belongs to the run after it; mirrored, that run follows the stop before it.
    auto mirror = [](auto& stops) {
        std::reverse(stops.begin(), stops.end());
        for (auto& s : stops) s.location = 1 - s.location;
        for (size_t i = 0; i + 1 < stops.size(); i++) stops[i].midpoint = 1 - stops[i + 1].midpoint;
        if (!stops.empty()) stops.back().midpoint = 0.5f;
    };
    mirror(colors);
    mirror(alphas);
}

void fillGradient(const Image& base, Image& out, const Affine& pixelToDocument, GradientShape shape, Point from, Point to, const GradientStops& stops, double opacity, const GrayImage* selection) {
    int w = out.width(), h = out.height();
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            Point d = pixelToDocument.apply({0.5, y + 0.5});
            Point dd = pixelToDocument.applyVector({1, 0});
            const uint8_t* b = base.row(y);
            uint8_t* o = out.row(y);
            for (int x = 0; x < w; x++, d = d + dd, b += 4, o += 4) {
                double t = gradientPosition(shape, from, to, d);
                float col[4];
                stops.sample(float(t), col);
                double cov = opacity * (selection ? selection->at(x, y) / 255.0 : 1.0);
                float a = col[3] * float(cov);
                if (a <= 0) { for (int c = 0; c < 4; c++) o[c] = b[c]; continue; }
                for (int c = 0; c < 3; c++) o[c] = uint8_t(clamp(col[c] * a * 255 + b[c] * (1 - a) + 0.5f, 0.0f, 255.0f));
                o[3] = uint8_t(clamp(a * 255 + b[3] * (1 - a) + 0.5f, 0.0f, 255.0f));
            }
        }
    });
}

void fillGradient(const GrayImage& base, GrayImage& out, const Affine& pixelToDocument, GradientShape shape, Point from, Point to, const GradientStops& stops, double opacity, const GrayImage* selection) {
    int w = out.width(), h = out.height();
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            Point d = pixelToDocument.apply({0.5, y + 0.5});
            Point dd = pixelToDocument.applyVector({1, 0});
            for (int x = 0; x < w; x++, d = d + dd) {
                double t = gradientPosition(shape, from, to, d);
                float col[4];
                stops.sample(float(t), col);
                const float value = col[0], alpha = col[3];
                double cov = opacity * alpha * (selection ? selection->at(x, y) / 255.0 : 1.0);
                out.at(x, y) = uint8_t(clamp(value * 255 * cov + base.at(x, y) * (1 - cov) + 0.5, 0.0, 255.0));
            }
        }
    });
}

} // namespace compositor
