// A port of Patchy's core/warp_mesh (MIT; see warpmesh.h): the patch, the preset bakes and the lattice resampler.
#include "compositor/warpmesh.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace compositor {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::array<double, 4> bernstein(int order, double t) {
    const double s = 1 - t;
    if (order == 2) return {s, t, 0, 0};
    if (order == 3) return {s * s, 2 * s * t, t * t, 0};
    return {s * s * s, 3 * s * s * t, 3 * s * t * t, t * t * t};
}

// One row of the cubic circle-arc approximation: chord (ax, y)..(bx, y), central angle 2 theta, bulging up (-y)
// when `up`.
void arcRow(WarpMesh& m, double ax, double bx, double y, double theta, bool up) {
    const double radius = (bx - ax) / (2 * std::sin(theta));
    const double handle = 4.0 / 3.0 * std::tan(theta / 2) * radius;
    const double dy = (up ? -1 : 1) * handle * std::sin(theta);
    m.xs.insert(m.xs.end(), {ax, ax + handle * std::cos(theta), bx - handle * std::cos(theta), bx});
    m.ys.insert(m.ys.end(), {y, y + dy, y + dy, y});
}

void flatRow(WarpMesh& m, double width, double y) {
    m.xs.insert(m.xs.end(), {0.0, width / 3, 2 * width / 3, width});
    m.ys.insert(m.ys.end(), {y, y, y, y});
}

WarpMesh mirrorVertically(const WarpMesh& m, double height) {
    WarpMesh r = m;
    for (int row = 0; row < m.vOrder; row++)
        for (int c = 0; c < m.uOrder; c++) {
            const size_t to = size_t(row * m.uOrder + c), from = size_t((m.vOrder - 1 - row) * m.uOrder + c);
            r.xs[to] = m.xs[from];
            r.ys[to] = height - m.ys[from];
        }
    return r;
}

WarpMesh swapAxes(const WarpMesh& m) {
    WarpMesh r;
    r.uOrder = m.vOrder;
    r.vOrder = m.uOrder;
    r.xs.resize(m.xs.size());
    r.ys.resize(m.ys.size());
    for (int row = 0; row < m.vOrder; row++)
        for (int c = 0; c < m.uOrder; c++) {
            const size_t from = size_t(row * m.uOrder + c), to = size_t(c * m.vOrder + row);
            r.xs[to] = m.ys[from];
            r.ys[to] = m.xs[from];
        }
    return r;
}

