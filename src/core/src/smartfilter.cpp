// Smart Filters: reading the SoLd 'filterFX' stack and the document's 'FEid' / 'FXid' cache, writing a fresh cache
// record, and drawing an instance through its stack. The descriptor keys, ranges and cache layout follow Patchy's
// psd_smart_objects.cpp and psd_filter_effects.cpp (MIT, src/third_party/patchy_psd/README.md); the kernels live in
// smartfilter_render.cpp.
#include "compositor/smartfilter.h"
#include "psd/psd_descriptor.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <type_traits>

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
        {"saturation", BlendMode::Saturation}, {"color", BlendMode::Color}, {"luminosity", BlendMode::Luminosity},
        {"dissolve", BlendMode::Dissolve}, {"linearBurn", BlendMode::LinearBurn}, {"darkerColor", BlendMode::DarkerColor},
        {"linearDodge", BlendMode::LinearDodge}, {"lighterColor", BlendMode::LighterColor}, {"softLight", BlendMode::SoftLight},
        {"hardLight", BlendMode::HardLight}, {"vividLight", BlendMode::VividLight}, {"linearLight", BlendMode::LinearLight},
        {"pinLight", BlendMode::PinLight}, {"hardMix", BlendMode::HardMix}, {"exclusion", BlendMode::Exclusion},
        {"blendSubtraction", BlendMode::Subtract}, {"blendDivide", BlendMode::Divide}};
    // Older files (and some writers) store the modes by their four-character ids.
    static const std::pair<const char*, BlendMode> ids[] = {
        {"Nrml", BlendMode::Normal}, {"Mltp", BlendMode::Multiply}, {"Scrn", BlendMode::Screen}, {"Ovrl", BlendMode::Overlay},
        {"Drkn", BlendMode::Darken}, {"Lghn", BlendMode::Lighten}, {"Dfrn", BlendMode::Difference}, {"CDdg", BlendMode::ColorDodge},
        {"CBrn", BlendMode::ColorBurn}, {"H   ", BlendMode::Hue}, {"Strt", BlendMode::Saturation}, {"Clr ", BlendMode::Color},
        {"Lmns", BlendMode::Luminosity}, {"Dslv", BlendMode::Dissolve}, {"SftL", BlendMode::SoftLight}, {"HrdL", BlendMode::HardLight},
        {"Xclu", BlendMode::Exclusion}};
    for (auto& [n, m] : modes) if (name == n) { out = m; return true; }
    for (auto& [n, m] : ids) if (name == n) { out = m; return true; }
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
            // Sizes in 64 bits: hostile edges would overflow an int (found by fuzzing). Photoshop's limit is 300,000.
            const int64_t cw = int64_t(right) - left, ch = int64_t(bottom) - top;
            if (cw <= 0 || ch <= 0 || cw > 300000 || ch > 300000 || depth != 8 || channels > 64) return rec;
            rec.cache.canvas = {left, top, int(cw), int(ch)};
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
        const int64_t mw = int64_t(right) - left, mh = int64_t(bottom) - top;
        if (mw <= 0 || mh <= 0 || mw > maxImageSide || mh > maxImageSide || maskLength != r.remaining()) return rec;
        const int w = int(mw), h = int(mh);
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
        if (next && next->empty()) continue;   // dropped
        const std::span<const uint8_t> bytes = next ? std::span<const uint8_t>(*next) : body;
        w.write_u64(bytes.size());
        w.write_bytes(bytes);
        while (w.bytes().size() % 4) w.write_u8(0);
    }
    return w.bytes();
}

