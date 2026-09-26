// Smart Filters: reading the SoLd 'filterFX' stack and the document's 'FEid' / 'FXid' cache, writing a fresh cache
// record, and drawing an instance through its stack. The descriptor keys, ranges and cache layout follow Patchy's
// psd_smart_objects.cpp and psd_filter_effects.cpp (MIT, src/third_party/patchy_psd/README.md); the kernels live in
// smartfilter_render.cpp.
#include "compositor/smartfilter.h"
#include "psd/psd_descriptor.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace compositor {

namespace psd = patchy::psd;
using V = psd::DescriptorValue;

namespace {

const V* either(const psd::DescriptorObject& o, const char* a, const char* b) {
    const V* v = psd::descriptor_value(o, a);
    return v ? v : psd::descriptor_value(o, b);
}
bool boolean(const psd::DescriptorObject& o, const char* k, bool& out) {
    const V* v = psd::descriptor_value(o, k);
    if (!v || v->type != V::Type::Bool) return false;
    out = v->bool_value;
    return true;
}
bool unitIn(const V* v, const char* unit, double lo, double hi, double& out) {
    if (!v || v->type != V::Type::UnitFloat || v->unit != unit || !std::isfinite(v->double_value) || v->double_value < lo || v->double_value > hi) return false;
    out = v->double_value;
    return true;
}
bool intIn(const V* v, int32_t lo, int32_t hi, int32_t& out) {
    if (!v || v->type != V::Type::Integer || v->integer_value < lo || v->integer_value > hi) return false;
    out = v->integer_value;
    return true;
}
bool rgb(const psd::DescriptorObject& o, const char* k) {
    const psd::DescriptorObject* c = psd::descriptor_object(o, k);
    if (!c || c->class_id != "RGBC") return false;
    for (const char* ch : {"Rd  ", "Grn ", "Bl  "}) {
        const V* v = psd::descriptor_value(*c, ch);
        if (!v || (v->type != V::Type::Double && v->type != V::Type::Integer)) return false;
    }
    return true;
}
bool blendOf(const std::string& name, BlendMode& out) {
    static const std::pair<const char*, BlendMode> modes[] = {
        {"normal", BlendMode::Normal}, {"multiply", BlendMode::Multiply}, {"screen", BlendMode::Screen}, {"overlay", BlendMode::Overlay},
        {"darken", BlendMode::Darken}, {"lighten", BlendMode::Lighten}, {"difference", BlendMode::Difference},
        {"colorDodge", BlendMode::ColorDodge}, {"colorBurn", BlendMode::ColorBurn}, {"hue", BlendMode::Hue},
        {"saturation", BlendMode::Saturation}, {"color", BlendMode::Color}, {"luminosity", BlendMode::Luminosity}};
    for (auto& [n, m] : modes) if (name == n) { out = m; return true; }
    return false;
}

// The filter itself ('Fltr' with the entry's 'filterID'); monostate when it is not one drawn here.
SmartFilterParameters parametersOf(const psd::DescriptorObject& f, uint32_t id) {
    using namespace smartfilter;
    const std::string& c = f.class_id;
    double d = 0, e = 0;
    int32_t i = 0, j = 0, k = 0;
    if (c == "GsnB" && id == 0x47736e42u && unitIn(either(f, "Rds ", "Rds"), "#Pxl", 0.1, 1000, d)) return GaussianBlur{d};
    if (c == "HghP" && id == 0x48676850u && unitIn(either(f, "Rds ", "Rds"), "#Pxl", 0.1, 1000, d)) return HighPass{d};
    if (c == "Mdn " && id == 0x4d646e20u && unitIn(either(f, "Rds ", "Rds"), "#Pxl", 1, 500, d)) return Median{d};
    if (c == "DstS" && id == 0x44737453u && intIn(either(f, "Rds ", "Rds"), 1, 500, i) && intIn(either(f, "Thsh", "threshold"), 0, 255, j))
        return DustAndScratches{i, j};
    if (c == "surfaceBlur" && f.class_id_long_form && id == 854u && unitIn(either(f, "Rds ", "Rds"), "#Pxl", 1, 100, d)
        && intIn(either(f, "Thsh", "threshold"), 2, 255, i))
        return SurfaceBlur{d, i};
    if (c == "UnsM" && id == 0x556e734du && unitIn(either(f, "Amnt", "amount"), "#Prc", 1, 500, d) && unitIn(either(f, "Rds ", "Rds"), "#Pxl", 0.1, 1000, e)
        && intIn(either(f, "Thsh", "threshold"), 0, 255, i))
        return UnsharpMask{d, e, i};
    if (c == "MtnB" && id == 0x4d746e42u && intIn(either(f, "Angl", "angle"), -360, 360, i) && unitIn(either(f, "Dstn", "distance"), "#Pxl", 1, 999, d)
        && std::floor(d) == d)
        return MotionBlur{i, int32_t(d)};
    if (c == "PlsW" && id == 0x506c7357u && intIn(either(f, "Hghl", "highlights"), 0, 20, i) && intIn(either(f, "Dtl ", "detail"), 1, 15, j)
        && intIn(either(f, "Smth", "smoothness"), 1, 15, k))
        return PlasticWrap{i, j, k};
    if (c == "Msc " && id == 0x4d736320u && unitIn(either(f, "ClSz", "cellSize"), "#Pxl", 2, 200, d) && std::floor(d) == d) return Mosaic{int32_t(d)};
    if (c == "Embs" && id == 0x456d6273u && intIn(either(f, "Angl", "angle"), -360, 360, i) && intIn(either(f, "Hght", "height"), 1, 100, j)
        && intIn(either(f, "Amnt", "amount"), 1, 500, k))
        return Emboss{i, j, k};
    if (c == "boxblur" && f.class_id_long_form && id == 843u && unitIn(either(f, "Rds ", "Rds"), "#Pxl", 1, 2000, d)) return BoxBlur{d};
    if (c == "RdlB" && id == 0x52646c42u && intIn(either(f, "Amnt", "amount"), 1, 100, i)) {
        const V* method = either(f, "BlrM", "blurMethod");
        const V* quality = either(f, "BlrQ", "blurQuality");
        if (method && method->type == V::Type::Enum && method->enum_value == "Spn " && quality && quality->type == V::Type::Enum) {
            const std::string& q = quality->enum_value;
            if (q == "Drft" || q == "Gd  " || q == "Bst ") return RadialBlur{i, q == "Drft" ? 8 : q == "Gd  " ? 16 : 32};
        }
    }
    if (c == "AdNs" && id == 0x41644e73u && unitIn(either(f, "Nose", "noise"), "#Prc", 0.1, 400, d) && intIn(either(f, "FlRs", "FlRs"), 0, 999999999, i)) {
        const V* dist = either(f, "Dstr", "distribution");
        const V* mono = either(f, "Mnch", "monochromatic");
        if (dist && dist->type == V::Type::Enum && (dist->enum_value == "Unfr" || dist->enum_value == "Gsn ") && mono && mono->type == V::Type::Bool)
            return AddNoise{d, dist->enum_value == "Gsn ", mono->bool_value, i};
    }
    return std::monostate{};
}

int32_t i32(uint32_t v) { return int32_t(v); }

// One FEid / FXid record body, read as far as the canvas and the mask.
struct Record { std::string placedId; bool ok = false; SmartFilterCache cache; };

Record readRecord(std::span<const uint8_t> body) {
    Record rec;
    try {
        psd::BigEndianReader r(body);
        const uint8_t idLength = r.read_u8();
        auto id = r.read_bytes(idLength);
        rec.placedId.assign(id.begin(), id.end());
        if (r.read_u32() != 1) return rec;
        const uint64_t cacheLength = r.read_u64();
        if (cacheLength > r.remaining()) return rec;
        const size_t cacheAt = r.position();
        {
            psd::BigEndianReader c(body.subspan(cacheAt, size_t(cacheLength)));
            const int32_t top = i32(c.read_u32()), left = i32(c.read_u32()), bottom = i32(c.read_u32()), right = i32(c.read_u32());
            const uint32_t depth = c.read_u32(), channels = c.read_u32();
            if (right <= left || bottom <= top || depth != 8 || channels > 64) return rec;
            rec.cache.canvas = {left, top, right - left, bottom - top};
            for (uint32_t slot = 0; slot < channels + 2; slot++) {
                const uint32_t written = c.read_u32();
                if (!written) continue;
                if (written != 1) return rec;
                const uint64_t n = c.read_u64();
                if (n > c.remaining()) return rec;
                c.skip(size_t(n));
            }
            if (c.remaining()) return rec;
        }
        r.skip(size_t(cacheLength));
        if (r.remaining() == 0) { rec.ok = true; return rec; }
        const uint8_t present = r.read_u8();
        if (present == 0) { rec.ok = r.remaining() == 0; return rec; }
        if (present != 1) return rec;
        const int32_t top = i32(r.read_u32()), left = i32(r.read_u32()), bottom = i32(r.read_u32()), right = i32(r.read_u32());
        const uint64_t maskLength = r.read_u64();
        if (right <= left || bottom <= top || maskLength != r.remaining()) return rec;
        const int w = right - left, h = bottom - top;
        if (w > maxImageSide || h > maxImageSide) return rec;
        auto mask = std::make_shared<GrayImage>(w, h);
        const uint16_t compression = r.read_u16();
        if (compression == 0) {
            if (r.remaining() != size_t(w) * size_t(h)) return rec;
            for (int y = 0; y < h; y++) { auto row = r.read_bytes(size_t(w)); std::memcpy(mask->row(y), row.data(), size_t(w)); }
        } else if (compression == 1) {
            std::vector<uint32_t> lengths(static_cast<size_t>(h));
            for (auto& n : lengths) n = r.read_u32();
            for (int y = 0; y < h; y++) {
                if (lengths[size_t(y)] > r.remaining()) return rec;
                auto encoded = r.read_bytes(lengths[size_t(y)]);
                size_t used = 0;
                auto row = psd::decode_packbits(encoded, size_t(w), &used);
                if (used != encoded.size() || row.size() != size_t(w)) return rec;
                std::memcpy(mask->row(y), row.data(), size_t(w));
            }
            if (r.remaining()) return rec;
        } else return rec;
        rec.cache.mask = mask;
        rec.cache.maskBounds = {left, top, w, h};
        rec.ok = true;
    } catch (std::exception&) { rec.ok = false; }
    return rec;
}

// The records of a block: body spans in `payload`, or none when it cannot be walked.
std::optional<std::vector<std::pair<size_t, size_t>>> walk(const std::vector<uint8_t>& payload) {
    try {
        psd::BigEndianReader r(payload);
        const uint32_t version = r.read_u32();
        if (version < 1 || version > 3) return std::nullopt;
        std::vector<std::pair<size_t, size_t>> out;
        while (r.remaining()) {
            if (r.remaining() < 8) {
                // Photoshop's alignment inside the block: up to three zeros.
                while (r.remaining()) if (r.read_u8() != 0) return std::nullopt;
                break;
            }
            const uint64_t n = r.read_u64();
            if (n > r.remaining()) return std::nullopt;
            out.push_back({r.position(), size_t(n)});
            r.skip(size_t(n));
            // Each record is aligned to four bytes (zeros outside its length).
            const size_t pad = (4 - r.position() % 4) % 4;
            if (pad && r.remaining() >= pad) {
                bool zeros = true;
                for (size_t k = 0; k < pad; k++) zeros &= payload[r.position() + k] == 0;
                if (zeros && (r.remaining() == pad || r.remaining() - pad >= 8)) r.skip(pad);
            }
        }
        return out;
    } catch (std::exception&) { return std::nullopt; }
}

} // namespace