WarpMesh horizontalStyle(std::string_view style, double value, double width, double height) {
    const double bend = std::clamp(value, -100.0, 100.0);
    const double theta = std::abs(bend) * kPi / 200;
    const double d = 2 * height * bend / 100;   // flag, wave, rise: height based
    const bool flat = std::abs(bend) < 1e-9;
    WarpMesh m;
    m.uOrder = 4;
    m.vOrder = style == "warpWave" ? 3 : 2;
    if (style == "warpArcLower" || style == "warpArcUpper") {
        if (flat) return identityWarpMesh(0, 0, width, height, 4, 2);
        flatRow(m, width, 0);
        arcRow(m, 0, width, height, theta, bend < 0);
        return style == "warpArcUpper" ? mirrorVertically(m, height) : m;
    }
    if (style == "warpFish" || style == "warpFlag") {
        flatRow(m, width, 0);
        flatRow(m, width, height);
        const double s = style == "warpFish" ? -1 : 1;
        m.ys[1] -= d; m.ys[2] += d; m.ys[5] -= s * d; m.ys[6] += s * d;
        return m;
    }
    if (style == "warpFisheye") {
        m = identityWarpMesh(0, 0, width, height, 4, 4);
        const double t = bend / 50;
        for (int row = 1; row <= 2; row++)
            for (int c = 1; c <= 2; c++) {
                const size_t i = size_t(row * 4 + c);
                m.xs[i] += t * ((c == 1 ? 0 : width) - m.xs[i]);
                m.ys[i] += t * ((row == 1 ? 0 : height) - m.ys[i]);
            }
        return m;
    }
    if (style == "warpInflate" || style == "warpSqueeze") {
        m = identityWarpMesh(0, 0, width, height, 3, 3);
        const double dx = width * bend / 200, dy = height * bend / 200;
        m.ys[1] -= dy; m.ys[7] += dy;
        const double s = style == "warpInflate" ? 1 : -1;
        m.xs[3] -= s * dx; m.xs[5] += s * dx;
        return m;
    }
    if (style == "warpTwist") {
        m = identityWarpMesh(0, 0, width, height, 4, 4);
        const double mx = width * std::abs(bend) / 100, my = height * std::abs(bend) / 100;
        if (bend > 0) { m.xs[5] += mx; m.ys[6] += my; m.xs[10] -= mx; m.ys[9] -= my; }
        else if (bend < 0) { m.ys[5] += my; m.xs[6] -= mx; m.ys[10] -= my; m.xs[9] += mx; }
        return m;
    }
    if (style == "warpShellLower" || style == "warpShellUpper") {
        if (flat) return identityWarpMesh(0, 0, width, height, 4, 4);
        const double st = std::sin(theta), ct = std::cos(theta);
        m.vOrder = 4;
        flatRow(m, width, 0);
        flatRow(m, width, height / 3);
        if (bend > 0) {
            const double r = 2 * height / 3;
            m.xs.insert(m.xs.end(), {-r * st, width / 3, 2 * width / 3, width + r * st});
            m.ys.insert(m.ys.end(), {r * ct, 2 * height / 3, 2 * height / 3, r * ct});
            arcRow(m, -height * st, width + height * st, height * ct, theta, false);
        } else {
            const double r = height / 3;
            m.xs.insert(m.xs.end(), {-r * st, width / 3, 2 * width / 3, width + r * st});
            m.ys.insert(m.ys.end(), {height - r * ct, 2 * height / 3, 2 * height / 3, height - r * ct});
            arcRow(m, 0, width, height, theta, true);
        }
        return style == "warpShellUpper" ? mirrorVertically(m, height) : m;
    }
    if (style == "warpArc") {
        if (flat) return identityWarpMesh(0, 0, width, height, 4, 2);
        const double st = std::sin(theta);
        arcRow(m, -height * st, width + height * st, height * (1 - std::cos(theta)), theta, true);
        arcRow(m, 0, width, height, theta, true);
        return bend < 0 ? mirrorVertically(m, height) : m;
    }
    if (style == "warpArch" || style == "warpBulge") {
        if (flat) return identityWarpMesh(0, 0, width, height, 4, 2);
        const bool up = bend > 0;
        arcRow(m, 0, width, 0, theta, up);
        arcRow(m, 0, width, height, theta, style == "warpBulge" ? !up : up);
        return m;
    }
    if (style == "warpWave") {
        flatRow(m, width, 0);
        flatRow(m, width, height / 2);
        flatRow(m, width, height);
        m.ys[5] += d; m.ys[6] -= d;
        return m;
    }
    // warpRise: a rigid ramp of the columns.
    flatRow(m, width, 0);
    flatRow(m, width, height);
    m.ys[0] += d; m.ys[1] += d; m.ys[4] += d; m.ys[5] += d;
    return m;
}

using H = std::array<double, 9>;

std::optional<H> rectToQuad(double left, double top, double right, double bottom, const std::array<double, 8>& q) {
    const double w = right - left, h = bottom - top;
    if (std::abs(w) < 1e-12 || std::abs(h) < 1e-12) return std::nullopt;
    const double dx1 = q[2] - q[4], dx2 = q[6] - q[4], dx3 = q[0] - q[2] + q[4] - q[6];
    const double dy1 = q[3] - q[5], dy2 = q[7] - q[5], dy3 = q[1] - q[3] + q[5] - q[7];
    double g = 0, k = 0;
    if (std::abs(dx3) > 1e-12 || std::abs(dy3) > 1e-12) {
        const double den = dx1 * dy2 - dx2 * dy1;
        if (std::abs(den) < 1e-12) return std::nullopt;
        g = (dx3 * dy2 - dx2 * dy3) / den;
        k = (dx1 * dy3 - dx3 * dy1) / den;
    }
    const H unit{q[2] - q[0] + g * q[2], q[6] - q[0] + k * q[6], q[0], q[3] - q[1] + g * q[3], q[7] - q[1] + k * q[7], q[1], g, k, 1};
    H m{};
    for (size_t r = 0; r < 3; r++) {
        m[r * 3] = unit[r * 3] / w;
        m[r * 3 + 1] = unit[r * 3 + 1] / h;
        m[r * 3 + 2] = unit[r * 3 + 2] - unit[r * 3] / w * left - unit[r * 3 + 1] / h * top;
    }
    return m;
}

