#include "compositor/vectorlayer.h"
#include "compositor/psd_carry.h"
#include "psd/psd_descriptor.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace compositor {

namespace psd = patchy::psd;

namespace {

constexpr double kFixed = 16777216.0;
constexpr size_t kRecord = 26;

void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) { b[at] = uint8_t(v >> 8); b[at + 1] = uint8_t(v); }
void put32(std::vector<uint8_t>& b, size_t at, int32_t v) {
    const uint32_t u = uint32_t(v);
    b[at] = uint8_t(u >> 24); b[at + 1] = uint8_t(u >> 16); b[at + 2] = uint8_t(u >> 8); b[at + 3] = uint8_t(u);
}
int32_t fixed(double v, int extent) {
    const double f = std::round(v / std::max(1, extent) * kFixed);
    return int32_t(std::clamp(f, -2147483648.0, 2147483647.0));
}
void pad4(std::vector<uint8_t>& b) { while (b.size() % 4) b.push_back(0); }

/// A descriptor object built in order (4-character keys as charIDs, longer ones as stringIDs).
struct Desc {
    psd::DescriptorObject o;
    explicit Desc(const char* cls) { o.class_id = cls; o.class_id_long_form = std::string(cls).size() != 4; }
    Desc& put(const std::string& k, psd::DescriptorValue v) { o.key_order.push_back({k, k.size() != 4}); o.values[k] = std::move(v); return *this; }
    Desc& boolean(const std::string& k, bool b) { psd::DescriptorValue v; v.type = psd::DescriptorValue::Type::Bool; v.bool_value = b; return put(k, v); }
    Desc& integer(const std::string& k, int32_t i) { psd::DescriptorValue v; v.type = psd::DescriptorValue::Type::Integer; v.integer_value = i; return put(k, v); }
    Desc& number(const std::string& k, double d) { psd::DescriptorValue v; v.type = psd::DescriptorValue::Type::Double; v.double_value = d; return put(k, v); }
    Desc& unit(const std::string& k, const char* u, double d) { psd::DescriptorValue v; v.type = psd::DescriptorValue::Type::UnitFloat; v.unit = u; v.double_value = d; return put(k, v); }
    Desc& enumeration(const std::string& k, const std::string& t, const std::string& e) {
        psd::DescriptorValue v; v.type = psd::DescriptorValue::Type::Enum; v.enum_type = t; v.enum_value = e;
        v.enum_type_long_form = t.size() != 4; v.enum_value_long_form = e.size() != 4;
        return put(k, v);
    }
    Desc& object(const std::string& k, Desc d) {
        psd::DescriptorValue v; v.type = psd::DescriptorValue::Type::Object; v.object_value = std::make_shared<psd::DescriptorObject>(std::move(d.o));
        return put(k, v);
    }
};

Desc colour(uint8_t r, uint8_t g, uint8_t b) { Desc c("RGBC"); c.number("Rd  ", r).number("Grn ", g).number("Bl  ", b); return c; }

std::vector<uint8_t> descriptorBlock(const Desc& d) {
    psd::BigEndianWriter w;
    w.write_u32(16);
    psd::write_descriptor(w, d.o);
    auto bytes = w.bytes();
    pad4(bytes);
    return bytes;
}

const std::vector<uint8_t>* block(const Layer& layer, const char* key) {
    if (!layer.psdCarry) return nullptr;
    for (auto& b : layer.psdCarry->blocks) if (b.key == key) return &b.data;
    return nullptr;
}

/// A cubic Bézier quarter-circle's handle length, as a fraction of the radius.
constexpr double kKappa = 0.5522847498307936;

VectorPath::Knot corner(double x, double y) { return {x, y, x, y, x, y}; }

VectorPath single(std::vector<VectorPath::Knot> knots) {
    VectorPath p;
    VectorPath::Subpath s;
    s.knots = std::move(knots);
    s.closed = true;
    s.op = VectorPath::Op::Add;
    p.subpaths.push_back(std::move(s));
    return p;
}

