// Artboards and slices (artboard.h). Descriptors go through Patchy's psd_descriptor (MIT, Seth A. Robinson).
#include "compositor/artboard.h"
#include "psd/psd_descriptor.hpp"
#include <algorithm>
#include <cmath>
#include <exception>

namespace compositor {

namespace psd = patchy::psd;

namespace {

uint8_t unit(double v) { return uint8_t(std::lround(std::clamp(v, 0.0, 1.0) * 255)); }

psd::DescriptorValue number(double v) { psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::Double; d.double_value = v; return d; }
psd::DescriptorValue integer(int32_t v) { psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::Integer; d.integer_value = v; return d; }
psd::DescriptorValue text(const std::string& s) { psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::String; d.string_value = s; return d; }
psd::DescriptorValue object(psd::DescriptorObject o) {
    psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::Object; d.object_value = std::make_shared<psd::DescriptorObject>(std::move(o)); return d;
}
void put(psd::DescriptorObject& o, const std::string& key, psd::DescriptorValue v) {
    o.values[key] = std::move(v);
    o.key_order.push_back({key, false});
}

std::string stringOf(const psd::DescriptorObject& o, const char* key) {
    const auto* v = psd::descriptor_value(o, key);
    return v && v->type == psd::DescriptorValue::Type::String ? v->string_value : std::string();
}

// Resource 1050's own strings: a u32 count of UTF-16 code units, no terminator.
void writeUnicode(psd::BigEndianWriter& w, const std::string& utf8) {
    psd::BigEndianWriter tmp;
    psd::write_descriptor_unicode_string(tmp, utf8);   // count + units + a NUL we drop
    const auto& b = tmp.bytes();
    const uint32_t count = (uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]) - 1;
    w.write_u32(count);
    w.write_bytes(std::span<const uint8_t>(b.data() + 4, size_t(count) * 2));
}

} // namespace

void Artboard::fill(uint8_t rgba[4]) const {
    switch (background) {
    case Black: rgba[0] = rgba[1] = rgba[2] = 0; rgba[3] = 255; break;
    case Transparent: rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; break;
    case Other: rgba[0] = unit(red); rgba[1] = unit(green); rgba[2] = unit(blue); rgba[3] = 255; break;
    default: rgba[0] = rgba[1] = rgba[2] = rgba[3] = 255; break;
    }
}

