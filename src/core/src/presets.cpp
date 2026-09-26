// Photoshop preset libraries (.asl, .pat, .grd). The envelopes, the pattern record shapes and the gradient
// descriptors are ported from Patchy (MIT, src/third_party/patchy_psd/README.md): src/psd/asl_io.cpp,
// pat_reader.cpp, grd_io.cpp and psd_patterns.cpp there. The effects inside a style are read by the lfx2 parser
// (layerstyle.cpp) and pattern pixels by the 'Patt' decoder, since an .asl's 'Lefx' and a .pat's records are the same
// structures a PSD carries.
#include "compositor/presets.h"
#include "compositor/document.h"
#include "compositor/psd_carry.h"
#include "compositor/uuid.h"
#include "psd/psd_descriptor.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <set>

namespace compositor {

namespace psd = patchy::psd;

namespace {

constexpr uint32_t kModeIndexed = 2, kModeRgb = 3;
constexpr size_t kMaxFileBytes = 256u << 20;
constexpr uint32_t kMaxCount = 10000;

void note(std::vector<std::string>* notes, std::string text) { if (notes) notes->push_back(std::move(text)); }
void fail(std::string* error, const char* text) { if (error) *error = text; }

/// "$$$/Presets/Gradients/Name=Red, Green" → "Red, Green": Photoshop's localisable names.
std::string displayName(std::string name) {
    if (name.rfind("$$$/", 0) == 0) {
        const auto equals = name.rfind('=');
        if (equals != std::string::npos && equals + 1 < name.size()) name.erase(0, equals + 1);
    }
    return name;
}

// ---- Pattern records -------------------------------------------------------------------------------------------

/// A pattern record's header: everything before its pixels (the Virtual Memory Array).
struct RecordHeader {
    uint32_t version = 0, mode = 0;
    int width = 0, height = 0;
    std::string name, id;
    size_t headerEnd = 0;   // offset of the VMA within the record (after the palette, for indexed patterns)
};

RecordHeader readRecordHeader(psd::BigEndianReader& r, size_t start) {
    RecordHeader h;
    h.version = r.read_u32();
    h.mode = r.read_u32();
    h.height = r.read_u16();
    h.width = r.read_u16();
    h.name = psd::read_descriptor_unicode_string(r);
    auto id = r.read_span(r.read_u8());
    h.id.assign(id.begin(), id.end());
    while (!h.id.empty() && h.id.back() == '\0') h.id.pop_back();
    if (h.mode == kModeIndexed) r.skip(768);
    h.headerEnd = r.position() - start;
    return h;
}

/// A record body (no length prefix) as a 'Patt' block entry: u32 length, the body, padding to four bytes.
std::vector<uint8_t> blockEntry(const std::vector<uint8_t>& body) {
    // Sized once and filled in place (GCC 11 misreads an insert after the four length bytes as an overread).
    const uint32_t n = uint32_t(body.size());
    std::vector<uint8_t> out((4 + body.size() + 3) / 4 * 4, 0);
    out[0] = uint8_t(n >> 24); out[1] = uint8_t(n >> 16); out[2] = uint8_t(n >> 8); out[3] = uint8_t(n);
    if (!body.empty()) std::memcpy(out.data() + 4, body.data(), body.size());
    return out;
}

// ---- Descriptors ---------------------------------------------------------------------------------------------------

psd::DescriptorValue textValue(std::string v) { psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::String; d.string_value = std::move(v); return d; }
psd::DescriptorValue integerValue(int32_t v) { psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::Integer; d.integer_value = v; return d; }
psd::DescriptorValue doubleValue(double v) { psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::Double; d.double_value = v; return d; }
psd::DescriptorValue percentValue(double v) { psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::UnitFloat; d.unit = "#Prc"; d.double_value = v; return d; }
psd::DescriptorValue enumValue(std::string type, std::string v) {
    psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::Enum; d.enum_type = std::move(type); d.enum_value = std::move(v); return d;
}
psd::DescriptorValue objectValue(psd::DescriptorObject o) {
    psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::Object; d.object_value = std::make_shared<psd::DescriptorObject>(std::move(o)); return d;
}
void append(psd::DescriptorObject& o, std::string key, psd::DescriptorValue v) {
    o.key_order.push_back({key, key.size() != 4});
    o.values.emplace(std::move(key), std::move(v));
}

std::string textOf(const psd::DescriptorObject& o, const char* key) {
    auto v = psd::descriptor_value(o, key);
    return v && v->type == psd::DescriptorValue::Type::String ? v->string_value : std::string();
}
std::string enumOf(const psd::DescriptorObject& o, const char* key, const char* fallback) {
    auto v = psd::descriptor_value(o, key);
    return v && v->type == psd::DescriptorValue::Type::Enum ? v->enum_value : fallback;
}

uint8_t byte(double v) { return uint8_t(std::clamp(std::lround(std::isfinite(v) ? v : 0.0), 0L, 255L)); }

/// A colour object as RGB: 'RGBC', 'HSBC', 'CMYC' (a plain ink mix) and 'Grsc' (the ink percentage).
StyleColor colorOf(const psd::DescriptorObject& c) {
    if (c.class_id == "Grsc") { const uint8_t g = byte(255 * (1 - psd::descriptor_number(c, "Gry ") / 100.0)); return {g, g, g}; }
    if (c.class_id == "CMYC") {
        auto ink = [&](const char* k) { return std::clamp(psd::descriptor_number(c, k) / 100.0, 0.0, 1.0); };
        const double k = ink("Blck");
        return {byte(255 * (1 - ink("Cyn ")) * (1 - k)), byte(255 * (1 - ink("Mgnt")) * (1 - k)), byte(255 * (1 - ink("Ylw ")) * (1 - k))};
    }
    if (c.class_id == "HSBC") {
        const double h = std::fmod(std::fmod(psd::descriptor_number(c, "H   "), 360.0) + 360.0, 360.0) / 60.0;
        const double s = std::clamp(psd::descriptor_number(c, "Strt") / 100.0, 0.0, 1.0), v = std::clamp(psd::descriptor_number(c, "Brgh") / 100.0, 0.0, 1.0);
        const double chroma = v * s, x = chroma * (1 - std::abs(std::fmod(h, 2.0) - 1)), m = v - chroma;
        double rgb[3] = {0, 0, 0};
        const int sector = int(h) % 6;
        const double table[6][3] = {{chroma, x, 0}, {x, chroma, 0}, {0, chroma, x}, {0, x, chroma}, {x, 0, chroma}, {chroma, 0, x}};
        for (int i = 0; i < 3; i++) rgb[i] = (table[sector][i] + m) * 255;
        return {byte(rgb[0]), byte(rgb[1]), byte(rgb[2])};
    }
    return {byte(psd::descriptor_number(c, "Rd  ")), byte(psd::descriptor_number(c, "Grn ")), byte(psd::descriptor_number(c, "Bl  "))};
}

float unit(double v) { return std::isfinite(v) ? std::clamp(float(v), 0.0f, 1.0f) : 0.0f; }

std::optional<GradientPreset> gradientFrom(const psd::DescriptorObject& wrapper, const std::string& fallback, std::vector<std::string>* notes) {
    const psd::DescriptorObject* source = psd::descriptor_object(wrapper, "Grad");
    if (!source) source = &wrapper;
    GradientPreset g;
    g.name = displayName(textOf(*source, "Nm  "));
    if (g.name.empty()) g.name = fallback;
    if (enumOf(*source, "GrdF", "CstS") == "ClNs") { note(notes, g.name + ": a noise gradient, left out"); return std::nullopt; }
    g.smoothness = unit(psd::descriptor_number(*source, "Intr", 4096) / 4096);
    if (auto list = psd::descriptor_value(*source, "Clrs"); list && list->type == psd::DescriptorValue::Type::List)
        for (auto& item : list->list_value) {
            if (item.type != psd::DescriptorValue::Type::Object || !item.object_value || g.colors.size() >= 256) continue;
            const auto& s = *item.object_value;
            GradientPreset::Color c;
            const std::string type = enumOf(s, "Type", "UsrS");
            c.source = type == "FrgC" ? GradientPreset::Source::Foreground : type == "BckC" ? GradientPreset::Source::Background : GradientPreset::Source::User;
            if (auto color = psd::descriptor_object(s, "Clr ")) c.color = colorOf(*color);
            c.location = unit(psd::descriptor_number(s, "Lctn") / 4096);
            c.midpoint = unit(psd::descriptor_number(s, "Mdpn", 50) / 100);
            g.colors.push_back(c);
        }
    if (auto list = psd::descriptor_value(*source, "Trns"); list && list->type == psd::DescriptorValue::Type::List)
        for (auto& item : list->list_value) {
            if (item.type != psd::DescriptorValue::Type::Object || !item.object_value || g.alphas.size() >= 256) continue;
            const auto& s = *item.object_value;
            g.alphas.push_back({unit(psd::descriptor_number(s, "Lctn") / 4096), unit(psd::descriptor_number(s, "Opct", 100) / 100),
                                unit(psd::descriptor_number(s, "Mdpn", 50) / 100)});
        }
    if (g.colors.size() < 2) { note(notes, g.name + ": fewer than two colour stops, left out"); return std::nullopt; }
    if (g.alphas.empty()) g.alphas = {{0, 1, 0.5f}, {1, 1, 0.5f}};
    std::stable_sort(g.colors.begin(), g.colors.end(), [](auto& a, auto& b) { return a.location < b.location; });
    std::stable_sort(g.alphas.begin(), g.alphas.end(), [](auto& a, auto& b) { return a.location < b.location; });
    return g;
}

} // namespace

// ---- Patterns ----------------------------------------------------------------------------------------------------

std::vector<PatternPreset> patternRecords(const std::vector<uint8_t>& payload) {
    std::vector<PatternPreset> out;
    psd::BigEndianReader r(payload);
    try {
        while (r.remaining() >= 4) {
            const size_t entryStart = r.position();
            const uint32_t length = r.read_u32();
            if (length == 0 || length > r.remaining()) break;
            const size_t start = r.position();
            PatternPreset p;
            try {
                psd::BigEndianReader header(std::span<const uint8_t>(payload).subspan(start, length));
                const RecordHeader h = readRecordHeader(header, 0);
                p.id = h.id; p.name = h.name; p.width = h.width; p.height = h.height;
            } catch (std::exception&) {}
            r.skip(length);
            while (r.position() % 4 && r.remaining()) r.skip(1);
            p.record.assign(payload.begin() + std::ptrdiff_t(entryStart), payload.begin() + std::ptrdiff_t(start + length));
            while (p.record.size() % 4) p.record.push_back(0);
            if (!p.id.empty()) out.push_back(std::move(p));
        }
    } catch (std::exception&) {}
    return out;
}

std::optional<PatternTile> decodePattern(const PatternPreset& pattern) {
    auto tiles = parsePatternBlock(pattern.record);
    if (tiles.empty()) return std::nullopt;
    auto it = tiles.find(pattern.id);
    return it != tiles.end() ? it->second : tiles.begin()->second;
}

PatternPreset makePattern(const std::string& id, const std::string& name, int width, int height, const std::vector<uint8_t>& rgba) {
    PatternPreset p;
    p.id = id.substr(0, 255); p.name = name;
    p.width = std::clamp(width, 1, 30000); p.height = std::clamp(height, 1, 30000);
    const size_t pixels = size_t(p.width) * size_t(p.height);
    if (rgba.size() < pixels * 4) return p;
    bool transparent = false;
    for (size_t i = 0; i < pixels && !transparent; i++) transparent = rgba[i * 4 + 3] != 255;
    constexpr uint32_t maxChannels = 24, headerBytes = 23;   // Photoshop declares 24 channels whatever the mode
    const uint32_t written = transparent ? 4 : 3, slots = maxChannels + 2;
    const uint32_t vmaLength = 16 + 4 + written * (8 + headerBytes + uint32_t(pixels)) + (slots - written) * 4;
    psd::BigEndianWriter w;
    w.write_u32(1); w.write_u32(kModeRgb);
    w.write_u16(uint16_t(p.height)); w.write_u16(uint16_t(p.width));
    psd::write_descriptor_unicode_string(w, p.name);
    w.write_u8(uint8_t(p.id.size()));
    w.write_bytes(std::span(reinterpret_cast<const uint8_t*>(p.id.data()), p.id.size()));
    w.write_u32(3); w.write_u32(vmaLength);
    w.write_u32(0); w.write_u32(0); w.write_u32(uint32_t(p.height)); w.write_u32(uint32_t(p.width));
    w.write_u32(maxChannels);
    auto plane = [&](size_t component) {
        w.write_u32(1); w.write_u32(headerBytes + uint32_t(pixels)); w.write_u32(8);
        w.write_u32(0); w.write_u32(0); w.write_u32(uint32_t(p.height)); w.write_u32(uint32_t(p.width));
        w.write_u16(8); w.write_u8(0);
        std::vector<uint8_t> data(pixels);
        for (size_t i = 0; i < pixels; i++) data[i] = rgba[i * 4 + component];
        w.write_bytes(data);
    };
    plane(0); plane(1); plane(2);
    for (uint32_t slot = 3; slot < slots; slot++) {
        if (transparent && slot == maxChannels + 1) plane(3);
        else w.write_u32(0);
    }
    p.record = blockEntry(w.bytes());
    return p;
}

std::optional<std::vector<PatternPreset>> readPat(const std::vector<uint8_t>& bytes, std::string* error, std::vector<std::string>* notes) {
    if (bytes.size() > kMaxFileBytes) { fail(error, "the file is too large"); return std::nullopt; }
    std::vector<PatternPreset> out;
    try {
        psd::BigEndianReader r(bytes);
        auto signature = r.read_span(4);
        if (std::string(signature.begin(), signature.end()) != "8BPT") { fail(error, "not a Photoshop pattern file"); return std::nullopt; }
        if (r.read_u16() != 1) { fail(error, "unsupported pattern file version"); return std::nullopt; }
        const uint32_t count = r.read_u32();
        if (count == 0 || count > kMaxCount) { fail(error, "the file holds no patterns"); return std::nullopt; }
        for (uint32_t index = 0; index < count; index++) {
            const std::string label = "Pattern " + std::to_string(index + 1);
            RecordHeader h;
            size_t start = 0, vmaStart = 0, end = 0;
            try {
                start = r.position();
                h = readRecordHeader(r, start);
                if (h.mode == kModeIndexed) r.skip(4);   // colours used, transparent index: not in a 'Patt' record
                vmaStart = r.position();
                (void)r.read_u32();
                const uint32_t vmaLength = r.read_u32();
                r.skip(vmaLength);
                end = r.position();
            } catch (std::exception&) {
                note(notes, label + ": the file ends inside it; stopped there");
                break;
            }
            // The record as a 'Patt' block has it: the header (with the palette), then the VMA.
            std::vector<uint8_t> body(bytes.begin() + std::ptrdiff_t(start), bytes.begin() + std::ptrdiff_t(start + h.headerEnd));
            body.insert(body.end(), bytes.begin() + std::ptrdiff_t(vmaStart), bytes.begin() + std::ptrdiff_t(end));
            PatternPreset p{h.id, h.name, h.width, h.height, blockEntry(body)};
            const std::string shown = h.name.empty() ? label : h.name;
            if (h.version != 1) { note(notes, shown + ": unsupported record version, left out"); continue; }
            auto tile = decodePattern(p);
            if (!tile) { note(notes, shown + ": its pixels could not be read (only 8-bit patterns up to 4096 px are), left out"); continue; }
            if (p.id.empty()) {   // styles could not name it: give it an id, re-encoded so the record carries it
                p = makePattern(makeUuid(), h.name, tile->width, tile->height, tile->rgba);
                note(notes, shown + ": had no id; given one");
            }
            out.push_back(std::move(p));
        }
    } catch (std::exception&) {
        if (out.empty()) { fail(error, "the file is damaged"); return std::nullopt; }
    }
    if (out.empty()) { fail(error, "the file holds no usable patterns"); return std::nullopt; }
    return out;
}

std::vector<uint8_t> writePat(const std::vector<PatternPreset>& patterns) {
    psd::BigEndianWriter w;
    for (char c : std::string("8BPT")) w.write_u8(uint8_t(c));
    w.write_u16(1);
    std::vector<std::vector<uint8_t>> bodies;
    for (auto& p : patterns) {
        if (p.record.size() < 8) continue;
        psd::BigEndianReader r(p.record);
        const uint32_t length = r.read_u32();
        if (length > r.remaining()) continue;
        std::vector<uint8_t> body(p.record.begin() + 4, p.record.begin() + 4 + std::ptrdiff_t(length));
        try {
            psd::BigEndianReader header(body);
            const RecordHeader h = readRecordHeader(header, 0);
            if (h.mode == kModeIndexed) {   // a .pat record adds colours used and the transparent index
                const uint8_t extra[4] = {1, 0, 0xFF, 0xFF};
                body.insert(body.begin() + std::ptrdiff_t(h.headerEnd), std::begin(extra), std::end(extra));
            }
        } catch (std::exception&) { continue; }
        bodies.push_back(std::move(body));
    }
    w.write_u32(uint32_t(bodies.size()));
    for (auto& b : bodies) w.write_bytes(b);
    return w.bytes();
}

std::vector<std::string> stylePatternIds(const LayerStyle& style) {
    std::vector<std::string> ids;
    auto add = [&](const std::string& id) { if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id); };
    for (auto& p : style.patternOverlays) add(p.patternId);
    for (auto& b : style.bevels) if (b.useTexture) add(b.texturePattern);
    return ids;
}