std::optional<PlacedRaster> placedSmartObjectRaster(const SmartObjectInstance& instance, const Image& source, const std::array<double, 8>& quad,
                                                    const Rect* clip) {
    auto mesh = smartObjectWarp(instance);
    const WarpMesh flat = identityWarpMesh(0, 0, source.width(), source.height(), 2, 2);
    auto raster = renderWarpedImage(source, mesh ? *mesh : flat, quad, clip);
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
    // A canvas past what a buffer can hold is not drawn here (a hostile file could claim 300,000 pixels a side).
    if (!cache || cache->canvas.width > maxImageSide || cache->canvas.height > maxImageSide
        || int64_t(cache->canvas.width) * cache->canvas.height > int64_t(Document::pixelBudget)) return std::nullopt;
    stack->mask = cache->mask;
    stack->maskBounds = cache->maskBounds;
    // Only what lies on the filter canvas can show (past it the filters repeat the canvas's edge).
    const Rect canvas(cache->canvas.x, cache->canvas.y, cache->canvas.width, cache->canvas.height);
    auto placed = placedSmartObjectRaster(instance, source, quad, &canvas);
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

namespace {

V dvObject(const char* cls, bool longForm, const std::string& name = {}) {
    V v; v.type = V::Type::Object; v.object_value = std::make_shared<psd::DescriptorObject>();
    v.object_value->class_id = cls; v.object_value->class_id_long_form = longForm; v.object_value->name = name;
    return v;
}
void dvAdd(V& o, const char* key, bool longForm, V value) { o.object_value->key_order.push_back({key, longForm}); o.object_value->values[key] = std::move(value); }
V dvBool(bool b) { V v; v.type = V::Type::Bool; v.bool_value = b; return v; }
V dvInt(int32_t i) { V v; v.type = V::Type::Integer; v.integer_value = i; return v; }
V dvDouble(double d) { V v; v.type = V::Type::Double; v.double_value = d; return v; }
V dvUnit(const char* unit, double d) { V v; v.type = V::Type::UnitFloat; v.unit = unit; v.double_value = d; return v; }
V dvText(const std::string& t) { V v; v.type = V::Type::String; v.string_value = t; return v; }
V dvEnum(const char* type, bool typeLong, const char* value, bool valueLong) {
    V v; v.type = V::Type::Enum; v.enum_type = type; v.enum_type_long_form = typeLong; v.enum_value = value; v.enum_value_long_form = valueLong; return v;
}
V dvColour(double r, double g, double b) {
    V c = dvObject("RGBC", false);
    dvAdd(c, "Rd  ", false, dvDouble(r)); dvAdd(c, "Grn ", false, dvDouble(g)); dvAdd(c, "Bl  ", false, dvDouble(b));
    return c;
}
const char* blendName(BlendMode m) {
    switch (m) {
    case BlendMode::Multiply: return "multiply"; case BlendMode::Screen: return "screen"; case BlendMode::Overlay: return "overlay";
    case BlendMode::Darken: return "darken"; case BlendMode::Lighten: return "lighten"; case BlendMode::Difference: return "difference";
    case BlendMode::ColorDodge: return "colorDodge"; case BlendMode::ColorBurn: return "colorBurn"; case BlendMode::Hue: return "hue";
    case BlendMode::Saturation: return "saturation"; case BlendMode::Color: return "color"; case BlendMode::Luminosity: return "luminosity";
    case BlendMode::Dissolve: return "dissolve"; case BlendMode::LinearBurn: return "linearBurn"; case BlendMode::DarkerColor: return "darkerColor";
    case BlendMode::LinearDodge: return "linearDodge"; case BlendMode::LighterColor: return "lighterColor"; case BlendMode::SoftLight: return "softLight";
    case BlendMode::HardLight: return "hardLight"; case BlendMode::VividLight: return "vividLight"; case BlendMode::LinearLight: return "linearLight";
    case BlendMode::PinLight: return "pinLight"; case BlendMode::HardMix: return "hardMix"; case BlendMode::Exclusion: return "exclusion";
    case BlendMode::Subtract: return "blendSubtraction"; case BlendMode::Divide: return "blendDivide";
    default: return "normal";
    }
}

// One filterFX entry in Patchy's authoring shape (Photoshop 2026's key orders, pinned by its captures).
std::optional<V> entryDescriptor(const SmartFilterEntry& entry) {
    using namespace smartfilter;
    struct Spec { const char* name; const char* cls; bool longForm; uint32_t id; };
    std::optional<Spec> spec;
    std::vector<std::pair<const char*, V>> keys;
    std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, GaussianBlur>) { spec = Spec{"Gaussian Blur", "GsnB", false, 0x47736e42u}; keys = {{"Rds ", dvUnit("#Pxl", p.radius)}}; }
        else if constexpr (std::is_same_v<T, HighPass>) { spec = Spec{"High Pass", "HghP", false, 0x48676850u}; keys = {{"Rds ", dvUnit("#Pxl", p.radius)}}; }
        else if constexpr (std::is_same_v<T, Median>) { spec = Spec{"Median", "Mdn ", false, 0x4d646e20u}; keys = {{"Rds ", dvUnit("#Pxl", p.radius)}}; }
        else if constexpr (std::is_same_v<T, DustAndScratches>) { spec = Spec{"Dust & Scratches", "DstS", false, 0x44737453u}; keys = {{"Rds ", dvInt(p.radius)}, {"Thsh", dvInt(p.threshold)}}; }
        else if constexpr (std::is_same_v<T, SurfaceBlur>) { spec = Spec{"Surface Blur", "surfaceBlur", true, 854u}; keys = {{"Rds ", dvUnit("#Pxl", p.radius)}, {"Thsh", dvInt(p.threshold)}}; }
        else if constexpr (std::is_same_v<T, UnsharpMask>) { spec = Spec{"Unsharp Mask", "UnsM", false, 0x556e734du}; keys = {{"Amnt", dvUnit("#Prc", p.amount)}, {"Rds ", dvUnit("#Pxl", p.radius)}, {"Thsh", dvInt(p.threshold)}}; }
        else if constexpr (std::is_same_v<T, MotionBlur>) { spec = Spec{"Motion Blur", "MtnB", false, 0x4d746e42u}; keys = {{"Angl", dvInt(p.angle)}, {"Dstn", dvUnit("#Pxl", p.distance)}}; }
        else if constexpr (std::is_same_v<T, PlasticWrap>) { spec = Spec{"Plastic Wrap", "PlsW", false, 0x506c7357u}; keys = {{"Hghl", dvInt(p.highlight)}, {"Dtl ", dvInt(p.detail)}, {"Smth", dvInt(p.smoothness)}}; }
        else if constexpr (std::is_same_v<T, Mosaic>) { spec = Spec{"Mosaic", "Msc ", false, 0x4d736320u}; keys = {{"ClSz", dvUnit("#Pxl", p.cellSize)}}; }
        else if constexpr (std::is_same_v<T, Emboss>) { spec = Spec{"Emboss", "Embs", false, 0x456d6273u}; keys = {{"Angl", dvInt(p.angle)}, {"Hght", dvInt(p.height)}, {"Amnt", dvInt(p.amount)}}; }
        else if constexpr (std::is_same_v<T, BoxBlur>) { spec = Spec{"Box Blur", "boxblur", true, 843u}; keys = {{"Rds ", dvUnit("#Pxl", p.radius)}}; }
        else if constexpr (std::is_same_v<T, RadialBlur>) {
            spec = Spec{"Radial Blur", "RdlB", false, 0x52646c42u};
            keys = {{"Amnt", dvInt(p.amount)}, {"BlrM", dvEnum("BlrM", false, "Spn ", false)},
                    {"BlrQ", dvEnum("BlrQ", false, p.samples <= 8 ? "Drft" : p.samples <= 16 ? "Gd  " : "Bst ", false)}};
        } else if constexpr (std::is_same_v<T, AddNoise>) {
            spec = Spec{"Add Noise", "AdNs", false, 0x41644e73u};
            keys = {{"Dstr", dvEnum("Dstr", false, p.gaussian ? "Gsn " : "Unfr", false)}, {"Nose", dvUnit("#Prc", p.amount)}, {"Mnch", dvBool(p.monochromatic)}, {"FlRs", dvInt(p.seed)}};
        }
    }, entry.parameters);
    if (!spec) return std::nullopt;
    V item = dvObject("filterFX", true);
    dvAdd(item, "Nm  ", false, dvText(entry.name.empty() ? smartFilterName(entry.parameters) : entry.name));
    V blend = dvObject("blendOptions", true);
    dvAdd(blend, "Opct", false, dvUnit("#Prc", std::clamp(entry.opacity, 0.0, 1.0) * 100));
    dvAdd(blend, "Md  ", false, dvEnum("BlnM", false, blendName(entry.blend), true));
    dvAdd(item, "blendOptions", true, blend);
    dvAdd(item, "enab", false, dvBool(entry.enabled));
    dvAdd(item, "hasoptions", true, dvBool(true));
    dvAdd(item, "FrgC", false, dvColour(0, 0, 0));
    dvAdd(item, "BckC", false, dvColour(255, 255, 255));
    V filter = dvObject(spec->cls, spec->longForm, spec->name);
    for (auto& [k, v] : keys) dvAdd(filter, k, false, v);
    dvAdd(item, "Fltr", false, filter);
    dvAdd(item, "filterID", true, dvInt(int32_t(spec->id)));
    return item;
}

} // namespace

