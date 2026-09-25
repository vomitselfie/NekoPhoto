#include "compositor/psd_writer.h"
#include "compositor/adjustments.h"
#include "compositor/parallel.h"
#include "compositor/render.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace compositor {

namespace {

/// Big-endian fields appended to a byte vector.
struct Out {
    std::vector<uint8_t> b;
    void u8(unsigned v) { b.push_back(uint8_t(v)); }
    void u16(unsigned v) { u8((v >> 8) & 0xff); u8(v & 0xff); }
    void i16(int v) { u16(unsigned(uint16_t(int16_t(v)))); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xffff); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void f32(float v) { uint32_t bits; std::memcpy(&bits, &v, 4); u32(bits); }
    void str(const char* s) { b.insert(b.end(), s, s + std::strlen(s)); }
    void bytes(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
};

/// PackBits: runs of three or more equal bytes as repeats, the rest as literals of up to 128.
void packBits(const uint8_t* row, int n, std::vector<uint8_t>& out) {
    int i = 0;
    while (i < n) {
        int run = 1;
        while (i + run < n && run < 128 && row[i + run] == row[i]) run++;
        if (run >= 3) { out.push_back(uint8_t(257 - run)); out.push_back(row[i]); i += run; continue; }
        const int start = i;
        while (i < n && i - start < 128) {
            if (i + 2 < n && row[i] == row[i + 1] && row[i] == row[i + 2]) break;
            i++;
        }
        out.push_back(uint8_t(i - start - 1));
        out.insert(out.end(), row + start, row + i);
    }
}

/// One channel's data as a layer record stores it: the compression word, then the rows.
std::vector<uint8_t> encodeChannel(const std::vector<uint8_t>& plane, int w, int h, bool compress) {
    Out o;
    if (w <= 0 || h <= 0) { o.u16(0); return o.b; }
    if (compress) {
        std::vector<std::vector<uint8_t>> rows(static_cast<size_t>(h));
        size_t total = 0;
        parallelRows(0, h, [&](int ya, int yb) { for (int y = ya; y < yb; y++) packBits(plane.data() + size_t(y) * w, w, rows[size_t(y)]); }, 64);
        for (auto& r : rows) total += r.size();
        if (total + size_t(h) * 2 < plane.size()) {
            o.b.reserve(2 + size_t(h) * 2 + total);
            o.u16(1);
            for (auto& r : rows) o.u16(unsigned(r.size()));
            for (auto& r : rows) o.bytes(r);
            return o.b;
        }
    }
    o.b.reserve(2 + plane.size());
    o.u16(0);
    o.bytes(plane);
    return o.b;
}

/// Straight (not premultiplied) planes from premultiplied RGBA: PSD channels hold straight colour, and
/// writing premultiplied values would darken every soft edge.
std::array<std::vector<uint8_t>, 4> straightPlanes(const Image& image) {
    const int w = image.width(), h = image.height();
    std::array<std::vector<uint8_t>, 4> planes;
    for (auto& p : planes) p.resize(size_t(w) * h);
    parallelRows(0, h, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint8_t* s = image.row(y);
            for (int x = 0; x < w; x++, s += 4) {
                const size_t i = size_t(y) * w + x;
                const unsigned a = s[3];
                planes[3][i] = uint8_t(a);
                for (int c = 0; c < 3; c++) planes[size_t(c)][i] = a == 0 ? 0 : uint8_t(std::min(255u, (s[c] * 255u + a / 2) / a));
            }
        }
    }, 64);
    return planes;
}

const char* blendKey(BlendMode mode) {
    switch (mode) {
    case BlendMode::Normal: return "norm";
    case BlendMode::Multiply: return "mul ";
    case BlendMode::Screen: return "scrn";
    case BlendMode::Overlay: return "over";
    case BlendMode::Darken: return "dark";
    case BlendMode::Lighten: return "lite";
    case BlendMode::Difference: return "diff";
    case BlendMode::ColorDodge: return "div ";
    case BlendMode::ColorBurn: return "idiv";
    case BlendMode::Hue: return "hue ";
    case BlendMode::Saturation: return "sat ";
    case BlendMode::Color: return "colr";
    case BlendMode::Luminosity: return "lum ";
    }
    return "norm";
}