int addDocumentPatterns(Document& document, const std::vector<PatternPreset>& patterns) {
    std::set<std::string> have;
    if (auto existing = documentPatterns(document)) for (auto& [id, tile] : *existing) have.insert(id);
    std::vector<uint8_t> added;
    int count = 0;
    for (auto& p : patterns) {
        if (p.id.empty() || p.record.empty() || !have.insert(p.id).second) continue;
        added.insert(added.end(), p.record.begin(), p.record.end());
        count++;
    }
    if (!count) return 0;
    auto carry = document.psdCarry ? std::make_shared<PsdDocumentCarry>(*document.psdCarry) : std::make_shared<PsdDocumentCarry>();
    if (!document.psdCarry) { carry->width = document.width; carry->height = document.height; }
    auto block = std::find_if(carry->globals.begin(), carry->globals.end(), [](const PsdBlock& b) { return b.key == "Patt"; });
    if (block == carry->globals.end()) { carry->globals.push_back({"Patt", {}}); block = carry->globals.end() - 1; }
    while (block->data.size() % 4) block->data.push_back(0);
    block->data.insert(block->data.end(), added.begin(), added.end());
    document.psdCarry = carry;
    return count;
}

// ---- Layer styles --------------------------------------------------------------------------------------------------

std::optional<StyleLibrary> readAsl(const std::vector<uint8_t>& bytes, std::string* error, std::vector<std::string>* notes) {
    if (bytes.size() > kMaxFileBytes) { fail(error, "the file is too large"); return std::nullopt; }
    StyleLibrary library;
    bool blendingNoted = false;
    try {
        psd::BigEndianReader r(bytes);
        const uint16_t version = r.read_u16();
        auto signature = r.read_span(4);
        if (std::string(signature.begin(), signature.end()) != "8BSL") { fail(error, "not a Photoshop styles file"); return std::nullopt; }
        if (version != 2) { fail(error, "unsupported styles file version"); return std::nullopt; }
        (void)r.read_u16();   // patterns section version (3)
        const uint32_t patternsLength = r.read_u32();
        if (patternsLength > r.remaining()) { fail(error, "the file's patterns are cut short"); return std::nullopt; }
        if (patternsLength) {
            const std::vector<uint8_t> section(bytes.begin() + std::ptrdiff_t(r.position()), bytes.begin() + std::ptrdiff_t(r.position() + patternsLength));
            library.patterns = patternRecords(section);
            r.skip(patternsLength);
        }
        const uint32_t count = r.read_u32();
        if (count > kMaxCount) { fail(error, "the file holds too many styles"); return std::nullopt; }
        for (uint32_t index = 0; index < count; index++) {
            const std::string label = "Style " + std::to_string(index + 1);
            if (r.remaining() < 4) { note(notes, label + ": the file ends before it"); break; }
            const uint32_t length = r.read_u32();
            if (length > r.remaining()) { note(notes, label + ": cut short, left out"); break; }
            const std::vector<uint8_t> record(bytes.begin() + std::ptrdiff_t(r.position()), bytes.begin() + std::ptrdiff_t(r.position() + length));
            r.skip(std::min<size_t>(r.remaining(), (size_t(length) + 3) & ~size_t(3)));
            try {
                psd::BigEndianReader rr(record);
                if (rr.read_u32() != 16) { note(notes, label + ": unsupported descriptor version, left out"); continue; }
                const psd::DescriptorObject identity = psd::read_descriptor(rr);
                StylePreset style;
                style.name = displayName(textOf(identity, "Nm  "));
                style.id = textOf(identity, "Idnt");
                if (style.name.empty()) style.name = label;
                if (style.id.empty()) style.id = makeUuid();
                if (rr.read_u32() != 16) { note(notes, style.name + ": unsupported descriptor version, left out"); continue; }
                const psd::DescriptorObject styl = psd::read_descriptor(rr);
                const psd::DescriptorObject* effects = psd::descriptor_object(styl, "Lefx");
                if (!effects) { note(notes, style.name + ": no effects (blending options only), left out"); continue; }
                psd::BigEndianWriter w;
                w.write_u32(0); w.write_u32(16);
                psd::write_descriptor(w, *effects);
                auto parsed = parseLayerStyleBlock(w.bytes());
                if (!parsed) { note(notes, style.name + ": its effects could not be read, left out"); continue; }
                style.style = std::move(*parsed);
                if (psd::descriptor_object(styl, "blendOptions") && !blendingNoted) {
                    note(notes, "Styles' blending options (opacity, fill, blend mode) are not applied; their effects are");
                    blendingNoted = true;
                }
                library.styles.push_back(std::move(style));
            } catch (std::exception&) { note(notes, label + ": damaged, left out"); }
        }
    } catch (std::exception&) {
        if (library.styles.empty()) { fail(error, "the file is damaged"); return std::nullopt; }
        note(notes, "The file is damaged past the last style read");
    }
    if (library.styles.empty()) { if (error && error->empty()) *error = "the file holds no usable styles"; return std::nullopt; }
    return library;
}

