#include "zip.h"
#include <zlib.h>
#include <algorithm>

namespace compositor {

namespace {

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }

} // namespace

std::optional<ZipArchive> ZipArchive::open(const uint8_t* data, size_t size) {
    if (size < 22) return std::nullopt;
    // The end-of-central-directory record: within the last 64 KiB and 22 bytes (its comment may run up to that).
    size_t end = size_t(-1);
    for (size_t i = size - 22 + 1; i-- > (size > 65557 ? size - 65557 : 0);)
        if (le32(data + i) == 0x06054b50) { end = i; break; }
    if (end == size_t(-1)) return std::nullopt;
    uint64_t count = le16(data + end + 10), directory = le32(data + end + 16), directorySize = le32(data + end + 12);
    // ZIP64: the real values sit in the ZIP64 end record, which a locator just before this one points at.
    if ((count == 0xFFFF || directory == 0xFFFFFFFF) && end >= 20 && le32(data + end - 20) == 0x07064b50) {
        const uint64_t record = le64(data + end - 20 + 8);
        if (record + 56 > size || le32(data + record) != 0x06064b50) return std::nullopt;
        count = le64(data + record + 32);
        directorySize = le64(data + record + 40);
        directory = le64(data + record + 48);
    }
    if (directory > size || directorySize > size - directory || count > directorySize / 46) return std::nullopt;
    ZipArchive zip;
    zip.data_ = data;
    zip.size_ = size;
    size_t p = size_t(directory);
    for (uint64_t i = 0; i < count; i++) {
        if (p + 46 > size || le32(data + p) != 0x02014b50) return std::nullopt;
        Entry e;
        e.method = le16(data + p + 10);
        e.crc = le32(data + p + 16);
        e.packed = le32(data + p + 20);
        e.size = le32(data + p + 24);
        const size_t nameLength = le16(data + p + 28), extraLength = le16(data + p + 30), commentLength = le16(data + p + 32);
        e.header = le32(data + p + 42);
        if (p + 46 + nameLength + extraLength + commentLength > size) return std::nullopt;
        e.name.assign(reinterpret_cast<const char*>(data + p + 46), nameLength);
        // ZIP64 sizes and offset, in the extra field, for the values marked 0xFFFFFFFF.
        for (size_t x = p + 46 + nameLength; x + 4 <= p + 46 + nameLength + extraLength;) {
            const uint16_t id = le16(data + x), length = le16(data + x + 2);
            if (x + 4 + length > p + 46 + nameLength + extraLength) break;
            if (id == 0x0001) {
                size_t q = x + 4;
                auto take = [&](uint64_t& field) { if (field == 0xFFFFFFFF && q + 8 <= x + 4 + length) { field = le64(data + q); q += 8; } };
                take(e.size);
                take(e.packed);
                take(e.header);
            }
            x += 4 + length;
        }
        p += 46 + nameLength + extraLength + commentLength;
        zip.entries_.push_back(std::move(e));
    }
    return zip;
}

std::vector<std::string> ZipArchive::names() const {
    std::vector<std::string> out;
    for (const Entry& e : entries_) out.push_back(e.name);
    return out;
}

bool ZipArchive::contains(const std::string& name) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.name == name; });
}

std::optional<std::vector<uint8_t>> ZipArchive::read(const std::string& name, size_t limit) const {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.name == name; });
    if (it == entries_.end() || it->size > limit) return std::nullopt;
    const Entry& e = *it;
    if (e.header + 30 > size_ || le32(data_ + e.header) != 0x04034b50) return std::nullopt;
    const uint64_t start = e.header + 30 + le16(data_ + e.header + 26) + le16(data_ + e.header + 28);
    if (start > size_ || e.packed > size_ - start) return std::nullopt;
    const uint8_t* packed = data_ + start;
    std::vector<uint8_t> out(static_cast<size_t>(e.size));
    if (e.method == 0) {
        if (e.packed != e.size) return std::nullopt;
        std::copy(packed, packed + e.size, out.begin());
    } else if (e.method == 8) {
        z_stream z{};
        if (inflateInit2(&z, -15) != Z_OK) return std::nullopt;
        z.next_in = const_cast<Bytef*>(packed);
        z.avail_in = uInt(std::min<uint64_t>(e.packed, 0xFFFFFFFFu));
        z.next_out = out.data();
        z.avail_out = uInt(out.size());
        const int status = inflate(&z, Z_FINISH);
        const bool complete = (status == Z_STREAM_END || (status == Z_BUF_ERROR && z.avail_out == 0)) && z.total_out == e.size;
        inflateEnd(&z);
        if (!complete) return std::nullopt;
    } else return std::nullopt;
    if (crc32(0, out.data(), uInt(out.size())) != e.crc) return std::nullopt;
    return out;
}

} // namespace compositor