std::string smartFilterName(const SmartFilterParameters& parameters) {
    static const char* names[] = {"", "Gaussian Blur...", "High Pass...", "Median...", "Dust && Scratches...", "Surface Blur...", "Unsharp Mask...",
                                  "Motion Blur...", "Plastic Wrap...", "Mosaic...", "Emboss...", "Box Blur...", "Radial Blur...", "Add Noise..."};
    return parameters.index() < std::size(names) ? names[parameters.index()] : "";
}

std::optional<std::vector<uint8_t>> setPsdSmartFilterStack(const std::string& key, const std::vector<uint8_t>& payload, const SmartFilterStack& stack) {
    if ((key != "SoLd" && key != "SoLE") || stack.entries.empty()) return std::nullopt;
    try {
        psd::BigEndianReader r(payload);
        if (r.read_bytes(4) != std::vector<uint8_t>{'s', 'o', 'L', 'D'}) return std::nullopt;
        const uint32_t version = r.read_u32(), descriptorVersion = r.read_u32();
        psd::DescriptorObject d = psd::read_descriptor(r);
        const size_t tail = r.position();
        V root = dvObject("filterFXStyle", true);
        dvAdd(root, "enab", false, dvBool(stack.enabled));
        dvAdd(root, "validAtPosition", true, dvBool(true));
        dvAdd(root, "filterMaskEnable", true, dvBool(stack.maskEnabled));
        dvAdd(root, "filterMaskLinked", true, dvBool(false));
        dvAdd(root, "filterMaskExtendWithWhite", true, dvBool(stack.maskDefault == 255));
        V list; list.type = V::Type::List;
        for (const SmartFilterEntry& e : stack.entries) {
            auto item = entryDescriptor(e);
            if (!item) return std::nullopt;
            list.list_value.push_back(std::move(*item));
        }
        dvAdd(root, "filterFXList", true, list);
        if (!d.values.count("filterFX")) {
            // Photoshop puts it before the trailing 'comp' / 'compInfo'.
            auto at = std::find_if(d.key_order.begin(), d.key_order.end(), [](const psd::DescriptorObject::KeyEntry& k) { return k.key == "comp" || k.key == "compInfo"; });
            d.key_order.insert(at, {"filterFX", true});
        }
        d.values["filterFX"] = root;
        psd::BigEndianWriter w;
        for (char c : std::string("soLD")) w.write_u8(uint8_t(c));
        w.write_u32(version);
        w.write_u32(descriptorVersion);
        psd::write_descriptor(w, d);
        w.write_bytes(std::span<const uint8_t>(payload.data() + tail, payload.size() - tail));
        return w.bytes();
    } catch (std::exception&) {}
    return std::nullopt;
}