std::vector<uint8_t> writeAsl(const StyleLibrary& library) {
    psd::BigEndianWriter w;
    w.write_u16(2);
    for (char c : std::string("8BSL")) w.write_u8(uint8_t(c));
    w.write_u16(3);
    std::vector<uint8_t> patterns;
    for (auto& p : library.patterns) patterns.insert(patterns.end(), p.record.begin(), p.record.end());
    w.write_u32(uint32_t(patterns.size()));
    w.write_bytes(patterns);
    w.write_u32(uint32_t(library.styles.size()));
    for (auto& s : library.styles) {
        psd::BigEndianWriter record;
        psd::DescriptorObject identity;
        identity.class_id = "null";
        append(identity, "Nm  ", textValue(s.name));
        append(identity, "Idnt", textValue(s.id));
        record.write_u32(16);
        psd::write_descriptor(record, identity);
        const std::vector<uint8_t> block = authorLayerStyleBlock(s.style);
        psd::BigEndianReader br(block);
        br.skip(8);
        psd::DescriptorObject effects = psd::read_descriptor(br);
        effects.class_id = "Lefx";
        psd::DescriptorObject styl;
        styl.class_id = "Styl";
        append(styl, "Lefx", objectValue(std::move(effects)));
        record.write_u32(16);
        psd::write_descriptor(record, styl);
        const auto& bytes = record.bytes();
        w.write_u32(uint32_t(bytes.size()));
        w.write_bytes(bytes);
        for (size_t n = bytes.size(); n % 4; n++) w.write_u8(0);
    }
    return w.bytes();
}

