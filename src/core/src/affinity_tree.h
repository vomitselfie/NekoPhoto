// The tagged object tree inside an Affinity document's doc.dat stream, parsed without knowing any class: every
// field carries a type byte that fixes its layout, so the whole tree walks and the importer then asks for fields
// by their four-character tag. Ported from Patchy (MIT, src/third_party/patchy_psd/README.md): its
// src/formats/af_tree.{hpp,cpp} and the little-endian reader they use, bounded against hostile files the same way.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace compositor::affinity {

/// A little-endian reader over a byte span; reading past the end throws std::runtime_error.
class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes, const char* underrun = "Affinity document is truncated")
        : bytes_(bytes), underrun_(underrun) {}
    size_t position() const { return offset_; }
    size_t remaining() const { return bytes_.size() - offset_; }
    uint8_t u8() { need(1); return bytes_[offset_++]; }
    uint16_t u16() { need(2); uint16_t v = uint16_t(bytes_[offset_] | (bytes_[offset_ + 1] << 8)); offset_ += 2; return v; }
    uint32_t u32() {
        need(4);
        uint32_t v = uint32_t(bytes_[offset_]) | uint32_t(bytes_[offset_ + 1]) << 8 | uint32_t(bytes_[offset_ + 2]) << 16 | uint32_t(bytes_[offset_ + 3]) << 24;
        offset_ += 4;
        return v;
    }
    uint64_t u64() { const uint64_t low = u32(); const uint64_t high = u32(); return low | high << 32; }
    void skip(size_t n) { need(n); offset_ += n; }
    void seek(size_t at) { if (at > bytes_.size()) throw std::runtime_error(underrun_); offset_ = at; }

private:
    void need(size_t n) const { if (n > remaining()) throw std::runtime_error(underrun_); }
    std::span<const uint8_t> bytes_;
    const char* underrun_;
    size_t offset_ = 0;
};

class Class;

struct Enum { uint16_t id = 0, version = 0; };
/// A reference to another stream of the container ("d/1f" and the like).
struct Embedded { uint32_t tag = 0; std::string data; };
/// Fixed-size records (vector path points ride here as 18-byte x, y, flags triples).
struct CurveArray { uint16_t recordSize = 0; std::vector<uint8_t> bytes; };
/// A value that parsed and was consumed but is not kept.
struct Skipped {};

using Value = std::variant<Skipped, bool, int64_t, double, std::string, Enum, Embedded, CurveArray, std::vector<double>,
                           std::vector<int64_t>, std::vector<uint8_t>, std::shared_ptr<Class>, std::vector<std::shared_ptr<Class>>>;

struct Field { uint32_t tag = 0; Value value; };

class Class {
public:
    uint32_t type = 0;       // the concrete class's tag
    uint32_t sharedId = 0;
    std::vector<Field> fields;

    const Field* field(uint32_t tag) const;
    const Class* child(uint32_t tag) const;
    const std::vector<std::shared_ptr<Class>>* list(uint32_t tag) const;
    bool boolean(uint32_t tag, bool fallback) const;
    double number(uint32_t tag, double fallback) const;
    int64_t integer(uint32_t tag, int64_t fallback) const;
    std::string string(uint32_t tag) const;
    /// A numeric vector (integers or reals) as doubles; empty when absent.
    std::vector<double> vector(uint32_t tag) const;
};

struct Tree {
    uint32_t rootType = 0;
    uint32_t documentVersion = 0;
    std::shared_ptr<Class> root;
};

/// Parses a doc.dat payload (or a block stream of the same grammar); throws std::runtime_error when malformed.
Tree parseTree(std::span<const uint8_t> bytes);

/// A tag as the reader sees it: "Desc" is stored reversed on the wire, so reading it little-endian gives
/// 'D' << 24 | 'e' << 16 | 's' << 8 | 'c'.
constexpr uint32_t tag(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint32_t(uint8_t(s[3]));
}

} // namespace compositor::affinity
