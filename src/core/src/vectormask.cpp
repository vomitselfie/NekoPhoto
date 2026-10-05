// Vector masks: parsing Photoshop's path records and drawing them as antialiased coverage. Layout and combine
// rules after Patchy (MIT; src/psd/psd_vector.cpp and core/vector_raster.cpp there).
#include "compositor/vectormask.h"
#include "compositor/document.h"
#include "compositor/smartobject.h"
#include "compositor/blur.h"
#include "compositor/depth.h"
#include "compositor/layerstyle.h"
#include "compositor/parallel.h"
#include "psd/psd_descriptor.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace compositor {

namespace {

constexpr size_t kRecord = 26;
constexpr double kFixed = 16777216.0;

uint16_t u16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
int32_t i32(const uint8_t* p) { return int32_t(u32(p)); }
void put32(uint8_t* p, int32_t v) { const uint32_t u = uint32_t(v); p[0] = uint8_t(u >> 24); p[1] = uint8_t(u >> 16); p[2] = uint8_t(u >> 8); p[3] = uint8_t(u); }

} // namespace

std::optional<VectorPath> parseVectorMask(const std::vector<uint8_t>& payload, int width, int height) {
    if (payload.size() < 8 + kRecord || width <= 0 || height <= 0) return std::nullopt;
    VectorPath path;
    const uint32_t flags = u32(payload.data() + 4);
    path.inverted = flags & 1;
    path.disabled = flags & 4;
    VectorPath::Subpath* current = nullptr;
    size_t expected = 0;
    for (size_t at = 8; at + kRecord <= payload.size(); at += kRecord) {
        const uint8_t* r = payload.data() + at;
        const uint16_t selector = u16(r);
        switch (selector) {
        case 0: case 3: {
            if (current && current->knots.size() != expected) return std::nullopt;
            uint16_t op = u16(r + 4);
            if (op == 0xFFFF) op = 0;   // CS4-era: parity (exclude) over what came before
            if (op > 3) return std::nullopt;
            VectorPath::Subpath s;
            s.closed = selector == 0;
            s.op = VectorPath::Op(op);
            s.group = int32_t(u32(r + 12));
            expected = u16(r + 2);
            path.subpaths.push_back(std::move(s));
            current = &path.subpaths.back();
            break;
        }
        case 1: case 2: case 4: case 5: {
            if (!current || current->knots.size() >= expected) return std::nullopt;
            auto c = [&](size_t o, int extent) { return i32(r + o) / kFixed * extent; };
            current->knots.push_back({c(6, width), c(2, height), c(14, width), c(10, height), c(22, width), c(18, height)});
            break;
        }
        case 6: case 7: case 8: break;
        default: return std::nullopt;
        }
    }
    if (current && current->knots.size() != expected) return std::nullopt;
    return path;
}

std::optional<std::vector<uint8_t>> mapVectorMask(const std::vector<uint8_t>& payload, int width, int height,
                                                  const std::function<Point(Point)>& map) {
    if (!parseVectorMask(payload, width, height)) return std::nullopt;
    std::vector<uint8_t> out = payload;
    for (size_t at = 8; at + kRecord <= out.size(); at += kRecord) {
        uint8_t* r = out.data() + at;
        const uint16_t selector = u16(r);
        if (selector != 1 && selector != 2 && selector != 4 && selector != 5) continue;
        for (size_t pair = 2; pair < 26; pair += 8) {
            const Point p = map({i32(r + pair + 4) / kFixed * width, i32(r + pair) / kFixed * height});
            put32(r + pair, int32_t(std::lround(std::clamp(p.y / height * kFixed, -2e9, 2e9))));
            put32(r + pair + 4, int32_t(std::lround(std::clamp(p.x / width * kFixed, -2e9, 2e9))));
        }
    }
    return out;
}

// ---- Drawing ---------------------------------------------------------------------------------------------------