// ---- Gradients -----------------------------------------------------------------------------------------------------

GradientStops GradientPreset::stops(const float foreground[3], const float background[3]) const {
    GradientStops s;
    for (auto& c : colors) {
        GradientColorStop stop;
        stop.location = c.location;
        stop.midpoint = c.midpoint;
        for (int k = 0; k < 3; k++)
            stop.rgb[k] = c.source == Source::Foreground ? foreground[k] : c.source == Source::Background ? background[k]
                        : float(k == 0 ? c.color.r : k == 1 ? c.color.g : c.color.b) / 255.0f;
        s.colors.push_back(stop);
    }
    for (auto& a : alphas) s.alphas.push_back({a.location, a.opacity, a.midpoint});
    if (!s.colors.empty()) {
        for (int k = 0; k < 3; k++) { s.start[k] = s.colors.front().rgb[k]; s.end[k] = s.colors.back().rgb[k]; }
        s.start[3] = s.alphas.empty() ? 1 : s.alphas.front().opacity;
        s.end[3] = s.alphas.empty() ? 1 : s.alphas.back().opacity;
    }
    return s;
}

StyleGradient GradientPreset::styleGradient(StyleColor foreground, StyleColor background) const {
    StyleGradient g;
    for (auto& c : colors)
        g.colors.push_back({c.location, c.source == Source::Foreground ? foreground : c.source == Source::Background ? background : c.color, c.midpoint});
    g.alphas = alphas;
    g.smoothness = smoothness;
    return g;
}

