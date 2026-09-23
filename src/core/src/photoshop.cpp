#include "photoshop.h"

namespace compositor::photoshop {

Descriptor readDescriptorValue(Reader& r, const std::string& type, int depth);

Descriptor readDescriptorObject(Reader& r, int depth) {
    Descriptor d;
    d.type = "Objc";
    if (depth > 32) throw Truncated{};
    r.unicode();   // class name
    d.classId = r.key();
    uint32_t count = r.u32();
    if (count > 100000) throw Truncated{};
    for (uint32_t i = 0; i < count; i++) {
        std::string key = r.key();
        std::string type = r.chars(4);
        d.items[key] = readDescriptorValue(r, type, depth + 1);
    }
    return d;
}

Descriptor readDescriptorValue(Reader& r, const std::string& type, int depth) {
    // Every kind that nests (objects, lists, object arrays) comes through here, so this bounds them all.
    if (depth > 32) throw Truncated{};
    Descriptor d;
    d.type = type;
    if (type == "Objc" || type == "GlbO") return readDescriptorObject(r, depth);
    if (type == "doub") d.number = r.f64();
    else if (type == "UntF") { d.unit = r.chars(4); d.number = r.f64(); }
    else if (type == "long") d.number = r.i32();
    else if (type == "comp") d.number = double(int64_t(r.u64()));
    else if (type == "bool") d.number = r.u8();
    else if (type == "TEXT") d.text = r.unicode();
    else if (type == "enum") { r.key(); d.text = r.key(); }
    else if (type == "type" || type == "GlbC") { r.unicode(); r.key(); }
    else if (type == "alis" || type == "tdta") { uint32_t n = r.u32(); r.skip(n); }
    else if (type == "VlLs") {
        uint32_t count = r.u32();
        if (count > 100000) throw Truncated{};
        for (uint32_t i = 0; i < count; i++) { std::string t = r.chars(4); d.list.push_back(readDescriptorValue(r, t, depth + 1)); }
    } else if (type == "ObAr") {
        r.u32(); r.unicode(); r.key();
        uint32_t count = r.u32();
        if (count > 100000) throw Truncated{};
        for (uint32_t i = 0; i < count; i++) { std::string key = r.key(); std::string t = r.chars(4); d.items[key] = readDescriptorValue(r, t, depth + 1); }
    } else if (type == "obj ") {
        uint32_t count = r.u32();
        if (count > 1000) throw Truncated{};
        for (uint32_t i = 0; i < count; i++) {
            std::string t = r.chars(4);
            if (t == "prop") { r.unicode(); r.key(); r.key(); }
            else if (t == "Clss") { r.unicode(); r.key(); }
            else if (t == "Enmr") { r.unicode(); r.key(); r.key(); r.key(); }
            else if (t == "rele") { r.unicode(); r.key(); r.u32(); }
            else if (t == "Idnt" || t == "indx") { r.unicode(); r.key(); r.u32(); }
            else if (t == "name") { r.unicode(); r.key(); r.unicode(); }
            else throw Truncated{};
        }
    } else throw Truncated{};   // an unknown item type: the rest of the descriptor cannot be walked
    return d;
}

/// A descriptor block: its version (16), then the object.
Descriptor readDescriptor(Reader& r) {
    if (r.u32() != 16) throw Truncated{};
    return readDescriptorObject(r, 0);
}

} // namespace compositor::photoshop