std::optional<SmartFilterStack> parseSmartFilterStack(const std::string& key, const std::vector<uint8_t>& payload) {
    if (key != "SoLd" && key != "SoLE") return std::nullopt;
    try {
        psd::BigEndianReader r(payload);
        if (r.read_bytes(4) != std::vector<uint8_t>{'s', 'o', 'L', 'D'}) return std::nullopt;
        const uint32_t version = r.read_u32();
        if (version != 4 && version != 5) return std::nullopt;
        (void)r.read_u32();
        const psd::DescriptorObject d = psd::read_descriptor(r);
        const V* fx = psd::descriptor_value(d, "filterFX");
        if (!fx) return std::nullopt;
        SmartFilterStack stack;
        if (fx->type != V::Type::Object || !fx->object_value) return stack;
        const psd::DescriptorObject& root = *fx->object_value;
        bool valid = true, linked = true, extendWhite = true;
        bool ok = root.class_id == "filterFXStyle" && boolean(root, "enab", stack.enabled) && boolean(root, "validAtPosition", valid)
            && boolean(root, "filterMaskEnable", stack.maskEnabled) && boolean(root, "filterMaskLinked", linked)
            && boolean(root, "filterMaskExtendWithWhite", extendWhite);
        // A linked filter mask is not calibrated (Photoshop would not author one for Patchy to measure).
        ok = ok && !linked;
        stack.maskDefault = extendWhite ? 255 : 0;
        const V* list = psd::descriptor_value(root, "filterFXList");
        if (!list || list->type != V::Type::List || list->list_value.empty()) ok = false;
        else
            for (const V& item : list->list_value) {
                SmartFilterEntry entry;
                bool good = item.type == V::Type::Object && item.object_value && item.object_value->class_id == "filterFX";
                if (good) {
                    const psd::DescriptorObject& e = *item.object_value;
                    bool options = true;
                    const V* name = either(e, "Nm  ", "Nm");
                    if (name && name->type == V::Type::String) entry.name = name->string_value; else good = false;
                    good = good && boolean(e, "enab", entry.enabled) && boolean(e, "hasoptions", options);
                    const V* id = psd::descriptor_value(e, "filterID");
                    const uint32_t filterId = id && id->type == V::Type::Integer ? uint32_t(id->integer_value) : 0;
                    if (!id || id->type != V::Type::Integer) good = false;
                    const psd::DescriptorObject* blend = psd::descriptor_object(e, "blendOptions");
                    double opacity = 0;
                    if (blend && blend->class_id == "blendOptions" && unitIn(either(*blend, "Opct", "Opct"), "#Prc", 0, 100, opacity)) {
                        entry.opacity = opacity / 100;
                        const V* mode = either(*blend, "Md  ", "Md");
                        if (!mode || mode->type != V::Type::Enum || mode->enum_type != "BlnM" || !blendOf(mode->enum_value, entry.blend)) good = false;
                    } else good = false;
                    good = good && rgb(e, "FrgC") && rgb(e, "BckC");
                    if (const psd::DescriptorObject* f = psd::descriptor_object(e, "Fltr"); f && good) entry.parameters = parametersOf(*f, filterId);
                }
                if (!good) entry.parameters = std::monostate{};
                ok = ok && !std::holds_alternative<std::monostate>(entry.parameters);
                stack.entries.push_back(std::move(entry));
            }
        stack.supported = ok;
        return stack;
    } catch (std::exception&) {}
    return SmartFilterStack{};
}