namespace {

struct Edge { double x0, y0, x1, y1; };

/// The subpath as line segments in output pixels (cubic segments flattened to about a quarter pixel).
void flatten(const VectorPath::Subpath& s, const Rect& region, double scale, std::vector<Edge>& edges, bool asStroke = false) {
    const size_t n = s.knots.size();
    if (n < 2) return;
    auto out = [&](double x, double y) { return Point{(x - region.x) * scale, (y - region.y) * scale}; };
    // An open subpath still fills as closed (Photoshop closes it for the mask); a stroke leaves it open.
    const size_t segments = asStroke && !s.closed ? n - 1 : n;
    for (size_t i = 0; i < segments; i++) {
        const auto& a = s.knots[i];
        const auto& b = s.knots[(i + 1) % n];
        const Point p0 = out(a.x, a.y), p1 = out(a.outX, a.outY), p2 = out(b.inX, b.inY), p3 = out(b.x, b.y);
        const double length = std::hypot(p1.x - p0.x, p1.y - p0.y) + std::hypot(p2.x - p1.x, p2.y - p1.y) + std::hypot(p3.x - p2.x, p3.y - p2.y);
        const int steps = std::clamp(int(std::ceil(std::sqrt(length) * 2)), 1, 256);
        Point prev = p0;
        for (int k = 1; k <= steps; k++) {
            const double t = double(k) / steps, u = 1 - t;
            const Point q{u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x,
                          u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y};
            edges.push_back({prev.x, prev.y, q.x, q.y});
            prev = q;
        }
    }
}

/// Coverage over the box [x0, x1) x [y0, y1) of a w x h output; zero outside it.
struct Coverage {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::vector<float> v;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    float at(int x, int y) const {
        return x < x0 || x >= x1 || y < y0 || y >= y1 ? 0.0f : v[size_t(y - y0) * size_t(x1 - x0) + size_t(x - x0)];
    }
};

/// The pixel box of spans between minX..maxX and rows minY..maxY, inside w x h.
void coverageBox(Coverage& c, double minX, double maxX, double minY, double maxY, int w, int h) {
    auto lo = [](double v, int limit) { return int(std::clamp(std::floor(v), 0.0, double(limit))); };
    auto hi = [](double v, int limit) { return int(std::clamp(std::ceil(v), 0.0, double(limit))); };
    // A span's right end lands in pixel floor(maxX) (the partial pixel), so one past it.
    c.x0 = lo(minX, w); c.x1 = std::min(w, lo(maxX, w) + 1);
    c.y0 = lo(minY, h); c.y1 = hi(maxY, h);
    if (c.empty()) { c.x1 = c.x0; c.y1 = c.y0; return; }
    c.v.assign(size_t(c.x1 - c.x0) * size_t(c.y1 - c.y0), 0.0f);
}

/// Even-odd coverage of `edges` over w x h: 4 sample rows per pixel, exact horizontal coverage of each span.
/// Only the edges' bounding box is computed (rows in parallel); the rest is zero.
Coverage fill(const std::vector<Edge>& edges, int w, int h) {
    Coverage c;
    if (edges.empty()) return c;
    double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
    for (auto& e : edges) {
        minX = std::min({minX, e.x0, e.x1}); maxX = std::max({maxX, e.x0, e.x1});
        minY = std::min({minY, e.y0, e.y1}); maxY = std::max({maxY, e.y0, e.y1});
    }
    coverageBox(c, minX, maxX, minY, maxY, w, h);
    if (c.empty()) return c;
    constexpr int sub = 4;
    const int bw = c.x1 - c.x0;
    parallelRows(c.y0, c.y1, [&](int ya, int yb) {
        std::vector<double> xs;
        std::vector<float> row(size_t(bw) + 2);
        for (int y = ya; y < yb; y++) {
            std::fill(row.begin(), row.end(), 0.0f);
            for (int s = 0; s < sub; s++) {
                const double sy = y + (s + 0.5) / sub;
                xs.clear();
                for (auto& e : edges) {
                    if ((e.y0 <= sy) == (e.y1 <= sy)) continue;
                    xs.push_back(e.x0 + (sy - e.y0) / (e.y1 - e.y0) * (e.x1 - e.x0));
                }
                std::sort(xs.begin(), xs.end());
                for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                    const double a = std::clamp(xs[k], 0.0, double(w)), b = std::clamp(xs[k + 1], 0.0, double(w));
                    if (b <= a) continue;
                    const int ia = int(a), ib = int(b);
                    if (ia == ib) { row[size_t(ia - c.x0)] += float(b - a); continue; }
                    row[size_t(ia - c.x0)] += float(ia + 1 - a);
                    for (int x = ia + 1; x < ib; x++) row[size_t(x - c.x0)] += 1;
                    if (ib < w) row[size_t(ib - c.x0)] += float(b - ib);
                }
            }
            float* out = c.v.data() + size_t(y - c.y0) * size_t(bw);
            for (int x = 0; x < bw; x++) out[x] = std::min(1.0f, row[size_t(x)] / sub);
        }
    });
    return c;
}

/// Coverage 0..1 as a sample of either depth (0..255, or 0..32768), rounded to nearest.
template <class Gray> auto coverageSample(float v) {
    if constexpr (std::is_same_v<Gray, GrayImage>) return uint8_t(std::lround(v * 255));
    else return uint16_t(std::lround(v * 32768));
}

template <class Gray>
std::shared_ptr<Gray> rasterizeMask(const VectorPath& path, const Rect& region, double scale, int w, int h) {
    // Each shape group: consecutive subpaths sharing its index, filled together even-odd, over its own box.
    std::vector<Coverage> groups;
    std::vector<VectorPath::Op> ops;
    for (size_t i = 0; i < path.subpaths.size();) {
        const int32_t group = path.subpaths[i].group;
        ops.push_back(path.subpaths[i].op);
        std::vector<Edge> edges;
        size_t j = i;
        for (; j < path.subpaths.size() && path.subpaths[j].group == group; j++) flatten(path.subpaths[j], region, scale, edges);
        i = j;
        groups.push_back(fill(edges, w, h));
    }
    auto start = [](float g, VectorPath::Op op) { return op == VectorPath::Op::Subtract ? 1 - g : g; };
    auto combine = [](float a, float b, VectorPath::Op op) {
        switch (op) {
        case VectorPath::Op::Add: return a + b - a * b;
        case VectorPath::Op::Subtract: return a - a * b;
        case VectorPath::Op::Intersect: return a * b;
        case VectorPath::Op::Xor: return a + b - 2 * a * b;
        }
        return a;
    };
    auto byte = [&](float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        if (path.inverted) v = 1 - v;
        return coverageSample<Gray>(v);
    };
    // No subpaths: everything (Photoshop's Reveal All vector mask; inverted, Hide All).
    if (groups.empty()) return std::make_shared<Gray>(w, h, byte(1));
    // Outside every group's box each coverage is zero, so the result there is one value.
    float outside = 0;
    if (!groups.empty()) {
        outside = start(0, ops[0]);
        for (size_t k = 1; k < groups.size(); k++) outside = combine(outside, 0, ops[k]);
    }
    auto mask = std::make_shared<Gray>(w, h, byte(outside));
    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (const Coverage& g : groups)
        if (!g.empty()) { x0 = std::min(x0, g.x0); y0 = std::min(y0, g.y0); x1 = std::max(x1, g.x1); y1 = std::max(y1, g.y1); }
    if (x1 <= x0 || y1 <= y0) return mask;
    parallelRows(y0, y1, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            auto* r = mask->row(y);
            for (int x = x0; x < x1; x++) {
                float a = start(groups[0].at(x, y), ops[0]);
                for (size_t k = 1; k < groups.size(); k++) a = combine(a, groups[k].at(x, y), ops[k]);
                r[x] = byte(a);
            }
        }
    });
    return mask;
}

} // namespace