/// UTF-8 to UTF-16 code units (invalid bytes become U+FFFD).
std::vector<uint16_t> utf16(const std::string& s) {
    std::vector<uint16_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        uint32_t cp = 0xFFFD;
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + size_t(len) > s.size()) len = 1;
        if (len == 1) cp = c < 0x80 ? c : 0xFFFD;
        else {
            cp = c & (0xFF >> (len + 1));
            bool ok = true;
            for (int k = 1; k < len; k++) { const unsigned char d = s[i + size_t(k)]; if ((d >> 6) != 2) { ok = false; break; } cp = (cp << 6) | (d & 0x3F); }
            if (!ok) { cp = 0xFFFD; len = 1; }
        }
        if (cp >= 0x10000) { cp -= 0x10000; out.push_back(uint16_t(0xD800 + (cp >> 10))); out.push_back(uint16_t(0xDC00 + (cp & 0x3FF))); }
        else out.push_back(uint16_t(cp));
        i += size_t(len);
    }
    return out;
}

/// The legacy name field: ASCII only (anything else becomes '_'), at most 255 bytes; luni carries the real one.
std::string asciiName(const std::string& s) {
    std::string out;
    for (unsigned char c : s) { if (c >= 0x80 && (c & 0xC0) == 0x80) continue; out += c >= 0x20 && c < 0x7F ? char(c) : '_'; if (out.size() == 255) break; }
    return out;
}

std::string percent(double v) { char b[16]; std::snprintf(b, sizeof b, "%d%%", int(std::lround(v * 100))); return b; }

/// A layer record, ready to encode.
struct Record {
    int top = 0, left = 0, bottom = 0, right = 0;
    std::string name;
    std::string blend = "norm";
    uint8_t opacity = 255;
    bool clipping = false, hidden = false;
    int section = 0;                                     // lsct: 1 folder, 3 its end marker
    std::vector<std::pair<int, std::vector<uint8_t>>> channels;   // id, encoded data
    bool mask = false;
    int maskTop = 0, maskLeft = 0, maskBottom = 0, maskRight = 0;
    uint8_t maskDefault = 255, maskFlags = 0;
    const char* adjustmentKey = nullptr;
    std::vector<uint8_t> adjustmentData;
};

class Writer {
public:
    Writer(const Document& document, const PsdExportOptions& options, bool encode, PsdExportSummary& summary)
        : doc_(document), options_(options), encode_(encode), summary_(summary) {
        for (const Layer& l : doc_.layers) byId_[l.id] = &l;
    }

    std::vector<Record> records() {
        emit(std::nullopt);
        return std::move(records_);
    }

private:
    const Document& doc_;
    const PsdExportOptions& options_;
    const bool encode_;
    PsdExportSummary& summary_;
    std::map<Uuid, const Layer*> byId_;
    std::vector<Record> records_;

    std::vector<const Layer*> children(const std::optional<Uuid>& parent) const {
        std::vector<const Layer*> out;
        for (const Layer& l : doc_.layers) if (l.parentId == parent) out.push_back(&l);
        return out;
    }

    void emptyChannels(Record& r, bool alpha = true) const {
        if (!encode_) return;
        for (int id : {-1, 0, 1, 2}) if (id != -1 || alpha) r.channels.push_back({id, encodeChannel({}, 0, 0, false)});
    }

    void setPixels(Record& r, const Image& image, int left, int top) const {
        r.left = left; r.top = top; r.right = left + image.width(); r.bottom = top + image.height();
        if (!encode_) return;
        auto planes = straightPlanes(image);
        r.channels.push_back({-1, encodeChannel(planes[3], image.width(), image.height(), options_.compress)});
        for (int c = 0; c < 3; c++) r.channels.push_back({c, encodeChannel(planes[size_t(c)], image.width(), image.height(), options_.compress)});
    }

