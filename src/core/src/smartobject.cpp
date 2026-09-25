// Smart objects: placement geometry, and Photoshop's placed-layer and linked-file structures. The layouts
// follow Patchy (MIT; src/psd/psd_smart_objects.cpp and docs/smart-objects.md there), pinned against Photoshop
// 2026: 'lnk2' elements are a u64 length, 'liFD' (embedded) / 'liFE' (external) / 'liFA', a version, the
// Pascal uuid, the Unicode file name, type and creator, a u64 data size, an optional open descriptor, the data,
// then version-dependent trailers, each element padded to four bytes; 'SoLd' is 'soLD', version 4 or 5, then a
// version-16 descriptor ('Idnt', 'placed', 'Trnf', 'nonAffineTransform', 'Sz  ', 'Rslt', 'warp', 'filterFX');
// 'PlLd' is 'plcL', version 3, the Pascal uuid, page, pages, antialias, type and the eight quad doubles.
#include "compositor/smartobject.h"
#include "compositor/uuid.h"
#include "psd/psd_descriptor.hpp"
#include <cmath>
#include <cstring>

namespace compositor {

namespace psd = patchy::psd;

const char* smartObjectLockDescription(SmartObjectInstance::Lock lock) {
    switch (lock) {
    case SmartObjectInstance::Lock::Warp: return "warped";
    case SmartObjectInstance::Lock::Perspective: return "placed in perspective or skewed";
    case SmartObjectInstance::Lock::Filters: return "carrying Smart Filters";
    case SmartObjectInstance::Lock::Unreadable: return "of a kind NekoPhoto cannot read yet";
    case SmartObjectInstance::Lock::Linked: return "linked to a file outside the document";
    case SmartObjectInstance::Lock::Legacy: return "stored in Photoshop's old placed-layer form only";
    default: return "editable";
    }
}

// ---- Geometry ----------------------------------------------------------------------------------------------

Point mapThroughTransform(const LayerTransform& t, int w, int h, double x, double y) {
    if (w <= 0 || h <= 0) return t.origin;
    double u = x / w, v = y / h;
    if (t.flipX) u = 1 - u;
    if (t.flipY) v = 1 - v;
    const Point c = t.center();
    const double px = t.origin.x + u * t.size.width - c.x, py = t.origin.y + v * t.size.height - c.y;
    const double a = t.rotation * M_PI / 180, cs = std::cos(a), sn = std::sin(a);
    return {c.x + px * cs - py * sn, c.y + px * sn + py * cs};
}

std::optional<LayerTransform> transformForQuad(const std::array<double, 8>& q, int w, int h) {
    if (w <= 0 || h <= 0) return std::nullopt;
    const double ux = q[2] - q[0], uy = q[3] - q[1], vx = q[6] - q[0], vy = q[7] - q[1];
    const double lu = std::hypot(ux, uy), lv = std::hypot(vx, vy);
    if (lu < 1e-6 || lv < 1e-6) return std::nullopt;
    // A rectangle: the fourth corner closes the parallelogram and the sides are square to each other.
    if (std::abs(q[4] - (q[0] + ux + vx)) > 0.01 || std::abs(q[5] - (q[1] + uy + vy)) > 0.01) return std::nullopt;
    if (std::abs(ux * vx + uy * vy) / (lu * lv) > 1e-4) return std::nullopt;
    LayerTransform t;
    t.size = Size(lu, lv);
    const Point centre{(q[0] + q[4]) / 2, (q[1] + q[5]) / 2};
    t.origin = Point(centre.x - lu / 2, centre.y - lv / 2);
    t.rotation = std::atan2(uy, ux) * 180 / M_PI;
    if (std::abs(t.rotation) < 1e-9) t.rotation = 0;
    // v turned clockwise from u (y down) is the unflipped layout; the other way round is flipped vertically.
    t.flipY = ux * vy - uy * vx < 0;
    return t;
}

std::array<double, 8> moveQuad(const std::array<double, 8>& quad, const LayerTransform& before, int w0, int h0,
                               const LayerTransform& after, int w1, int h1) {
    // Each corner back to raster coordinates under `before` (inverting it), then through `after`.
    std::array<double, 8> out{};
    const Point c = before.center();
    const double a = -before.rotation * M_PI / 180, cs = std::cos(a), sn = std::sin(a);
    for (int i = 0; i < 4; i++) {
        const double dx = quad[size_t(i * 2)] - c.x, dy = quad[size_t(i * 2 + 1)] - c.y;
        const double px = c.x + dx * cs - dy * sn - before.origin.x, py = c.y + dx * sn + dy * cs - before.origin.y;
        double u = before.size.width > 0 ? px / before.size.width : 0, v = before.size.height > 0 ? py / before.size.height : 0;
        if (before.flipX) u = 1 - u;
        if (before.flipY) v = 1 - v;
        (void)w0; (void)h0;
        const Point p = mapThroughTransform(after, w1, h1, u * w1, v * h1);
        out[size_t(i * 2)] = p.x;
        out[size_t(i * 2 + 1)] = p.y;
    }
    return out;
}

// ---- Photoshop -----------------------------------------------------------------------------------------------

namespace {

std::string pascal(psd::BigEndianReader& r) { const size_t n = r.read_u8(); auto s = r.read_span(n); return std::string(s.begin(), s.end()); }
std::string four(psd::BigEndianReader& r) { auto s = r.read_span(4); return std::string(s.begin(), s.end()); }

std::optional<std::array<double, 8>> quadOf(const psd::DescriptorValue* v) {
    if (!v || v->type != psd::DescriptorValue::Type::List || v->list_value.size() != 8) return std::nullopt;
    std::array<double, 8> q{};
    for (size_t i = 0; i < 8; i++) {
        const auto& item = v->list_value[i];
        if (item.type == psd::DescriptorValue::Type::Double || item.type == psd::DescriptorValue::Type::UnitFloat) q[i] = item.double_value;
        else if (item.type == psd::DescriptorValue::Type::Integer) q[i] = item.integer_value;
        else return std::nullopt;
    }
    return q;
}

} // namespace

std::vector<SmartObjectSource> parsePsdLinkBlock(const std::vector<uint8_t>& payload) {
    std::vector<SmartObjectSource> out;
    try {
        psd::BigEndianReader r(payload);
        while (r.remaining() >= 8) {
            const uint64_t length = r.read_u64();
            if (length > r.remaining()) return {};
            const size_t start = r.position(), end = start + size_t(length);
            SmartObjectSource s;
            const std::string kind = four(r);
            if (kind != "liFD" && kind != "liFE" && kind != "liFA") return {};
            s.kind = kind == "liFD" ? SmartObjectSource::Kind::Embedded : SmartObjectSource::Kind::Linked;
            const uint32_t version = r.read_u32();
            if (version < 1 || version > 32) return {};
            s.id = pascal(r);
            s.fileName = psd::read_descriptor_unicode_string(r);
            s.fileType = four(r);
            (void)four(r);   // creator
            const uint64_t dataSize = r.read_u64();
            if (r.read_u8() != 0) { (void)r.read_u32(); (void)psd::read_descriptor(r); }
            if (s.kind == SmartObjectSource::Kind::Embedded) {
                if (r.position() > end || dataSize > end - r.position()) return {};
                auto data = r.read_span(size_t(dataSize));
                s.bytes = std::make_shared<const std::vector<uint8_t>>(data.begin(), data.end());
            } else if (kind == "liFE" && r.position() + 4 <= end) {
                try {
                    (void)r.read_u32();
                    const auto link = psd::read_descriptor(r);
                    for (const char* key : {"originalPath", "fullPath", "relPath"})
                        if (auto v = psd::descriptor_value(link, key); v && v->type == psd::DescriptorValue::Type::String && !v->string_value.empty()) { s.linkedPath = v->string_value; break; }
                } catch (std::exception&) {}
            }
            if (r.position() < end) r.skip(end - r.position());
            r.skip(std::min<size_t>(r.remaining(), size_t((4 - length % 4) % 4)));
            s.psdElement = std::make_shared<const std::vector<uint8_t>>(payload.begin() + long(start - 8), payload.begin() + long(r.position()));
            out.push_back(std::move(s));
        }
    } catch (std::exception&) { return {}; }
    return out;
}

std::optional<PsdPlacement> parsePsdPlacement(const std::string& key, const std::vector<uint8_t>& payload) {
    try {
        psd::BigEndianReader r(payload);
        PsdPlacement p;
        if (key == "SoLd" || key == "SoLE") {
            if (four(r) != "soLD") return std::nullopt;
            const uint32_t version = r.read_u32();
            if (version != 4 && version != 5) return std::nullopt;
            (void)r.read_u32();
            const psd::DescriptorObject d = psd::read_descriptor(r);
            auto text = [&](const char* k) { auto v = psd::descriptor_value(d, k); return v && v->type == psd::DescriptorValue::Type::String ? v->string_value : std::string(); };
            p.sourceId = text("Idnt");
            p.placedId = text("placed");
            auto quad = quadOf(psd::descriptor_value(d, "Trnf"));
            if (p.sourceId.empty() || !quad) return std::nullopt;
            p.quad = *quad;
            if (auto na = quadOf(psd::descriptor_value(d, "nonAffineTransform"))) {
                bool differs = false;
                for (size_t i = 0; i < 8; i++) differs |= std::abs((*na)[i] - p.quad[i]) > 1e-6;
                if (differs) p.nonAffine = *na;
            }
            if (auto size = psd::descriptor_object(d, "Sz  ")) { p.width = psd::descriptor_number(*size, "Wdth", 0); p.height = psd::descriptor_number(*size, "Hght", 0); }
            p.resolution = psd::descriptor_number(d, "Rslt", 72);
            if (auto warp = psd::descriptor_object(d, "warp")) {
                auto style = psd::descriptor_value(*warp, "warpStyle");
                p.warped = (style && style->type == psd::DescriptorValue::Type::Enum && style->enum_value != "warpNone")
                    || psd::descriptor_value(*warp, "customEnvelopeWarp") != nullptr
                    || psd::descriptor_number(*warp, "warpValue", 0) != 0 || psd::descriptor_number(*warp, "warpPerspective", 0) != 0;
            }
            p.warped |= psd::descriptor_value(d, "quiltWarp") != nullptr;
            p.filtered = psd::descriptor_value(d, "filterFX") != nullptr;
            return p;
        }
        if (key == "PlLd" || key == "plLd") {
            if (four(r) != "plcL" || r.read_u32() != 3) return std::nullopt;
            p.sourceId = pascal(r);
            for (int i = 0; i < 4; i++) (void)r.read_u32();
            for (auto& v : p.quad) v = psd::read_f64(r);
            return p;
        }
    } catch (std::exception&) {}
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> patchPsdPlacement(const std::string& key, const std::vector<uint8_t>& payload,
                                                      const std::array<double, 8>& quad, const std::string& placedId) {
    try {
        if (key == "SoLd" || key == "SoLE") {
            psd::BigEndianReader r(payload);
            if (four(r) != "soLD") return std::nullopt;
            const uint32_t version = r.read_u32(), descriptorVersion = r.read_u32();
            psd::DescriptorObject d = psd::read_descriptor(r);
            const size_t tail = r.position();
            auto trnf = const_cast<psd::DescriptorValue*>(psd::descriptor_value(d, "Trnf"));
            auto old = quadOf(trnf);
            if (!old) return std::nullopt;
            for (size_t i = 0; i < 8; i++) trnf->list_value[i].double_value = quad[i];
            // A perspective quad moves by the same per-corner delta (exact for translations, which is all a locked
            // perspective placement is allowed).
            if (auto na = const_cast<psd::DescriptorValue*>(psd::descriptor_value(d, "nonAffineTransform")); na && quadOf(na))
                for (size_t i = 0; i < 8; i++) na->list_value[i].double_value += quad[i] - (*old)[i];
            if (!placedId.empty())
                if (auto placed = const_cast<psd::DescriptorValue*>(psd::descriptor_value(d, "placed")); placed && placed->type == psd::DescriptorValue::Type::String)
                    placed->string_value = placedId;
            psd::BigEndianWriter w;
            for (char c : std::string("soLD")) w.write_u8(uint8_t(c));
            w.write_u32(version);
            w.write_u32(descriptorVersion);
            psd::write_descriptor(w, d);
            w.write_bytes(std::span<const uint8_t>(payload.data() + tail, payload.size() - tail));
            return w.bytes();
        }
        if (key == "PlLd" || key == "plLd") {
            psd::BigEndianReader r(payload);
            if (four(r) != "plcL" || r.read_u32() != 3) return std::nullopt;
            (void)pascal(r);
            for (int i = 0; i < 4; i++) (void)r.read_u32();
            const size_t at = r.position();
            if (at + 64 > payload.size()) return std::nullopt;
            std::vector<uint8_t> out = payload;
            psd::BigEndianWriter w;
            for (double v : quad) psd::write_f64(w, v);
            std::memcpy(out.data() + at, w.bytes().data(), 64);
            return out;
        }
    } catch (std::exception&) {}
    return std::nullopt;
}

std::string newSmartObjectId() {
    std::string id = makeUuid();
    for (auto& c : id) c = char(std::tolower(static_cast<unsigned char>(c)));
    return id;
}

std::vector<uint8_t> authorPsdPlacement(const std::string& sourceId, const std::string& placedId, const std::array<double, 8>& quad,
                                        double width, double height, double resolution) {
    using V = psd::DescriptorValue;
    auto text = [](const std::string& v) { V d; d.type = V::Type::String; d.string_value = v; return d; };
    auto integer = [](int32_t v) { V d; d.type = V::Type::Integer; d.integer_value = v; return d; };
    auto number = [](double v) { V d; d.type = V::Type::Double; d.double_value = v; return d; };
    auto object = [](const char* cls, bool longForm) { V d; d.type = V::Type::Object; d.object_value = std::make_shared<psd::DescriptorObject>(); d.object_value->class_id = cls; d.object_value->class_id_long_form = longForm; return d; };
    auto enumeration = [](const char* type, bool typeLong, const char* value, bool valueLong) {
        V d; d.type = V::Type::Enum; d.enum_type = type; d.enum_type_long_form = typeLong; d.enum_value = value; d.enum_value_long_form = valueLong; return d;
    };
    auto add = [](psd::DescriptorObject& o, const std::string& key, bool longForm, V v) { o.key_order.push_back({key, longForm}); o.values.emplace(key, std::move(v)); };
    auto quadList = [&] { V d; d.type = V::Type::List; for (double c : quad) d.list_value.push_back(number(c)); return d; };
    auto rational = [&] { V r = object("null", false); add(*r.object_value, "numerator", true, integer(0)); add(*r.object_value, "denominator", true, integer(600)); return r; };
    psd::DescriptorObject root;
    root.class_id = "null";
    add(root, "Idnt", false, text(sourceId));
    add(root, "placed", true, text(placedId));
    add(root, "PgNm", false, integer(1));
    add(root, "totalPages", true, integer(1));
    add(root, "Crop", false, integer(1));
    add(root, "frameStep", true, rational());
    add(root, "duration", true, rational());
    add(root, "frameCount", true, integer(1));
    add(root, "Annt", false, integer(16));
    add(root, "Type", false, integer(2));
    add(root, "Trnf", false, quadList());
    add(root, "nonAffineTransform", true, quadList());
    V warp = object("warp", true);
    add(*warp.object_value, "warpStyle", true, enumeration("warpStyle", true, "warpNone", true));
    add(*warp.object_value, "warpValue", true, number(0));
    add(*warp.object_value, "warpPerspective", true, number(0));
    add(*warp.object_value, "warpPerspectiveOther", true, number(0));
    add(*warp.object_value, "warpRotate", true, enumeration("Ornt", false, "Hrzn", false));
    V bounds = object("classFloatRect", true);
    add(*bounds.object_value, "Top ", false, number(0));
    add(*bounds.object_value, "Left", false, number(0));
    add(*bounds.object_value, "Btom", false, number(height));
    add(*bounds.object_value, "Rght", false, number(width));
    add(*warp.object_value, "bounds", true, bounds);
    add(*warp.object_value, "uOrder", true, integer(4));
    add(*warp.object_value, "vOrder", true, integer(4));
    add(root, "warp", true, warp);
    V size = object("Pnt ", false);
    add(*size.object_value, "Wdth", false, number(width));
    add(*size.object_value, "Hght", false, number(height));
    add(root, "Sz  ", false, size);
    V rslt; rslt.type = V::Type::UnitFloat; rslt.unit = "#Rsl"; rslt.double_value = resolution;
    add(root, "Rslt", false, rslt);
    add(root, "comp", false, integer(-1));
    V comp = object("null", false);
    add(*comp.object_value, "compID", true, integer(-1));
    add(*comp.object_value, "originalCompID", true, integer(-1));
    add(root, "compInfo", true, comp);
    V clmg = object("ClMg", false);
    add(*clmg.object_value, "placedLayerOCIOConversion", true, enumeration("placedLayerOCIOConversion", true, "placedLayerOCIOConvertEmbedded", true));
    add(root, "ClMg", false, clmg);
    psd::BigEndianWriter w;
    for (char c : std::string("soLD")) w.write_u8(uint8_t(c));
    w.write_u32(4);
    w.write_u32(16);
    psd::write_descriptor(w, root);
    while (w.bytes().size() % 4) w.write_u8(0);
    return w.bytes();
}

std::vector<uint8_t> psdEmbeddedElement(const SmartObjectSource& s) {
    psd::BigEndianWriter body;
    for (char c : std::string("liFD")) body.write_u8(uint8_t(c));
    body.write_u32(7);
    body.write_u8(uint8_t(std::min<size_t>(s.id.size(), 255)));
    for (size_t i = 0; i < std::min<size_t>(s.id.size(), 255); i++) body.write_u8(uint8_t(s.id[i]));
    psd::write_descriptor_unicode_string(body, s.fileName);
    auto ostype = [&](const std::string& t) { for (size_t i = 0; i < 4; i++) body.write_u8(uint8_t(i < t.size() ? t[i] : ' ')); };
    ostype(s.fileType);
    ostype(s.fileType == "8BPB" || s.fileType == "8BPS" ? "8BIM" : "    ");
    body.write_u64(s.bytes ? s.bytes->size() : 0);
    body.write_u8(0);   // no open descriptor
    if (s.bytes) body.write_bytes(*s.bytes);
    body.write_u32(0);   // child document id: empty
    psd::write_f64(body, 0);
    body.write_u8(0);    // not locked
    psd::BigEndianWriter element;
    element.write_u64(body.bytes().size());
    element.write_bytes(body.bytes());
    for (size_t n = body.bytes().size(); n % 4; n++) element.write_u8(0);
    return element.bytes();
}

std::optional<std::vector<uint8_t>> repointPsdPlacement(const std::string& key, const std::vector<uint8_t>& payload, const std::array<double, 8>& quad,
                                                        const std::string& sourceId, double width, double height) {
    try {
        if (key == "SoLd" || key == "SoLE") {
            psd::BigEndianReader r(payload);
            if (four(r) != "soLD") return std::nullopt;
            const uint32_t version = r.read_u32(), descriptorVersion = r.read_u32();
            psd::DescriptorObject d = psd::read_descriptor(r);
            const size_t tail = r.position();
            auto set = [&](psd::DescriptorObject& o, const char* k, double v) {
                if (auto p = const_cast<psd::DescriptorValue*>(psd::descriptor_value(o, k)); p && (p->type == psd::DescriptorValue::Type::Double || p->type == psd::DescriptorValue::Type::UnitFloat)) p->double_value = v;
            };
            if (auto id = const_cast<psd::DescriptorValue*>(psd::descriptor_value(d, "Idnt")); id && id->type == psd::DescriptorValue::Type::String) id->string_value = sourceId;
            for (const char* k : {"Trnf", "nonAffineTransform"})
                if (auto q = const_cast<psd::DescriptorValue*>(psd::descriptor_value(d, k)); q && quadOf(q))
                    for (size_t i = 0; i < 8; i++) q->list_value[i].double_value = quad[i];
            if (auto size = const_cast<psd::DescriptorObject*>(psd::descriptor_object(d, "Sz  "))) { set(*size, "Wdth", width); set(*size, "Hght", height); }
            if (auto warp = const_cast<psd::DescriptorObject*>(psd::descriptor_object(d, "warp")))
                if (auto bounds = const_cast<psd::DescriptorObject*>(psd::descriptor_object(*warp, "bounds"))) { set(*bounds, "Top ", 0); set(*bounds, "Left", 0); set(*bounds, "Btom", height); set(*bounds, "Rght", width); }
            psd::BigEndianWriter w;
            for (char c : std::string("soLD")) w.write_u8(uint8_t(c));
            w.write_u32(version);
            w.write_u32(descriptorVersion);
            psd::write_descriptor(w, d);
            w.write_bytes(std::span<const uint8_t>(payload.data() + tail, payload.size() - tail));
            return w.bytes();
        }
        if (key == "PlLd" || key == "plLd") {
            // The old form names the source too: rebuilt with the new id, the tail kept.
            psd::BigEndianReader r(payload);
            if (four(r) != "plcL" || r.read_u32() != 3) return std::nullopt;
            (void)pascal(r);
            uint32_t fields[4];
            for (auto& f : fields) f = r.read_u32();
            for (int i = 0; i < 8; i++) (void)psd::read_f64(r);
            const size_t tail = r.position();
            psd::BigEndianWriter w;
            for (char c : std::string("plcL")) w.write_u8(uint8_t(c));
            w.write_u32(3);
            w.write_u8(uint8_t(std::min<size_t>(sourceId.size(), 255)));
            for (size_t i = 0; i < std::min<size_t>(sourceId.size(), 255); i++) w.write_u8(uint8_t(sourceId[i]));
            for (uint32_t f : fields) w.write_u32(f);
            for (double v : quad) psd::write_f64(w, v);
            w.write_bytes(std::span<const uint8_t>(payload.data() + tail, payload.size() - tail));
            return w.bytes();
        }
    } catch (std::exception&) {}
    return std::nullopt;
}

// ---- Project package ------------------------------------------------------------------------------------------

std::vector<uint8_t> serializeSmartObjectSource(const SmartObjectSource& s) {
    psd::BigEndianWriter w;
    for (char c : std::string("NPSS")) w.write_u8(uint8_t(c));
    w.write_u32(2);
    auto str = [&](const std::string& v) { w.write_u32(uint32_t(v.size())); w.write_bytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(v.data()), v.size())); };
    str(s.id);
    w.write_u32(uint32_t(s.kind));
    str(s.fileName); str(s.fileType); str(s.linkedPath);
    w.write_u32(uint32_t(s.width)); w.write_u32(uint32_t(s.height));
    psd::write_f64(w, s.resolution);
    w.write_u64(s.bytes ? s.bytes->size() : 0);
    if (s.bytes) w.write_bytes(*s.bytes);
    str(s.psdBlock);
    w.write_u64(s.psdElement ? s.psdElement->size() : 0);
    if (s.psdElement) w.write_bytes(*s.psdElement);
    return w.bytes();
}