std::shared_ptr<GrayImage> rasterizeVectorMask(const VectorPath& path, const Rect& region, double scale, int w, int h) {
    return rasterizeMask<GrayImage>(path, region, scale, w, h);
}

std::shared_ptr<Gray16> rasterizeVectorMask16(const VectorPath& path, const Rect& region, double scale, int w, int h) {
    return rasterizeMask<Gray16>(path, region, scale, w, h);
}

// ---- Mask parameters -------------------------------------------------------------------------------------------

std::optional<MaskParameters> parseMaskParameters(const std::vector<uint8_t>& section) {
    // The section: rectangle, default colour, flags; with a vector mask's derived plane (bit 3) or without
    // parameters, the real mask's flags, default and rectangle next (Photoshop's order); then, with flag bit 4, a
    // parameter flags byte and the values it names in order.
    if (section.size() < 18) return std::nullopt;
    const uint8_t flags = section[17];
    if (!(flags & 0x10)) return std::nullopt;
    size_t at = 18;
    const bool parametersOnly = (flags & 0x18) == 0x10;
    if (section.size() >= 36 && !parametersOnly) at += 18;
    if (at >= section.size()) return std::nullopt;
    const uint8_t which = section[at++];
    MaskParameters p;
    auto f64 = [&](double& out) {
        if (at + 8 > section.size()) return false;
        uint64_t u = 0;
        for (int i = 0; i < 8; i++) u = u << 8 | section[at + size_t(i)];
        std::memcpy(&out, &u, 8);
        at += 8;
        return std::isfinite(out);
    };
    if (which & 1) { if (at >= section.size()) return std::nullopt; p.userDensity = section[at++]; }
    if (which & 2) { double v; if (!f64(v)) return std::nullopt; p.userFeather = v; }
    if (which & 4) { if (at >= section.size()) return std::nullopt; p.vectorDensity = section[at++]; }
    if (which & 8) { double v; if (!f64(v)) return std::nullopt; p.vectorFeather = v; }
    return p;
}

void applyMaskParameters(GrayImage& coverage, std::optional<int> density, std::optional<double> feather, double scale, bool clampEdges) {
    if (feather && *feather > 0) {
        if (!clampEdges) gaussianBlur(coverage, *feather * scale);
        else {
            // The pixel mask's feather repeats the edge pixels past the canvas (Photoshop's clamp).
            const int pad = int(std::ceil(*feather * scale * 3)) + 1, w = coverage.width(), h = coverage.height();
            GrayImage padded(w + 2 * pad, h + 2 * pad, 0);
            for (int y = 0; y < h + 2 * pad; y++) {
                const uint8_t* src = coverage.row(std::clamp(y - pad, 0, h - 1));
                uint8_t* dst = padded.row(y);
                for (int x = 0; x < w + 2 * pad; x++) dst[x] = src[std::clamp(x - pad, 0, w - 1)];
            }
            gaussianBlur(padded, *feather * scale);
            for (int y = 0; y < h; y++) std::memcpy(coverage.row(y), padded.row(y + pad) + pad, size_t(w));
        }
    }
    if (density && *density < 255) {
        // Density: what the mask hides shows at (255 - density) / 255.
        const int floor = 255 - std::clamp(*density, 0, 255);
        for (size_t i = 0; i < coverage.byteCount(); i++) coverage.data()[i] = uint8_t(floor + (coverage.data()[i] * (255 - floor) + 127) / 255);
    }
}

void applyMaskParameters(Gray16& coverage, std::optional<int> density, std::optional<double> feather, double scale, bool clampEdges) {
    if (feather && *feather > 0) {
        if (!clampEdges) gaussianBlur(coverage, *feather * scale);
        else {
            const int pad = int(std::ceil(*feather * scale * 3)) + 1, w = coverage.width(), h = coverage.height();
            Gray16 padded(w + 2 * pad, h + 2 * pad, 0);
            for (int y = 0; y < h + 2 * pad; y++) {
                const uint16_t* src = coverage.row(std::clamp(y - pad, 0, h - 1));
                uint16_t* dst = padded.row(y);
                for (int x = 0; x < w + 2 * pad; x++) dst[x] = src[std::clamp(x - pad, 0, w - 1)];
            }
            gaussianBlur(padded, *feather * scale);
            for (int y = 0; y < h; y++) std::memcpy(coverage.row(y), padded.row(y + pad) + pad, size_t(w) * sizeof(uint16_t));
        }
    }
    if (density && *density < 255) {
        // The density's floor, (255 - density) / 255, at 15 bits.
        const uint32_t floor = uint32_t(((255 - std::clamp(*density, 0, 255)) * 32768 + 127) / 255);
        const size_t n = size_t(coverage.width()) * size_t(coverage.height());
        for (size_t i = 0; i < n; i++) coverage.data()[i] = uint16_t(floor + ((coverage.data()[i] * (32768 - floor) + 16384) >> 15));
    }
}

// ---- Shape strokes --------------------------------------------------------------------------------------------