    /// The layer's mask over `rect` (document pixels): a copy when it lies on the layer's own pixel grid,
    /// else sampled the way the renderer samples it.
    void setMask(Record& r, const Layer& layer, const Rect& rect, bool onGrid) const {
        if (!layer.mask || !layer.mask->asset.image || rect.isEmpty()) return;
        const GrayImage& mask = *layer.mask->asset.image;
        r.mask = true;
        r.maskLeft = int(rect.x); r.maskTop = int(rect.y); r.maskRight = int(rect.x + rect.width); r.maskBottom = int(rect.y + rect.height);
        r.maskFlags = layer.mask->enabled ? 0 : 2;
        const int w = int(rect.width), h = int(rect.height);
        std::vector<uint8_t> plane(size_t(w) * h);
        if (onGrid && !layer.mask->placement && mask.width() == w && mask.height() == h) {
            r.maskDefault = 255;
            if (encode_) for (int y = 0; y < h; y++) std::memcpy(plane.data() + size_t(y) * w, mask.row(y), size_t(w));
        } else {
            r.maskDefault = 0;
            if (encode_) {
                GrayImage sampled(w, h, 0);
                sampleMaskCoverage(layer.mask->asset.image, layer.mask->placement ? *layer.mask->placement : layer.transform, rect, 1.0, 0, sampled, false);
                for (int y = 0; y < h; y++) std::memcpy(plane.data() + size_t(y) * w, sampled.row(y), size_t(w));
            }
        }
        if (encode_) r.channels.push_back({-2, encodeChannel(plane, w, h, options_.compress)});
        summary_.masks++;
    }

    Record base(const Layer& l) const {
        Record r;
        r.name = l.name;
        r.blend = blendKey(l.blendMode);
        r.opacity = uint8_t(std::clamp(std::lround(l.opacity * 255), 0L, 255L));
        r.hidden = !l.visible;
        return r;
    }

    /// Whether PSD's clipping (to the nearest unclipped layer below, in the same folder) says what ours does:
    /// every sibling between the base and this layer is clipped to the same base.
    bool clippingFits(const Layer& layer) const {
        if (!layer.maskSourceId) return false;
        auto siblings = children(layer.parentId);
        auto at = std::find(siblings.begin(), siblings.end(), &layer);
        while (at != siblings.begin()) {
            --at;
            const Layer& below = **at;
            if (below.id == *layer.maskSourceId) return !below.isGroup && !below.adjustment;
            if (below.isGroup || below.maskSourceId != layer.maskSourceId) return false;
        }
        return false;
    }

    /// The document rendered with only the given layers showing (and the folders that hold them), at 1:1.
    std::shared_ptr<Image> renderOnly(const std::vector<Uuid>& showing, bool keepClips, std::optional<Uuid> neutralise = std::nullopt, bool folderMasks = true) const {
        Document copy = doc_;
        std::map<Uuid, bool> keep;
        for (const Uuid& id : showing) {
            keep[id] = true;
            for (auto p = byId_.at(id)->parentId; p; p = byId_.at(*p)->parentId) keep[*p] = true;
        }
        for (Layer& l : copy.layers) {
            if (!keep.count(l.id)) l.visible = false;
            if (!keepClips) l.maskSourceId.reset();
            if (neutralise && l.id == *neutralise) { l.opacity = 1; l.blendMode = BlendMode::Normal; }
            if (!folderMasks && l.isGroup) l.mask.reset();
        }
        return renderFlattened(copy);
    }

    /// Every layer drawn before `layer` in the stack, and the layer: what an adjustment applies to.
    std::vector<Uuid> throughLayer(const Layer& layer) const {
        std::vector<Uuid> out;
        for (const Layer* l : renderLayers(doc_.layers)) { out.push_back(l->id); if (l->id == layer.id) break; }
        return out;
    }

    void emit(const std::optional<Uuid>& parent) {
        for (const Layer* layerPtr : children(parent)) {
            const Layer& l = *layerPtr;
            if (l.isGroup) { emitFolder(l); continue; }
            if (l.adjustment) { emitAdjustment(l); continue; }
            emitPixels(l);
        }
    }

