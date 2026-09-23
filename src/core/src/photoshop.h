// Reading Photoshop's binary structures, shared by the PSD and ABR readers: a bounds-checked big-endian
// reader and the action descriptors Photoshop keeps settings in. Internal to the core.
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace compositor::photoshop {

struct Truncated {};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    size_t position() const { return pos_; }
    size_t remaining() const { return size_ - pos_; }
    void seek(size_t pos) { if (pos > size_) throw Truncated{}; pos_ = pos; }
    void skip(size_t n) { seek(pos_ + n); }
    const uint8_t* bytes(size_t n) { if (n > remaining()) throw Truncated{}; const uint8_t* p = data_ + pos_; pos_ += n; return p; }
    uint8_t u8() { return *bytes(1); }
    uint16_t u16() { const uint8_t* p = bytes(2); return uint16_t((p[0] << 8) | p[1]); }
    uint32_t u32() { const uint8_t* p = bytes(4); return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
    uint64_t u64() { uint64_t hi = u32(); return (hi << 32) | u32(); }
    int16_t i16() { return int16_t(u16()); }
    int32_t i32() { return int32_t(u32()); }
    float f32() { uint32_t v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
    double f64() { uint64_t v = u64(); double d; std::memcpy(&d, &v, 8); return d; }
    /// A section length: 4 bytes, or 8 in a PSB.
    uint64_t length(bool psb) { return psb ? u64() : u32(); }
    std::string chars(size_t n) { const uint8_t* p = bytes(n); return std::string(reinterpret_cast<const char*>(p), n); }
    /// A Pascal string (length byte, text) padded to a multiple of `pad`.
    std::string pascal(size_t pad) {
        size_t start = pos_;
        uint8_t n = u8();
        std::string s = chars(n);
        size_t used = pos_ - start;
        if (used % pad) skip(pad - used % pad);
        return s;
    }
    /// A Unicode string: a character count, then UTF-16BE, as UTF-8.
    std::string unicode() {
        uint32_t count = u32();
        if (count > remaining() / 2) throw Truncated{};
        std::string out;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t c = u16();
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < count) { uint32_t low = u16(); i++; c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00); }
            if (c == 0) continue;
            if (c < 0x80) out += char(c);
            else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
            else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
            else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        }
        return out;
    }
    /// A key in a descriptor: a length, then that many characters, or four when the length is zero.
    std::string key() { uint32_t n = u32(); return chars(n ? n : 4); }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

// ---- Descriptors (the structure Photoshop stores settings in) ------------------------------------

struct Descriptor {
    std::string type;                                  // doub, long, bool, TEXT, enum, Objc, VlLs, UntF, ...
    std::string classId;                               // Objc: the object's class ("sampledBrush", "Grsc", ...)
    std::string unit;                                  // UntF: "#Pxl", "#Prc", "#Ang", ...
    double number = 0;
    std::string text;                                  // TEXT, enum value
    std::map<std::string, Descriptor> items;           // Objc
    std::vector<Descriptor> list;                      // VlLs
    const Descriptor* item(const std::string& key) const { auto it = items.find(key); return it == items.end() ? nullptr : &it->second; }
    /// The number under `key`, or `fallback` when there is none.
    double numberAt(const std::string& key, double fallback) const { const Descriptor* d = item(key); return d ? d->number : fallback; }
};

/// A descriptor object without the version word (as descriptors nest in lists and in ABR sections).
Descriptor readDescriptorObject(Reader& r, int depth = 0);
/// A descriptor block: its version (16), then the object.
Descriptor readDescriptor(Reader& r);

} // namespace compositor::photoshop