Point apply(const H& m, Point p) {
    double w = m[6] * p.x + m[7] * p.y + m[8];
    if (std::abs(w) < 1e-12) w = 1e-12;
    return {(m[0] * p.x + m[1] * p.y + m[2]) / w, (m[3] * p.x + m[4] * p.y + m[5]) / w};
}

// (s, t) in [0, 1]^2 with the bilinear cell p00, p10, p11, p01 at them equal to (x, y); none outside the cell.
std::optional<std::array<double, 2>> invertCell(double x, double y, Point p00, Point p10, Point p11, Point p01) {
    const double ax = p10.x - p00.x, ay = p10.y - p00.y, bx = p01.x - p00.x, by = p01.y - p00.y;
    const double cx = p00.x - p10.x - p01.x + p11.x, cy = p00.y - p10.y - p01.y + p11.y;
    const double dx = x - p00.x, dy = y - p00.y;
    const double k2 = ax * cy - ay * cx, k1 = (ax * by - ay * bx) - (dx * cy - dy * cx), k0 = -(dx * by - dy * bx);
    constexpr double e = 1e-9;
    double s;
    if (std::abs(k2) < e) {
        if (std::abs(k1) < e) return std::nullopt;
        s = -k0 / k1;
    } else {
        const double disc = k1 * k1 - 4 * k2 * k0;
        if (disc < 0) return std::nullopt;
        const double root = std::sqrt(disc), s0 = (-k1 + root) / (2 * k2), s1 = (-k1 - root) / (2 * k2);
        const bool v0 = s0 >= -e && s0 <= 1 + e, v1 = s1 >= -e && s1 <= 1 + e;
        if (v0 && v1) s = std::clamp(s0, 0.0, 1.0);
        else if (v0) s = s0;
        else if (v1) s = s1;
        else return std::nullopt;
    }
    const double tx = bx + cx * s, ty = by + cy * s;
    double t;
    if (std::abs(tx) >= std::abs(ty)) { if (std::abs(tx) < e) return std::nullopt; t = (dx - ax * s) / tx; }
    else t = (dy - ay * s) / ty;
    if (s < -e || s > 1 + e || t < -e || t > 1 + e) return std::nullopt;
    return std::array<double, 2>{std::clamp(s, 0.0, 1.0), std::clamp(t, 0.0, 1.0)};
}

// Half the size, each pixel the mean of four (premultiplied, so a plain mean is right).
Image halve(const Image& src) {
    Image out(std::max(1, src.width() / 2), std::max(1, src.height() / 2));
    for (int y = 0; y < out.height(); y++)
        for (int x = 0; x < out.width(); x++) {
            const int x0 = std::min(2 * x, src.width() - 1), x1 = std::min(2 * x + 1, src.width() - 1);
            const int y0 = std::min(2 * y, src.height() - 1), y1 = std::min(2 * y + 1, src.height() - 1);
            for (int c = 0; c < 4; c++)
                out.pixel(x, y)[c] = uint8_t((src.pixel(x0, y0)[c] + src.pixel(x1, y0)[c] + src.pixel(x0, y1)[c] + src.pixel(x1, y1)[c] + 2) / 4);
        }
    return out;
}

// Bilinear at (x, y) in pixel units (centres at +0.5), the edges extended (the surface's own edge bounds it).
void sample(const Image& img, double x, double y, uint8_t* out) {
    x -= 0.5; y -= 0.5;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    double acc[4] = {0, 0, 0, 0};
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            const int px = std::clamp(x0 + i, 0, img.width() - 1), py = std::clamp(y0 + j, 0, img.height() - 1);
            const double w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            const uint8_t* p = img.pixel(px, py);
            for (int c = 0; c < 4; c++) acc[c] += w * p[c];
        }
    for (int c = 0; c < 4; c++) out[c] = uint8_t(std::clamp(std::lround(acc[c]), 0L, 255L));
}

} // namespace

WarpMesh identityWarpMesh(double left, double top, double right, double bottom, int uOrder, int vOrder) {
    WarpMesh m;
    m.uOrder = std::clamp(uOrder, 2, 4);
    m.vOrder = std::clamp(vOrder, 2, 4);
    for (int row = 0; row < m.vOrder; row++)
        for (int c = 0; c < m.uOrder; c++) {
            m.xs.push_back(left + (right - left) * c / (m.uOrder - 1));
            m.ys.push_back(top + (bottom - top) * row / (m.vOrder - 1));
        }
    return m;
}