    void emitFolder(const Layer& l) {
        Record end;
        end.name = "</Layer group>";
        end.section = 3;
        emptyChannels(end);
        records_.push_back(std::move(end));
        emit(l.id);
        Record folder = base(l);
        folder.section = 1;
        folder.blend = l.blendMode == BlendMode::Normal ? "pass" : blendKey(l.blendMode);   // our folders pass through
        emptyChannels(folder);
        setMask(folder, l, doc_.rect(), false);
        if (l.opacity < 1) summary_.warnings.push_back("Folder \"" + l.name + "\": its opacity (" + percent(l.opacity) + ") is written as it is set; NekoPhoto does not apply folder opacity, so Photoshop will show the folder fainter than it looks here.");
        if (l.blendMode != BlendMode::Normal) summary_.warnings.push_back("Folder \"" + l.name + "\": its blend mode (" + blendModeName(l.blendMode) + ") is written as it is set; NekoPhoto does not apply folder blend modes, so Photoshop will show the folder differently.");
        records_.push_back(std::move(folder));
        summary_.folders++;
    }

    /// Photoshop's own adjustment block for settings that mean the same there, or none.
    bool nativeAdjustment(const AdjustmentSettings& s, Record& r) const {
        Out o;
        switch (s.kind) {
        case AdjustmentKind::Levels: {
            o.u16(2);
            for (int i = 0; i < 29; i++) {
                LevelsRange range = i < 4 ? s.levels.ranges[size_t(i)] : LevelsRange{};
                o.u16(unsigned(std::lround(std::clamp(range.black, 0.0, 253.0))));
                o.u16(unsigned(std::lround(std::clamp(range.white, 2.0, 255.0))));
                o.u16(unsigned(std::lround(std::clamp(range.outputBlack, 0.0, 255.0))));
                o.u16(unsigned(std::lround(std::clamp(range.outputWhite, 0.0, 255.0))));
                o.u16(unsigned(std::lround(std::clamp(range.gamma, 0.1, 9.99) * 100)));
            }
            r.adjustmentKey = "levl";
            break;
        }
        case AdjustmentKind::Curves: {
            o.u8(0);
            o.u16(1);
            uint32_t mask = 0;
            for (int c = 0; c < 4; c++) if (s.curves.channels[size_t(c)].size() >= 2) mask |= 1u << c;
            o.u32(mask);
            for (int c = 0; c < 4; c++) {
                const auto& points = s.curves.channels[size_t(c)];
                if (points.size() < 2) continue;
                o.u16(unsigned(points.size()));
                for (const CurvePoint& p : points) { o.u16(unsigned(std::lround(std::clamp(p.y, 0.0, 255.0)))); o.u16(unsigned(std::lround(std::clamp(p.x, 0.0, 255.0)))); }
            }
            r.adjustmentKey = "curv";
            break;
        }
        case AdjustmentKind::Exposure:
            o.u16(1);
            o.f32(float(s.exposure.exposure)); o.f32(float(s.exposure.offset)); o.f32(float(s.exposure.gamma));
            r.adjustmentKey = "expA";
            break;
        case AdjustmentKind::HueSaturation: {
            // Only Photoshop's own saturation curve means the same there; the Mac's plain scale does not.
            if (!s.hsv.photoshopSaturation || s.hsv.invertRange) return false;
            auto value = [&](int range) { auto it = s.hsv.adjustments.find(range); return it == s.hsv.adjustments.end() ? RangeAdjustment{} : it->second; };
            o.u16(2);
            o.u8(s.hsv.colorize ? 1 : 0); o.u8(0);
            const RangeAdjustment master = value(0);
            if (s.hsv.colorize) { o.i16(int(std::lround(master.hue))); o.i16(int(std::lround(master.saturation))); o.i16(int(std::lround(master.lightness))); o.i16(0); o.i16(0); o.i16(0); }
            else { o.i16(0); o.i16(25); o.i16(0); o.i16(int(std::lround(master.hue))); o.i16(int(std::lround(master.saturation))); o.i16(int(std::lround(master.lightness))); }
            for (int range = 1; range <= 6; range++) {
                auto band = s.hsv.bands.find(range);
                const HueBand b = band == s.hsv.bands.end() ? HueBand::defaultBand(range) : band->second;
                for (double v : {b.falloffStart, b.rangeStart, b.rangeEnd, b.falloffEnd}) o.i16(int(std::lround(v)));
                const RangeAdjustment a = s.hsv.colorize ? RangeAdjustment{} : value(range);
                o.i16(int(std::lround(a.hue))); o.i16(int(std::lround(a.saturation))); o.i16(int(std::lround(a.lightness)));
            }
            r.adjustmentKey = "hue2";
            break;
        }
        default:
            return false;
        }
        r.adjustmentData = std::move(o.b);
        return true;
    }