std::optional<std::vector<GradientPreset>> readGrd(const std::vector<uint8_t>& bytes, std::string* error, std::vector<std::string>* notes) {
    if (bytes.size() > kMaxFileBytes) { fail(error, "the file is too large"); return std::nullopt; }
    std::vector<GradientPreset> out;
    try {
        psd::BigEndianReader r(bytes);
        auto signature = r.read_span(4);
        if (std::string(signature.begin(), signature.end()) != "8BGR") { fail(error, "not a Photoshop gradients file"); return std::nullopt; }
        const uint16_t version = r.read_u16();
        if (version != 5) { fail(error, version == 3 ? "a Photoshop 5 gradients file (version 3), which is not supported" : "unsupported gradients file version"); return std::nullopt; }
        if (r.read_u32() != 16) { fail(error, "unsupported descriptor version"); return std::nullopt; }
        const psd::DescriptorObject root = psd::read_descriptor(r);
        auto list = psd::descriptor_value(root, "GrdL");
        if (!list || list->type != psd::DescriptorValue::Type::List || list->list_value.size() > kMaxCount) { fail(error, "the file has no gradient list"); return std::nullopt; }
        size_t index = 0;
        for (auto& item : list->list_value) {
            index++;
            if (item.type != psd::DescriptorValue::Type::Object || !item.object_value) { note(notes, "Gradient " + std::to_string(index) + ": damaged, left out"); continue; }
            if (auto g = gradientFrom(*item.object_value, "Gradient " + std::to_string(index), notes)) out.push_back(std::move(*g));
        }
    } catch (std::exception&) {
        if (out.empty()) { fail(error, "the file is damaged"); return std::nullopt; }
        note(notes, "The file is damaged past the last gradient read");
    }
    if (out.empty()) { if (error && error->empty()) *error = "the file holds no usable gradients"; return std::nullopt; }
    return out;
}

