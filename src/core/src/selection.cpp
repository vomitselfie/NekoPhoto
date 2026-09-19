#include "compositor/selection.h"
#include "compositor/morphology.h"
#include "compositor/render.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>

extern "C" {
#include "BrushPixels.h"
#include "WandPixels.h"
}

namespace compositor {

namespace {

// Exact-area antialiasing: every edge deposits its signed area and cover into an accumulation buffer
// (one float per pixel plus two per row), and a running sum along each row turns that into coverage
// (the font-rs / stb_truetype v2 formulation). Nonzero winding is approximated by min(1, |sum|),
// which is exact for outlines that don't cross themselves and keeps overlaps opaque.
void fillPolygonAntialiased(const std::vector<Point>& points, GrayImage& out) {
    const int w = out.width(), h = out.height();
    if (points.size() < 3 || w <= 0 || h <= 0) return;
    double minY = 1e300, maxY = -1e300;
    for (auto& p : points) { minY = std::min(minY, p.y); maxY = std::max(maxY, p.y); }
    const int y0 = std::max(0, int(std::floor(minY))), y1 = std::min(h, int(std::ceil(maxY)));
    if (y1 <= y0) return;
    const int stride = w + 2;
    std::vector<float> acc(size_t(stride) * size_t(y1 - y0), 0.0f);

    auto drawLine = [&](Point p0, Point p1) {
        if (std::fabs(p0.y - p1.y) < 1e-12) return;
        double dir = 1;
        if (p0.y > p1.y) { std::swap(p0, p1); dir = -1; }
        const double dxdy = (p1.x - p0.x) / (p1.y - p0.y);
        double x = p0.x;
        int yStart = std::max(y0, int(std::floor(p0.y)));
        if (p0.y < yStart) x += (yStart - p0.y) * dxdy;
        const int yEnd = std::min(y1, int(std::ceil(p1.y)));
        for (int y = yStart; y < yEnd; y++) {
            float* line = &acc[size_t(y - y0) * stride];
            const double dy = std::min(double(y + 1), p1.y) - std::max(double(y), p0.y);
            const double xnext = x + dxdy * dy;
            const double d = dy * dir;
            double xa = x, xb = xnext;
            if (xa > xb) std::swap(xa, xb);
            const double x0floor = std::floor(xa);
            const int x0i = int(x0floor);
            const double x1ceil = std::ceil(xb);
            const int x1i = int(x1ceil);
            if (x1i <= x0i + 1) {
                // The piece stays within one pixel column: split its cover by where it sits in the column.
                const double xmf = 0.5 * (x + xnext) - x0floor;
                line[x0i] += float(d - d * xmf);
                line[x0i + 1] += float(d * xmf);
            } else {
                const double s = 1.0 / (xb - xa);
                const double x0f = xa - x0floor;
                const double a0 = 0.5 * s * (1.0 - x0f) * (1.0 - x0f);
                const double x1f = xb - x1ceil + 1.0;
                const double am = 0.5 * s * x1f * x1f;
                line[x0i] += float(d * a0);
                if (x1i == x0i + 2) {
                    line[x0i + 1] += float(d * (1.0 - a0 - am));
                } else {
                    const double a1 = s * (1.5 - x0f);
                    line[x0i + 1] += float(d * (a1 - a0));
                    for (int xi = x0i + 2; xi < x1i - 1; xi++) line[xi] += float(d * s);
                    const double a2 = a1 + (x1i - x0i - 3) * s;
                    line[x1i - 1] += float(d * (1.0 - a2 - am));
                }
                line[x1i] += float(d * am);
            }
            x = xnext;
        }
    };
    // Edges are clipped to the buffer's columns [0, w]: a piece left of the image becomes a vertical edge at
    // 0 (its winding still counts from column 0 on), a piece right of it lands on column w, past every pixel.
    auto clipped = [&](Point a, Point b) {
        auto clampX = [&](Point p) { return Point{std::clamp(p.x, 0.0, double(w)), p.y}; };
        auto emit = [&](Point p, Point q) { drawLine(clampX(p), clampX(q)); };
        for (double bound : {0.0, double(w)}) {
            if ((a.x < bound) != (b.x < bound) && a.x != b.x) {
                double t = (bound - a.x) / (b.x - a.x);
                Point m{bound, a.y + (b.y - a.y) * t};
                emit(a, m);
                a = m;
            }
        }
        emit(a, b);
    };
    for (size_t i = 0; i < points.size(); i++) clipped(points[i], points[(i + 1) % points.size()]);
    for (int y = y0; y < y1; y++) {
        const float* line = &acc[size_t(y - y0) * stride];
        uint8_t* o = out.row(y);
        float sum = 0;
        for (int x = 0; x < w; x++) {
            sum += line[x];
            float v = std::min(1.0f, std::fabs(sum));
            if (v > 0.0005f) o[x] = std::max(o[x], uint8_t(v * 255 + 0.5f));
        }
    }
}

// Aliased fill: a pixel is in when its centre is inside (nonzero winding), by scanline.
struct Edge { double x0, y0, x1, y1; int dir; };

void fillPolygonAliased(const std::vector<Point>& points, GrayImage& out) {
    if (points.size() < 3) return;
    std::vector<Edge> edges;
    for (size_t i = 0; i < points.size(); i++) {
        Point a = points[i], b = points[(i + 1) % points.size()];
        if (a.y == b.y) continue;
        if (a.y < b.y) edges.push_back({a.x, a.y, b.x, b.y, 1});
        else edges.push_back({b.x, b.y, a.x, a.y, -1});
    }
    if (edges.empty()) return;
    int w = out.width(), h = out.height();
    double minY = edges[0].y0, maxY = edges[0].y1;
    for (auto& e : edges) { minY = std::min(minY, e.y0); maxY = std::max(maxY, e.y1); }
    int y0 = std::max(0, int(std::floor(minY))), y1 = std::min(h, int(std::ceil(maxY)));
    struct Crossing { double x; int dir; };
    std::vector<Crossing> crossings;
    for (int y = y0; y < y1; y++) {
        double sy = y + 0.5;
        crossings.clear();
        for (auto& e : edges) {
            if (sy < e.y0 || sy >= e.y1) continue;
            crossings.push_back({e.x0 + (sy - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), e.dir});
        }
        std::sort(crossings.begin(), crossings.end(), [](const Crossing& a, const Crossing& b) { return a.x < b.x; });
        int winding = 0;
        uint8_t* o = out.row(y);
        for (size_t i = 0; i + 1 < crossings.size(); i++) {
            winding += crossings[i].dir;
            if (winding == 0) continue;
            int ia = std::max(0, int(std::ceil(crossings[i].x - 0.5))), ib = std::min(w, int(std::ceil(crossings[i + 1].x - 0.5)));
            for (int x = ia; x < ib; x++) o[x] = 255;
        }
    }
}

void fillPolygon(const std::vector<Point>& points, GrayImage& out, bool antialiased) {
    if (antialiased) fillPolygonAntialiased(points, out); else fillPolygonAliased(points, out);
}

} // namespace

std::shared_ptr<GrayImage> rasterizePolygon(const std::vector<Point>& points, int width, int height, bool antialiased) {
    auto out = std::make_shared<GrayImage>(width, height, 0);
    fillPolygon(points, *out, antialiased);
    return out;
}

std::shared_ptr<GrayImage> rasterizeRect(const Rect& rect, int width, int height, bool antialiased) {
    return rasterizePolygon({{rect.minX(), rect.minY()}, {rect.maxX(), rect.minY()}, {rect.maxX(), rect.maxY()}, {rect.minX(), rect.maxY()}}, width, height, antialiased);
}

std::shared_ptr<GrayImage> rasterizeEllipse(const Rect& rect, int width, int height, bool antialiased) {
    // Enough chords that the sagitta stays under 0.05 px: n = pi / acos(1 - tolerance / r).
    const double r = std::max(1.0, std::max(rect.width, rect.height) / 2);
    const double tolerance = 0.05;
    int segments = int(std::ceil(M_PI / std::acos(std::max(-1.0, 1.0 - tolerance / r))));
    segments = std::clamp(segments, 32, 4096);
    std::vector<Point> points;
    points.reserve(size_t(segments));
    for (int i = 0; i < segments; i++) {
        double t = 2 * M_PI * i / segments;
        points.push_back({rect.midX() + std::cos(t) * rect.width / 2, rect.midY() + std::sin(t) * rect.height / 2});
    }
    return rasterizePolygon(points, width, height, antialiased);
}

namespace {
/// `op(a, b)` per pixel into a fresh raster, row by row so the compiler vectorises the byte operation.
template <class Op>
std::shared_ptr<GrayImage> combineRows(const GrayImage& a, const GrayImage& b, Op op) {
    auto out = std::make_shared<GrayImage>(a.width(), a.height());
    const int w = std::min(a.width(), b.width()), h = std::min(a.height(), b.height());
    for (int y = 0; y < h; y++) {
        const uint8_t *pa = a.row(y), *pb = b.row(y);
        uint8_t* po = out->row(y);
        for (int x = 0; x < w; x++) po[x] = op(pa[x], pb[x]);
        for (int x = w; x < a.width(); x++) po[x] = pa[x];
    }
    for (int y = h; y < a.height(); y++) std::memcpy(out->row(y), a.row(y), size_t(a.width()));
    return out;
}
} // namespace

std::optional<Selection> combineSelection(const std::optional<Selection>& current, const GrayImage& shape, SelectionMode mode, bool antialiased) {
    Selection result;
    result.antialiased = antialiased;
    switch (mode) {
    case SelectionMode::Replace:
        result.coverage = std::make_shared<GrayImage>(shape);
        return result;
    case SelectionMode::Add: {
        if (!current || !current->coverage) { result.coverage = std::make_shared<GrayImage>(shape); return result; }
        result.coverage = combineRows(*current->coverage, shape, [](uint8_t a, uint8_t b) { return std::max(a, b); });
        return result;
    }
    case SelectionMode::Subtract: {
        if (!current || !current->coverage) return current;
        result.coverage = combineRows(*current->coverage, shape, [](uint8_t a, uint8_t b) { return uint8_t(a > b ? a - b : 0); });
        return result;
    }
    case SelectionMode::Intersect: {
        if (!current || !current->coverage) return current;
        result.coverage = combineRows(*current->coverage, shape, [](uint8_t a, uint8_t b) { return std::min(a, b); });
        return result;
    }
    }
    return current;
}

Selection invertSelection(const Selection& selection, int width, int height) {
    Selection result = selection;
    auto out = std::make_shared<GrayImage>(width, height, 255);
    if (selection.coverage)
        for (int y = 0; y < height; y++) {
            const uint8_t* in = selection.coverage->row(y);
            uint8_t* po = out->row(y);
            for (int x = 0; x < width; x++) po[x] = uint8_t(255 - in[x]);
        }
    result.coverage = out;
    return result;
}

Selection resizeSelection(const Selection& selection, int amount) {
    Selection result = selection;
    if (!selection.coverage || amount == 0) return result;
    result.coverage = growSelection(*selection.coverage, amount);
    return result;
}

std::vector<std::vector<Point>> selectionOutline(const GrayImage& coverage, bool* tooDetailed) {
    std::vector<std::vector<Point>> loops;
    if (coverage.isEmpty()) return loops;
    // Trace pixels at least half selected.
    std::vector<uint8_t> mask(size_t(coverage.width()) * coverage.height());
    for (int y = 0; y < coverage.height(); y++) for (int x = 0; x < coverage.width(); x++) mask[size_t(y) * coverage.width() + x] = coverage.at(x, y) >= 128 ? 255 : 0;
    int32_t* points = nullptr; int32_t* counts = nullptr; size_t pointCount = 0, loopCount = 0;
    int result = wand_trace(mask.data(), size_t(coverage.width()), size_t(coverage.height()), &points, &pointCount, &counts, &loopCount);
    if (tooDetailed) *tooDetailed = result == -2;
    if (result != 0) return loops;
    size_t offset = 0;
    for (size_t i = 0; i < loopCount; i++) {
        std::vector<Point> loop;
        for (int32_t k = 0; k < counts[i] && offset + 1 < pointCount * 2; k++, offset += 2) loop.push_back({double(points[offset]), double(points[offset + 1])});
        loops.push_back(std::move(loop));
    }
    std::free(points);
    std::free(counts);
    return loops;
}

std::shared_ptr<GrayImage> coverageFromLayer(const Document& document, const Layer& layer) {
    auto out = std::make_shared<GrayImage>(document.width, document.height);
    if (!layer.asset || !layer.asset->image) return out;
    const Image& image = *layer.asset->image;
    const LayerTransform& t = layer.transform;
    // Pixels sitting on the document grid at whole coordinates: copy their alpha straight across.
    bool onGrid = t.rotation == 0 && !t.flipX && !t.flipY && t.size.width == image.width() && t.size.height == image.height()
        && t.origin.x == std::floor(t.origin.x) && t.origin.y == std::floor(t.origin.y);
    if (onGrid) {
        int ox = int(t.origin.x), oy = int(t.origin.y);
        int x0 = std::max(0, ox), y0 = std::max(0, oy), x1 = std::min(document.width, ox + image.width()), y1 = std::min(document.height, oy + image.height());
        for (int y = y0; y < y1; y++) {
            const uint8_t* src = image.pixel(x0 - ox, y - oy) + 3;
            uint8_t* dst = out->row(y) + x0;
            for (int x = x0; x < x1; x++, src += 4) dst[x - x0] = *src;
        }
        return out;
    }
    Image pixels(document.width, document.height);
    {
        DrawParams params;
        params.image = layer.asset->image;
        params.transform = layer.transform;
        params.layerTransformForMask = layer.transform;
        drawLayer(params, document.rect(), 1, nullptr, pixels);
    }
    layer_extract_alpha(pixels.data(), size_t(pixels.stride()), out->data(), size_t(out->stride()), size_t(document.width), size_t(document.height));
    return out;
}

} // namespace compositor