namespace {

// The placement block without its 'filterFX' (the stack removed); none for a block that is not a SoLd / SoLE.
std::optional<std::vector<uint8_t>> removePsdSmartFilterStack(const std::string& key, const std::vector<uint8_t>& payload) {
    if (key != "SoLd" && key != "SoLE") return std::nullopt;
    try {
        psd::BigEndianReader r(payload);
        if (r.read_bytes(4) != std::vector<uint8_t>{'s', 'o', 'L', 'D'}) return std::nullopt;
        const uint32_t version = r.read_u32(), descriptorVersion = r.read_u32();
        psd::DescriptorObject d = psd::read_descriptor(r);
        const size_t tail = r.position();
        d.values.erase("filterFX");
        d.key_order.erase(std::remove_if(d.key_order.begin(), d.key_order.end(), [](const psd::DescriptorObject::KeyEntry& k) { return k.key == "filterFX"; }),
                          d.key_order.end());
        psd::BigEndianWriter w;
        for (char c : std::string("soLD")) w.write_u8(uint8_t(c));
        w.write_u32(version);
        w.write_u32(descriptorVersion);
        psd::write_descriptor(w, d);
        w.write_bytes(std::span<const uint8_t>(payload.data() + tail, payload.size() - tail));
        return w.bytes();
    } catch (std::exception&) {}
    return std::nullopt;
}

template <typename T> void clampTo(T& v, double lo, double hi) {
    const double d = double(v);
    v = T(std::isfinite(d) ? std::clamp(d, lo, hi) : lo);
}

} // namespace