std::optional<SmartObjectSource> parseSmartObjectSource(const std::vector<uint8_t>& bytes) {
    try {
        psd::BigEndianReader r(bytes);
        if (four(r) != "NPSS") return std::nullopt;
        const uint32_t version = r.read_u32();
        if (version != 1 && version != 2) return std::nullopt;
        auto str = [&]() { const uint32_t n = r.read_u32(); auto v = r.read_span(n); return std::string(v.begin(), v.end()); };
        SmartObjectSource s;
        s.id = str();
        const uint32_t kind = r.read_u32();
        if (kind > 1) return std::nullopt;
        s.kind = SmartObjectSource::Kind(kind);
        s.fileName = str(); s.fileType = str(); s.linkedPath = str();
        s.width = int(r.read_u32()); s.height = int(r.read_u32());
        s.resolution = psd::read_f64(r);
        const uint64_t n = r.read_u64();
        if (n > r.remaining()) return std::nullopt;
        if (n) { auto v = r.read_span(size_t(n)); s.bytes = std::make_shared<const std::vector<uint8_t>>(v.begin(), v.end()); }
        if (version >= 2) {
            s.psdBlock = str();
            const uint64_t e = r.read_u64();
            if (e > r.remaining()) return std::nullopt;
            if (e) { auto v = r.read_span(size_t(e)); s.psdElement = std::make_shared<const std::vector<uint8_t>>(v.begin(), v.end()); }
        }
        if (r.remaining() || s.id.empty()) return std::nullopt;
        return s;
    } catch (std::exception&) { return std::nullopt; }
}