std::optional<SmartFilterCache> findSmartFilterCache(const std::vector<PsdBlock>& globals, const std::string& placedId) {
    if (placedId.empty()) return std::nullopt;
    std::optional<SmartFilterCache> found;
    int matches = 0;
    for (const PsdBlock& b : globals) {
        if (b.key != "FEid" && b.key != "FXid") continue;
        auto records = walk(b.data);
        if (!records) return std::nullopt;   // an unreadable cache block: nothing in it can be told apart
        for (auto [at, n] : *records) {
            Record rec = readRecord(std::span<const uint8_t>(b.data).subspan(at, n));
            if (rec.placedId != placedId) continue;
            matches++;
            if (rec.ok) found = rec.cache;
        }
    }
    return matches == 1 ? found : std::nullopt;
}

std::vector<uint8_t> authorSmartFilterRecord(const std::string& placedId, const PixelRect& document, const PlacedRaster& unfiltered,
                                             const GrayImage* mask, const PixelRect& maskBounds, uint8_t maskDefault) {
    auto plane = [&](auto sample) {
        std::vector<std::vector<uint8_t>> rows;
        std::vector<uint8_t> row(size_t(document.width));
        for (int y = 0; y < document.height; y++) {
            for (int x = 0; x < document.width; x++) row[size_t(x)] = sample(document.x + x, document.y + y);
            rows.push_back(psd::encode_packbits_row(row));
        }
        psd::BigEndianWriter w;
        w.write_u16(1);
        for (auto& r : rows) w.write_u32(uint32_t(r.size()));
        for (auto& r : rows) w.write_bytes(r);
        return w.bytes();
    };
    // The cache holds straight colour, as a PSD's channels do.
    auto channel = [&](int c) {
        return [&, c](int x, int y) -> uint8_t {
            const int lx = x - unfiltered.x, ly = y - unfiltered.y;
            if (!unfiltered.image || lx < 0 || ly < 0 || lx >= unfiltered.image->width() || ly >= unfiltered.image->height()) return 0;
            const uint8_t* p = unfiltered.image->pixel(lx, ly);
            if (c == 3) return p[3];
            return p[3] ? uint8_t(std::min(255, (p[c] * 255 + p[3] / 2) / p[3])) : 0;
        };
    };
    auto maskAt = [&](int x, int y) -> uint8_t {
        const int lx = x - maskBounds.x, ly = y - maskBounds.y;
        if (mask && lx >= 0 && ly >= 0 && lx < mask->width() && ly < mask->height()) return mask->row(ly)[lx];
        return maskDefault;
    };
    psd::BigEndianWriter cache;
    cache.write_u32(uint32_t(document.y));
    cache.write_u32(uint32_t(document.x));
    cache.write_u32(uint32_t(document.y + document.height));
    cache.write_u32(uint32_t(document.x + document.width));
    cache.write_u32(8);
    cache.write_u32(24);
    for (uint32_t slot = 0; slot < 26; slot++) {
        const int c = slot <= 2 ? int(slot) : slot == 25 ? 3 : -1;
        if (c < 0) { cache.write_u32(0); continue; }
        const auto bytes = plane(channel(c));
        cache.write_u32(1);
        cache.write_u64(bytes.size());
        cache.write_bytes(bytes);
    }
    const auto maskPlane = plane(maskAt);
    psd::BigEndianWriter body;
    body.write_u8(uint8_t(placedId.size()));
    for (char ch : placedId) body.write_u8(uint8_t(ch));
    body.write_u32(1);
    body.write_u64(cache.bytes().size());
    body.write_bytes(cache.bytes());
    body.write_u8(1);
    body.write_u32(uint32_t(document.y));
    body.write_u32(uint32_t(document.x));
    body.write_u32(uint32_t(document.y + document.height));
    body.write_u32(uint32_t(document.x + document.width));
    body.write_u64(maskPlane.size());
    body.write_bytes(maskPlane);
    return body.bytes();
}