std::optional<VectorStroke> layerVectorStroke(const Layer& layer) {
    if (!layer.psdCarry) return std::nullopt;
    for (auto& b : layer.psdCarry->blocks) {
        if (b.key != "vstk") continue;
        try {
            namespace psd = patchy::psd;
            psd::BigEndianReader r(b.data);
            if (r.read_u32() != 16) return std::nullopt;
            const psd::DescriptorObject d = psd::read_descriptor(r);
            VectorStroke s;
            s.enabled = psd::descriptor_bool(d, "strokeEnabled", false);
            s.fillEnabled = psd::descriptor_bool(d, "fillEnabled", true);
            s.width = std::max(0.0, psd::descriptor_number(d, "strokeStyleLineWidth", 3));
            if (auto a = psd::descriptor_value(d, "strokeStyleLineAlignment"); a && a->type == psd::DescriptorValue::Type::Enum)
                s.align = a->enum_value == "strokeStyleAlignInside" ? VectorStroke::Align::Inside
                        : a->enum_value == "strokeStyleAlignOutside" ? VectorStroke::Align::Outside : VectorStroke::Align::Center;
            s.opacity = float(std::clamp(psd::descriptor_number(d, "strokeStyleOpacity", 100) / 100.0, 0.0, 1.0));
            auto enumOf = [&](const char* k) { auto v = psd::descriptor_value(d, k); return v && v->type == psd::DescriptorValue::Type::Enum ? v->enum_value : std::string(); };
            const std::string cap = enumOf("strokeStyleLineCapType"), join = enumOf("strokeStyleLineJoinType");
            s.cap = cap == "strokeStyleRoundCap" ? VectorStroke::Cap::Round : cap == "strokeStyleSquareCap" ? VectorStroke::Cap::Square : VectorStroke::Cap::Butt;
            s.join = join == "strokeStyleRoundJoin" ? VectorStroke::Join::Round : join == "strokeStyleBevelJoin" ? VectorStroke::Join::Bevel : VectorStroke::Join::Miter;
            s.miterLimit = std::max(1.0, psd::descriptor_number(d, "strokeStyleMiterLimit", 100));
            s.dashOffset = psd::descriptor_number(d, "strokeStyleLineDashOffset", 0);
            if (auto dash = psd::descriptor_value(d, "strokeStyleLineDashSet"); dash && dash->type == psd::DescriptorValue::Type::List)
                for (auto& v : dash->list_value) if (v.type == psd::DescriptorValue::Type::UnitFloat || v.type == psd::DescriptorValue::Type::Double) s.dashes.push_back(std::max(0.0, v.double_value));
            if (s.dashes.size() % 2) s.dashes.insert(s.dashes.end(), s.dashes.begin(), s.dashes.end());   // odd lists repeat
            double total = 0;
            for (double v : s.dashes) total += v;
            if (total <= 1e-6) s.dashes.clear();
            if (auto content = psd::descriptor_object(d, "strokeStyleContent")) {
                if (auto c = psd::descriptor_object(*content, "Clr ")) {
                    auto byte = [](double v) { return uint8_t(std::clamp(std::lround(v), 0L, 255L)); };
                    s.r = byte(psd::descriptor_number(*c, "Rd  ")); s.g = byte(psd::descriptor_number(*c, "Grn ")); s.b = byte(psd::descriptor_number(*c, "Bl  "));
                }
                // A gradient or pattern stroke: the content object read as a fill block would be.
                if (psd::descriptor_object(*content, "Grad") || psd::descriptor_object(*content, "Ptrn")) {
                    psd::BigEndianWriter w;
                    w.write_u32(16);
                    psd::write_descriptor(w, *content);
                    const std::vector<uint8_t> bytes = w.bytes();
                    if (auto g = parseFillGradient(bytes)) { s.paint.kind = VectorPaint::Kind::Gradient; s.paint.gradient = *g; }
                    else if (auto p = parseFillPattern(bytes)) { s.paint.kind = VectorPaint::Kind::Pattern; s.paint.pattern = *p; }
                }
            }
            return s;
        } catch (std::exception&) { return std::nullopt; }
    }
    return std::nullopt;
}

namespace {

using Poly = std::vector<Point>;

/// Nonzero-winding coverage of closed polygons (all wound the same way), antialiased like `fill`, over their box.
Coverage fillNonZero(const std::vector<Poly>& polys, int w, int h) {
    struct DirEdge { double x0, y0, x1, y1; int dir; };
    std::vector<DirEdge> edges;
    double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
    for (auto& p : polys)
        for (size_t i = 0; i < p.size(); i++) {
            const Point& a = p[i];
            const Point& b = p[(i + 1) % p.size()];
            if (a.y == b.y) continue;
            edges.push_back({a.x, a.y, b.x, b.y, a.y < b.y ? 1 : -1});
            minX = std::min({minX, a.x, b.x}); maxX = std::max({maxX, a.x, b.x});
            minY = std::min({minY, a.y, b.y}); maxY = std::max({maxY, a.y, b.y});
        }
    Coverage c;
    if (edges.empty()) return c;
    coverageBox(c, minX, maxX, minY, maxY, w, h);
    if (c.empty()) return c;
    constexpr int sub = 4;
    const int bw = c.x1 - c.x0;
    parallelRows(c.y0, c.y1, [&](int ya, int yb) {
        std::vector<std::pair<double, int>> xs;
        std::vector<float> row(size_t(bw) + 2);
        for (int y = ya; y < yb; y++) {
            std::fill(row.begin(), row.end(), 0.0f);
            for (int s = 0; s < sub; s++) {
                const double sy = y + (s + 0.5) / sub;
                xs.clear();
                for (auto& e : edges) {
                    if ((e.y0 <= sy) == (e.y1 <= sy)) continue;
                    xs.push_back({e.x0 + (sy - e.y0) / (e.y1 - e.y0) * (e.x1 - e.x0), e.dir});
                }
                std::sort(xs.begin(), xs.end());
                int winding = 0;
                for (size_t k = 0; k + 1 < xs.size(); k++) {
                    winding += xs[k].second;
                    if (winding == 0) continue;
                    const double a = std::clamp(xs[k].first, 0.0, double(w)), b = std::clamp(xs[k + 1].first, 0.0, double(w));
                    if (b <= a) continue;
                    const int ia = int(a), ib = int(b);
                    if (ia == ib) { row[size_t(ia - c.x0)] += float(b - a); continue; }
                    row[size_t(ia - c.x0)] += float(ia + 1 - a);
                    for (int x = ia + 1; x < ib; x++) row[size_t(x - c.x0)] += 1;
                    if (ib < w) row[size_t(ib - c.x0)] += float(b - ib);
                }
            }
            float* out = c.v.data() + size_t(y - c.y0) * size_t(bw);
            for (int x = 0; x < bw; x++) out[x] = std::min(1.0f, row[size_t(x)] / sub);
        }
    });
    return c;
}

/// Counter-clockwise (in y-down pixels, positive signed area) so overlapping pieces add instead of cancelling.
void orient(Poly& p) {
    double area = 0;
    for (size_t i = 0; i < p.size(); i++) { const Point& a = p[i]; const Point& b = p[(i + 1) % p.size()]; area += a.x * b.y - b.x * a.y; }
    if (area < 0) std::reverse(p.begin(), p.end());
}

Poly disc(Point c, double r) {
    Poly p;
    const int n = std::clamp(int(std::ceil(r * 2)), 8, 128);
    for (int i = 0; i < n; i++) { const double a = 2 * M_PI * i / n; p.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)}); }
    return p;
}

