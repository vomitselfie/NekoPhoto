// The brush files the importer tests write byte by byte (a Photoshop .abr, a Procreate .brushset and a Clip
// Studio .sut), shared with the brush parity harness so it can paint with what the importers make of them.
#pragma once
#include "compositor/image.h"
#include "compositor/png.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>
#include <zlib.h>
#ifdef COMPOSITOR_HAVE_SQLITE
#include <sqlite3.h>
#endif

namespace brushfixtures {

using namespace compositor;
namespace fs = std::filesystem;

/// Writes `bytes` to a file in the temporary folder and returns its path.
inline std::string writeTemp(const std::string& name, const std::vector<uint8_t>& bytes) {
    fs::path path = fs::temp_directory_path() / name;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return path.string();
}

// ---- Photoshop .abr ---------------------------------------------------------------------------------------------

struct Out {
    std::vector<uint8_t> b;
    void u8(unsigned v) { b.push_back(uint8_t(v)); }
    void u16(unsigned v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xFFFF); }
    void f64(double v) { uint64_t bits; std::memcpy(&bits, &v, 8); u32(uint32_t(bits >> 32)); u32(uint32_t(bits)); }
    void chars(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
    void unicode(const std::string& s) { u32(uint32_t(s.size() + 1)); for (char c : s) u16(uint8_t(c)); u16(0); }
    void key(const std::string& k) { if (k.size() == 4) { u32(0); chars(k); } else { u32(uint32_t(k.size())); chars(k); } }
    void append(const Out& o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
};

/// A 16x12 tip: a solid block in the right half, empty left half.
inline std::vector<uint8_t> tipPixels() {
    std::vector<uint8_t> p(16 * 12, 0);
    for (int y = 0; y < 12; y++) for (int x = 8; x < 16; x++) p[size_t(y * 16 + x)] = 255;
    return p;
}

/// The same rows PackBits-encoded (a literal run of 8 zeros, then a repeat of 8 x 255), counts first.
inline void packedTip(Out& o) {
    for (int y = 0; y < 12; y++) o.u16(4);
    for (int y = 0; y < 12; y++) { o.u8(uint8_t(-7)); o.u8(0); o.u8(uint8_t(-7)); o.u8(255); }
}

/// A version 6 file: one sampled tip and one preset, "Leaf", using it with pressure on size (down to 25%), size
/// jitter, scatter and a count of 3.
inline std::vector<uint8_t> abrVersion6File() {
    const std::string uuid = "11111111-2222-3333-4444-555555555555";
    // samp: one entry, bounds 301 bytes after its start (the version 6.2 layout).
    Out entry;
    entry.u8(uint8_t(uuid.size())); entry.chars(uuid);
    while (entry.b.size() < 301) entry.u8(0);
    entry.u32(0); entry.u32(0); entry.u32(12); entry.u32(16); entry.u16(8); entry.u8(1);
    packedTip(entry);
    Out samp;
    samp.u32(uint32_t(entry.b.size())); samp.append(entry);
    while (samp.b.size() % 4) samp.u8(0);
    // desc: one preset using that sample, with pressure on size and scatter.
    Out d;
    d.u32(16); d.unicode(""); d.key("null"); d.u32(1);
    d.key("Brsh"); d.chars("VlLs"); d.u32(1);
    d.chars("Objc"); d.unicode(""); d.key("brushPreset"); d.u32(8);
    d.key("Nm  "); d.chars("TEXT"); d.unicode("Leaf");
    d.key("Brsh"); d.chars("Objc"); d.unicode(""); d.key("sampledBrush"); d.u32(6);
    d.key("Dmtr"); d.chars("UntF"); d.chars("#Pxl"); d.f64(64);
    d.key("Angl"); d.chars("UntF"); d.chars("#Ang"); d.f64(30);
    d.key("Rndn"); d.chars("UntF"); d.chars("#Prc"); d.f64(80);
    d.key("Spcn"); d.chars("UntF"); d.chars("#Prc"); d.f64(40);
    d.key("Intr"); d.chars("bool"); d.u8(1);
    d.key("sampledData"); d.chars("TEXT"); d.unicode(uuid);
    d.key("useTipDynamics"); d.chars("bool"); d.u8(1);
    d.key("szVr"); d.chars("Objc"); d.unicode(""); d.key("brVr"); d.u32(2);
    d.key("bVTy"); d.chars("long"); d.u32(2);
    d.key("jitter"); d.chars("UntF"); d.chars("#Prc"); d.f64(20);
    d.key("minimumDiameter"); d.chars("UntF"); d.chars("#Prc"); d.f64(25);
    d.key("useScatter"); d.chars("bool"); d.u8(1);
    d.key("scatterDynamics"); d.chars("Objc"); d.unicode(""); d.key("brVr"); d.u32(1);
    d.key("jitter"); d.chars("UntF"); d.chars("#Prc"); d.f64(150);
    d.key("Cnt "); d.chars("doub"); d.f64(3);
    Out f;
    f.u16(6); f.u16(2);
    f.chars("8BIM"); f.chars("samp"); f.u32(uint32_t(samp.b.size())); f.append(samp);
    f.chars("8BIM"); f.chars("desc"); f.u32(uint32_t(d.b.size())); f.append(d);
    return f.b;
}

// ---- Procreate .brushset -----------------------------------------------------------------------------------------

inline uint32_t crc32(const std::vector<uint8_t>& data) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint8_t b : data) {
        crc ^= b;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

/// A ZIP of stored (uncompressed) files.
inline std::vector<uint8_t> zip(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    std::vector<uint8_t> out, directory;
    auto le16 = [](std::vector<uint8_t>& v, unsigned x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); };
    auto le32 = [&](std::vector<uint8_t>& v, uint32_t x) { le16(v, x & 0xFFFF); le16(v, x >> 16); };
    for (const auto& [name, data] : files) {
        const uint32_t offset = uint32_t(out.size()), crc = crc32(data), size = uint32_t(data.size());
        le32(out, 0x04034b50); le16(out, 20); le16(out, 0); le16(out, 0); le32(out, 0); le32(out, crc); le32(out, size); le32(out, size);
        le16(out, unsigned(name.size())); le16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());
        le32(directory, 0x02014b50); le16(directory, 20); le16(directory, 20); le16(directory, 0); le16(directory, 0); le32(directory, 0);
        le32(directory, crc); le32(directory, size); le32(directory, size); le16(directory, unsigned(name.size())); le16(directory, 0); le16(directory, 0);
        le16(directory, 0); le16(directory, 0); le32(directory, 0); le32(directory, offset);
        directory.insert(directory.end(), name.begin(), name.end());
    }
    const uint32_t start = uint32_t(out.size());
    out.insert(out.end(), directory.begin(), directory.end());
    le32(out, 0x06054b50); le16(out, 0); le16(out, 0); le16(out, unsigned(files.size())); le16(out, unsigned(files.size()));
    le32(out, uint32_t(directory.size())); le32(out, start); le16(out, 0);
    return out;
}