void clampSmartFilterParameters(SmartFilterParameters& parameters) {
    using namespace smartfilter;
    std::visit([&](auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, GaussianBlur> || std::is_same_v<T, HighPass>) clampTo(p.radius, 0.1, 1000);
        else if constexpr (std::is_same_v<T, Median>) clampTo(p.radius, 1, 500);
        else if constexpr (std::is_same_v<T, DustAndScratches>) { clampTo(p.radius, 1, 500); clampTo(p.threshold, 0, 255); }
        else if constexpr (std::is_same_v<T, SurfaceBlur>) { clampTo(p.radius, 1, 100); clampTo(p.threshold, 2, 255); }
        else if constexpr (std::is_same_v<T, UnsharpMask>) { clampTo(p.amount, 1, 500); clampTo(p.radius, 0.1, 1000); clampTo(p.threshold, 0, 255); }
        else if constexpr (std::is_same_v<T, MotionBlur>) { clampTo(p.angle, -360, 360); clampTo(p.distance, 1, 999); }
        else if constexpr (std::is_same_v<T, PlasticWrap>) { clampTo(p.highlight, 0, 20); clampTo(p.detail, 1, 15); clampTo(p.smoothness, 1, 15); }
        else if constexpr (std::is_same_v<T, Mosaic>) clampTo(p.cellSize, 2, 200);
        else if constexpr (std::is_same_v<T, Emboss>) { clampTo(p.angle, -360, 360); clampTo(p.height, 1, 100); clampTo(p.amount, 1, 500); }
        else if constexpr (std::is_same_v<T, BoxBlur>) clampTo(p.radius, 1, 2000);
        else if constexpr (std::is_same_v<T, RadialBlur>) { clampTo(p.amount, 1, 100); p.samples = p.samples <= 8 ? 8 : p.samples <= 16 ? 16 : 32; }
        else if constexpr (std::is_same_v<T, AddNoise>) { clampTo(p.amount, 0.1, 400); clampTo(p.seed, 0, 999999999); }
    }, parameters);
}

std::optional<SmartFilterStack> smartFilterStackOf(const Document& document, const Layer& layer) {
    if (!layer.smartObject) return std::nullopt;
    std::optional<SmartFilterStack> stack;
    for (const PsdBlock& b : layer.smartObject->psdBlocks)
        if ((stack = parseSmartFilterStack(b.key, b.data))) break;
    if (!stack) return std::nullopt;
    auto cache = document.psdCarry ? findSmartFilterCache(document.psdCarry->globals, layer.smartObject->placedId) : std::nullopt;
    if (cache) { stack->mask = cache->mask; stack->maskBounds = cache->maskBounds; }
    else stack->supported = false;
    return stack;
}