Point evaluateWarpMesh(const WarpMesh& m, double u, double v) {
    const auto wu = bernstein(m.uOrder, u), wv = bernstein(m.vOrder, v);
    Point p(0, 0);
    for (int row = 0; row < m.vOrder; row++)
        for (int c = 0; c < m.uOrder; c++) {
            const double w = wu[size_t(c)] * wv[size_t(row)];
            p.x += w * m.xs[size_t(row * m.uOrder + c)];
            p.y += w * m.ys[size_t(row * m.uOrder + c)];
        }
    return p;
}

bool warpStyleBakes(std::string_view s) {
    for (const char* k : {"warpArc", "warpArch", "warpBulge", "warpFlag", "warpWave", "warpRise", "warpArcLower", "warpArcUpper",
                          "warpShellLower", "warpShellUpper", "warpFish", "warpFisheye", "warpInflate", "warpSqueeze", "warpTwist"})
        if (s == k) return true;
    return false;
}

std::optional<WarpMesh> styleWarpMesh(std::string_view style, double bend, bool vertical, double width, double height) {
    if (!warpStyleBakes(style) || width <= 0 || height <= 0) return std::nullopt;
    // Twist bakes the same either way round.
    if (!vertical || style == "warpTwist") return horizontalStyle(style, bend, width, height);
    return swapAxes(horizontalStyle(style, bend, height, width));
}

void distortWarpMesh(WarpMesh& m, double horizontal, double vertical) {
    vertical = std::clamp(vertical, -100.0, 100.0);
    horizontal = std::clamp(horizontal, -100.0, 100.0);
    auto scaleLine = [&](size_t first, size_t step, int count, double scale) {
        const size_t last = first + step * size_t(count - 1);
        const double mx = (m.xs[first] + m.xs[last]) / 2, my = (m.ys[first] + m.ys[last]) / 2;
        for (int i = 0; i < count; i++) {
            const size_t k = first + step * size_t(i);
            m.xs[k] = mx + (m.xs[k] - mx) * scale;
            m.ys[k] = my + (m.ys[k] - my) * scale;
        }
    };
    if (vertical != 0)
        for (int row = 0; row < m.vOrder; row++)
            scaleLine(size_t(row * m.uOrder), 1, m.uOrder, 1 + (2.0 * row / (m.vOrder - 1) - 1) * vertical / 100);
    if (horizontal != 0)
        for (int c = 0; c < m.uOrder; c++)
            scaleLine(size_t(c), size_t(m.uOrder), m.vOrder, 1 + (2.0 * c / (m.uOrder - 1) - 1) * horizontal / 100);
}