/// The stroke's outline pieces for one polyline: a quad per segment, a join at each inner vertex (and around a
/// closed one), caps at the ends of an open one. `h` is half the band's width.
void strokePolyline(const std::vector<Point>& pts, bool closed, double h, const VectorStroke& s, std::vector<Poly>& out) {
    std::vector<Point> p;
    for (const Point& q : pts) if (p.empty() || std::hypot(q.x - p.back().x, q.y - p.back().y) > 1e-9) p.push_back(q);
    if (closed && p.size() > 2 && std::hypot(p.front().x - p.back().x, p.front().y - p.back().y) < 1e-9) p.pop_back();
    const size_t n = p.size();
    if (n < 2) {
        if (n == 1 && s.cap == VectorStroke::Cap::Round) out.push_back(disc(p[0], h));
        return;
    }
    const size_t segments = closed ? n : n - 1;
    auto normal = [&](size_t i) {
        const Point& a = p[i]; const Point& b = p[(i + 1) % n];
        const double len = std::hypot(b.x - a.x, b.y - a.y);
        return Point{-(b.y - a.y) / len, (b.x - a.x) / len};
    };
    for (size_t i = 0; i < segments; i++) {
        const Point& a = p[i]; const Point& b = p[(i + 1) % n];
        const Point nn = normal(i);
        Poly quad{{a.x + nn.x * h, a.y + nn.y * h}, {b.x + nn.x * h, b.y + nn.y * h}, {b.x - nn.x * h, b.y - nn.y * h}, {a.x - nn.x * h, a.y - nn.y * h}};
        orient(quad);
        out.push_back(std::move(quad));
    }
    // Joins.
    for (size_t v = closed ? 0 : 1; v < (closed ? n : n - 1); v++) {
        const size_t before = (v + n - 1) % n;
        const Point n0 = normal(before), n1 = normal(v);
        const Point& c = p[v];
        const double cross = n0.x * n1.y - n0.y * n1.x;
        if (std::abs(cross) < 1e-9 && n0.x * n1.x + n0.y * n1.y > 0) continue;   // straight on
        if (s.join == VectorStroke::Join::Round) { out.push_back(disc(c, h)); continue; }
        // The outer side of the turn gets the wedge; the inner side is covered by the quads.
        const double side = cross > 0 ? -1 : 1;
        const Point a{c.x + n0.x * h * side, c.y + n0.y * h * side}, b{c.x + n1.x * h * side, c.y + n1.y * h * side};
        Poly wedge{c, a, b};
        if (s.join == VectorStroke::Join::Miter) {
            const Point bis{n0.x + n1.x, n0.y + n1.y};
            const double bl = std::hypot(bis.x, bis.y);
            if (bl > 1e-9) {
                const double cosHalf = bl / 2;                      // cos of half the angle between the normals
                const double ratio = 1 / std::max(1e-9, cosHalf);   // miter length / half width
                if (ratio <= s.miterLimit) wedge = {c, a, {c.x + bis.x / bl * h * ratio * side, c.y + bis.y / bl * h * ratio * side}, b};
            }
        }
        orient(wedge);
        out.push_back(std::move(wedge));
    }
    // Caps.
    if (!closed && s.cap != VectorStroke::Cap::Butt)
        for (int end = 0; end < 2; end++) {
            const Point& c = end ? p[n - 1] : p[0];
            const Point& o = end ? p[n - 2] : p[1];
            if (s.cap == VectorStroke::Cap::Round) { out.push_back(disc(c, h)); continue; }
            const double len = std::hypot(c.x - o.x, c.y - o.y);
            const Point d{(c.x - o.x) / len * h, (c.y - o.y) / len * h}, nn{-d.y, d.x};
            Poly square{{c.x + nn.x, c.y + nn.y}, {c.x + nn.x + d.x, c.y + nn.y + d.y}, {c.x - nn.x + d.x, c.y - nn.y + d.y}, {c.x - nn.x, c.y - nn.y}};
            orient(square);
            out.push_back(std::move(square));
        }
}