bool setSmartFilters(Document& document, Layer& layer, const SmartFilterStack& wanted, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    if (!layer.isLiveSmartObject()) return fail("Smart Filters go on a smart object: convert the layer first.");
    SmartObjectInstance& so = *layer.smartObject;
    if (so.locked()) return fail("This smart object shows the preview its file carried; its filters cannot be changed here.");
    auto source = document.smartObjects.find(so.sourceId);
    if (source == document.smartObjects.end() || !source->second->image) return fail("Its contents cannot be read.");
    if (auto existing = smartFilterStackOf(document, layer); existing && !existing->supported)
        return fail("Its Smart Filters include one NekoPhoto does not draw (or a cache it cannot read); they cannot be changed here.");
    SmartFilterStack stack = wanted;
    for (SmartFilterEntry& e : stack.entries) {
        if (std::holds_alternative<std::monostate>(e.parameters)) return fail("That filter is not one NekoPhoto draws as a Smart Filter.");
        clampSmartFilterParameters(e.parameters);
        e.opacity = std::isfinite(e.opacity) ? std::clamp(e.opacity, 0.0, 1.0) : 1.0;
        if (e.name.empty()) e.name = smartFilterName(e.parameters);
    }
    stack.supported = true;
    const bool removing = stack.entries.empty();
    if (removing && !smartObjectFiltered(so)) return true;   // nothing to remove
    // Where it is now (a moved instance's quad follows the layer).
    const std::array<double, 8> quad = moveQuad(so.quad, so.placedTransform, so.placedWidth, so.placedHeight, layer.transform,
                                                layer.asset->image->width(), layer.asset->image->height());
    SmartObjectInstance next = so;
    if (next.placedId.empty()) next.placedId = newSmartObjectId();
    bool written = false;
    for (PsdBlock& b : next.psdBlocks) {
        auto placed = patchPsdPlacement(b.key, b.data, quad, next.placedId);
        auto withStack = !placed ? std::nullopt : removing ? removePsdSmartFilterStack(b.key, *placed) : setPsdSmartFilterStack(b.key, *placed, stack);
        if (withStack) { b.data = std::move(*withStack); written = true; }
    }
    if (!written) return fail("Its placement cannot take Smart Filters.");
    next.quad = quad;
    auto carry = std::make_shared<PsdDocumentCarry>(document.psdCarry ? *document.psdCarry : PsdDocumentCarry{});
    if (!document.psdCarry) { carry->width = document.width; carry->height = document.height; }
    // Its cache record: the unfiltered instance over the canvas, and the mask; none when the stack goes.
    std::vector<uint8_t> record;
    if (!removing) {
        const PixelRect canvas{0, 0, document.width, document.height};
        auto unfiltered = placedSmartObjectRaster(next, *source->second->image, quad);
        if (!unfiltered) return fail("It could not be drawn.");
        record = authorSmartFilterRecord(next.placedId, canvas, *unfiltered, stack.mask.get(), stack.maskBounds, stack.maskDefault);
    }
    bool placed = false;
    for (size_t k = 0; k < carry->globals.size(); k++) {
        PsdBlock& b = carry->globals[k];
        if (b.key != "FEid" && b.key != "FXid") continue;
        if (findSmartFilterCache({b}, next.placedId) || removing) {
            if (!walk(b.data)) { if (removing) continue; return fail("The document's filter cache cannot be rewritten."); }
            auto replaced = replaceSmartFilterRecords(b.data, {{next.placedId, record}});
            if (!replaced) return fail("The document's filter cache cannot be rewritten.");
            b.data = std::move(*replaced);
            placed = true;
            // A block left with no records goes.
            if (auto left = walk(b.data); removing && left && left->empty()) { carry->globals.erase(carry->globals.begin() + std::ptrdiff_t(k)); k--; }
            continue;
        }
        if (placed) continue;
        // A new record at the end of the block, aligned as Photoshop aligns them.
        psd::BigEndianWriter w;
        w.write_bytes(b.data);
        while (w.bytes().size() % 4) w.write_u8(0);
        w.write_u64(record.size());
        w.write_bytes(record);
        while (w.bytes().size() % 4) w.write_u8(0);
        b.data = w.bytes();
        placed = true;
    }
    if (!placed && !removing) {
        psd::BigEndianWriter w;
        w.write_u32(3);
        w.write_u64(record.size());
        w.write_bytes(record);
        while (w.bytes().size() % 4) w.write_u8(0);
        carry->globals.push_back({"FEid", w.bytes()});
    }
    auto drawn = removing ? placedSmartObjectRaster(next, *source->second->image, quad) : filteredSmartObjectRaster(carry->globals, next, *source->second->image, quad);
    if (!drawn || !drawn->image) return fail("The filters could not be drawn.");
    document.psdCarry = carry;
    if (layer.mask && !layer.mask->placement) layer.mask->placement = layer.maskTransform();
    const Sampling sampling = layer.transform.sampling;
    layer.asset = Asset::make(drawn->image, layer.name);
    layer.transform = LayerTransform(Point(drawn->x, drawn->y), Size(drawn->image->width(), drawn->image->height()));
    layer.transform.sampling = sampling;
    layer.smartImage = drawn->image;
    next.placedTransform = layer.transform;
    next.placedWidth = drawn->image->width();
    next.placedHeight = drawn->image->height();
    so = std::move(next);
    return true;
}

bool addSmartFilter(Document& document, Layer& layer, const SmartFilterEntry& entry, std::string* error) {
    if (!layer.isLiveSmartObject()) { if (error) *error = "Smart Filters go on a smart object: convert the layer first."; return false; }
    if (std::holds_alternative<std::monostate>(entry.parameters)) { if (error) *error = "That filter is not one NekoPhoto draws as a Smart Filter."; return false; }
    SmartFilterStack stack;
    if (auto existing = smartFilterStackOf(document, layer)) {
        if (!existing->supported) { if (error) *error = "Its Smart Filters include one NekoPhoto does not draw; they cannot be added to here."; return false; }
        stack = *existing;
    }
    stack.entries.push_back(entry);
    return setSmartFilters(document, layer, stack, error);
}

} // namespace compositor