/// A polygon through `points` (fractions of `box`), corners only.
VectorPath fromUnitPoints(const Rect& box, std::initializer_list<std::pair<double, double>> points) {
    std::vector<VectorPath::Knot> k;
    for (auto [u, v] : points) k.push_back(corner(box.x + u * box.width, box.y + v * box.height));
    return single(std::move(k));
}

} // namespace

std::vector<uint8_t> authorVectorMask(const VectorPath& path, int w, int h) {
    std::vector<uint8_t> out(8, 0);
    put32(out, 0, 3);   // version
    put32(out, 4, int32_t((path.inverted ? 1 : 0) | (path.disabled ? 4 : 0)));
    auto record = [&] { const size_t at = out.size(); out.resize(at + kRecord, 0); return at; };
    const size_t fillRule = record();
    put16(out, fillRule, 6);
    const size_t initialFill = record();
    put16(out, initialFill, 8);
    for (const auto& s : path.subpaths) {
        const size_t length = record();
        put16(out, length, s.closed ? 0 : 3);
        put16(out, length + 2, uint16_t(s.knots.size()));
        put16(out, length + 4, uint16_t(s.op));
        put16(out, length + 6, 1);
        put32(out, length + 12, s.group);
        for (const auto& k : s.knots) {
            const size_t at = record();
            const bool smooth = knotIsSmooth(k);
            put16(out, at, s.closed ? (smooth ? 1 : 2) : (smooth ? 4 : 5));
            put32(out, at + 2, fixed(k.inY, h)); put32(out, at + 6, fixed(k.inX, w));
            put32(out, at + 10, fixed(k.y, h)); put32(out, at + 14, fixed(k.x, w));
            put32(out, at + 18, fixed(k.outY, h)); put32(out, at + 22, fixed(k.outX, w));
        }
    }
    pad4(out);
    return out;
}

std::vector<uint8_t> authorVectorStroke(const VectorStroke& s, bool fillEnabled) {
    // Photoshop 2026's 16-item strokeStyle, in its order (Patchy's capture).
    Desc d("strokeStyle");
    d.integer("strokeStyleVersion", 2).boolean("strokeEnabled", s.enabled).boolean("fillEnabled", fillEnabled);
    d.unit("strokeStyleLineWidth", "#Pxl", s.width).unit("strokeStyleLineDashOffset", "#Pnt", s.dashOffset);
    d.number("strokeStyleMiterLimit", s.miterLimit);
    d.enumeration("strokeStyleLineCapType", "strokeStyleLineCapType",
                  s.cap == VectorStroke::Cap::Round ? "strokeStyleRoundCap" : s.cap == VectorStroke::Cap::Square ? "strokeStyleSquareCap" : "strokeStyleButtCap");
    d.enumeration("strokeStyleLineJoinType", "strokeStyleLineJoinType",
                  s.join == VectorStroke::Join::Round ? "strokeStyleRoundJoin" : s.join == VectorStroke::Join::Bevel ? "strokeStyleBevelJoin" : "strokeStyleMiterJoin");
    d.enumeration("strokeStyleLineAlignment", "strokeStyleLineAlignment",
                  s.align == VectorStroke::Align::Inside ? "strokeStyleAlignInside" : s.align == VectorStroke::Align::Outside ? "strokeStyleAlignOutside" : "strokeStyleAlignCenter");
    d.boolean("strokeStyleScaleLock", false).boolean("strokeStyleStrokeAdjust", false);
    psd::DescriptorValue dashes;
    dashes.type = psd::DescriptorValue::Type::List;
    for (double v : s.dashes) { psd::DescriptorValue u; u.type = psd::DescriptorValue::Type::UnitFloat; u.unit = "#Nne"; u.double_value = v; dashes.list_value.push_back(u); }
    d.put("strokeStyleLineDashSet", dashes);
    d.enumeration("strokeStyleBlendMode", "BlnM", "normal").unit("strokeStyleOpacity", "#Prc", s.opacity * 100.0);
    Desc content("solidColorLayer");
    content.object("Clr ", colour(s.r, s.g, s.b));
    d.object("strokeStyleContent", std::move(content)).number("strokeStyleResolution", 72);
    return descriptorBlock(d);
}