/// A polyline cut into dashes (lengths in pixels, alternately on and off), each an open polyline.
std::vector<std::vector<Point>> dash(const std::vector<Point>& pts, const std::vector<double>& pattern, double offset) {
    std::vector<std::vector<Point>> out;
    double total = 0;
    for (double v : pattern) total += v;
    if (total <= 0 || pts.size() < 2) return out;
    size_t k = 0;
    double left = pattern[0];
    offset = std::fmod(offset, total);
    if (offset < 0) offset += total;
    while (offset > 0) {
        if (offset >= left) { offset -= left; k = (k + 1) % pattern.size(); left = pattern[k]; }
        else { left -= offset; offset = 0; }
    }
    std::vector<Point> cur;
    if (k % 2 == 0) cur.push_back(pts[0]);
    for (size_t i = 1; i < pts.size(); i++) {
        Point a = pts[i - 1];
        const Point b = pts[i];
        double seg = std::hypot(b.x - a.x, b.y - a.y);
        while (seg > 0) {
            const double step = std::min(seg, left);
            const double t = step / seg;
            const Point m{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
            if (k % 2 == 0) cur.push_back(m);
            seg -= step; left -= step; a = m;
            if (left <= 1e-9) {
                if (k % 2 == 0) { if (cur.size() > 1) out.push_back(cur); cur.clear(); }
                k = (k + 1) % pattern.size();
                left = pattern[k];
                if (k % 2 == 0) cur = {a};
            }
        }
    }
    if (k % 2 == 0 && cur.size() > 1) out.push_back(cur);
    return out;
}

} // namespace

namespace {

template <class Gray>
std::shared_ptr<Gray> rasterizeStroke(const VectorPath& path, const VectorStroke& stroke, const Rect& region, double scale, int w, int h) {
    constexpr double one = std::is_same_v<Gray, GrayImage> ? 255.0 : 32768.0;
    auto out = std::make_shared<Gray>(w, h, 0);
    // Inside and outside strokes are a centred band twice as wide, kept to one side of the path (Photoshop's way).
    const double width = stroke.width * scale;
    const double half = stroke.align == VectorStroke::Align::Center ? width / 2 : width;
    std::vector<Poly> pieces;
    for (auto& s : path.subpaths) {
        std::vector<Edge> edges;
        flatten(s, region, scale, edges, true);
        if (edges.empty()) continue;
        std::vector<Point> pts{{edges[0].x0, edges[0].y0}};
        for (auto& e : edges) pts.push_back({e.x1, e.y1});
        if (stroke.dashes.empty()) { strokePolyline(pts, s.closed, half, stroke, pieces); continue; }
        std::vector<double> pattern;
        for (double v : stroke.dashes) pattern.push_back(v * width);
        if (s.closed) pts.push_back(pts.front());
        for (auto& d : dash(pts, pattern, stroke.dashOffset * width)) strokePolyline(d, false, half, stroke, pieces);
    }
    const Coverage band = fillNonZero(pieces, w, h);
    if (band.empty()) return out;
    std::shared_ptr<Gray> inside;
    if (stroke.align != VectorStroke::Align::Center) {
        VectorPath plain = path;
        plain.inverted = false;
        inside = rasterizeMask<Gray>(plain, region, scale, w, h);
    }
    parallelRows(band.y0, band.y1, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            auto* row = out->row(y);
            for (int x = band.x0; x < band.x1; x++) {
                double v = band.at(x, y);
                if (inside) { const double in = inside->row(y)[x] / one; v *= stroke.align == VectorStroke::Align::Inside ? in : 1 - in; }
                row[x] = std::remove_reference_t<decltype(row[x])>(std::lround(std::clamp(v, 0.0, 1.0) * one));
            }
        }
    });
    return out;
}

} // namespace

std::shared_ptr<GrayImage> rasterizeVectorStroke(const VectorPath& path, const VectorStroke& stroke, const Rect& region, double scale, int w, int h) {
    return rasterizeStroke<GrayImage>(path, stroke, region, scale, w, h);
}

std::shared_ptr<Gray16> rasterizeVectorStroke16(const VectorPath& path, const VectorStroke& stroke, const Rect& region, double scale, int w, int h) {
    return rasterizeStroke<Gray16>(path, stroke, region, scale, w, h);
}

// ---- Fill layers --------------------------------------------------------------------------------------------------