    void emitAdjustment(const Layer& l) {
        AdjustmentSettings settings;
        const bool parsed = AdjustmentSettings::parse(l.adjustment->json, settings);
        Record r = base(l);
        if (parsed && nativeAdjustment(settings, r)) {
            r.clipping = l.maskSourceId && clippingFits(l);
            emptyChannels(r);
            setMask(r, l, doc_.rect(), false);
            summary_.adjustments++;
            if (r.clipping) summary_.clipped++;
            records_.push_back(std::move(r));
            return;
        }
        // No Photoshop equivalent: the adjusted look of everything beneath, as a pixel layer in its place.
        summary_.warnings.push_back("Adjustment \"" + l.name + "\" (" + adjustmentKindName(l.adjustment->kind) + ") has no Photoshop equivalent; its result is written as a pixel layer that covers the layers beneath it (they stay in the file).");
        Record baked;
        baked.name = l.name;
        baked.hidden = !l.visible;
        if (!l.visible) emptyChannels(baked);   // it showed nothing: an empty hidden layer keeps its place
        else if (encode_) setPixels(baked, *renderOnly(throughLayer(l), true), 0, 0);
        else { baked.right = doc_.width; baked.bottom = doc_.height; }
        records_.push_back(std::move(baked));
        summary_.layers++;
    }

    void emitPixels(const Layer& l) {
        Record r = base(l);
        const bool hasPixels = l.asset && l.asset->image && !l.asset->image->isEmpty();
        if (l.isLiveText()) summary_.notes.push_back("Text \"" + l.name + "\" is written as pixels; it stays editable text in the NekoPhoto project.");
        else if (l.isLiveShape()) summary_.notes.push_back("Shape \"" + l.name + "\" is written as pixels; it stays an editable shape in the NekoPhoto project.");
        const bool clipped = l.maskSourceId.has_value();
        const bool clipFits = clipped && clippingFits(l);
        if (!hasPixels) {
            emptyChannels(r);
            setMask(r, l, doc_.rect(), false);
            r.clipping = clipFits;
            summary_.layers++;
            if (r.clipping) summary_.clipped++;
            records_.push_back(std::move(r));
            return;
        }
        const Image& image = *l.asset->image;
        const LayerTransform& t = l.transform;
        const bool onGrid = t.rotation == 0 && !t.flipX && !t.flipY && t.origin.x == std::round(t.origin.x) && t.origin.y == std::round(t.origin.y)
            && t.size.width == image.width() && t.size.height == image.height();
        if (clipped && !clipFits) {
            // PSD clips only to the layer right beneath: this one is written as it shows, clip and mask applied.
            summary_.warnings.push_back("Layer \"" + l.name + "\" is clipped to a layer that is not right beneath it, which PSD cannot say; it is written as it shows, unclipped.");
            const Rect bounds = t.bounds().integral().intersection(doc_.rect());
            r.clipping = false;
            if (encode_ && !bounds.isEmpty()) {
                // Without the folders' masks: the layer stays in its folder, which applies them in Photoshop.
                auto own = renderOnly({l.id}, false, l.id, false);
                auto source = renderOnly({*l.maskSourceId}, true, std::nullopt, false);
                Image out(int(bounds.width), int(bounds.height));
                for (int y = 0; y < out.height(); y++) {
                    const uint8_t* s = own->pixel(int(bounds.x), int(bounds.y) + y);
                    const uint8_t* c = source->pixel(int(bounds.x), int(bounds.y) + y);
                    uint8_t* d = out.row(y);
                    for (int x = 0; x < out.width(); x++, s += 4, c += 4, d += 4)
                        for (int k = 0; k < 4; k++) d[k] = uint8_t((s[k] * c[3] + 127) / 255);
                }
                setPixels(r, out, int(bounds.x), int(bounds.y));
            } else { r.left = int(bounds.x); r.top = int(bounds.y); r.right = int(bounds.x + bounds.width); r.bottom = int(bounds.y + bounds.height); }
            records_.push_back(std::move(r));
            summary_.layers++;
            return;
        }
        r.clipping = clipFits;
        if (onGrid) {
            setPixels(r, image, int(t.origin.x), int(t.origin.y));
            setMask(r, l, Rect(t.origin.x, t.origin.y, image.width(), image.height()), true);
        } else {
            summary_.notes.push_back("Layer \"" + l.name + "\" is scaled, rotated or flipped; it is written resampled into place.");
            const Rect bounds = t.bounds().integral();
            if (encode_ && !bounds.isEmpty()) {
                LayerTransform target(Point(bounds.x, bounds.y), Size(bounds.width, bounds.height));
                auto placed = resampleLayer(l.asset->image, t, target, int(bounds.width), int(bounds.height));
                setPixels(r, *placed, int(bounds.x), int(bounds.y));
            } else { r.left = int(bounds.x); r.top = int(bounds.y); r.right = int(bounds.x + bounds.width); r.bottom = int(bounds.y + bounds.height); }
            setMask(r, l, bounds, false);
        }
        summary_.layers++;
        if (r.clipping) summary_.clipped++;
        records_.push_back(std::move(r));
    }
};