std::optional<Artboard> parseArtboardBlock(const std::vector<uint8_t>& data) {
    try {
        psd::BigEndianReader r(data);
        if (r.read_u32() != 16) return std::nullopt;
        const psd::DescriptorObject d = psd::read_descriptor(r);
        const psd::DescriptorObject* rect = psd::descriptor_object(d, "artboardRect");
        if (!rect) return std::nullopt;
        Artboard a;
        const double top = psd::descriptor_number(*rect, "Top "), left = psd::descriptor_number(*rect, "Left");
        const double bottom = psd::descriptor_number(*rect, "Btom"), right = psd::descriptor_number(*rect, "Rght");
        a.x = int(std::lround(left)); a.y = int(std::lround(top));
        a.width = int(std::lround(right)) - a.x; a.height = int(std::lround(bottom)) - a.y;
        if (a.width <= 0 || a.height <= 0) return std::nullopt;
        a.presetName = stringOf(d, "artboardPresetName");
        a.background = std::clamp(int(psd::descriptor_number(d, "artboardBackgroundType", 1)), 1, 4);
        if (const psd::DescriptorObject* c = psd::descriptor_object(d, "Clr ")) {
            a.red = psd::descriptor_number(*c, "Rd  ", 255) / 255;
            a.green = psd::descriptor_number(*c, "Grn ", 255) / 255;
            a.blue = psd::descriptor_number(*c, "Bl  ", 255) / 255;
        }
        return a;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<uint8_t> artboardBlock(const Artboard& a) {
    psd::DescriptorObject rect;
    rect.class_id = "classFloatRect";
    put(rect, "Top ", number(a.y));
    put(rect, "Left", number(a.x));
    put(rect, "Btom", number(a.y + a.height));
    put(rect, "Rght", number(a.x + a.width));
    psd::DescriptorObject colour;
    colour.class_id = "RGBC";
    put(colour, "Rd  ", number(std::clamp(a.red, 0.0, 1.0) * 255));
    put(colour, "Grn ", number(std::clamp(a.green, 0.0, 1.0) * 255));
    put(colour, "Bl  ", number(std::clamp(a.blue, 0.0, 1.0) * 255));
    psd::DescriptorObject d;
    d.class_id = "artboard";
    put(d, "artboardRect", object(std::move(rect)));
    psd::DescriptorValue guides; guides.type = psd::DescriptorValue::Type::List;
    put(d, "guideIndeces", guides);
    put(d, "artboardPresetName", text(a.presetName));
    put(d, "Clr ", object(std::move(colour)));
    put(d, "artboardBackgroundType", integer(std::clamp(a.background, 1, 4)));
    psd::BigEndianWriter w;
    w.write_u32(16);
    psd::write_descriptor(w, d);
    return w.bytes();
}

bool parseSlicesResource(const std::vector<uint8_t>& data, std::vector<Slice>& slices, std::string* groupName) {
    slices.clear();
    try {
        psd::BigEndianReader r(data);
        const uint32_t version = r.read_u32();
        if (version == 6) {
            for (int i = 0; i < 4; i++) (void)r.read_u32();   // the bounds
            std::string group = psd::read_descriptor_unicode_string(r);
            if (groupName) *groupName = group;
            const uint32_t count = r.read_u32();
            if (count > 100000) return false;
            for (uint32_t i = 0; i < count; i++) {
                Slice s;
                s.id = r.read_u32();
                (void)r.read_u32();                  // group id
                const uint32_t origin = r.read_u32();
                if (origin == 1) (void)r.read_u32(); // the layer it follows
                s.name = psd::read_descriptor_unicode_string(r);
                (void)r.read_u32();                  // type
                const int32_t left = int32_t(r.read_u32()), top = int32_t(r.read_u32()), right = int32_t(r.read_u32()), bottom = int32_t(r.read_u32());
                s.x = left; s.y = top; s.width = right - left; s.height = bottom - top;
                s.url = psd::read_descriptor_unicode_string(r);
                s.target = psd::read_descriptor_unicode_string(r);
                s.message = psd::read_descriptor_unicode_string(r);
                s.altTag = psd::read_descriptor_unicode_string(r);
                (void)r.read_u8();                   // cell text is HTML
                (void)psd::read_descriptor_unicode_string(r);
                (void)r.read_u32(); (void)r.read_u32();   // alignment
                (void)r.read_u32();                  // ARGB
                if (origin != 0 && s.width > 0 && s.height > 0) slices.push_back(std::move(s));
            }
            return true;
        }
        if (version == 7 || version == 8) {
            if (r.read_u32() != 16) return false;
            const psd::DescriptorObject d = psd::read_descriptor(r);
            if (groupName) *groupName = stringOf(d, "baseName");
            const auto* list = psd::descriptor_value(d, "slices");
            if (!list || list->type != psd::DescriptorValue::Type::List) return true;
            for (const auto& item : list->list_value) {
                if (item.type != psd::DescriptorValue::Type::Object || !item.object_value) continue;
                const psd::DescriptorObject& o = *item.object_value;
                const auto* origin = psd::descriptor_value(o, "origin");
                if (origin && origin->enum_value == "autoGenerated") continue;
                Slice s;
                s.id = uint32_t(psd::descriptor_number(o, "sliceID", 1));
                s.name = stringOf(o, "Nm  ");
                if (const psd::DescriptorObject* b = psd::descriptor_object(o, "bounds")) {
                    const int top = int(psd::descriptor_number(*b, "Top ")), left = int(psd::descriptor_number(*b, "Left"));
                    s.x = left; s.y = top;
                    s.width = int(psd::descriptor_number(*b, "Rght")) - left; s.height = int(psd::descriptor_number(*b, "Btom")) - top;
                }
                s.url = stringOf(o, "url");
                s.target = stringOf(o, "null");
                s.message = stringOf(o, "Msge");
                s.altTag = stringOf(o, "altTag");
                if (s.width > 0 && s.height > 0) slices.push_back(std::move(s));
            }
            return true;
        }
    } catch (const std::exception&) {
    }
    slices.clear();
    return false;
}

std::vector<uint8_t> slicesResource(const std::vector<Slice>& slices, int width, int height, const std::string& groupName) {
    psd::BigEndianWriter w;
    w.write_u32(6);
    w.write_u32(0); w.write_u32(0); w.write_u32(uint32_t(height)); w.write_u32(uint32_t(width));
    writeUnicode(w, groupName);
    w.write_u32(uint32_t(slices.size() + 1));
    auto record = [&](uint32_t id, uint32_t origin, const std::string& name, int x, int y, int sw, int sh,
                      const std::string& url, const std::string& target, const std::string& message, const std::string& alt) {
        w.write_u32(id);
        w.write_u32(0);          // group
        w.write_u32(origin);     // 0 automatic, 2 user
        writeUnicode(w, name);
        w.write_u32(1);          // image slice
        w.write_u32(uint32_t(x)); w.write_u32(uint32_t(y)); w.write_u32(uint32_t(x + sw)); w.write_u32(uint32_t(y + sh));
        writeUnicode(w, url);
        writeUnicode(w, target);
        writeUnicode(w, message);
        writeUnicode(w, alt);
        w.write_u8(0);
        writeUnicode(w, "");
        w.write_u32(0); w.write_u32(0);
        w.write_u32(0);          // ARGB: none
    };
    record(0, 0, groupName, 0, 0, width, height, "", "", "", "");
    for (const Slice& s : slices) record(s.id, 2, s.name, s.x, s.y, s.width, s.height, s.url, s.target, s.message, s.altTag);
    return w.bytes();
}

uint32_t nextSliceId(const std::vector<Slice>& slices) {
    uint32_t id = 0;
    for (const Slice& s : slices) id = std::max(id, s.id);
    return id + 1;
}

} // namespace compositor