std::vector<uint8_t> serializeSmartObjectInstance(const SmartObjectInstance& s) {
    psd::BigEndianWriter w;
    for (char c : std::string("NPSO")) w.write_u8(uint8_t(c));
    w.write_u32(1);
    auto str = [&](const std::string& v) { w.write_u32(uint32_t(v.size())); w.write_bytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(v.data()), v.size())); };
    str(s.sourceId);
    for (double v : s.quad) psd::write_f64(w, v);
    w.write_u32(uint32_t(s.lock));
    str(s.placedId);
    w.write_u32(uint32_t(s.psdBlocks.size()));
    for (auto& b : s.psdBlocks) { str(b.key); w.write_u64(b.data.size()); w.write_bytes(b.data); }
    const LayerTransform& t = s.placedTransform;
    for (double v : {t.origin.x, t.origin.y, t.size.width, t.size.height, t.rotation}) psd::write_f64(w, v);
    w.write_u8(uint8_t((t.flipX ? 1 : 0) | (t.flipY ? 2 : 0)));
    w.write_u32(uint32_t(s.placedWidth));
    w.write_u32(uint32_t(s.placedHeight));
    return w.bytes();
}

std::optional<SmartObjectInstance> parseSmartObjectInstance(const std::vector<uint8_t>& bytes) {
    try {
        psd::BigEndianReader r(bytes);
        if (four(r) != "NPSO" || r.read_u32() != 1) return std::nullopt;
        auto str = [&]() { const uint32_t n = r.read_u32(); auto s = r.read_span(n); return std::string(s.begin(), s.end()); };
        SmartObjectInstance s;
        s.sourceId = str();
        for (auto& v : s.quad) v = psd::read_f64(r);
        const uint32_t lock = r.read_u32();
        if (lock > uint32_t(SmartObjectInstance::Lock::Legacy)) return std::nullopt;
        s.lock = SmartObjectInstance::Lock(lock);
        s.placedId = str();
        const uint32_t count = r.read_u32();
        if (count > 16) return std::nullopt;
        for (uint32_t i = 0; i < count; i++) {
            PsdBlock b;
            b.key = str();
            const uint64_t n = r.read_u64();
            if (b.key.size() != 4 || n > r.remaining()) return std::nullopt;
            b.data = r.read_bytes(size_t(n));
            s.psdBlocks.push_back(std::move(b));
        }
        LayerTransform& t = s.placedTransform;
        t.origin.x = psd::read_f64(r); t.origin.y = psd::read_f64(r); t.size.width = psd::read_f64(r); t.size.height = psd::read_f64(r); t.rotation = psd::read_f64(r);
        const uint8_t flips = r.read_u8();
        t.flipX = flips & 1; t.flipY = flips & 2;
        s.placedWidth = int(r.read_u32()); s.placedHeight = int(r.read_u32());
        if (r.remaining()) return std::nullopt;
        return s;
    } catch (std::exception&) { return std::nullopt; }
}

} // namespace compositor