std::vector<uint8_t> writeGrd(const std::vector<GradientPreset>& gradients) {
    psd::BigEndianWriter w;
    for (char c : std::string("8BGR")) w.write_u8(uint8_t(c));
    w.write_u16(5);
    w.write_u32(16);
    psd::DescriptorObject root;
    root.class_id = "null";
    psd::DescriptorValue list;
    list.type = psd::DescriptorValue::Type::List;
    for (auto& g : gradients) {
        psd::DescriptorObject grad;
        grad.class_id = "Grdn";
        append(grad, "Nm  ", textValue(g.name));
        append(grad, "GrdF", enumValue("GrdF", "CstS"));
        append(grad, "Intr", doubleValue(std::round(double(g.smoothness) * 4096)));
        psd::DescriptorValue colors;
        colors.type = psd::DescriptorValue::Type::List;
        for (auto& c : g.colors) {
            psd::DescriptorObject stop;
            stop.class_id = "Clrt";
            if (c.source == GradientPreset::Source::User) {
                psd::DescriptorObject rgb;
                rgb.class_id = "RGBC";
                append(rgb, "Rd  ", doubleValue(c.color.r));
                append(rgb, "Grn ", doubleValue(c.color.g));
                append(rgb, "Bl  ", doubleValue(c.color.b));
                append(stop, "Clr ", objectValue(std::move(rgb)));
            }
            append(stop, "Type", enumValue("Clry", c.source == GradientPreset::Source::Foreground ? "FrgC" : c.source == GradientPreset::Source::Background ? "BckC" : "UsrS"));
            append(stop, "Lctn", integerValue(int32_t(std::lround(double(c.location) * 4096))));
            append(stop, "Mdpn", integerValue(int32_t(std::lround(double(c.midpoint) * 100))));
            colors.list_value.push_back(objectValue(std::move(stop)));
        }
        append(grad, "Clrs", std::move(colors));
        psd::DescriptorValue alphas;
        alphas.type = psd::DescriptorValue::Type::List;
        for (auto& a : g.alphas) {
            psd::DescriptorObject stop;
            stop.class_id = "TrnS";
            append(stop, "Opct", percentValue(double(a.opacity) * 100));
            append(stop, "Lctn", integerValue(int32_t(std::lround(double(a.location) * 4096))));
            append(stop, "Mdpn", integerValue(int32_t(std::lround(double(a.midpoint) * 100))));
            alphas.list_value.push_back(objectValue(std::move(stop)));
        }
        append(grad, "Trns", std::move(alphas));
        psd::DescriptorObject wrapper;
        wrapper.name = "Gradient";
        wrapper.class_id = "Grdn";
        append(wrapper, "Grad", objectValue(std::move(grad)));
        list.list_value.push_back(objectValue(std::move(wrapper)));
    }
    append(root, "GrdL", std::move(list));
    psd::write_descriptor(w, root);
    return w.bytes();
}

} // namespace compositor