/// A keyed archive of one root dictionary with number and string settings, as a binary plist.
inline std::vector<uint8_t> keyedArchive(const std::map<std::string, std::variant<double, std::string>>& settings) {
    // Objects: 0 top dict, 1 "$objects" array, 2 "$null", 3 root dict, then keys and values; the keys of the top.
    std::vector<std::vector<uint8_t>> objects;
    auto add = [&](std::vector<uint8_t> o) { objects.push_back(std::move(o)); return uint8_t(objects.size() - 1); };
    auto ascii = [&](const std::string& s) { std::vector<uint8_t> o{uint8_t(0x50 | std::min<size_t>(s.size(), 15))}; if (s.size() >= 15) { o.push_back(0x10); o.push_back(uint8_t(s.size())); } o.insert(o.end(), s.begin(), s.end()); return add(o); };
    auto real = [&](double v) { uint64_t bits; std::memcpy(&bits, &v, 8); std::vector<uint8_t> o{0x23}; for (int k = 7; k >= 0; k--) o.push_back(uint8_t(bits >> (8 * k))); return add(o); };
    auto uid = [&](uint8_t v) { return add({0x80, v}); };
    add({});   // 0: top, filled at the end
    add({});   // 1: $objects, filled at the end
    const uint8_t null = ascii("$null");
    const uint8_t root = add({});   // filled below
    std::vector<uint8_t> keys, values;
    std::vector<uint8_t> archivedStrings;   // $objects entries for string settings
    for (const auto& [key, value] : settings) {
        keys.push_back(ascii(key));
        if (std::holds_alternative<double>(value)) values.push_back(real(std::get<double>(value)));
        else { const uint8_t s = ascii(std::get<std::string>(value)); archivedStrings.push_back(s); values.push_back(uid(uint8_t(1 + archivedStrings.size()))); }
    }
    auto dict = [](const std::vector<uint8_t>& k, const std::vector<uint8_t>& v) {
        std::vector<uint8_t> o{uint8_t(0xD0 | std::min<size_t>(k.size(), 15))};
        if (k.size() >= 15) { o.push_back(0x10); o.push_back(uint8_t(k.size())); }
        o.insert(o.end(), k.begin(), k.end());
        o.insert(o.end(), v.begin(), v.end());
        return o;
    };
    objects[root] = dict(keys, values);
    // $objects: [$null, root, the string values...]; UIDs index this array.
    std::vector<uint8_t> list{uint8_t(0xA0 | (2 + archivedStrings.size()))};
    list.push_back(null);
    list.push_back(root);
    for (uint8_t s : archivedStrings) list.push_back(s);
    objects[1] = list;
    const uint8_t topKey = ascii("$top"), objectsKey = ascii("$objects"), archiverKey = ascii("$archiver"), archiver = ascii("NSKeyedArchiver");
    const uint8_t rootKey = ascii("root"), rootUid = uid(1);
    const uint8_t topDict = add(dict({rootKey}, {rootUid}));
    objects[0] = dict({topKey, objectsKey, archiverKey}, {topDict, 1, archiver});
    std::vector<uint8_t> out{'b', 'p', 'l', 'i', 's', 't', '0', '0'};
    std::vector<uint32_t> offsets;
    for (const auto& o : objects) { offsets.push_back(uint32_t(out.size())); out.insert(out.end(), o.begin(), o.end()); }
    const uint32_t table = uint32_t(out.size());
    for (uint32_t off : offsets) for (int k = 3; k >= 0; k--) out.push_back(uint8_t(off >> (8 * k)));
    std::vector<uint8_t> trailer(32, 0);
    trailer[6] = 4; trailer[7] = 1;
    auto be64 = [&](size_t at, uint64_t v) { for (int k = 7; k >= 0; k--) trailer[at + size_t(7 - k)] = uint8_t(v >> (8 * k)); };
    be64(8, objects.size()); be64(16, 0); be64(24, table);
    out.insert(out.end(), trailer.begin(), trailer.end());
    return out;
}

