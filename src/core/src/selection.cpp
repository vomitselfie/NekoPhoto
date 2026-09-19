#include "compositor/selection.h"
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

// Scanline polygon fill with nonzero winding. Antialiasing: 4 sub-scanlines per pixel and exact
// horizontal span coverage; without antialiasing, pixel centers are tested.
struct Edge { double x0, y0, x1, y1; int dir; };

void fillPolygon(const std::vector<Point>& points, GrayImage& out, bool antialiased) {
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
    const int sub = antialiased ? 4 : 1;
    std::vector<float> row(static_cast<size_t>(w), 0.0f);
    struct Crossing { double x; int dir; };
    std::vector<Crossing> crossings;
    for (int y = y0; y < y1; y++) {
        std::fill(row.begin(), row.end(), 0.0f);
        for (int s = 0; s < sub; s++) {
            double sy = y + (s + 0.5) / sub;
            crossings.clear();
            for (auto& e : edges) {
                if (sy < e.y0 || sy >= e.y1) continue;
                double x = e.x0 + (sy - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0);
                crossings.push_back({x, e.dir});
            }
            std::sort(crossings.begin(), crossings.end(), [](const Crossing& a, const Crossing& b) { return a.x < b.x; });
            int winding = 0;
            for (size_t i = 0; i + 1 < crossings.size(); i++) {
                winding += crossings[i].dir;
                if (winding == 0) continue;
                double xa = crossings[i].x, xb = crossings[i + 1].x;
                if (xb <= xa) continue;
                if (!antialiased) {
                    int ia = std::max(0, int(std::ceil(xa - 0.5))), ib = std::min(w, int(std::ceil(xb - 0.5)));
                    for (int x = ia; x < ib; x++) row[size_t(x)] = 1;
                    continue;
                }
                double ca = clamp(xa, 0.0, double(w)), cb = clamp(xb, 0.0, double(w));
                if (cb <= ca) continue;
                int ia = int(std::floor(ca)), ib = int(std::ceil(cb));
                for (int x = ia; x < ib && x < w; x++) {
                    double cover = std::min(cb, double(x + 1)) - std::max(ca, double(x));
                    if (cover > 0) row[size_t(x)] += float(cover / sub);
                }
            }
        }
        uint8_t* o = out.row(y);
        for (int x = 0; x < w; x++) {
            float v = std::min(1.0f, row[size_t(x)]);
            if (v > 0) o[x] = std::max(o[x], uint8_t(v * 255 + 0.5f));
        }
    }
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
    int segments = std::max(32, int(std::ceil((rect.width + rect.height) * 2)));
    segments = std::min(segments, 4096);
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
    const GrayImage& src = *selection.coverage;
    int w = src.width(), h = src.height(), r = std::abs(amount);
    // A round structuring element: dilate (grow) or erode (shrink) the coverage.
    auto out = std::make_shared<GrayImage>(w, h, 0);
    std::vector<std::pair<int, int>> disc;
    for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) if (dx * dx + dy * dy <= r * r) disc.push_back({dx, dy});
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = amount > 0 ? 0 : 255;
            for (auto [dx, dy] : disc) {
                int sx = x + dx, sy = y + dy;
                int s = (sx < 0 || sy < 0 || sx >= w || sy >= h) ? 0 : src.at(sx, sy);
                v = amount > 0 ? std::max(v, s) : std::min(v, s);
            }
            out->at(x, y) = uint8_t(v);
        }
    result.coverage = out;
    return result;
}

std::vector<std::vector<Point>> selectionOutline(const GrayImage& coverage) {
    std::vector<std::vector<Point>> loops;
    if (coverage.isEmpty()) return loops;
    // Trace pixels at least half selected.
    std::vector<uint8_t> mask(size_t(coverage.width()) * coverage.height());
    for (int y = 0; y < coverage.height(); y++) for (int x = 0; x < coverage.width(); x++) mask[size_t(y) * coverage.width() + x] = coverage.at(x, y) >= 128 ? 255 : 0;
    int32_t* points = nullptr; int32_t* counts = nullptr; size_t pointCount = 0, loopCount = 0;
    int result = wand_trace(mask.data(), size_t(coverage.width()), size_t(coverage.height()), &points, &pointCount, &counts, &loopCount);
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