std::vector<uint8_t> authorSolidColour(uint8_t r, uint8_t g, uint8_t b) {
    Desc d("null");
    d.object("Clr ", colour(r, g, b));
    return descriptorBlock(d);
}

void pathCanvas(const Document& document, int& width, int& height) {
    width = document.psdCarry && document.psdCarry->width > 0 ? document.psdCarry->width : document.width;
    height = document.psdCarry && document.psdCarry->height > 0 ? document.psdCarry->height : document.height;
}

namespace {
/// The content hash of an image, remembered while the image lives (layer images do not change once shared).
uint64_t cachedContentHash(const std::shared_ptr<const Image>& image) {
    static std::mutex mutex;
    static std::map<const Image*, std::pair<std::weak_ptr<const Image>, uint64_t>> cache;
    std::lock_guard lock(mutex);
    auto it = cache.find(image.get());
    if (it != cache.end() && it->second.first.lock() == image) return it->second.second;
    if (cache.size() > 256) cache.clear();
    const uint64_t hash = psdContentHash(image.get());
    cache[image.get()] = {image, hash};
    return hash;
}

/// The blocks are there and the pixels are still the fill they describe (wherever the layer now is).
bool hasShapeBlocks(const Layer& layer) {
    return !layer.isGroup && !layer.adjustment && layer.asset && layer.asset->image && (block(layer, "vsms") || block(layer, "vmsk")) && block(layer, "SoCo")
        && layer.psdCarry->contentHash == cachedContentHash(layer.asset->image);
}
}

bool isVectorShapeLayer(const Layer& layer) { return hasShapeBlocks(layer); }

int refreshVectorShapes(Document& document) {
    int redrawn = 0;
    for (Layer& layer : document.layers) {
        if (!hasShapeBlocks(layer)) continue;
        const LayerTransform& at = layer.psdCarry->placement;
        const LayerTransform& now = layer.transform;
        if (at.origin == now.origin && at.size == now.size && at.rotation == now.rotation && at.flipX == now.flipX && at.flipY == now.flipY) continue;
        auto shape = vectorShapeOf(layer, document);   // the path already follows the layer to where it is
        if (!shape) continue;
        setVectorShape(layer, document, *shape);
        redrawn++;
    }
    return redrawn;
}

std::optional<VectorShape> vectorShapeOf(const Layer& layer, const Document& document) {
    if (!isVectorShapeLayer(layer)) return std::nullopt;
    auto path = layerVectorMask(layer, document);
    if (!path) return std::nullopt;
    VectorShape shape;
    shape.path = *path;
    try {
        psd::BigEndianReader r(*block(layer, "SoCo"));
        if (r.read_u32() == 16) {
            const psd::DescriptorObject d = psd::read_descriptor(r);
            if (auto c = psd::descriptor_object(d, "Clr ")) {
                auto byte = [](double v) { return uint8_t(std::clamp(std::lround(v), 0L, 255L)); };
                shape.r = byte(psd::descriptor_number(*c, "Rd  ")); shape.g = byte(psd::descriptor_number(*c, "Grn ")); shape.b = byte(psd::descriptor_number(*c, "Bl  "));
            }
        }
    } catch (std::exception&) {}
    if (auto stroke = layerVectorStroke(layer)) { shape.stroke = *stroke; shape.fill = stroke->fillEnabled; }
    else shape.stroke.enabled = false;
    return shape;
}

Rect pathBounds(const VectorPath& path) {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (auto& s : path.subpaths)
        for (auto& k : s.knots)
            for (auto [x, y] : {std::pair{k.inX, k.inY}, std::pair{k.x, k.y}, std::pair{k.outX, k.outY}}) {
                x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
            }
    return x1 < x0 ? Rect() : Rect(x0, y0, x1 - x0, y1 - y0);
}