/// A shape: white bars on Procreate's near-black (20) background.
inline std::vector<uint8_t> shapePng() {
    Image image(32, 32);
    image.fill(20, 20, 20, 255);
    for (int y = 0; y < 32; y++) for (int x = 20; x < 28; x++) { uint8_t* p = image.pixel(x, y); p[0] = p[1] = p[2] = 255; }
    std::vector<uint8_t> bytes;
    encodePngImage(image, bytes);
    return bytes;
}

/// A brushset of two brushes listed in the opposite order to the archive: "Bars" and "Soft Ink".
inline std::vector<uint8_t> procreateBrushsetFile() {
    const std::string list = R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict><key>brushes</key><array><string>BBBB</string><string>AAAA</string></array><key>name</key><string>Test &amp; Set</string></dict></plist>)";
    const auto archiveA = keyedArchive({{"name", std::string("Soft Ink")}, {"plotSpacing", 0.08}, {"plotJitter", 0.5}, {"shapeRotation", 1.0},
                                        {"shapeScatter", 0.5}, {"shapeCount", 0.25}, {"dynamicsPressureSize", 0.75}, {"maxSize", 0.1}, {"maxOpacity", 0.8}});
    const auto archiveB = keyedArchive({{"name", std::string("Bars")}, {"plotSpacing", 0.2}, {"maxSize", 0.5}, {"bundledGrainPath", std::string("paper")}});
    const auto file = zip({{"brushset.plist", std::vector<uint8_t>(list.begin(), list.end())},
                           {"AAAA/Brush.archive", archiveA}, {"AAAA/Shape.png", shapePng()},
                           {"AAAA/Reset/Brush.archive", archiveB},
                           {"BBBB/Brush.archive", archiveB}, {"BBBB/Shape.png", shapePng()}});
    return file;
}