namespace {

template <class Img>
std::shared_ptr<const Img> paintAt(const VectorPaint& paint, const Document& document, const Rect& bounds, const Rect& area, double scale, int w, int h) {
    constexpr bool eight = std::is_same_v<Img, Image>;
    if (w <= 0 || h <= 0 || scale <= 0) return nullptr;
    // Output pixel (px, py) is document (area.x + px / scale, area.y + py / scale): at scale 1 over the canvas, a
    // gradient is sampled at the pixel's corner and a pattern at its centre, as fill layers have always been drawn.
    if (paint.kind == VectorPaint::Kind::Gradient) {
        auto image = std::make_shared<Img>(w, h);
        const StyleGradient& g = paint.gradient;
        double bx = 0, by = 0, bw = document.width, bh = document.height;
        if (g.alignWithLayer && bounds.width > 0 && bounds.height > 0) { bx = bounds.x; by = bounds.y; bw = bounds.width; bh = bounds.height; }
        parallelRows(0, h, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                auto* row = image->row(y);
                for (int x = 0; x < w; x++, row += 4) {
                    const float t = gradientPosition(g, bx, by, bw, bh, area.x + x / scale, area.y + y / scale);
                    const float a = gradientOpacity(g, t);
                    if constexpr (eight) {
                        const StyleColor c = gradientColor(g, t);
                        row[0] = uint8_t(std::lround(c.r * a)); row[1] = uint8_t(std::lround(c.g * a)); row[2] = uint8_t(std::lround(c.b * a)); row[3] = uint8_t(std::lround(255 * a));
                    } else {
                        // The ramp's exact colour at 15 bits: no 8-bit steps in a long gradient.
                        double c[3];
                        gradientColorExact(g, t, c);
                        for (int k = 0; k < 3; k++) row[k] = uint16_t(std::lround(std::clamp(c[k], 0.0, 255.0) / 255.0 * a * 32768));
                        row[3] = uint16_t(std::lround(32768.0 * a));
                    }
                }
            }
        });
        return image;
    }
    if (paint.kind != VectorPaint::Kind::Pattern) return nullptr;
    const FillPattern& p = paint.pattern;
    auto patterns = documentPatterns(document);
    if (!patterns) return nullptr;
    auto tile = patterns->find(p.id);
    if (tile == patterns->end() || tile->second.width <= 0 || tile->second.height <= 0) return nullptr;
    const PatternTile& t = tile->second;
    auto image = std::make_shared<Img>(w, h);
    const double inv = 1.0 / std::max(0.01f, p.scale), a = p.angle * M_PI / 180, cs = std::cos(a), sn = std::sin(a);
    parallelRows(0, h, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            auto* row = image->row(y);
            for (int x = 0; x < w; x++, row += 4) {
                const double u0 = area.x + (x + 0.5) / scale - p.phaseX, v0 = area.y + (y + 0.5) / scale - p.phaseY;
                const double u = (u0 * cs - v0 * sn) * inv, v = (u0 * sn + v0 * cs) * inv;
                long tx = long(std::floor(u)) % t.width, ty = long(std::floor(v)) % t.height;
                if (tx < 0) tx += t.width;
                if (ty < 0) ty += t.height;
                const uint8_t* src = t.rgba.data() + (size_t(ty) * size_t(t.width) + size_t(tx)) * 4;
                if constexpr (eight) {
                    for (int k = 0; k < 3; k++) row[k] = uint8_t((src[k] * src[3] + 127) / 255);
                    row[3] = src[3];
                } else {
                    // Patterns are 8-bit tiles: premultiplied at 15 bits from the straight texel.
                    const uint32_t a = widen8(src[3]);
                    for (int k = 0; k < 3; k++) row[k] = uint16_t(mul15(widen8(src[k]), a));
                    row[3] = uint16_t(a);
                }
            }
        }
    });
    return image;
}

} // namespace

ImagePtr renderVectorPaint(const VectorPaint& paint, const Document& document, const Rect& bounds, const Rect& area, double scale, int w, int h) {
    return paintAt<Image>(paint, document, bounds, area, scale, w, h);
}

Image16Ptr renderVectorPaint16(const VectorPaint& paint, const Document& document, const Rect& bounds, const Rect& area, double scale, int w, int h) {
    return paintAt<Image16>(paint, document, bounds, area, scale, w, h);
}

namespace {

/// A fill layer's paint and the bounds its gradient spans; none when it has no fill block (or it cannot be read).
std::optional<std::pair<VectorPaint, Rect>> fillLayerPaint(const Layer& layer, const Document& document) {
    if (!layer.psdCarry) return std::nullopt;
    const std::vector<uint8_t>* gradientBlock = nullptr;
    const std::vector<uint8_t>* patternBlock = nullptr;
    for (auto& b : layer.psdCarry->blocks) { if (b.key == "GdFl") gradientBlock = &b.data; if (b.key == "PtFl") patternBlock = &b.data; }
    if (!gradientBlock && !patternBlock) return std::nullopt;
    VectorPaint paint;
    Rect bounds;
    if (gradientBlock) {
        auto g = parseFillGradient(*gradientBlock);
        if (!g) return std::nullopt;
        paint.kind = VectorPaint::Kind::Gradient;
        paint.gradient = *g;
        // Aligned with the layer: over its shape's bounds (the vector mask's hull), else the canvas.
        if (g->alignWithLayer)
            if (auto v = layerVectorMask(layer, document)) {
                double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
                for (auto& s : v->subpaths) for (auto& k : s.knots) for (auto [x, y] : {std::pair{k.x, k.y}, {k.inX, k.inY}, {k.outX, k.outY}}) {
                    x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
                }
                if (x1 > x0 && y1 > y0) bounds = Rect(x0, y0, x1 - x0, y1 - y0);
            }
    } else {
        auto p = parseFillPattern(*patternBlock);
        if (!p) return std::nullopt;
        // Linked with the layer (Photoshop's default), the tile is anchored at the layer's reference point ('fxrp'),
        // as a Pattern Overlay's is (a Photoshop-saved fill whose point is (0, -23) starts its 4-row tile a row down).
        if (p->linked)
            for (auto& b : layer.psdCarry->blocks)
                if (b.key == "fxrp" && b.data.size() >= 16) {
                    patchy::psd::BigEndianReader r(b.data);
                    p->phaseX += float(patchy::psd::read_f64(r));
                    p->phaseY += float(patchy::psd::read_f64(r));
                }
        paint.kind = VectorPaint::Kind::Pattern;
        paint.pattern = *p;
    }
    return std::pair{paint, bounds};
}

} // namespace

ImagePtr renderFillLayer(const Layer& layer, const Document& document) {
    auto fill = fillLayerPaint(layer, document);
    if (!fill) return nullptr;
    return renderVectorPaint(fill->first, document, fill->second, Rect(0, 0, document.width, document.height), 1, document.width, document.height);
}