std::optional<std::vector<uint8_t>> replaceSmartFilterRecords(const std::vector<uint8_t>& payload,
                                                              const std::vector<std::pair<std::string, std::vector<uint8_t>>>& replacements) {
    auto records = walk(payload);
    if (!records) return std::nullopt;
    psd::BigEndianWriter w;
    w.write_u32(psd::BigEndianReader(payload).read_u32());
    for (auto [at, n] : *records) {
        const auto body = std::span<const uint8_t>(payload).subspan(at, n);
        const std::string id = body.empty() || size_t(body[0]) + 1 > body.size() ? std::string() : std::string(body.begin() + 1, body.begin() + 1 + body[0]);
        const std::vector<uint8_t>* next = nullptr;
        for (auto& [pid, rec] : replacements) if (pid == id) next = &rec;
        const std::span<const uint8_t> bytes = next ? std::span<const uint8_t>(*next) : body;
        w.write_u64(bytes.size());
        w.write_bytes(bytes);
        while (w.bytes().size() % 4) w.write_u8(0);
    }
    return w.bytes();
}

std::optional<PlacedRaster> placedSmartObjectRaster(const SmartObjectInstance& instance, const Image& source, const std::array<double, 8>& quad) {
    auto mesh = smartObjectWarp(instance);
    const WarpMesh flat = identityWarpMesh(0, 0, source.width(), source.height(), 2, 2);
    auto raster = renderWarpedImage(source, mesh ? *mesh : flat, quad);
    if (!raster) return std::nullopt;
    return PlacedRaster{raster->image, int(std::lround(raster->transform.origin.x)), int(std::lround(raster->transform.origin.y))};
}

