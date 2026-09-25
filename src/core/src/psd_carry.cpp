#include "compositor/psd_carry.h"
#include <cstring>
#include <set>

namespace compositor {

PsdLayerCarry::Binding PsdLayerCarry::binding(const std::string& key) {
    // Text, smart objects (placed, embedded, linked, with their filters), fill layers, and the adjustment
    // layers NekoPhoto has no counterpart for: each says what the layer is instead of its pixels.
    static const std::set<std::string> content{"TySh", "tySh", "SoLd", "SoLE", "PlLd", "plLd", "GdFl", "PtFl", "SoCo",
        "brit", "blwh", "vibA", "phfl", "mixr", "clrL", "nvrt", "post", "thrs", "selc", "blnc", "CgEd"};
    // A shape's origination (live-shape parameters): coordinates the writer cannot move. The vector mask itself
    // ('vmsk', 'vsms') follows its layer (the writer maps it); the stroke and fill content hold no coordinates.
    static const std::set<std::string> placement{"vogk", "vowv"};
    if (content.count(key)) return Binding::Content;
    if (placement.count(key)) return Binding::Placement;
    return Binding::Always;
}

uint64_t psdContentHash(const Image* image) {
    if (!image || image->isEmpty()) return 0;
    uint64_t h = 0x9e3779b97f4a7c15ull ^ (uint64_t(image->width()) << 32 | uint32_t(image->height()));
    auto mix = [&](uint64_t v) { h ^= v; h *= 0xff51afd7ed558ccdull; h ^= h >> 29; };
    const size_t rowBytes = size_t(image->width()) * 4;
    for (int y = 0; y < image->height(); y++) {
        const uint8_t* p = image->row(y);
        size_t i = 0;
        for (; i + 8 <= rowBytes; i += 8) { uint64_t v; std::memcpy(&v, p + i, 8); mix(v); }
        uint64_t tail = 0;
        std::memcpy(&tail, p + i, rowBytes - i);
        mix(tail ^ (uint64_t(y) << 48));
    }
    return h ? h : 1;
}

uint64_t psdMaskHash(const GrayImage* mask, bool enabled) {
    if (!mask || mask->width() <= 0 || mask->height() <= 0) return 0;
    uint64_t h = 0x9e3779b97f4a7c15ull ^ (uint64_t(mask->width()) << 32 | uint32_t(mask->height())) ^ (enabled ? 0 : 0x5555);
    auto mix = [&](uint64_t v) { h ^= v; h *= 0xff51afd7ed558ccdull; h ^= h >> 29; };
    const size_t rowBytes = size_t(mask->width());
    for (int y = 0; y < mask->height(); y++) {
        const uint8_t* p = mask->row(y);
        size_t i = 0;
        for (; i + 8 <= rowBytes; i += 8) { uint64_t v; std::memcpy(&v, p + i, 8); mix(v); }
        uint64_t tail = 0;
        std::memcpy(&tail, p + i, rowBytes - i);
        mix(tail ^ (uint64_t(y) << 48));
    }
    return h ? h : 1;
}

namespace {

constexpr uint32_t layerMagic = 0x4e50434c;   // NPCL
constexpr uint32_t documentMagic = 0x4e504344; // NPCD
constexpr uint32_t carryVersion = 1;

struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
    void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
    void f64(double v) { uint64_t u; std::memcpy(&u, &v, 8); u64(u); }
    void bytes(const std::vector<uint8_t>& v) { u64(v.size()); b.insert(b.end(), v.begin(), v.end()); }
    void str(const std::string& s) { bytes(std::vector<uint8_t>(s.begin(), s.end())); }
};

struct Reader {
    const std::vector<uint8_t>& b;
    size_t at = 0;
    bool ok = true;
    bool need(uint64_t n) { if (!ok || n > b.size() - at) ok = false; return ok; }
    uint8_t u8() { return need(1) ? b[at++] : 0; }
    uint32_t u32() { uint32_t v = 0; if (need(4)) for (int i = 0; i < 4; i++) v = v << 8 | b[at++]; return v; }
    uint64_t u64() { uint64_t hi = u32(); return hi << 32 | u32(); }
    double f64() { uint64_t u = u64(); double v; std::memcpy(&v, &u, 8); return v; }
    std::vector<uint8_t> bytes() { uint64_t n = u64(); if (!need(n)) return {}; std::vector<uint8_t> v(b.begin() + long(at), b.begin() + long(at + n)); at += n; return v; }
    std::string str() { auto v = bytes(); return std::string(v.begin(), v.end()); }
};

void writeBlocks(Writer& w, const std::vector<PsdBlock>& blocks) {
    w.u32(uint32_t(blocks.size()));
    for (const PsdBlock& block : blocks) { w.str(block.key); w.bytes(block.data); }
}

bool readBlocks(Reader& r, std::vector<PsdBlock>& blocks) {
    const uint32_t count = r.u32();
    for (uint32_t i = 0; i < count && r.ok; i++) {
        PsdBlock block;
        block.key = r.str();
        block.data = r.bytes();
        if (block.key.size() != 4) return false;
        blocks.push_back(std::move(block));
    }
    return r.ok;
}

} // namespace