#ifdef COMPOSITOR_HAVE_SQLITE

// ---- Clip Studio .sut --------------------------------------------------------------------------------------------

inline void be32(std::vector<uint8_t>& out, uint32_t v) { const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; out.insert(out.end(), b, b + 4); }
inline void le32(std::vector<uint8_t>& out, uint32_t v) { const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; out.insert(out.end(), b, b + 4); }
inline void utf16be(std::vector<uint8_t>& out, const std::string& text) { for (char c : text) { out.push_back(0); out.push_back(uint8_t(c)); } }

inline std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// A layer file as Clip Studio writes one: a 1024-byte-page SQLite database filled by `fill`, in a C2F
/// container whose first dATA chunk stands for the enciphered first five pages and whose second holds the rest.
inline std::vector<uint8_t> layerFile(const std::function<void(sqlite3*)>& fill) {
    const fs::path path = fs::temp_directory_path() / "compositor-test-layer.db";
    fs::remove(path);
    sqlite3* db = nullptr;
    sqlite3_open(path.string().c_str(), &db);
    // Filler pushes the database past five pages; the rows before the real ones split the table's root, so
    // every leaf that matters lies beyond page 5.
    sqlite3_exec(db, "PRAGMA page_size=1024; CREATE TABLE Filler(b BLOB); CREATE TABLE Rows(a, b, c, d, Attribute BLOB, BlockData BLOB);"
        "INSERT INTO Filler VALUES (zeroblob(8000));"
        "WITH RECURSIVE k(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM k WHERE i < 20) INSERT INTO Rows(a, b) SELECT i, randomblob(200) FROM k;",
        nullptr, nullptr, nullptr);
    fill(db);
    sqlite3_close(db);
    const std::vector<uint8_t> file = readFile(path);
    fs::remove(path);
    std::vector<uint8_t> out = {0x89, 'C', '2', 'F', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& payload) {
        le32(out, uint32_t(payload.size()));
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), payload.begin(), payload.end());
        le32(out, 0);
    };
    chunk("HEAD", std::vector<uint8_t>(16, 0));
    chunk("dATA", std::vector<uint8_t>(5 * 1024 + 10, 0x5A));
    std::vector<uint8_t> pages(2 + file.size() - 5 * 1024, 0);   // a two-byte prefix, then pages 6 onwards
    std::copy(file.begin() + 5 * 1024, file.end(), pages.begin() + 2);
    chunk("dATA", pages);
    chunk("TAIL", {});
    return out;
}

/// A ustar archive of the files.
inline std::vector<uint8_t> tar(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    std::vector<uint8_t> out;
    for (const auto& [name, data] : files) {
        std::vector<uint8_t> header(512, 0);
        std::memcpy(header.data(), name.data(), name.size());
        std::snprintf(reinterpret_cast<char*>(header.data() + 124), 12, "%011o", unsigned(data.size()));
        header[156] = '0';
        std::memcpy(header.data() + 257, "ustar", 6);
        out.insert(out.end(), header.begin(), header.end());
        out.insert(out.end(), data.begin(), data.end());
        out.resize((out.size() + 511) / 512 * 512, 0);
    }
    out.resize(out.size() + 1024, 0);
    return out;
}

inline std::vector<uint8_t> text(const std::string& s) { return {s.begin(), s.end()}; }