void writeRecord(Out& o, const Record& r) {
    o.i32(r.top); o.i32(r.left); o.i32(r.bottom); o.i32(r.right);
    o.u16(unsigned(r.channels.size()));
    for (auto& [id, data] : r.channels) { o.i16(id); o.u32(uint32_t(data.size())); }
    o.str("8BIM"); o.str(r.blend.c_str());
    o.u8(r.opacity); o.u8(r.clipping ? 1 : 0); o.u8(r.hidden ? 2 : 0); o.u8(0);
    Out extra;
    if (r.mask) {
        extra.u32(20);
        extra.i32(r.maskTop); extra.i32(r.maskLeft); extra.i32(r.maskBottom); extra.i32(r.maskRight);
        extra.u8(r.maskDefault); extra.u8(r.maskFlags); extra.u16(0);
    } else extra.u32(0);
    extra.u32(0);   // blending ranges
    const std::string ascii = asciiName(r.name);
    extra.u8(unsigned(ascii.size())); extra.str(ascii.c_str());
    for (size_t used = 1 + ascii.size(); used % 4; used++) extra.u8(0);
    auto block = [&](const char* key, const std::vector<uint8_t>& data) {
        extra.str("8BIM"); extra.str(key);
        extra.u32(uint32_t(data.size() + (data.size() & 1)));
        extra.bytes(data);
        if (data.size() & 1) extra.u8(0);
    };
    {
        Out u;
        const auto units = utf16(r.name);
        u.u32(uint32_t(units.size()));
        for (uint16_t c : units) u.u16(c);
        block("luni", u.b);
    }
    if (r.section) {
        Out s;
        s.u32(uint32_t(r.section));
        if (r.section == 1) { s.str("8BIM"); s.str(r.blend.c_str()); }
        block("lsct", s.b);
    }
    if (r.adjustmentKey) block(r.adjustmentKey, r.adjustmentData);
    o.u32(uint32_t(extra.b.size()));
    o.bytes(extra.b);
}

} // namespace

PsdExportSummary planPsdExport(const Document& document) {
    PsdExportSummary summary;
    PsdExportOptions options;
    Writer(document, options, false, summary).records();
    return summary;
}