namespace {

// Draws `image` through a forward lattice: `at(u, v)` gives the document point for the image's (u, v) in [0, 1]^2.
std::optional<WarpedRaster> resampleThrough(const Image& image, const std::function<Point(double, double)>& at, const Rect* clip = nullptr) {
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (int j = 0; j <= 16; j++)
        for (int i = 0; i <= 16; i++) {
            const Point p = at(i / 16.0, j / 16.0);
            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
        }
    if (!(maxX - minX < maxImageSide) || !(maxY - minY < maxImageSide)) return std::nullopt;
    // Cells about two pixels across.
    const int cellsX = std::clamp(int(std::ceil((maxX - minX) / 2)), 8, 2048), cellsY = std::clamp(int(std::ceil((maxY - minY) / 2)), 8, 2048);
    std::vector<Point> lattice(size_t((cellsX + 1) * (cellsY + 1)));
    for (int j = 0; j <= cellsY; j++)
        for (int i = 0; i <= cellsX; i++) {
            const Point p = at(double(i) / cellsX, double(j) / cellsY);
            lattice[size_t(j * (cellsX + 1) + i)] = p;
            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
        }
    if (clip) {
        minX = std::max(minX, clip->x); minY = std::max(minY, clip->y);
        maxX = std::min(maxX, clip->x + clip->width); maxY = std::min(maxY, clip->y + clip->height);
        if (!(maxX > minX) || !(maxY > minY)) return std::nullopt;
    }
    const int left = int(std::floor(minX)), top = int(std::floor(minY));
    const int w = std::max(1, int(std::ceil(maxX)) - left), hgt = std::max(1, int(std::ceil(maxY)) - top);
    // Never more than a document may hold (a degenerate placement in a hostile file claimed far more).
    if (w > maxImageSide || hgt > maxImageSide || int64_t(w) * hgt > 100000000) return std::nullopt;

    // Minified: sample a reduction, not the full contents (bilinear alone would alias).
    const double spread = std::max(image.width() / std::max(1.0, maxX - minX), image.height() / std::max(1.0, maxY - minY));
    const Image* src = &image;
    Image reduced;
    for (double s = spread; s >= 2 && src->width() > 1 && src->height() > 1; s /= 2) { reduced = halve(*src); src = &reduced; }

    auto out = std::make_shared<Image>(w, hgt);
    std::vector<uint8_t> written(size_t(w) * size_t(hgt), 0);   // first writer wins where the surface folds
    for (int j = 0; j < cellsY; j++)
        for (int i = 0; i < cellsX; i++) {
            const Point p00 = lattice[size_t(j * (cellsX + 1) + i)], p10 = lattice[size_t(j * (cellsX + 1) + i + 1)];
            const Point p01 = lattice[size_t((j + 1) * (cellsX + 1) + i)], p11 = lattice[size_t((j + 1) * (cellsX + 1) + i + 1)];
            const int bx0 = std::max(0, int(std::floor(std::min({p00.x, p10.x, p01.x, p11.x}) - 0.5)) - left);
            const int bx1 = std::min(w - 1, int(std::ceil(std::max({p00.x, p10.x, p01.x, p11.x}) - 0.5)) - left);
            const int by0 = std::max(0, int(std::floor(std::min({p00.y, p10.y, p01.y, p11.y}) - 0.5)) - top);
            const int by1 = std::min(hgt - 1, int(std::ceil(std::max({p00.y, p10.y, p01.y, p11.y}) - 0.5)) - top);
            for (int y = by0; y <= by1; y++)
                for (int x = bx0; x <= bx1; x++) {
                    uint8_t& done = written[size_t(y) * size_t(w) + size_t(x)];
                    if (done) continue;
                    auto st = invertCell(left + x + 0.5, top + y + 0.5, p00, p10, p11, p01);
                    if (!st) continue;
                    done = 1;
                    const double u = (i + (*st)[0]) / cellsX, v = (j + (*st)[1]) / cellsY;
                    sample(*src, u * src->width(), v * src->height(), out->pixel(x, y));
                }
        }
    return WarpedRaster{out, LayerTransform(Point(left, top), Size(w, hgt))};
}

} // namespace

std::optional<WarpedRaster> renderWarpedImage(const Image& image, const WarpMesh& mesh, const std::array<double, 8>& quad, const Rect* clip) {
    if (image.isEmpty() || mesh.xs.size() != size_t(mesh.uOrder * mesh.vOrder) || mesh.ys.size() != mesh.xs.size()) return std::nullopt;
    for (double v : quad) if (!std::isfinite(v)) return std::nullopt;
    // The control-point hull, not the warp bounds, is what Photoshop's placement quad describes.
    const auto [x0, x1] = std::minmax_element(mesh.xs.begin(), mesh.xs.end());
    const auto [y0, y1] = std::minmax_element(mesh.ys.begin(), mesh.ys.end());
    const auto h = rectToQuad(*x0, *y0, *x1, *y1, quad);
    if (!h) return std::nullopt;
    return resampleThrough(image, [&](double u, double v) { return apply(*h, evaluateWarpMesh(mesh, u, v)); }, clip);
}

std::optional<WarpedRaster> renderWarpedOverBox(const Image& image, const WarpMesh& mesh, const Rect& box) {
    if (image.isEmpty() || !(box.width > 0) || !(box.height > 0) || mesh.xs.size() != size_t(mesh.uOrder * mesh.vOrder) || mesh.ys.size() != mesh.xs.size())
        return std::nullopt;
    // The image's (u, v) as the box's parameters: past the box they run outside [0, 1], where the Bernstein patch
    // extrapolates smoothly.
    const double u0 = -box.x / box.width, u1 = (image.width() - box.x) / box.width;
    const double v0 = -box.y / box.height, v1 = (image.height() - box.y) / box.height;
    return resampleThrough(image, [&](double u, double v) {
        const Point p = evaluateWarpMesh(mesh, u0 + (u1 - u0) * u, v0 + (v1 - v0) * v);
        return Point(box.x + p.x, box.y + p.y);
    });
}

} // namespace compositor