std::optional<PlacedRaster> filteredSmartObjectRaster(const std::vector<PsdBlock>& globals,
                                                      const SmartObjectInstance& instance, const Image& source, const std::array<double, 8>& quad) {
    std::optional<SmartFilterStack> stack;
    for (const PsdBlock& b : instance.psdBlocks)
        if (b.key == "SoLd" || b.key == "SoLE") { stack = parseSmartFilterStack(b.key, b.data); break; }
    if (!stack || !stack->supported) return std::nullopt;
    auto cache = findSmartFilterCache(globals, instance.placedId);
    if (!cache) return std::nullopt;
    stack->mask = cache->mask;
    stack->maskBounds = cache->maskBounds;
    auto placed = placedSmartObjectRaster(instance, source, quad);
    if (!placed) return std::nullopt;
    return renderSmartFilterStack(*placed, cache->canvas, *stack);
}

int refreshSmartObjectRasters(Document& document) {
    static const std::vector<PsdBlock> none;
    int redrawn = 0;
    for (Layer& layer : document.layers) {
        if (!layer.isLiveSmartObject() || layer.smartObject->locked()) continue;
        SmartObjectInstance& so = *layer.smartObject;
        if (layer.transform == so.placedTransform || smartObjectPixelsArePlacement(so)) continue;
        auto source = document.smartObjects.find(so.sourceId);
        if (source == document.smartObjects.end() || !source->second->image) continue;
        const int w = layer.asset->image->width(), h = layer.asset->image->height();
        const std::array<double, 8> quad = moveQuad(so.quad, so.placedTransform, so.placedWidth, so.placedHeight, layer.transform, w, h);
        std::optional<PlacedRaster> raster;
        if (smartObjectFiltered(so)) raster = filteredSmartObjectRaster(document.psdCarry ? document.psdCarry->globals : none, so, *source->second->image, quad);
        else if (auto warped = warpedSmartObjectRaster(so, *source->second->image, quad))
            raster = PlacedRaster{warped->image, int(std::lround(warped->transform.origin.x)), int(std::lround(warped->transform.origin.y))};
        if (!raster || !raster->image) continue;
        // The mask stays where it was on the canvas.
        if (layer.mask && !layer.mask->placement) layer.mask->placement = layer.maskTransform();
        const Sampling sampling = layer.transform.sampling;
        layer.asset = Asset::make(raster->image, layer.name);
        layer.transform = LayerTransform(Point(raster->x, raster->y), Size(raster->image->width(), raster->image->height()));
        layer.transform.sampling = sampling;
        layer.smartImage = raster->image;
        so.quad = quad;
        so.placedTransform = layer.transform;
        so.placedWidth = raster->image->width();
        so.placedHeight = raster->image->height();
        redrawn++;
    }
    return redrawn;
}

} // namespace compositor

