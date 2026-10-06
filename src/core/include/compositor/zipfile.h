// A small ZIP reader and writer for the .nekophoto document container (docs/project-format.md).
//
// The writer makes plain ZIP files (no ZIP64): entries stored or deflated, UTF-8 names, a central directory. The reader
// takes any such file and refuses what a hostile one could use against us: more than `ZipLimits::entries` entries,
// more uncompressed bytes than the limits, a deflated entry that claims a bomb's ratio, names that climb out of a
// folder or are absolute, duplicate names (also when they differ only in case), encrypted or ZIP64 entries, data that
// overlaps the directory, and anything past 4 GiB.
#pragma once
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace compositor {

struct ZipLimits {
    size_t entries = 65535;
    /// One entry, uncompressed.
    uint64_t entryBytes = 0xFFFFFFFFull;
    /// Every entry together, uncompressed.
    uint64_t totalBytes = 6ull << 30;
    /// Uncompressed over compressed, for deflated entries past 1 MiB.
    uint64_t ratio = 1000;
};

struct ZipFileEntry {
    std::string name;
    uint16_t method = 0;   // 0 stored, 8 deflated
    uint16_t flags = 0;
    uint32_t crc = 0;
    uint64_t packedSize = 0, size = 0, headerOffset = 0;
    bool isDirectory() const { return !name.empty() && name.back() == '/'; }
};

/// A name that stays inside the folder it is extracted to: relative, '/'-separated, no empty, "." or ".." part, no
/// backslash, colon or control character. A trailing '/' (a folder entry) is allowed.
bool isSafeZipEntryName(const std::string& name);

class ZipFileReader {
public:
    /// Reads the central directory of a file on disk; the file stays open until the reader is destroyed or closed.
    bool openFile(const std::string& path, std::string* error = nullptr, const ZipLimits& limits = {});
    /// The same over bytes in memory.
    bool openMemory(std::vector<uint8_t> bytes, std::string* error = nullptr, const ZipLimits& limits = {});
    void close();
    const std::vector<ZipFileEntry>& entries() const { return entries_; }
    const ZipFileEntry* find(const std::string& name) const;
    /// One entry's contents, decompressed and checked against its CRC.
    bool read(const ZipFileEntry& entry, std::vector<uint8_t>& out, std::string* error = nullptr);
    /// The offset of an entry's data, after its local header.
    bool dataOffset(const ZipFileEntry& entry, uint64_t& offset, std::string* error = nullptr);

private:
    bool parse(std::string* error, const ZipLimits& limits);
    bool readAt(uint64_t offset, void* out, size_t size);
    std::unique_ptr<std::ifstream> file_;
    std::vector<uint8_t> memory_;
    uint64_t size_ = 0;
    std::vector<ZipFileEntry> entries_;
};

class ZipFileWriter {
public:
    ZipFileWriter() = default;
    ZipFileWriter(const ZipFileWriter&) = delete;
    ZipFileWriter& operator=(const ZipFileWriter&) = delete;
    ~ZipFileWriter();
    bool open(const std::string& path, std::string* error = nullptr);
    /// Adds one entry; `deflate` compresses it (worth it for text, not for PNGs).
    bool add(const std::string& name, const uint8_t* data, size_t size, bool deflate, std::string* error = nullptr);
    bool add(const std::string& name, const std::vector<uint8_t>& data, bool deflate, std::string* error = nullptr) {
        return add(name, data.data(), data.size(), deflate, error);
    }
    /// Writes the central directory, flushes and syncs the file to disk, and closes it.
    bool finish(std::string* error = nullptr);

private:
    bool write(const void* data, size_t size);
    std::FILE* file_ = nullptr;
    uint64_t offset_ = 0;
    std::vector<ZipFileEntry> entries_;
    uint16_t time_ = 0, date_ = 0;
};

} // namespace compositor