void setVectorShape(Layer& layer, const Document& document, const VectorShape& shape) {
    // Pixels: the fill colour over the path's bounds and a margin for the stroke and antialiasing.
    const double margin = (shape.stroke.enabled ? shape.stroke.width : 0) + 2;
    Rect bounds = pathBounds(shape.path);
    // A straight line has no area but still has a place (its stroke margin gives it pixels); only no path has none.
    const bool noKnots = std::all_of(shape.path.subpaths.begin(), shape.path.subpaths.end(), [](const auto& s) { return s.knots.empty(); });
    if (noKnots) bounds = Rect(0, 0, 1, 1);
    const int x0 = int(std::floor(bounds.x - margin)), y0 = int(std::floor(bounds.y - margin));
    const int w = std::clamp(int(std::ceil(bounds.maxX() + margin)) - x0, 1, 30000), h = std::clamp(int(std::ceil(bounds.maxY() + margin)) - y0, 1, 30000);
    auto image = std::make_shared<Image>(w, h);
    image->fill(shape.r, shape.g, shape.b, 255);
    layer.asset = Asset::make(image, layer.name);
    layer.transform = LayerTransform(Point(x0, y0), Size(w, h));
    layer.shape.reset();
    layer.shapeImage.reset();
    layer.text.reset();
    layer.textImage.reset();

    auto carry = layer.psdCarry ? std::make_shared<PsdLayerCarry>(*layer.psdCarry) : std::make_shared<PsdLayerCarry>();
    auto& blocks = carry->blocks;
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(), [](const PsdBlock& b) {
        return b.key == "vmsk" || b.key == "vsms" || b.key == "vstk" || b.key == "SoCo" || b.key == "vogk" || b.key == "vscg" || b.key == "GdFl" || b.key == "PtFl";
    }), blocks.end());
    int cw = 0, ch = 0;
    pathCanvas(document, cw, ch);
    VectorPath path = shape.path;
    path.disabled = false;
    blocks.push_back({"SoCo", authorSolidColour(shape.r, shape.g, shape.b)});
    blocks.push_back({"vsms", authorVectorMask(path, cw, ch)});
    VectorStroke stroke = shape.stroke;
    blocks.push_back({"vstk", authorVectorStroke(stroke, shape.fill)});
    // The blocks describe these pixels where they now are, so they are kept (and not moved) until they change.
    carry->placement = layer.transform;
    carry->contentHash = psdContentHash(image.get());
    layer.psdCarry = std::move(carry);
}

std::vector<uint8_t> authorPathResource(const VectorPath& path, int w, int h) {
    auto bytes = authorVectorMask(path, w, h);
    return std::vector<uint8_t>(bytes.begin() + 8, bytes.end());
}

std::optional<VectorPath> parsePathResource(const std::vector<uint8_t>& data, int w, int h) {
    std::vector<uint8_t> block(8, 0);
    block[3] = 3;
    block.insert(block.end(), data.begin(), data.end());
    return parseVectorMask(block, w, h);
}

namespace {
bool isPathResource(uint16_t id) { return id == kWorkPathId || (id >= 2000 && id <= 2997); }

/// The document's carry, made (for the document's own canvas) when it has none; copied so it can change.
std::shared_ptr<PsdDocumentCarry> mutableCarry(Document& document) {
    auto carry = document.psdCarry ? std::make_shared<PsdDocumentCarry>(*document.psdCarry) : std::make_shared<PsdDocumentCarry>();
    if (!document.psdCarry) { carry->width = document.width; carry->height = document.height; }
    return carry;
}
}

std::vector<DocumentPath> documentPaths(const Document& document) {
    std::vector<DocumentPath> out;
    if (!document.psdCarry) return out;
    int w = 0, h = 0;
    pathCanvas(document, w, h);
    for (const auto& r : document.psdCarry->resources) {
        if (!isPathResource(r.id)) continue;
        auto path = parsePathResource(r.data, w, h);
        if (!path) continue;
        out.push_back({r.id, r.id == kWorkPathId ? std::string("Work Path") : r.name, std::move(*path)});
    }
    // The Work Path first, then saved paths in the order they were made, as in Photoshop's panel.
    std::stable_sort(out.begin(), out.end(), [](const DocumentPath& a, const DocumentPath& b) { return (a.id == kWorkPathId) > (b.id == kWorkPathId); });
    return out;
}

