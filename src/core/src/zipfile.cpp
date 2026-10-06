#include "compositor/zipfile.h"
#include <zlib.h>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <set>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace compositor {

namespace {

constexpr uint32_t localSignature = 0x04034b50, centralSignature = 0x02014b50, endSignature = 0x06054b50, zip64Locator = 0x07064b50;
constexpr uint64_t zipMax = 0xFFFFFFFFull;

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
void put16(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void put32(std::vector<uint8_t>& b, uint32_t v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

std::string lowered(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

bool isSafeZipEntryName(const std::string& name) {
    if (name.empty() || name.size() > 1024 || name.front() == '/') return false;
    for (char c : name)
        if (static_cast<unsigned char>(c) < 0x20 || c == '\\' || c == ':' || c == 0x7F) return false;
    const std::string body = name.back() == '/' ? name.substr(0, name.size() - 1) : name;
    if (body.empty()) return false;
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find('/', start);
        if (end == std::string::npos) end = body.size();
        const std::string part = body.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

// ---- Reader -----------------------------------------------------------------------------------------------------------

bool ZipFileReader::openFile(const std::string& path, std::string* error, const ZipLimits& limits) {
    close();
    auto in = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!*in) return fail(error, "could not open " + path);
    in->seekg(0, std::ios::end);
    const std::streamoff end = in->tellg();
    if (end < 0) return fail(error, "could not read " + path);
    size_ = uint64_t(end);
    file_ = std::move(in);
    if (!parse(error, limits)) { close(); return false; }
    return true;
}

bool ZipFileReader::openMemory(std::vector<uint8_t> bytes, std::string* error, const ZipLimits& limits) {
    close();
    memory_ = std::move(bytes);
    size_ = memory_.size();
    if (!parse(error, limits)) { close(); return false; }
    return true;
}

void ZipFileReader::close() {
    file_.reset();
    memory_.clear();
    size_ = 0;
    entries_.clear();
}

bool ZipFileReader::readAt(uint64_t offset, void* out, size_t size) {
    if (offset > size_ || size > size_ - offset) return false;
    if (size == 0) return true;
    if (file_) {
        file_->clear();
        file_->seekg(std::streamoff(offset));
        return bool(file_->read(static_cast<char*>(out), std::streamsize(size)));
    }
    std::copy(memory_.begin() + std::ptrdiff_t(offset), memory_.begin() + std::ptrdiff_t(offset + size), static_cast<uint8_t*>(out));
    return true;
}

bool ZipFileReader::parse(std::string* error, const ZipLimits& limits) {
    const std::string damaged = "The file is not a valid ZIP container, or it is damaged.";
    if (size_ > zipMax) return fail(error, "The file is larger than 4 GiB, which this format does not support.");
    if (size_ < 22) return fail(error, damaged);
    // The end record: within the last 64 KiB and 22 bytes (its comment may run up to that).
    const uint64_t tail = std::min<uint64_t>(size_, 65535 + 22);
    std::vector<uint8_t> buffer(static_cast<size_t>(tail));
    if (!readAt(size_ - tail, buffer.data(), buffer.size())) return fail(error, damaged);
    size_t end = size_t(-1);
    for (size_t i = buffer.size() - 22 + 1; i-- > 0;)
        if (le32(buffer.data() + i) == endSignature) { end = i; break; }
    if (end == size_t(-1)) return fail(error, damaged);
    const uint8_t* e = buffer.data() + end;
    const uint32_t count = le16(e + 10), directorySize = le32(e + 12), directory = le32(e + 16);
    if (le16(e + 4) != 0 || le16(e + 6) != 0 || le16(e + 8) != count) return fail(error, "Multi-part ZIP files are not supported.");
    if (count == 0xFFFF || directory == 0xFFFFFFFF || directorySize == 0xFFFFFFFF || (end >= 20 && le32(e - 20) == zip64Locator))
        return fail(error, "ZIP64 containers (over 4 GiB or 65535 entries) are not supported.");
    if (count > limits.entries) return fail(error, "The container holds more entries than allowed.");
    const uint64_t directoryEnd = size_ - tail + end;
    if (directory > directoryEnd || directorySize > directoryEnd - directory || uint64_t(count) * 46 > directorySize) return fail(error, damaged);
    std::vector<uint8_t> dir(directorySize);
    if (!readAt(directory, dir.data(), dir.size())) return fail(error, damaged);
    std::set<std::string> seen;
    uint64_t total = 0;
    size_t p = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 46 > dir.size() || le32(dir.data() + p) != centralSignature) return fail(error, damaged);
        const uint8_t* c = dir.data() + p;
        ZipFileEntry entry;
        entry.flags = le16(c + 8);
        entry.method = le16(c + 10);
        entry.crc = le32(c + 16);
        entry.packedSize = le32(c + 20);
        entry.size = le32(c + 24);
        const size_t nameLength = le16(c + 28), extraLength = le16(c + 30), commentLength = le16(c + 32);
        entry.headerOffset = le32(c + 42);
        if (p + 46 + nameLength + extraLength + commentLength > dir.size()) return fail(error, damaged);
        entry.name.assign(reinterpret_cast<const char*>(c + 46), nameLength);
        p += 46 + nameLength + extraLength + commentLength;
        if (entry.packedSize == zipMax || entry.size == zipMax || entry.headerOffset == zipMax)
            return fail(error, "ZIP64 entries are not supported.");
        if (entry.flags & 0x0041) return fail(error, "Encrypted ZIP entries are not supported.");
        if (entry.method != 0 && entry.method != 8) return fail(error, "A ZIP entry uses an unsupported compression method.");
        if (entry.method == 0 && entry.packedSize != entry.size) return fail(error, damaged);
        if (!isSafeZipEntryName(entry.name)) return fail(error, "A ZIP entry has an unsafe name: " + entry.name);
        if (!seen.insert(lowered(entry.name)).second) return fail(error, "The container holds two entries named " + entry.name + ".");
        if (entry.size > limits.entryBytes || entry.size > limits.totalBytes - total) return fail(error, "The container's contents are larger than allowed.");
        total += entry.size;
        if (entry.method == 8 && entry.size > (1u << 20) && entry.size / std::max<uint64_t>(entry.packedSize, 1) > limits.ratio)
            return fail(error, "A ZIP entry claims a suspicious compression ratio.");
        // The data must lie before the directory (no overlap with it, nothing beyond the end).
        if (entry.headerOffset + 30 > directory || entry.packedSize > directory - entry.headerOffset - 30) return fail(error, damaged);
        if (entry.isDirectory() && entry.size != 0) return fail(error, damaged);
        entries_.push_back(std::move(entry));
    }
    return true;
}

const ZipFileEntry* ZipFileReader::find(const std::string& name) const {
    for (const ZipFileEntry& e : entries_) if (e.name == name) return &e;
    return nullptr;
}

bool ZipFileReader::dataOffset(const ZipFileEntry& entry, uint64_t& offset, std::string* error) {
    uint8_t h[30];
    if (!readAt(entry.headerOffset, h, sizeof h) || le32(h) != localSignature) return fail(error, "A ZIP entry's header is damaged: " + entry.name);
    const size_t nameLength = le16(h + 26), extraLength = le16(h + 28);
    std::string name(nameLength, '\0');
    if (!readAt(entry.headerOffset + 30, name.data(), nameLength) || name != entry.name) return fail(error, "A ZIP entry's header does not match the directory: " + entry.name);
    offset = entry.headerOffset + 30 + nameLength + extraLength;
    if (offset > size_ || entry.packedSize > size_ - offset) return fail(error, "A ZIP entry runs past the end of the file: " + entry.name);
    return true;
}

bool ZipFileReader::read(const ZipFileEntry& entry, std::vector<uint8_t>& out, std::string* error) {
    uint64_t offset = 0;
    if (!dataOffset(entry, offset, error)) return false;
    std::vector<uint8_t> packed;
    try {
        packed.resize(size_t(entry.packedSize));
        out.assign(size_t(entry.size), 0);
    } catch (const std::bad_alloc&) {
        return fail(error, "Out of memory reading " + entry.name);
    }
    if (!readAt(offset, packed.data(), packed.size())) return fail(error, "Could not read " + entry.name);
    if (entry.method == 0) {
        out = std::move(packed);
    } else {
        z_stream z{};
        if (inflateInit2(&z, -15) != Z_OK) return fail(error, "Could not decompress " + entry.name);
        z.next_in = packed.data();
        z.avail_in = uInt(packed.size());
        z.next_out = out.data();
        z.avail_out = uInt(out.size());
        const int status = inflate(&z, Z_FINISH);
        const bool complete = status == Z_STREAM_END && z.total_out == entry.size;
        inflateEnd(&z);
        if (!complete) return fail(error, "A ZIP entry is damaged: " + entry.name);
    }
    if (uint32_t(crc32(0, out.data(), uInt(out.size()))) != entry.crc) return fail(error, "A ZIP entry fails its checksum: " + entry.name);
    return true;
}

// ---- Writer -----------------------------------------------------------------------------------------------------------

ZipFileWriter::~ZipFileWriter() {
    if (file_) std::fclose(file_);
}

bool ZipFileWriter::open(const std::string& path, std::string* error) {
    if (file_) std::fclose(file_);
    entries_.clear();
    offset_ = 0;
#ifdef _WIN32
    // The path as the rest of the core reads it (std::filesystem), opened wide.
    file_ = _wfopen(std::filesystem::path(path).c_str(), L"wb");
#else
    file_ = std::fopen(path.c_str(), "wb");
#endif
    if (!file_) return fail(error, "could not create " + path);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const int year = std::max(local.tm_year + 1900, 1980);
    time_ = uint16_t((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
    date_ = uint16_t(((year - 1980) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
    return true;
}

bool ZipFileWriter::write(const void* data, size_t size) {
    if (!file_) return false;
    if (size && std::fwrite(data, 1, size, file_) != size) return false;
    offset_ += size;
    return true;
}

bool ZipFileWriter::add(const std::string& name, const uint8_t* data, size_t size, bool deflate, std::string* error) {
    if (!file_) return fail(error, "the file is not open");
    if (!isSafeZipEntryName(name)) return fail(error, "unsafe entry name " + name);
    if (entries_.size() >= 65535) return fail(error, "too many files for one document");
    if (uint64_t(size) >= zipMax) return fail(error, name + " is larger than 4 GiB");
    ZipFileEntry entry;
    entry.name = name;
    entry.flags = 0x0800;   // UTF-8 names
    entry.size = size;
    entry.crc = uint32_t(crc32(0, data, uInt(size)));
    entry.headerOffset = offset_;
    std::vector<uint8_t> packed;
    const uint8_t* body = data;
    if (deflate && size > 0) {
        z_stream z{};
        if (deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return fail(error, "could not compress " + name);
        packed.resize(deflateBound(&z, uLong(size)));
        z.next_in = const_cast<Bytef*>(data);
        z.avail_in = uInt(size);
        z.next_out = packed.data();
        z.avail_out = uInt(packed.size());
        const int status = ::deflate(&z, Z_FINISH);
        const uLong produced = z.total_out;
        deflateEnd(&z);
        if (status != Z_STREAM_END) return fail(error, "could not compress " + name);
        packed.resize(produced);
        if (packed.size() < size) { entry.method = 8; body = packed.data(); }
    }
    entry.packedSize = entry.method == 8 ? packed.size() : size;
    if (offset_ + 30 + name.size() + entry.packedSize >= zipMax) return fail(error, "the document is larger than 4 GiB, which this format does not hold");
    std::vector<uint8_t> h;
    put32(h, localSignature);
    put16(h, 20);
    put16(h, entry.flags);
    put16(h, entry.method);
    put16(h, time_);
    put16(h, date_);
    put32(h, entry.crc);
    put32(h, uint32_t(entry.packedSize));
    put32(h, uint32_t(entry.size));
    put16(h, uint32_t(name.size()));
    put16(h, 0);
    h.insert(h.end(), name.begin(), name.end());
    if (!write(h.data(), h.size()) || !write(body, size_t(entry.packedSize))) return fail(error, "could not write " + name);
    entries_.push_back(std::move(entry));
    return true;
}

bool ZipFileWriter::finish(std::string* error) {
    if (!file_) return fail(error, "the file is not open");
    const uint64_t directory = offset_;
    std::vector<uint8_t> d;
    for (const ZipFileEntry& e : entries_) {
        put32(d, centralSignature);
        put16(d, 20);   // made by: MS-DOS attributes, version 2.0
        put16(d, 20);
        put16(d, e.flags);
        put16(d, e.method);
        put16(d, time_);
        put16(d, date_);
        put32(d, e.crc);
        put32(d, uint32_t(e.packedSize));
        put32(d, uint32_t(e.size));
        put16(d, uint32_t(e.name.size()));
        put16(d, 0);
        put16(d, 0);
        put16(d, 0);
        put16(d, 0);
        put32(d, 0);
        put32(d, uint32_t(e.headerOffset));
        d.insert(d.end(), e.name.begin(), e.name.end());
    }
    if (directory + d.size() + 22 >= zipMax) return fail(error, "the document is larger than 4 GiB, which this format does not hold");
    const uint32_t directorySize = uint32_t(d.size());
    put32(d, endSignature);
    put16(d, 0);
    put16(d, 0);
    put16(d, uint32_t(entries_.size()));
    put16(d, uint32_t(entries_.size()));
    put32(d, directorySize);
    put32(d, uint32_t(directory));
    put16(d, 0);
    bool ok = write(d.data(), d.size()) && std::fflush(file_) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(file_)) == 0;
#else
    ok = ok && ::fsync(fileno(file_)) == 0;
#endif
    ok = std::fclose(file_) == 0 && ok;
    file_ = nullptr;
    if (!ok) return fail(error, "could not write the file to disk");
    return true;
}

} // namespace compositor