std::vector<uint8_t> encodePsd(const Document& document, const PsdExportOptions& options, PsdExportSummary* summaryOut, std::string* error) {
    if (document.width > psdMaxSide || document.height > psdMaxSide || document.width < 1 || document.height < 1) {
        if (error) *error = "This document is larger than PSD allows (" + std::to_string(psdMaxSide) + " pixels a side). PSB export is not supported yet.";
        return {};
    }
    PsdExportSummary summary;
    std::vector<Record> records = Writer(document, options, true, summary).records();

    Out f;
    f.str("8BPS"); f.u16(1); for (int i = 0; i < 6; i++) f.u8(0);
    f.u16(4);   // RGB and the merged image's transparency
    f.u32(uint32_t(document.height)); f.u32(uint32_t(document.width));
    f.u16(8); f.u16(3);
    f.u32(0);   // colour mode data
    {
        // The resolution (ResolutionInfo, 0x03ED): pixels per inch as 16.16 fixed point, both axes.
        Out res;
        const uint32_t ppi = uint32_t(std::lround(std::clamp(document.resolution, 1.0, 30000.0) * 65536));
        res.str("8BIM"); res.u16(0x03ED); res.u8(0); res.u8(0); res.u32(16);
        res.u32(ppi); res.u16(1); res.u16(1); res.u32(ppi); res.u16(1); res.u16(1);
        f.u32(uint32_t(res.b.size())); f.bytes(res.b);
    }
    {
        Out info;
        // Negative: the merged image's first alpha channel is its transparency.
        info.i16(-int(records.size()));
        for (const Record& r : records) writeRecord(info, r);
        for (const Record& r : records) for (auto& [id, data] : r.channels) info.bytes(data);
        if (info.b.size() & 1) info.u8(0);
        Out section;
        section.u32(uint32_t(info.b.size()));
        section.bytes(info.b);
        section.u32(0);   // global layer mask info
        if (section.b.size() > 0xFFFFFFFFull) { if (error) *error = "The layers are too large for a PSD file."; return {}; }
        f.u32(uint32_t(section.b.size()));
        f.bytes(section.b);
    }
    {
        // The merged image from our own renderer, the look the layers should have. Its colour is matted
        // against white where it is transparent, as Photoshop stores it: premultiplied plus the white behind.
        auto flat = renderFlattened(document);
        std::array<std::vector<uint8_t>, 4> planes;
        for (auto& p : planes) p.resize(size_t(document.width) * document.height);
        parallelRows(0, document.height, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                const uint8_t* s = flat->row(y);
                for (int x = 0; x < document.width; x++, s += 4) {
                    const size_t i = size_t(y) * document.width + x;
                    planes[3][i] = s[3];
                    for (int c = 0; c < 3; c++) planes[size_t(c)][i] = uint8_t(std::min(255, int(s[c]) + 255 - int(s[3])));
                }
            }
        }, 64);
        const int w = document.width, h = document.height;
        std::vector<std::vector<uint8_t>> rows;
        rows.reserve(size_t(h) * 4);
        const int order[4] = {0, 1, 2, 3};
        bool rle = options.compress;
        if (rle) {
            rows.resize(size_t(h) * 4);
            parallelRows(0, h, [&](int ya, int yb) {
                for (int c : order) for (int y = ya; y < yb; y++) packBits(planes[size_t(c)].data() + size_t(y) * w, w, rows[size_t(c) * h + size_t(y)]);
            }, 64);
        }
        if (rle) {
            f.u16(1);
            for (auto& r : rows) f.u16(unsigned(r.size()));
            for (auto& r : rows) f.bytes(r);
        } else {
            f.u16(0);
            for (int c : order) f.bytes(planes[size_t(c)]);
        }
    }
    if (summaryOut) *summaryOut = summary;
    return std::move(f.b);
}

bool exportPsd(const Document& document, const std::string& path, const PsdExportOptions& options, PsdExportSummary* summary, std::string* error) {
    std::vector<uint8_t> bytes = encodePsd(document, options, summary, error);
    if (bytes.empty()) return false;
    const std::filesystem::path target(path);
    std::filesystem::path temp = target;
    temp += ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) { if (error) *error = "Couldn't write " + temp.string(); return false; }
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!out) { if (error) *error = "Couldn't write " + temp.string(); std::error_code ec; std::filesystem::remove(temp, ec); return false; }
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) { if (error) *error = "Couldn't write " + path + ": " + ec.message(); std::filesystem::remove(temp, ec); return false; }
    return true;
}

} // namespace compositor