std::optional<DocumentPath> documentPath(const Document& document, uint16_t id) {
    for (auto& p : documentPaths(document)) if (p.id == id) return p;
    return std::nullopt;
}

uint16_t setDocumentPath(Document& document, uint16_t id, const std::string& name, const VectorPath& path) {
    auto carry = mutableCarry(document);
    int w = 0, h = 0;
    pathCanvas(document, w, h);
    if (!document.psdCarry) { w = document.width; h = document.height; }
    if (id == 0) {
        id = 2000;
        for (auto& r : carry->resources) if (r.id >= 2000 && r.id <= 2997) id = std::max<uint16_t>(id, uint16_t(r.id + 1));
        if (id > 2997) id = 2997;
    }
    auto it = std::find_if(carry->resources.begin(), carry->resources.end(), [&](const PsdDocumentCarry::Resource& r) { return r.id == id; });
    PsdDocumentCarry::Resource resource{id, id == kWorkPathId ? std::string() : name, authorPathResource(path, w, h)};
    if (it != carry->resources.end()) { if (name.empty()) resource.name = it->name; *it = resource; }
    else carry->resources.push_back(resource);
    document.psdCarry = std::move(carry);
    return id;
}

void renameDocumentPath(Document& document, uint16_t id, const std::string& name) {
    if (!document.psdCarry || id == kWorkPathId) return;
    auto carry = mutableCarry(document);
    for (auto& r : carry->resources) if (r.id == id) r.name = name.substr(0, 255);
    document.psdCarry = std::move(carry);
}

void removeDocumentPath(Document& document, uint16_t id) {
    if (!document.psdCarry) return;
    auto carry = mutableCarry(document);
    carry->resources.erase(std::remove_if(carry->resources.begin(), carry->resources.end(), [&](const PsdDocumentCarry::Resource& r) { return r.id == id; }), carry->resources.end());
    document.psdCarry = std::move(carry);
}

