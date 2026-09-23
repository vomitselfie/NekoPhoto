// Reading ZIP archives (PKWARE's APPNOTE: the central directory, stored and deflated entries), for the brush
// formats that are ZIP files. Internal to the core.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace compositor {

class ZipArchive {
public:
    /// The archive in `data` (which must outlive this object), or nullopt when it is not a ZIP this reads.
    static std::optional<ZipArchive> open(const uint8_t* data, size_t size);
    /// Every file's name, in the archive's order.
    std::vector<std::string> names() const;
    bool contains(const std::string& name) const;
    /// A file's bytes, decompressed; nullopt when it is missing, damaged or larger than `limit`.
    std::optional<std::vector<uint8_t>> read(const std::string& name, size_t limit = 256u << 20) const;

private:
    struct Entry { std::string name; uint16_t method = 0; uint32_t crc = 0; uint64_t packed = 0, size = 0, header = 0; };
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    std::vector<Entry> entries_;
};

} // namespace compositor