Image16Ptr renderFillLayer16(const Layer& layer, const Document& document) {
    auto fill = fillLayerPaint(layer, document);
    if (!fill) return nullptr;
    return renderVectorPaint16(fill->first, document, fill->second, Rect(0, 0, document.width, document.height), 1, document.width, document.height);
}
AnyImage renderVectorPaintInks(const VectorPaint& paint, const Document& document, const Rect& bounds, const Rect& area, double scale, int w, int h, bool deep) {
    if (paint.kind != VectorPaint::Kind::Gradient || paint.gradient.colors.empty()) return {};
    for (auto& stop : paint.gradient.colors) if (!stop.ink) return {};
    // Interpolation is per channel, so the RGB renderer draws the inks: C, M and Y's complements as red, green and
    // blue, then K's as grey; the two share the alpha stops.
    VectorPaint cmy = paint, k = paint;
    // Inks run from ink to ink whatever the interpolation method: Linear and Perceptual are RGB spaces (unverified
    // against a Photoshop-saved CMYK file with them).
    cmy.gradient.interpolation = k.gradient.interpolation = StyleGradient::Interpolation::Classic;
    auto level = [](float ink) { return uint8_t(std::lround((1 - ink) * 255)); };
    for (size_t i = 0; i < paint.gradient.colors.size(); i++) {
        const auto& ink = *paint.gradient.colors[i].ink;
        cmy.gradient.colors[i].color = {level(ink[0]), level(ink[1]), level(ink[2])};
        const uint8_t kl = level(ink[3]);
        k.gradient.colors[i].color = {kl, kl, kl};
    }
    if (deep) {
        auto out = std::make_shared<Image16>(w, h, 5);
        auto a = renderVectorPaint16(cmy, document, bounds, area, scale, w, h), b = renderVectorPaint16(k, document, bounds, area, scale, w, h);
        if (!a || !b) return {};
        for (int y = 0; y < h; y++) {
            const auto* pa = a->row(y); const auto* pb = b->row(y); auto* po = out->row(y);
            for (int x = 0; x < w; x++, pa += 4, pb += 4, po += 5) { po[0] = pa[0]; po[1] = pa[1]; po[2] = pa[2]; po[3] = pb[0]; po[4] = pa[3]; }
        }
        return Image16Ptr(out);
    }
    auto out = std::make_shared<ImageC8>(w, h, 5);
    auto a = renderVectorPaint(cmy, document, bounds, area, scale, w, h), b = renderVectorPaint(k, document, bounds, area, scale, w, h);
    if (!a || !b) return {};
    for (int y = 0; y < h; y++) {
        const uint8_t* pa = a->row(y); const uint8_t* pb = b->row(y); uint8_t* po = out->row(y);
        for (int x = 0; x < w; x++, pa += 4, pb += 4, po += 5) { po[0] = pa[0]; po[1] = pa[1]; po[2] = pa[2]; po[3] = pb[0]; po[4] = pa[3]; }
    }
    return ImageC8Ptr(out);
}

AnyImage renderFillLayerInks(const Layer& layer, const Document& document, bool deep) {
    auto fill = fillLayerPaint(layer, document);
    if (!fill) return {};
    return renderVectorPaintInks(fill->first, document, fill->second, Rect(0, 0, document.width, document.height), 1, document.width, document.height, deep);
}

Point mapLayerPoint(Point p, const LayerTransform& before, int w0, int h0, const LayerTransform& after, int w1, int h1) {
    // One corner of a degenerate quad: moveQuad already inverts one transform and applies the other.
    const auto q = moveQuad({p.x, p.y, p.x, p.y, p.x, p.y, p.x, p.y}, before, w0, h0, after, w1, h1);
    return {q[0], q[1]};
}

std::optional<VectorPath> layerVectorMask(const Layer& layer, const Document& document) {
    if (!layer.psdCarry) return std::nullopt;
    const PsdLayerCarry& c = *layer.psdCarry;
    const std::vector<uint8_t>* block = nullptr;
    for (auto& b : c.blocks) if (b.key == "vmsk" || b.key == "vsms") { block = &b.data; break; }
    if (!block) return std::nullopt;
    // The canvas the path was stored against: the document's, as read (Image Size scales layers, not this).
    const int width = document.psdCarry && document.psdCarry->width > 0 ? document.psdCarry->width : document.width;
    const int height = document.psdCarry && document.psdCarry->height > 0 ? document.psdCarry->height : document.height;
    // Parsed each time: a few hundred bytes (a cache keyed by the block's address could outlive its document).
    std::optional<VectorPath> parsed = parseVectorMask(*block, width, height);
    if (!parsed || parsed->disabled) return std::nullopt;
    // Where the layer has gone since it was read, the path goes too.
    const LayerTransform& now = layer.transform;
    const int w1 = layer.pixelWidth(), h1 = layer.pixelHeight();
    const bool moved = !(c.placement.origin == now.origin && c.placement.size == now.size && c.placement.rotation == now.rotation
                         && c.placement.flipX == now.flipX && c.placement.flipY == now.flipY);
    if (moved) {
        // The raster size the placement described: the transform's own size when it was read unscaled.
        const int w0 = std::max(1, int(std::lround(c.placement.size.width))), h0 = std::max(1, int(std::lround(c.placement.size.height)));
        for (auto& s : parsed->subpaths)
            for (auto& k : s.knots) {
                auto map = [&](double& x, double& y) { const Point p = mapLayerPoint({x, y}, c.placement, w0, h0, now, w1, h1); x = p.x; y = p.y; };
                map(k.inX, k.inY); map(k.x, k.y); map(k.outX, k.outY);
            }
    }
    return parsed;
}

} // namespace compositor
