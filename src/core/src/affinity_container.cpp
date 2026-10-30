// The Affinity reader's container (affinity_read.h): the stream table, a stream's bytes (raw, zlib or zstd, its
// predictors undone, its CRC checked) and the flattened preview. Ported from Patchy with the rest (affinity.cpp).
#include "affinity_read.h"
#include "nekophoto/colour.h"
#include "nekophoto/png.h"
#include "nekophoto/render.h"
#include "nekophoto/vectormask.h"
#include "lcms2.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <tuple>
#include <unordered_map>
#include <zlib.h>

#ifdef NEKOPHOTO_HAVE_ZSTD
#include <zstd.h>
#endif

namespace nekophoto::affinity_read {

constexpr uint32_t magicTag = 0x414BFF00u;
constexpr uint32_t infTag = 0x666E4923u;    // "#Inf"
constexpr uint32_t protTag = 0x746F7250u;   // "Prot"
constexpr uint32_t filTag = 0x6C694623u;    // "#Fil"
constexpr uint32_t thmbTag = 0x626D6854u;   // "Thmb"
constexpr std::array<uint32_t, 4> fatTags = {0x54414623u, 0x32544623u, 0x33544623u, 0x34544623u};   // #FAT #FT2 #FT3 #FT4
constexpr size_t maxFatChain = 64;
constexpr uint32_t maxStreamsPerFat = 1u << 20;
constexpr uint16_t maxNameLength = 4096;
constexpr uint64_t maxStreamBytes = 512ull << 20;

// ------------------------------------------------------------------------------------------------ container

Container parseContainer(std::span<const uint8_t> bytes) {
    Container container;
    Reader r(bytes);
    if (r.u32() != magicTag) fail("Not an Affinity document.");
    container.version = r.u16();
    (void)r.u16();   // flags
    (void)r.u32();   // class tag
    if (r.u32() != infTag) fail("The Affinity document's info block is missing.");
    const uint64_t fatOffset = r.u64();
    container.thumbnailOffset = r.u64();
    r.skip(8 + 8 + 8 + 4 + 4);
    if (container.version > 7 && r.u32() != protTag) fail("The Affinity document's protocol block is missing.");

    // The #Inf offset names the NEWEST table link; each link's next offset leads to an older save. A name may be
    // declared only in an older link than its newest data record, so collect every link, then apply them oldest
    // first with newer records overwriting.
    std::unordered_map<uint32_t, std::string> names;
    std::vector<std::vector<std::pair<uint32_t, Stream>>> links;
    uint64_t next = fatOffset;
    while (next != 0) {
        if (links.size() >= maxFatChain) fail("The Affinity document's stream table recurses.");
        if (next > bytes.size()) fail("The Affinity document's stream table is out of range.");
        r.seek(size_t(next));
        const uint32_t fat = r.u32();
        if (std::find(fatTags.begin(), fatTags.end(), fat) == fatTags.end()) fail("The Affinity document's stream table is damaged.");
        const bool oldest = fat == fatTags[0], ft4 = fat == fatTags[3];
        next = r.u64();
        r.skip(8 * 4);
        const uint32_t files = r.u32();
        r.skip(8);
        const uint16_t dirs = r.u16();
        r.skip(1);
        if (files > maxStreamsPerFat) fail("The Affinity document's stream table is implausible.");
        auto& link = links.emplace_back();
        for (uint32_t i = 0; i < files; i++) {
            const uint32_t id = r.u32();
            const uint8_t flag = r.u8();
            if (flag > 2) fail("An Affinity stream entry is damaged.");
            Stream s;
            if (flag <= 1) {
                s.offset = r.u64();
                s.size = r.u64();
                s.compressed = r.u64();
                s.crc = r.u32();
                s.compression = r.u8();
                if (!oldest) r.skip(4);
                if (ft4) r.skip(4);
                if (oldest || fat == fatTags[1]) {
                    // The two oldest layouts store an indirect compression code.
                    static constexpr uint8_t codes[] = {0, 0x01, 0x41, 0x81, 0xC1};
                    s.compression = s.compression < 5 ? codes[s.compression] : 0;
                }
            }
            if (flag == 0) {
                const uint16_t length = r.u16();
                if (length > maxNameLength) fail("An Affinity stream name is implausible.");
                std::string name(length, '\0');
                for (auto& c : name) c = char(r.u8());
                names.try_emplace(id, std::move(name));   // walked newest first: the newest name wins
            }
            if (flag <= 1) link.emplace_back(id, s);
        }
        for (uint16_t d = 0; d < dirs; d++) {
            const uint16_t length = r.u16();
            r.skip(2 + 8);
            if (length > maxNameLength) fail("An Affinity directory name is implausible.");
            r.skip(length);
        }
    }
    for (auto link = links.rbegin(); link != links.rend(); ++link)
        for (const auto& [id, s] : *link)
            if (auto name = names.find(id); name != names.end()) container.streams[name->second] = s;
    return container;
}

std::vector<uint8_t> extractStream(std::span<const uint8_t> bytes, const Stream& s, const std::string& name, std::vector<std::string>* notes) {
    if (s.size == 0 || s.size > maxStreamBytes) fail("Affinity stream '" + name + "' has an implausible size.");
    if (s.offset + 4 > bytes.size()) fail("Affinity stream '" + name + "' is out of range.");
    Reader r(bytes);
    r.seek(size_t(s.offset));
    if (r.u32() != filTag) fail("Affinity stream '" + name + "' is damaged.");
    const uint32_t algorithm = s.compression & 0x03u;
    bool alternate = ((s.compression >> 5) & 1u) != 0;
    uint32_t predictor = 0;
    switch (s.compression & 0xC0u) {
    case 0x40: predictor = 1; alternate = false; break;
    case 0x80: predictor = 2; break;
    case 0xC0: predictor = 3; break;
    default: alternate = false; break;
    }
    const uint64_t stored = algorithm == 0 ? s.size : s.compressed;
    if (stored > r.remaining()) fail("Affinity stream '" + name + "' is truncated.");
    const uint8_t* source = bytes.data() + r.position();
    // The declared size is only trusted as a ceiling: the output grows in steps as the data inflates, so a small
    // stream claiming hundreds of megabytes fails before that memory is taken.
    std::vector<uint8_t> out;
    constexpr size_t step = size_t(8) << 20;
    const size_t expected = size_t(s.size);
    if (algorithm == 1) {
        z_stream z{};
        if (inflateInit(&z) != Z_OK) fail("Affinity stream '" + name + "' failed to decompress.");
        z.next_in = const_cast<Bytef*>(source);
        z.avail_in = uInt(std::min<uint64_t>(stored, UINT32_MAX));
        int rc = Z_OK;
        while (rc == Z_OK && out.size() < expected) {
            const size_t have = out.size();
            out.resize(std::min(expected, have + step));
            z.next_out = out.data() + have;
            z.avail_out = uInt(out.size() - have);
            rc = inflate(&z, Z_NO_FLUSH);
            out.resize(out.size() - z.avail_out);
            if (rc == Z_OK && out.size() == have) break;
        }
        inflateEnd(&z);
        if (rc != Z_STREAM_END || out.size() != expected) fail("Affinity stream '" + name + "' failed to decompress.");
    } else if (algorithm == 2) {
#ifdef NEKOPHOTO_HAVE_ZSTD
        ZSTD_DStream* z = ZSTD_createDStream();
        if (!z) fail("Affinity stream '" + name + "' failed to decompress.");
        ZSTD_inBuffer in{source, size_t(stored), 0};
        size_t rc = 1;
        bool broken = false;
        while (out.size() < expected) {
            const size_t have = out.size();
            out.resize(std::min(expected, have + step));
            ZSTD_outBuffer o{out.data() + have, out.size() - have, 0};
            rc = ZSTD_decompressStream(z, &o, &in);
            out.resize(have + o.pos);
            if (ZSTD_isError(rc)) { broken = true; break; }
            if (o.pos == 0 && (rc == 0 || in.pos == in.size)) break;   // frame done or input spent
        }
        ZSTD_freeDStream(z);
        if (broken || out.size() != expected) fail("Affinity stream '" + name + "' failed to decompress.");
#else
        fail("This build reads no zstd-compressed Affinity streams (it was made without libzstd).");
#endif
    } else if (algorithm == 0) {
        out.assign(source, source + size_t(stored));   // stored <= what the file holds
    } else {
        fail("Affinity stream '" + name + "' uses an unknown compression.");
    }
    if (!alternate && predictor == 1) affinity_detail::undoByteDelta(out);
    else if (!alternate && predictor == 2) affinity_detail::undoU16Delta(out);
    else if (alternate && predictor == 2) { affinity_detail::undoByteDelta(out); affinity_detail::undoTileInterleave(out); }
    const uint32_t crc = uint32_t(crc32(0, out.data(), uInt(out.size())));
    if (crc != s.crc && notes) notes->push_back("Affinity stream '" + name + "' failed its checksum; the file may be damaged.");
    return out;
}

/// The embedded preview (premultiplied), from the PNG at the thumbnail offset.
std::shared_ptr<Image> extractPreview(std::span<const uint8_t> bytes, const Container& container) {
    const uint64_t at = container.thumbnailOffset;
    if (at == 0 || at + 8 > bytes.size()) return nullptr;
    Reader r(bytes);
    r.seek(size_t(at));
    if (r.u32() != 0xFFFFFFFFu || r.u32() != thmbTag) return nullptr;
    static constexpr uint8_t signature[4] = {0x89, 0x50, 0x4E, 0x47};
    const size_t start = r.position(), end = std::min(bytes.size(), start + 256);
    for (size_t i = start; i + 4 <= end; i++)
        if (std::equal(signature, signature + 4, bytes.begin() + std::ptrdiff_t(i)))
            return decodePngImage(bytes.data() + i, bytes.size() - i);
    return nullptr;
}

} // namespace nekophoto::affinity_read
