#include "compositor/psd_writer.h"
#include "compositor/smartfilter.h"
#include "psd/psd_descriptor.hpp"
#include "compositor/adjustments.h"
#include "compositor/parallel.h"
#include "compositor/render.h"
#include "compositor/vectormask.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace compositor {

namespace {

/// Big-endian fields appended to a byte vector.
struct Out {
    std::vector<uint8_t> b;
    void u8(unsigned v) { b.push_back(uint8_t(v)); }
    void u16(unsigned v) { u8((v >> 8) & 0xff); u8(v & 0xff); }
    void i16(int v) { u16(unsigned(uint16_t(int16_t(v)))); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xffff); }
    void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
    /// A length: 64 bits in a PSB where Photoshop widens it, else 32.
    void length(uint64_t v, bool wide) { if (wide) u64(v); else u32(uint32_t(v)); }
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
/// The tagged blocks whose length is 64 bits in a PSB.
bool longKey(const std::string& key) {
    static const std::set<std::string> keys{"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD", "cinf"};   // cinf: Photoshop writes it wide in a PSB too (Patchy)
    return keys.count(key) > 0;
}

/// A PackBits row made even in length, decoding the same: one literal run split in two, else the row written
/// as literal runs sized to come out even. Photoshop rejects a smart object's embedded file whose merged image
/// has an odd row when the document keeps Smart Filter caches (Patchy's pinned rule); even rows everywhere.
void makeRowEven(std::vector<uint8_t>& row, const uint8_t* raw, int w) {
    if (row.size() % 2 == 0) return;
    for (size_t i = 0; i < row.size();) {
        const int8_t header = int8_t(row[i]);
        if (header >= 1) {   // a literal run of two or more: split its first byte off
            const size_t count = size_t(header) + 1;
            std::vector<uint8_t> out(row.begin(), row.begin() + long(i));
            out.push_back(0); out.push_back(row[i + 1]);
            out.push_back(uint8_t(count - 2)); out.insert(out.end(), row.begin() + long(i + 2), row.end());
            row.swap(out);
            return;
        }
        i += header >= 0 ? size_t(header) + 2 : header == -128 ? 1 : 2;
    }
    // No literal run to split: the row as literals, one chunk split short to fix the parity.
    std::vector<uint8_t> out;
    int x = 0;
    const int chunks = (w + 127) / 128;
    bool evenOut = (size_t(w) + size_t(chunks)) % 2 == 0;
    while (x < w) {
        int n = std::min(128, w - x);
        if (!evenOut && n >= 2) { n = 1; evenOut = true; }
        out.push_back(uint8_t(n - 1));
        out.insert(out.end(), raw + x, raw + x + n);
        x += n;
    }
    row.swap(out);
}

std::vector<uint8_t> encodeChannel(const std::vector<uint8_t>& plane, int w, int h, bool compress, bool large = false) {
    Out o;
    if (w <= 0 || h <= 0) { o.u16(0); return o.b; }
    if (compress) {
        std::vector<std::vector<uint8_t>> rows(static_cast<size_t>(h));
        size_t total = 0;
        parallelRows(0, h, [&](int ya, int yb) { for (int y = ya; y < yb; y++) packBits(plane.data() + size_t(y) * w, w, rows[size_t(y)]); }, 64);
        for (auto& r : rows) total += r.size();
        if (total + size_t(h) * (large ? 4 : 2) < plane.size()) {
            o.b.reserve(2 + size_t(h) * 4 + total);
            o.u16(1);
            for (auto& r : rows) { if (large) o.u32(uint32_t(r.size())); else o.u16(unsigned(r.size())); }
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
    case BlendMode::Dissolve: return "diss";
    case BlendMode::LinearBurn: return "lbrn";
    case BlendMode::DarkerColor: return "dkCl";
    case BlendMode::LinearDodge: return "lddg";
    case BlendMode::LighterColor: return "lgCl";
    case BlendMode::SoftLight: return "sLit";
    case BlendMode::HardLight: return "hLit";
    case BlendMode::VividLight: return "vLit";
    case BlendMode::LinearLight: return "lLit";
    case BlendMode::PinLight: return "pLit";
    case BlendMode::HardMix: return "hMix";
    case BlendMode::Exclusion: return "smud";
    case BlendMode::Subtract: return "fsub";
    case BlendMode::Divide: return "fdiv";
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


/// A layer record, ready to encode.
struct Record {
    int top = 0, left = 0, bottom = 0, right = 0;
    std::string name;
    std::string blend = "norm";
    uint8_t opacity = 255;
    bool clipping = false, hidden = false;
    uint8_t flags = 0;                     // besides hidden (bit 1) and bit 3, which are always written
    int section = 0;                                     // lsct: 1 folder, 3 its end marker
    std::vector<std::pair<int, std::vector<uint8_t>>> channels;   // id, encoded data
    bool mask = false;
    int maskTop = 0, maskLeft = 0, maskBottom = 0, maskRight = 0;
    uint8_t maskDefault = 255, maskFlags = 0;
    const char* adjustmentKey = nullptr;
    std::vector<uint8_t> adjustmentData;
    /// Carried blocks a freshly written adjustment replaces (a Brightness/Contrast's 'CgEd' beside its 'brit').
    std::set<std::string> replaces;
    // Carried from the PSD the layer came from (psd_carry.h).
    std::vector<PsdBlock> carried;
    std::vector<uint8_t> blendingRanges;
    std::vector<uint8_t> rawMask;          // the mask section as stored; its channels are in `channels`
    uint8_t fill = 255;                    // written as 'iOpa' below 255
    uint32_t layerId = 0;                  // 'lyid': assigned for every record
};

class Writer {
public:
    Writer(const Document& document, const PsdExportOptions& options, bool encode, PsdExportSummary& summary)
        : doc_(document), options_(options), encode_(encode), summary_(summary) {
        for (const Layer& l : doc_.layers) byId_[l.id] = &l;
    }

    std::vector<Record> records() {
        emit(std::nullopt);
        // Every record gets a unique 'lyid', keeping Photoshop's own where it is still unique.
        uint32_t next = 1;
        for (const Record& r : records_) next = std::max(next, r.layerId + 1);
        std::set<uint32_t> used;
        for (Record& r : records_) {
            if (r.layerId == 0 || !used.insert(r.layerId).second) { r.layerId = next++; used.insert(r.layerId); }
        }
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
        for (int id : {-1, 0, 1, 2}) if (id != -1 || alpha) r.channels.push_back({id, encodeChannel({}, 0, 0, false, options_.large)});
    }

    void setPixels(Record& r, const Image& image, int left, int top) const {
        r.left = left; r.top = top; r.right = left + image.width(); r.bottom = top + image.height();
        if (!encode_) return;
        auto planes = straightPlanes(image);
        r.channels.push_back({-1, encodeChannel(planes[3], image.width(), image.height(), options_.compress, options_.large)});
        for (int c = 0; c < 3; c++) r.channels.push_back({c, encodeChannel(planes[size_t(c)], image.width(), image.height(), options_.compress, options_.large)});
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
        if (encode_) r.channels.push_back({-2, encodeChannel(plane, w, h, options_.compress, options_.large)});
        summary_.masks++;
    }

    /// What the layer carries from its PSD, onto the finished record: the blocks still true of it, Blend
    /// If, Fill, its id, and the mask section as stored while the mask is unchanged. `sameContent` is false
    /// when the record's pixels are not the layer's own (written clipped or resampled).
    void applyCarry(Record& r, const Layer& l, bool sameContent = true) const {
        if (!l.psdCarry) return;
        const PsdLayerCarry& c = *l.psdCarry;
        const LayerTransform& t = l.transform;
        const bool placementKept = c.placement.origin == t.origin && c.placement.size == t.size && c.placement.rotation == t.rotation
            && c.placement.flipX == t.flipX && c.placement.flipY == t.flipY;
        const bool contentKept = sameContent && placementKept && c.contentHash == psdContentHash(l.asset ? l.asset->image.get() : nullptr);
        std::set<std::string> dropped;
        for (const PsdBlock& block : c.blocks) {
            const auto binding = PsdLayerCarry::binding(block.key);
            if ((binding == PsdLayerCarry::Binding::Content && !contentKept) || (binding == PsdLayerCarry::Binding::Placement && !placementKept)) { dropped.insert(block.key); continue; }
            if ((r.adjustmentKey && block.key == r.adjustmentKey) || r.replaces.count(block.key)) continue;
            if (!placementKept && (block.key == "vmsk" || block.key == "vsms")) {
                // The path follows the layer to where it now is.
                const int cw = doc_.psdCarry && doc_.psdCarry->width > 0 ? doc_.psdCarry->width : doc_.width;
                const int ch = doc_.psdCarry && doc_.psdCarry->height > 0 ? doc_.psdCarry->height : doc_.height;
                const int w0 = std::max(1, int(std::lround(c.placement.size.width))), h0 = std::max(1, int(std::lround(c.placement.size.height)));
                auto moved = mapVectorMask(block.data, cw, ch, [&](Point p) { return mapLayerPoint(p, c.placement, w0, h0, t, l.pixelWidth(), l.pixelHeight()); });
                if (moved) { r.carried.push_back({block.key, std::move(*moved)}); continue; }
                dropped.insert(block.key);
                continue;
            }
            r.carried.push_back(block);
        }
        auto droppedAny = [&](std::initializer_list<const char*> keys) { for (const char* k : keys) if (dropped.count(k)) return true; return false; };
        if (droppedAny({"TySh", "tySh"})) summary_.notes.push_back("Layer \"" + l.name + "\": its pixels changed here, so it is written as pixels, not editable text.");
        if (droppedAny({"SoLd", "SoLE", "PlLd", "plLd"})) summary_.notes.push_back("Layer \"" + l.name + "\": its pixels changed here, so it is written as pixels, not a smart object.");
        if (droppedAny({"GdFl", "PtFl", "SoCo"})) summary_.notes.push_back("Layer \"" + l.name + "\": its pixels changed here, so it is written as pixels, not a fill layer.");
        if (droppedAny({"vmsk", "vsms"})) summary_.warnings.push_back("Layer \"" + l.name + "\": its vector mask could not be moved with it, so it is left out.");
        if (droppedAny({"brit", "blwh", "vibA", "phfl", "mixr", "clrL", "nvrt", "post", "thrs", "selc", "blnc", "CgEd"}))
            summary_.warnings.push_back("Layer \"" + l.name + "\": it was painted on here, so the Photoshop adjustment it held is left out.");
        r.blendingRanges = c.blendingRanges;
        r.layerId = c.layerId;
        if (c.blendAs == int(l.blendMode) && c.blendKey.size() == 4 && (!l.isGroup || (c.blendKey == "pass") == l.passThrough)) r.blend = c.blendKey;
        // Transparency lock (bit 0) always; "pixel data irrelevant" (bit 4) while what makes it so is kept.
        r.flags = uint8_t((c.flags & 0x01) | (contentKept ? c.flags & 0x10 : 0));
        if (r.section == 1 && c.closedFolder) r.section = 2;
        // Opacity and Fill as they were while the combined opacity is unchanged; else ours alone.
        if (std::abs(l.opacity - c.opacity / 255.0 * (c.fill / 255.0)) < 0.5 / 255) { r.opacity = c.opacity; r.fill = c.fill; }
        const uint64_t maskHash = l.mask ? psdMaskHash(l.mask->asset.image.get(), l.mask->enabled) : 0;
        // The stored mask channels are PSD's (16-bit row counts): as they are into a PSD only.
        if (!options_.large && !c.maskData.empty() && placementKept && maskHash == c.maskHash && !dropped.count("vmsk") && !dropped.count("vsms")) {
            r.rawMask = c.maskData;
            r.channels.erase(std::remove_if(r.channels.begin(), r.channels.end(), [](auto& ch) { return ch.first == -2 || ch.first == -3; }), r.channels.end());
            if (encode_) for (auto& ch : c.maskChannels) r.channels.push_back(ch);
        }
    }

    std::set<std::string> placedIds_;

public:
    /// Smart Filter cache records written anew (placed id, record body), for the document's 'FEid' block.
    std::vector<std::pair<std::string, std::vector<uint8_t>>> filterRecords_;

private:

    /// A smart object still placed by the layer: its Photoshop blocks, the quad following the layer's transform.
    void applySmartObject(Record& r, const Layer& l, const Image& image) {
        if (!l.smartObject) return;
        const SmartObjectInstance& so = *l.smartObject;
        if (!l.isLiveSmartObject()) {
            summary_.notes.push_back("Layer \"" + l.name + "\": its pixels changed here, so it is written as pixels, not a smart object.");
            return;
        }
        const int w = image.width(), h = image.height();
        std::array<double, 8> quad{};
        if (!so.locked() && smartObjectPixelsArePlacement(so)) {
            const double corners[4][2] = {{0, 0}, {double(w), 0}, {double(w), double(h)}, {0, double(h)}};
            for (int i = 0; i < 4; i++) { const Point p = mapThroughTransform(l.transform, w, h, corners[i][0], corners[i][1]); quad[size_t(i * 2)] = p.x; quad[size_t(i * 2 + 1)] = p.y; }
        } else quad = moveQuad(so.quad, so.placedTransform, so.placedWidth, so.placedHeight, l.transform, w, h);
        // Moved: against the placement the blocks hold (a redrawn instance's own quad is already the new one).
        std::array<double, 8> stored = so.quad;
        for (const PsdBlock& b : so.psdBlocks)
            if (auto p = parsePsdPlacement(b.key, b.data)) { stored = p->quad; break; }
        bool moved = false;
        for (size_t i = 0; i < 8; i++) moved |= std::abs(quad[i] - stored[i]) > 1e-4;
        const bool filtered = smartObjectFiltered(so);
        auto source = doc_.smartObjects.find(so.sourceId);
        if (filtered && !so.locked() && source != doc_.smartObjects.end() && source->second->image && (moved || !source->second->psdElement)) {
            // Drawn here, and moved or given new contents: Photoshop's cache for it (the unfiltered pixels over the
            // canvas, and the shared mask) is written anew.
            auto cache = doc_.psdCarry ? findSmartFilterCache(doc_.psdCarry->globals, so.placedId) : std::nullopt;
            auto unfiltered = cache ? placedSmartObjectRaster(so, *source->second->image, quad) : std::nullopt;
            if (!unfiltered || !placedIds_.insert(so.placedId).second) {
                summary_.warnings.push_back("Layer \"" + l.name + "\": its Smart Filters' cache could not be rewritten, so it is written as pixels.");
                return;
            }
            uint8_t outside = 255;
            for (const PsdBlock& b : so.psdBlocks)
                if (auto stack = (b.key == "SoLd" || b.key == "SoLE") ? parseSmartFilterStack(b.key, b.data) : std::nullopt) { outside = stack->maskDefault; break; }
            filterRecords_.push_back({so.placedId, authorSmartFilterRecord(so.placedId, PixelRect{0, 0, doc_.width, doc_.height}, *unfiltered,
                                                                             cache->mask.get(), cache->maskBounds, outside)});
            for (const PsdBlock& b : so.psdBlocks) {
                auto patched = patchPsdPlacement(b.key, b.data, quad);
                if (patched) r.carried.push_back({b.key, std::move(*patched)});
            }
            summary_.smartObjects++;
            return;
        }
        if (moved && filtered) {
            // Smart Filters keep a document-space cache Photoshop checks against the placement.
            summary_.warnings.push_back("Layer \"" + l.name + "\": a smart object with Smart Filters moved here, so it is written as pixels.");
            return;
        }
        // A duplicate needs its own instance id (Photoshop aliases layers that share one).
        std::string placed;
        if (!so.placedId.empty() && !placedIds_.insert(so.placedId).second) {
            if (filtered) {
                summary_.warnings.push_back("Layer \"" + l.name + "\": a copy of a smart object with Smart Filters is written as pixels.");
                return;
            }
            placed = makeUuid();
            std::transform(placed.begin(), placed.end(), placed.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        }
        for (const PsdBlock& b : so.psdBlocks) {
            if (!moved && placed.empty()) { r.carried.push_back(b); continue; }
            auto patched = patchPsdPlacement(b.key, b.data, quad, placed);
            if (patched) r.carried.push_back({b.key, std::move(*patched)});
        }
        summary_.smartObjects++;
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
        if (l.psdCarry) { end.carried = l.psdCarry->endBlocks; end.blendingRanges = l.psdCarry->endRanges; }
        records_.push_back(std::move(end));
        emit(l.id);
        Record folder = base(l);
        folder.section = 1;
        folder.blend = l.passThrough ? "pass" : blendKey(l.blendMode);
        emptyChannels(folder);
        setMask(folder, l, doc_.rect(), false);
        applyCarry(folder, l);
        records_.push_back(std::move(folder));
        summary_.folders++;
    }

    /// Photoshop's own adjustment block for settings that mean the same there, or none.
    bool nativeAdjustment(const AdjustmentSettings& s, Record& r, const Layer& l) const {
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
            o.u16(0);   // Photoshop's block is 16 bytes: two zero bytes after the gamma
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
            return otherAdjustment(s, r, l);
        }
        r.adjustmentData = std::move(o.b);
        return true;
    }

    /// Photoshop's other adjustment layers: the file's own block while the settings are what it said (byte for byte),
    /// otherwise one written anew in Photoshop's layout.
    bool otherAdjustment(const AdjustmentSettings& s, Record& r, const Layer& l) const {
        static const std::map<AdjustmentKind, const char*> keys{{AdjustmentKind::Invert, "nvrt"}, {AdjustmentKind::BrightnessContrast, "brit"},
            {AdjustmentKind::Posterize, "post"}, {AdjustmentKind::Threshold, "thrs"}, {AdjustmentKind::BlackWhite, "blwh"}, {AdjustmentKind::ColorBalance, "blnc"},
            {AdjustmentKind::Vibrance, "vibA"}, {AdjustmentKind::PhotoFilter, "phfl"}, {AdjustmentKind::ChannelMixer, "mixr"}, {AdjustmentKind::SelectiveColor, "selc"},
            {AdjustmentKind::ColorLookup, "clrL"}};
        auto key = keys.find(s.kind);
        if (key == keys.end()) return false;
        // Unchanged since the file was read: its carried block goes back as it was.
        if (l.psdCarry && !l.psdCarry->adjustmentJson.empty() && l.psdCarry->adjustmentJson == s.toJson()) {
            for (const PsdBlock& b : l.psdCarry->blocks) if (b.key == key->second) return true;
        }
        Out o;
        auto descriptor = [&](const patchy::psd::DescriptorObject& d) {
            patchy::psd::BigEndianWriter w;
            w.write_u32(16);
            patchy::psd::write_descriptor(w, d);
            o.bytes(w.bytes());
        };
        using DV = patchy::psd::DescriptorValue;
        auto add = [](patchy::psd::DescriptorObject& d, const char* k, bool longForm, DV v) { d.key_order.push_back({k, longForm}); d.values[k] = std::move(v); };
        auto integer = [](int v) { DV x; x.type = DV::Type::Integer; x.integer_value = v; return x; };
        auto boolean = [](bool v) { DV x; x.type = DV::Type::Bool; x.bool_value = v; return x; };
        auto dbl = [](double v) { DV x; x.type = DV::Type::Double; x.double_value = v; return x; };
        switch (s.kind) {
        case AdjustmentKind::Invert: break;   // no data
        case AdjustmentKind::Posterize: o.u16(unsigned(std::clamp(s.posterize.levels, 2, 255))); o.u16(0); break;
        case AdjustmentKind::Threshold: o.u16(unsigned(std::clamp(s.threshold.level, 1, 255))); o.u16(0); break;
        case AdjustmentKind::ColorBalance:
            for (const auto& range : s.colorBalance.ranges) for (double v : range) o.i16(int(std::lround(std::clamp(v, -100.0, 100.0))));
            o.u8(s.colorBalance.preserveLuminosity ? 1 : 0); o.u8(0);
            break;
        case AdjustmentKind::BrightnessContrast: {
            const BrightnessContrastSettings b = s.brightnessContrast.normalized();
            // Legacy: the values in 'brit'. Modern: an all-zero 'brit' beside Photoshop 2026's 'CgEd' descriptor (Patchy's shape).
            if (b.legacy) { o.i16(b.brightness); o.i16(b.contrast); o.u16(127); o.u8(0); o.u8(0); }
            else {
                o.u16(0); o.u16(0); o.u16(0); o.u8(0); o.u8(0);
                patchy::psd::DescriptorObject d;
                d.class_id = "null";
                add(d, "Vrsn", false, integer(1)); add(d, "Brgh", false, integer(b.brightness)); add(d, "Cntr", false, integer(b.contrast));
                add(d, "means", true, integer(127)); add(d, "Lab ", false, boolean(false)); add(d, "useLegacy", true, boolean(false)); add(d, "Auto", false, boolean(false));
                patchy::psd::BigEndianWriter w;
                w.write_u32(16);
                patchy::psd::write_descriptor(w, d);
                r.carried.push_back({"CgEd", w.bytes()});
            }
            r.replaces.insert("CgEd");
            break;
        }
        case AdjustmentKind::ChannelMixer: {
            o.u16(1); o.u16(s.channelMixer.monochrome ? 1 : 0);
            // Four output records of red, green, blue, (CMYK's fourth), constant; monochrome's grey in the first.
            for (int out = 0; out < 4; out++) {
                const auto& row = s.channelMixer.monochrome ? (out == 0 ? s.channelMixer.rows[3] : std::array<double, 4>{}) : (out < 3 ? s.channelMixer.rows[size_t(out)] : std::array<double, 4>{});
                for (int k = 0; k < 3; k++) o.i16(int(std::lround(std::clamp(row[size_t(k)], -200.0, 200.0))));
                o.i16(0);
                o.i16(int(std::lround(std::clamp(row[3], -200.0, 200.0))));
            }
            break;
        }
        case AdjustmentKind::SelectiveColor:
            o.u16(1); o.u16(s.selectiveColor.absolute ? 1 : 0);
            for (int k = 0; k < 4; k++) o.i16(0);   // the reserved first record
            for (const auto& range : s.selectiveColor.ranges) for (double v : range) o.i16(int(std::lround(std::clamp(v, -100.0, 100.0))));
            break;
        case AdjustmentKind::PhotoFilter: {
            // Version 2 with an RGB colour (version 3's XYZ scale is not documented).
            const AdjustmentColor c = s.photoFilter.color.clamped();
            o.u16(2); o.u16(0);
            for (double v : {c.red, c.green, c.blue}) o.u16(unsigned(std::lround(v * 65535)));
            o.u16(0);
            o.u32(uint32_t(std::lround(std::clamp(s.photoFilter.density, 0.0, 100.0))));
            o.u8(s.photoFilter.preserveLuminosity ? 1 : 0);
            break;
        }
        case AdjustmentKind::Vibrance: {
            patchy::psd::DescriptorObject d;
            d.class_id = "null";
            add(d, "vibrance", true, integer(int(std::lround(s.vibrance.vibrance))));
            add(d, "Strt", false, integer(int(std::lround(s.vibrance.saturation))));
            descriptor(d);
            break;
        }
        case AdjustmentKind::BlackWhite: {
            patchy::psd::DescriptorObject d;
            d.class_id = "null";
            const char* names[6] = {"Rd  ", "Yllw", "Grn ", "Cyn ", "Bl  ", "Mgnt"};
            for (size_t i = 0; i < 6; i++) add(d, names[i], false, integer(int(std::lround(s.blackWhite.weights[i]))));
            add(d, "useTint", true, boolean(s.blackWhite.tint));
            DV colour; colour.type = DV::Type::Object; colour.object_value = std::make_shared<patchy::psd::DescriptorObject>();
            colour.object_value->class_id = "RGBC";
            const AdjustmentColor t = s.blackWhite.tintColor.clamped();
            add(*colour.object_value, "Rd  ", false, dbl(t.red * 255)); add(*colour.object_value, "Grn ", false, dbl(t.green * 255)); add(*colour.object_value, "Bl  ", false, dbl(t.blue * 255));
            add(d, "tintColor", true, colour);
            add(d, "bwPresetKind", true, integer(1));
            descriptor(d);
            break;
        }
        case AdjustmentKind::ColorLookup: {
            // Photoshop's descriptor with the LUT file embedded (or the profile), after a version.
            const ColorLookupSettings& c = s.colorLookup;
            if (c.format.empty() || !colorLookupReadable(c)) return false;
            o.u16(1);
            patchy::psd::DescriptorObject d;
            d.class_id = "null";
            auto text = [](const std::string& v) { DV x; x.type = DV::Type::String; x.string_value = v; return x; };
            auto raw = [](std::vector<uint8_t> v) { DV x; x.type = DV::Type::Raw; x.raw_value = std::move(v); return x; };
            auto enumeration = [](const char* type, const char* value) {
                DV x; x.type = DV::Type::Enum; x.enum_type = type; x.enum_type_long_form = true; x.enum_value = value; x.enum_value_long_form = true; return x;
            };
            add(d, "Vrsn", false, integer(1));
            const bool icc = c.format == "icc";
            add(d, "lookupType", true, enumeration("colorLookupType", icc ? "abstractProfile" : "3DLUT"));
            add(d, "Nm  ", false, text(c.name));
            add(d, "Dthr", false, boolean(c.dither));
            if (icc) {
                auto bytes = fromBase64(c.data);
                if (!bytes) return false;
                add(d, "profile", true, raw(*bytes));
            } else {
                add(d, "LUTFormat", true, enumeration("LUTFormatType", c.format == "3dl" ? "LUTFormat3DL" : "LUTFormatCUBE"));
                add(d, "dataOrder", true, enumeration("colorDataOrder", "rgbOrder"));
                add(d, "tableOrder", true, enumeration("colorTableOrder", "bgrOrder"));
                add(d, "LUT3DFileData", true, raw(std::vector<uint8_t>(c.data.begin(), c.data.end())));
                add(d, "LUT3DFileName", true, text(c.name));
            }
            descriptor(d);
            break;
        }
        default: return false;
        }
        r.adjustmentKey = key->second;
        r.adjustmentData = std::move(o.b);
        return true;
    }

    void emitAdjustment(const Layer& l) {
        AdjustmentSettings settings;
        const bool parsed = AdjustmentSettings::parse(l.adjustment->json, settings);
        Record r = base(l);
        if (parsed && nativeAdjustment(settings, r, l)) {
            r.clipping = l.maskSourceId && clippingFits(l);
            emptyChannels(r);
            setMask(r, l, doc_.rect(), false);
            summary_.adjustments++;
            if (r.clipping) summary_.clipped++;
            applyCarry(r, l);
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
        std::optional<PsdTextMetrics> textMetrics;
        if (l.isLiveText() && options_.textMetrics) textMetrics = options_.textMetrics(*l.text);
        if (l.isLiveText() && !textMetrics) summary_.notes.push_back("Text \"" + l.name + "\" is written as pixels; it stays editable text in the NekoPhoto project.");
        else if (l.isLiveShape()) summary_.notes.push_back("Shape \"" + l.name + "\" is written as pixels; it stays an editable shape in the NekoPhoto project.");
        const bool clipped = l.maskSourceId.has_value();
        const bool clipFits = clipped && clippingFits(l);
        if (!hasPixels) {
            emptyChannels(r);
            setMask(r, l, doc_.rect(), false);
            r.clipping = clipFits;
            summary_.layers++;
            if (r.clipping) summary_.clipped++;
            applyCarry(r, l);
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
            applyCarry(r, l, false);
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
        applyCarry(r, l, onGrid);
        applySmartObject(r, l, image);
        // Photoshop's own type layer, still true of the pixels, says more than ours can (several styles, warps).
        if (textMetrics && std::any_of(r.carried.begin(), r.carried.end(), [](const PsdBlock& b) { return b.key == "TySh"; })) { textMetrics.reset(); summary_.texts++; }
        if (textMetrics) {
            const Rect bounds(r.left, r.top, r.right - r.left, r.bottom - r.top);
            if (auto block = photoshopTypeBlock(*l.text, *textMetrics, t, image.width(), image.height(), bounds)) {
                r.carried.push_back({"TySh", std::move(*block)});
                summary_.texts++;
            } else summary_.notes.push_back("Text \"" + l.name + "\" is flipped, which Photoshop text cannot be; it is written as pixels.");
        }
        records_.push_back(std::move(r));
    }
};

void writeRecord(Out& o, const Record& r, bool large) {
    o.i32(r.top); o.i32(r.left); o.i32(r.bottom); o.i32(r.right);
    o.u16(unsigned(r.channels.size()));
    for (auto& [id, data] : r.channels) { o.i16(id); o.length(data.size(), large); }
    // A pass-through folder says so in 'lsct' only; its record says Normal, as Photoshop writes it.
    o.str("8BIM"); o.str((r.section && r.blend == "pass") ? "norm" : r.blend.c_str());
    o.u8(r.opacity); o.u8(r.clipping ? 1 : 0); o.u8(0x08 | r.flags | (r.hidden ? 2 : 0)); o.u8(0);   // bit 3: bit 4 is meaningful; Photoshop applies legacy semantics without it
    Out extra;
    if (!r.rawMask.empty()) {
        extra.u32(uint32_t(r.rawMask.size()));
        extra.bytes(r.rawMask);
    } else if (r.mask) {
        extra.u32(20);
        extra.i32(r.maskTop); extra.i32(r.maskLeft); extra.i32(r.maskBottom); extra.i32(r.maskRight);
        extra.u8(r.maskDefault); extra.u8(r.maskFlags); extra.u16(0);
    } else extra.u32(0);
    extra.u32(uint32_t(r.blendingRanges.size()));   // blending ranges (Blend If)
    extra.bytes(r.blendingRanges);
    const std::string ascii = asciiName(r.name);
    extra.u8(unsigned(ascii.size())); extra.str(ascii.c_str());
    for (size_t used = 1 + ascii.size(); used % 4; used++) extra.u8(0);
    auto block = [&](const char* key, const std::vector<uint8_t>& data) {
        extra.str("8BIM"); extra.str(key);
        extra.length(data.size() + (data.size() & 1), large && longKey(key));
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
    {
        Out id;
        id.u32(r.layerId);
        block("lyid", id.b);
    }
    if (r.fill < 255) {
        Out f;
        f.u8(r.fill); f.u8(0); f.u8(0); f.u8(0);
        block("iOpa", f.b);
    }
    if (r.section) {
        Out s;
        s.u32(uint32_t(r.section));
        if (r.section == 1 || r.section == 2) { s.str("8BIM"); s.str(r.blend.c_str()); }
        block("lsct", s.b);
    }
    if (r.adjustmentKey) block(r.adjustmentKey, r.adjustmentData);
    for (const PsdBlock& carried : r.carried) block(carried.key.c_str(), carried.data);
    o.u32(uint32_t(extra.b.size()));
    o.bytes(extra.b);
}

} // namespace

PsdExportSummary planPsdExport(const Document& document, const PsdExportOptions& options) {
    PsdExportSummary summary;
    Writer(document, options, false, summary).records();
    return summary;
}

std::vector<uint8_t> encodePsd(const Document& document, const PsdExportOptions& options, PsdExportSummary* summaryOut, std::string* error) {
    const bool large = options.large;
    const int maxSide = large ? psbMaxSide : psdMaxSide;
    if (document.width > maxSide || document.height > maxSide || document.width < 1 || document.height < 1) {
        if (error) *error = "This document is larger than PSD allows (" + std::to_string(psdMaxSide) + " pixels a side); export it as PSB.";
        return {};
    }
    PsdExportSummary summary;
    Writer writer(document, options, true, summary);
    std::vector<Record> records = writer.records();

    Out f;
    f.str("8BPS"); f.u16(large ? 2 : 1); for (int i = 0; i < 6; i++) f.u8(0);
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
        // The PSD's own resources, when the document came from one.
        if (document.psdCarry) {
            const PsdDocumentCarry& c = *document.psdCarry;
            const bool sameCanvas = c.width == document.width && c.height == document.height;
            for (const auto& resource : c.resources) {
                // Guides, slices and paths (stored relative to the canvas size) belong to the canvas they were made on.
                const bool canvasBound = resource.id == 1032 || resource.id == 1050 || resource.id == 1025 || (resource.id >= 2000 && resource.id <= 2999);
                if (!sameCanvas && canvasBound) continue;
                res.str("8BIM"); res.u16(resource.id);
                const std::string name = resource.name.substr(0, 255);
                res.u8(unsigned(name.size())); res.bytes(std::vector<uint8_t>(name.begin(), name.end()));
                if ((name.size() + 1) & 1) res.u8(0);
                res.u32(uint32_t(resource.data.size())); res.bytes(resource.data);
                if (resource.data.size() & 1) res.u8(0);
            }
        }
        f.u32(uint32_t(res.b.size())); f.bytes(res.b);
    }
    {
        Out info;
        // Negative: the merged image's first alpha channel is its transparency.
        info.i16(-int(records.size()));
        for (const Record& r : records) writeRecord(info, r, large);
        for (const Record& r : records) for (auto& [id, data] : r.channels) info.bytes(data);
        if (info.b.size() & 1) info.u8(0);
        Out section;
        section.length(info.b.size(), large);
        section.bytes(info.b);
        section.u32(0);   // global layer mask info
        // Global blocks from the PSD the document came from (linked smart object data, patterns, text
        // engine data), each padded to four bytes outside its declared length, as Photoshop reads them.
        // Smart object sources: while every embedded one is as it was read, the file's own 'lnk2' goes back as it
        // was; otherwise 'lnk2' is rebuilt, unchanged elements byte for byte and new or edited ones written anew.
        bool rebuildLinks = false;
        for (auto& [id, source] : document.smartObjects)
            rebuildLinks |= source->kind == SmartObjectSource::Kind::Embedded && !source->psdElement;
        std::vector<uint8_t> links;
        if (rebuildLinks)
            for (auto& [id, source] : document.smartObjects) {
                if (!source->psdBlock.empty() && source->psdBlock != "lnk2") continue;   // stays in its own block
                if (source->psdElement) links.insert(links.end(), source->psdElement->begin(), source->psdElement->end());
                else if (source->kind == SmartObjectSource::Kind::Embedded) { auto e = psdEmbeddedElement(*source); links.insert(links.end(), e.begin(), e.end()); }
            }
        if (document.psdCarry) for (const PsdBlock& stored : document.psdCarry->globals) {
            if (rebuildLinks && stored.key == "lnk2") continue;
            PsdBlock block = stored;
            if (!writer.filterRecords_.empty() && (block.key == "FEid" || block.key == "FXid"))
                if (auto replaced = replaceSmartFilterRecords(block.data, writer.filterRecords_)) block.data = std::move(*replaced);
            section.str("8BIM"); section.str(block.key.c_str());
            section.length(block.data.size(), large && longKey(block.key)); section.bytes(block.data);
            for (size_t n = block.data.size(); n % 4; n++) section.u8(0);
        }
        if (!links.empty()) {
            section.str("8BIM"); section.str("lnk2");
            section.length(links.size(), large); section.bytes(links);
            for (size_t n = links.size(); n % 4; n++) section.u8(0);
        }
        if (!large && section.b.size() > 0xFFFFFFFFull) { if (error) *error = "The layers are too large for a PSD file; export it as PSB."; return {}; }
        f.length(section.b.size(), large);
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
                for (int c : order) for (int y = ya; y < yb; y++) {
                    const uint8_t* raw = planes[size_t(c)].data() + size_t(y) * w;
                    packBits(raw, w, rows[size_t(c) * h + size_t(y)]);
                    makeRowEven(rows[size_t(c) * h + size_t(y)], raw, w);
                }
            }, 64);
        }
        if (rle) {
            f.u16(1);
            for (auto& r : rows) { if (large) f.u32(uint32_t(r.size())); else f.u16(unsigned(r.size())); }
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