std::vector<uint8_t> serializePsdCarry(const PsdLayerCarry& c) {
    Writer w;
    w.u32(layerMagic); w.u32(carryVersion);
    writeBlocks(w, c.blocks);
    w.bytes(c.blendingRanges);
    w.u8(c.opacity); w.u8(c.fill); w.u32(c.layerId);
    w.str(c.blendKey); w.u32(uint32_t(c.blendAs)); w.u8(c.flags); w.u8(c.closedFolder ? 1 : 0);
    w.u64(c.contentHash);
    const LayerTransform& t = c.placement;
    w.f64(t.origin.x); w.f64(t.origin.y); w.f64(t.size.width); w.f64(t.size.height); w.f64(t.rotation);
    w.u8(uint8_t((t.flipX ? 1 : 0) | (t.flipY ? 2 : 0)));
    w.bytes(c.maskData);
    w.u32(uint32_t(c.maskChannels.size()));
    for (auto& [id, data] : c.maskChannels) { w.u32(uint32_t(id)); w.bytes(data); }
    w.u64(c.maskHash);
    writeBlocks(w, c.endBlocks);
    w.bytes(c.endRanges);
    return std::move(w.b);
}

std::shared_ptr<const PsdLayerCarry> parsePsdLayerCarry(const std::vector<uint8_t>& bytes) {
    Reader r{bytes};
    if (r.u32() != layerMagic || r.u32() != carryVersion) return nullptr;
    auto c = std::make_shared<PsdLayerCarry>();
    if (!readBlocks(r, c->blocks)) return nullptr;
    c->blendingRanges = r.bytes();
    c->opacity = r.u8(); c->fill = r.u8(); c->layerId = r.u32();
    c->blendKey = r.str(); c->blendAs = int(int32_t(r.u32())); c->flags = r.u8(); c->closedFolder = r.u8() != 0;
    if (c->blendKey.size() != 4 && !c->blendKey.empty()) return nullptr;
    c->contentHash = r.u64();
    LayerTransform& t = c->placement;
    t.origin.x = r.f64(); t.origin.y = r.f64(); t.size.width = r.f64(); t.size.height = r.f64(); t.rotation = r.f64();
    const uint8_t flips = r.u8();
    t.flipX = flips & 1; t.flipY = flips & 2;
    c->maskData = r.bytes();
    const uint32_t channels = r.u32();
    for (uint32_t i = 0; i < channels && r.ok; i++) {
        const int id = int(int32_t(r.u32()));
        if (id != -2 && id != -3) return nullptr;
        c->maskChannels.push_back({id, r.bytes()});
    }
    c->maskHash = r.u64();
    if (!readBlocks(r, c->endBlocks)) return nullptr;
    c->endRanges = r.bytes();
    if (!r.ok || r.at != bytes.size()) return nullptr;
    return c;
}

std::vector<uint8_t> serializePsdCarry(const PsdDocumentCarry& c) {
    Writer w;
    w.u32(documentMagic); w.u32(carryVersion);
    w.u32(uint32_t(c.resources.size()));
    for (const auto& res : c.resources) { w.u32(res.id); w.str(res.name); w.bytes(res.data); }
    writeBlocks(w, c.globals);
    w.u32(uint32_t(c.width)); w.u32(uint32_t(c.height));
    return std::move(w.b);
}

std::shared_ptr<const PsdDocumentCarry> parsePsdDocumentCarry(const std::vector<uint8_t>& bytes) {
    Reader r{bytes};
    if (r.u32() != documentMagic || r.u32() != carryVersion) return nullptr;
    auto c = std::make_shared<PsdDocumentCarry>();
    const uint32_t count = r.u32();
    for (uint32_t i = 0; i < count && r.ok; i++) {
        PsdDocumentCarry::Resource res;
        const uint32_t id = r.u32();
        if (id > 0xFFFF) return nullptr;
        res.id = uint16_t(id);
        res.name = r.str();
        res.data = r.bytes();
        if (res.name.size() > 255) return nullptr;
        c->resources.push_back(std::move(res));
    }
    if (!readBlocks(r, c->globals)) return nullptr;
    c->width = int(r.u32()); c->height = int(r.u32());
    if (!r.ok || r.at != bytes.size()) return nullptr;
    return c;
}

} // namespace compositor