/// A Variant's reference to a material: a count, the entry's length, and its path in UTF-16LE.
inline std::vector<uint8_t> reference(const std::string& path) {
    std::vector<uint8_t> out;
    be32(out, 8); be32(out, 1); be32(out, uint32_t(16 + path.size() * 2)); be32(out, uint32_t(path.size() * 2));
    for (char c : path) { out.push_back(uint8_t(c)); out.push_back(0); }
    return out;
}

inline void bindBlob(sqlite3_stmt* statement, int index, const std::vector<uint8_t>& blob) {
    sqlite3_bind_blob(statement, index, blob.data(), int(blob.size()), SQLITE_TRANSIENT);
}

/// The value of the tiled tip at (x, y) of its 256-pixel tile, before the reader stretches it.
inline uint8_t tileValue(int x, int y) { return uint8_t(1 + (x * 31 + y * 17) % 100); }


/// Writes a .sut of two brushes to `path`: "Soft Pencil" (a paper texture) and "Spray" (a tiled tip, spray, and size
/// on pressure down to 10%).
inline bool writeClipStudioFile(const fs::path& path) {
    // A tip as Offscreen tiles: 300 by 200 on a grid of 2 by 1, where only tile 1 has data.
    std::vector<uint8_t> attribute, blocks;
    attribute.insert(attribute.end(), 16, uint8_t(0x11));
    be32(attribute, 9); utf16be(attribute, "Parameter");
    be32(attribute, 300); be32(attribute, 200); be32(attribute, 2); be32(attribute, 1);
    attribute.resize(attribute.size() + 40, 0);
    auto block = [&](const std::string& name, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> b;
        be32(b, uint32_t(8 + name.size() * 2 + body.size()));
        be32(b, uint32_t(name.size()));
        utf16be(b, name);
        b.insert(b.end(), body.begin(), body.end());
        blocks.insert(blocks.end(), b.begin(), b.end());
    };
    std::vector<uint8_t> empty;
    be32(empty, 0); be32(empty, 65536); be32(empty, 256); be32(empty, 256); be32(empty, 0);
    block("BlockDataBeginChunk", empty);
    block("BlockStatus", std::vector<uint8_t>(12, 0));
    std::vector<uint8_t> pixels(65536);
    for (int y = 0; y < 256; y++) for (int x = 0; x < 256; x++) pixels[size_t(y) * 256 + size_t(x)] = tileValue(x, y);
    uLongf packedSize = compressBound(uLong(pixels.size()));
    std::vector<uint8_t> packed(packedSize);
    if (!(compress(packed.data(), &packedSize, pixels.data(), uLong(pixels.size())) == Z_OK)) return false;
    packed.resize(packedSize);
    std::vector<uint8_t> full;
    be32(full, 1); be32(full, 65536); be32(full, 256); be32(full, 256); be32(full, 1);
    be32(full, uint32_t(packed.size())); le32(full, uint32_t(packed.size()));
    full.insert(full.end(), packed.begin(), packed.end());
    block("BlockDataBeginChunk", full);
    block("BlockDataEndChunk", {});
    const std::vector<uint8_t> tipLayer = layerFile([&](sqlite3* db) {
        sqlite3_stmt* insert = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO Rows VALUES (NULL, 3, 0, 3, ?1, ?2)", -1, &insert, nullptr);
        bindBlob(insert, 1, attribute);
        bindBlob(insert, 2, blocks);
        sqlite3_step(insert);
        sqlite3_finalize(insert);
    });
    // A paper texture as a PNG: 40 by 30, its alpha peaking at 200.
    Image texture(40, 30);
    for (int y = 0; y < 30; y++) for (int x = 0; x < 40; x++) texture.pixel(x, y)[3] = uint8_t((x * 5 + y) % 201);
    std::vector<uint8_t> png;
    if (!(encodePngImage(texture, png))) return false;
    const std::vector<uint8_t> textureLayer = layerFile([&](sqlite3* db) {
        sqlite3_stmt* insert = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO Rows(a, b) VALUES (1, ?1)", -1, &insert, nullptr);
        bindBlob(insert, 1, png);
        sqlite3_step(insert);
        sqlite3_finalize(insert);
    });
    const auto tipMaterial = tar({{"icedata/layerData.xml", text("<data>BrushPattern</data>")}, {"data/material_0.layer", tipLayer}});
    const auto textureMaterial = tar({{"icedata/layerData.xml", text("<data>PaperTexture</data>")}, {"data/material.layer", textureLayer}});

    fs::remove(path);
    sqlite3* db = nullptr;
    if (!(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK)) return false;
    const char* script =
        "CREATE TABLE Node(_PW_ID INTEGER PRIMARY KEY AUTOINCREMENT, NodeName TEXT, NodeVariantID INTEGER);"
        "CREATE TABLE Variant(_PW_ID INTEGER PRIMARY KEY AUTOINCREMENT, VariantID INTEGER, BrushSize REAL, BrushSizeUnit INTEGER,"
        " BrushHardness INTEGER, BrushInterval REAL, BrushThickness INTEGER, BrushRotation REAL, BrushFlow INTEGER, BrushUseSpray INTEGER,"
        " BrushSpraySize REAL, BrushSpraySizeUnit INTEGER, BrushSprayDensity INTEGER, BrushUsePatternImage INTEGER, BrushUseWaterColor INTEGER,"
        " UseDualBrush INTEGER, BrushPatternImageArray BLOB, TextureImage BLOB, TextureDensity INTEGER, TextureScale2 REAL, TextureReverseDensity INTEGER,"
        " BrushSizeEffector BLOB, BrushOpacityEffector BLOB);"
        "CREATE TABLE MaterialFile(_PW_ID INTEGER PRIMARY KEY AUTOINCREMENT, FileData BLOB);"
        "INSERT INTO Node(NodeName, NodeVariantID) VALUES ('Soft Pencil', 11), ('Spray', 12);"
        "INSERT INTO Variant(VariantID, BrushSize, BrushSizeUnit, BrushHardness, BrushInterval, BrushThickness, BrushRotation, BrushFlow,"
        " BrushUseSpray, BrushSpraySize, BrushSpraySizeUnit, BrushSprayDensity, BrushUsePatternImage, BrushUseWaterColor, UseDualBrush)"
        " VALUES (11, 12.0, 0, 40, 5.0, 100, 0.0, 80, 0, 0, 0, 0, 0, 0, 0), (12, 40.0, 0, 100, 25.0, 50, 45.0, 100, 1, 20.0, 0, 6, 1, 0, 0);";
    if (!(sqlite3_exec(db, script, nullptr, nullptr, nullptr) == SQLITE_OK)) return false;
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(db, "UPDATE Variant SET TextureImage = ?1, TextureDensity = 40, TextureScale2 = 50, TextureReverseDensity = 1 WHERE VariantID = 11", -1, &statement, nullptr);
    bindBlob(statement, 1, reference(".:Install:Paint002:1234:data:material.layer"));
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    sqlite3_prepare_v2(db, "UPDATE Variant SET BrushPatternImageArray = ?1 WHERE VariantID = 12", -1, &statement, nullptr);
    bindBlob(statement, 1, reference(".:Install:Paint110:5678:data:material_0.layer"));
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    // Size follows pressure down to 10% (flags 0x90, as Clip Studio writes it); opacity does not (flags 0).
    auto effector = [](uint32_t flags, uint32_t minimum) {
        std::vector<uint8_t> out;
        for (uint32_t v : {44u, 0xF0u, flags, minimum, 100u, 0u, 0u, 0u, 0u, 0u, 0u}) be32(out, v);
        return out;
    };
    sqlite3_prepare_v2(db, "UPDATE Variant SET BrushSizeEffector = ?1, BrushOpacityEffector = ?2 WHERE VariantID = 12", -1, &statement, nullptr);
    bindBlob(statement, 1, effector(0x90, 10));
    bindBlob(statement, 2, effector(0, 0));
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    sqlite3_prepare_v2(db, "INSERT INTO MaterialFile(FileData) VALUES (?1), (?2)", -1, &statement, nullptr);
    bindBlob(statement, 1, textureMaterial);
    bindBlob(statement, 2, tipMaterial);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return true;
}

#endif

} // namespace brushfixtures
