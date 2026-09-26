// Shared by the small file-format readers and writers (TGA, ICO, GIF, Aseprite): whole-file reads and writes,
// and a bounds-checked little-endian cursor that reports running out instead of reading past the end.
#pragma once
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace compositor::format_io {

inline bool readFile(const std::string& path, std::vector<uint8_t>& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (error) *error = "cannot open " + path; return false; }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) { if (error) *error = "cannot read " + path; return false; }
    in.seekg(0);
    out.resize(size_t(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(out.data()), size)) { if (error) *error = "cannot read " + path; return false; }
    return true;
}

inline bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes, std::string* error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())) || !out.flush()) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

/// Reads little-endian values; past the end every read yields 0 and `ok` turns false.
struct Reader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    bool ok = true;
    Reader(const uint8_t* d, size_t n) : data(d), size(n) {}
    size_t remaining() const { return pos < size ? size - pos : 0; }
    bool has(size_t n) const { return pos <= size && n <= size - pos; }
    uint8_t u8() { if (!has(1)) { ok = false; pos = size; return 0; } return data[pos++]; }
    uint16_t u16() { const uint16_t lo = u8(); return uint16_t(lo | (uint16_t(u8()) << 8)); }
    uint32_t u32() { const uint32_t lo = u16(); return lo | (uint32_t(u16()) << 16); }
    void skip(size_t n) { if (!has(n)) { ok = false; pos = size; } else pos += n; }
    void seek(size_t p) { if (p > size) { ok = false; pos = size; } else pos = p; }
};

struct Writer {
    std::vector<uint8_t> bytes;
    void u8(uint32_t v) { bytes.push_back(uint8_t(v)); }
    void u16(uint32_t v) { u8(v); u8(v >> 8); }
    void u32(uint32_t v) { u16(v); u16(v >> 16); }
    void append(const std::vector<uint8_t>& b) { bytes.insert(bytes.end(), b.begin(), b.end()); }
};

} // namespace compositor::format_io