namespace {
Point bezier(Point a, Point b, Point c, Point d, double t) {
    const double u = 1 - t;
    return {u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * c.x + t * t * t * d.x,
            u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * c.y + t * t * t * d.y};
}
Point lerp(Point a, Point b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
}

std::optional<PathHit> nearestPathSegment(const VectorPath& path, Point p) {
    std::optional<PathHit> best;
    for (int si = 0; si < int(path.subpaths.size()); si++) {
        const auto& s = path.subpaths[size_t(si)];
        const int n = int(s.knots.size());
        const int segments = s.closed ? n : n - 1;
        for (int seg = 0; seg < segments; seg++) {
            const auto& k0 = s.knots[size_t(seg)];
            const auto& k1 = s.knots[size_t((seg + 1) % n)];
            const Point a(k0.x, k0.y), b(k0.outX, k0.outY), c(k1.inX, k1.inY), d(k1.x, k1.y);
            // Sampled, then refined around the best sample.
            double bestT = 0, bestD = 1e300;
            for (int i = 0; i <= 64; i++) {
                const double t = i / 64.0;
                const Point q = bezier(a, b, c, d, t);
                const double dist = std::hypot(q.x - p.x, q.y - p.y);
                if (dist < bestD) { bestD = dist; bestT = t; }
            }
            double lo = std::max(0.0, bestT - 1 / 64.0), hi = std::min(1.0, bestT + 1 / 64.0);
            for (int i = 0; i < 30; i++) {
                const double m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
                const Point q1 = bezier(a, b, c, d, m1), q2 = bezier(a, b, c, d, m2);
                if (std::hypot(q1.x - p.x, q1.y - p.y) < std::hypot(q2.x - p.x, q2.y - p.y)) hi = m2; else lo = m1;
            }
            bestT = (lo + hi) / 2;
            const Point q = bezier(a, b, c, d, bestT);
            bestD = std::hypot(q.x - p.x, q.y - p.y);
            if (!best || bestD < best->distance) best = PathHit{si, seg, bestT, bestD};
        }
    }
    return best;
}

int insertAnchor(VectorPath& path, int subpath, int segment, double t) {
    auto& s = path.subpaths[size_t(subpath)];
    const int n = int(s.knots.size());
    auto& k0 = s.knots[size_t(segment)];
    auto& k1 = s.knots[size_t((segment + 1) % n)];
    const Point a(k0.x, k0.y), b(k0.outX, k0.outY), c(k1.inX, k1.inY), d(k1.x, k1.y);
    // de Casteljau: the two halves' control points.
    const Point ab = lerp(a, b, t), bc = lerp(b, c, t), cd = lerp(c, d, t);
    const Point abc = lerp(ab, bc, t), bcd = lerp(bc, cd, t), m = lerp(abc, bcd, t);
    k0.outX = ab.x; k0.outY = ab.y;
    k1.inX = cd.x; k1.inY = cd.y;
    const VectorPath::Knot added{abc.x, abc.y, m.x, m.y, bcd.x, bcd.y};
    s.knots.insert(s.knots.begin() + segment + 1, added);
    return segment + 1;
}

std::optional<std::pair<int, int>> nearestKnot(const VectorPath& path, Point p, double radius) {
    std::optional<std::pair<int, int>> best;
    double bestD = radius;
    for (int si = 0; si < int(path.subpaths.size()); si++)
        for (int ki = 0; ki < int(path.subpaths[size_t(si)].knots.size()); ki++) {
            const auto& k = path.subpaths[size_t(si)].knots[size_t(ki)];
            const double d = std::hypot(k.x - p.x, k.y - p.y);
            if (d <= bestD) { bestD = d; best = std::make_pair(si, ki); }
        }
    return best;
}

void removeAnchor(VectorPath& path, int subpath, int knot) {
    auto& knots = path.subpaths[size_t(subpath)].knots;
    knots.erase(knots.begin() + knot);
    if (knots.size() < 2) path.subpaths.erase(path.subpaths.begin() + subpath);
}

bool knotIsSmooth(const VectorPath::Knot& k) {
    const double ax = k.inX - k.x, ay = k.inY - k.y, bx = k.outX - k.x, by = k.outY - k.y;
    const double la = std::hypot(ax, ay), lb = std::hypot(bx, by);
    if (la < 1e-6 && lb < 1e-6) return false;   // a corner without handles
    if (la < 1e-6 || lb < 1e-6) return false;
    return (ax * bx + ay * by) / (la * lb) < -0.9999 ;   // opposite directions
}

// ---- Shapes ---------------------------------------------------------------------------------------------------

VectorPath rectanglePath(const Rect& box, double radius) {
    radius = std::clamp(radius, 0.0, std::min(box.width, box.height) / 2);
    const double l = box.x, t = box.y, r = box.maxX(), b = box.maxY();
    if (radius <= 0) return single({corner(l, t), corner(r, t), corner(r, b), corner(l, b)});
    const double k = radius * kKappa;
    // Each rounded corner is two knots; the straight edges run between them.
    return single({
        {l, t + radius, l, t + radius, l, t + radius - k},   // the left edge's top end, its out handle towards the corner
        {l + radius - k, t, l + radius, t, l + radius, t},
        {r - radius, t, r - radius, t, r - radius + k, t},
        {r, t + radius - k, r, t + radius, r, t + radius},
        {r, b - radius, r, b - radius, r, b - radius + k},
        {r - radius + k, b, r - radius, b, r - radius, b},
        {l + radius, b, l + radius, b, l + radius - k, b},
        {l, b - radius + k, l, b - radius, l, b - radius},
    });
}

VectorPath ellipsePath(const Rect& box) {
    const double cx = box.midX(), cy = box.midY(), rx = box.width / 2, ry = box.height / 2, kx = rx * kKappa, ky = ry * kKappa;
    return single({
        {cx - kx, cy - ry, cx, cy - ry, cx + kx, cy - ry},
        {cx + rx, cy - ky, cx + rx, cy, cx + rx, cy + ky},
        {cx + kx, cy + ry, cx, cy + ry, cx - kx, cy + ry},
        {cx - rx, cy + ky, cx - rx, cy, cx - rx, cy - ky},
    });
}

VectorPath polygonPath(const Rect& box, int sides, double starInset) {
    sides = std::clamp(sides, 3, 100);
    starInset = std::clamp(starInset, 0.0, 0.99);
    std::vector<VectorPath::Knot> k;
    const int n = starInset > 0 ? sides * 2 : sides;
    for (int i = 0; i < n; i++) {
        const double a = -M_PI / 2 + 2 * M_PI * i / n;
        const double f = starInset > 0 && (i % 2) ? 1 - starInset : 1;
        k.push_back(corner(box.midX() + std::cos(a) * box.width / 2 * f, box.midY() + std::sin(a) * box.height / 2 * f));
    }
    return single(std::move(k));
}

VectorPath linePath(Point a, Point b, double weight) {
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    const double nx = len > 0 ? -(b.y - a.y) / len * weight / 2 : 0, ny = len > 0 ? (b.x - a.x) / len * weight / 2 : weight / 2;
    return single({corner(a.x + nx, a.y + ny), corner(b.x + nx, b.y + ny), corner(b.x - nx, b.y - ny), corner(a.x - nx, a.y - ny)});
}

const std::vector<std::string>& customShapeNames() {
    static const std::vector<std::string> names{"Heart", "Star", "Arrow", "Speech Bubble", "Check Mark", "Lightning"};
    return names;
}

VectorPath customShapePath(const std::string& name, const Rect& box) {
    if (name == "Star") return polygonPath(box, 5, 0.5);
    if (name == "Arrow") return fromUnitPoints(box, {{0, 0.3}, {0.6, 0.3}, {0.6, 0}, {1, 0.5}, {0.6, 1}, {0.6, 0.7}, {0, 0.7}});
    if (name == "Check Mark") return fromUnitPoints(box, {{0, 0.55}, {0.15, 0.4}, {0.38, 0.62}, {0.85, 0.1}, {1, 0.25}, {0.38, 0.92}});
    if (name == "Lightning") return fromUnitPoints(box, {{0.55, 0}, {0.1, 0.58}, {0.45, 0.58}, {0.3, 1}, {0.9, 0.38}, {0.55, 0.38}, {0.75, 0}});
    auto at = [&](double u, double v) { return Point(box.x + u * box.width, box.y + v * box.height); };
    auto knot = [&](Point in, Point p, Point out) { return VectorPath::Knot{in.x, in.y, p.x, p.y, out.x, out.y}; };
    if (name == "Speech Bubble") {
        // A rounded body with a tail at the lower left.
        VectorPath body = rectanglePath(Rect(box.x, box.y, box.width, box.height * 0.78), std::min(box.width, box.height) * 0.18);
        auto& k = body.subpaths[0].knots;
        // Between the bottom-left pair of knots (indices 6 and 7 run along the bottom towards the left).
        const Point tail = at(0.12, 1.0), base0 = at(0.34, 0.78), base1 = at(0.2, 0.78);
        k.insert(k.begin() + 7, {corner(base0.x, base0.y), corner(tail.x, tail.y), corner(base1.x, base1.y)});
        return body;
    }
    // Heart: two lobes meeting at the top centre and the bottom point.
    return single({
        knot(at(0.5, 0.12), at(0.5, 0.25), at(0.5, 0.12)),
        knot(at(0.62, 0.0), at(0.78, 0.0), at(0.95, 0.0)),
        knot(at(1.02, 0.22), at(1.0, 0.33), at(0.98, 0.5)),
        knot(at(0.75, 0.72), at(0.5, 1.0), at(0.5, 1.0)),
        knot(at(0.5, 1.0), at(0.5, 1.0), at(0.25, 0.72)),
        knot(at(0.02, 0.5), at(0.0, 0.33), at(-0.02, 0.22)),
        knot(at(0.05, 0.0), at(0.22, 0.0), at(0.38, 0.0)),
    });
}

} // namespace compositor
