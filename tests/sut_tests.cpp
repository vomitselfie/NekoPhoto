// Clip Studio import: a .sut-shaped SQLite database made here (Node and Variant rows with the columns the
// reader uses, and MaterialFile rows holding tars of C2F layer files built from SQLite databases made here
// too), when the build has SQLite; and a real file when COMPOSITOR_SUT_SAMPLE names one.
#include "check.h"
#include "compositor/brushimport.h"
#include "compositor/png.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include <zlib.h>

#ifdef COMPOSITOR_HAVE_SQLITE
#include <sqlite3.h>
#endif

using namespace compositor;
namespace fs = std::filesystem;

#ifdef COMPOSITOR_HAVE_SQLITE

namespace {

void be32(std::vector<uint8_t>& out, uint32_t v) { const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; out.insert(out.end(), b, b + 4); }
void le32(std::vector<uint8_t>& out, uint32_t v) { const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; out.insert(out.end(), b, b + 4); }
void utf16be(std::vector<uint8_t>& out, const std::string& text) { for (char c : text) { out.push_back(0); out.push_back(uint8_t(c)); } }

std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// A layer file as Clip Studio writes one: a 1024-byte-page SQLite database filled by `fill`, in a C2F
/// container whose first dATA chunk stands for the enciphered first five pages and whose second holds the rest.
std::vector<uint8_t> layerFile(const std::function<void(sqlite3*)>& fill) {
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
std::vector<uint8_t> tar(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
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

std::vector<uint8_t> text(const std::string& s) { return {s.begin(), s.end()}; }

/// A Variant's reference to a material: a count, the entry's length, and its path in UTF-16LE.
std::vector<uint8_t> reference(const std::string& path) {
    std::vector<uint8_t> out;
    be32(out, 8); be32(out, 1); be32(out, uint32_t(16 + path.size() * 2)); be32(out, uint32_t(path.size() * 2));
    for (char c : path) { out.push_back(uint8_t(c)); out.push_back(0); }
    return out;
}

void bindBlob(sqlite3_stmt* statement, int index, const std::vector<uint8_t>& blob) {
    sqlite3_bind_blob(statement, index, blob.data(), int(blob.size()), SQLITE_TRANSIENT);
}

/// The value of the tiled tip at (x, y) of its 256-pixel tile, before the reader stretches it.
uint8_t tileValue(int x, int y) { return uint8_t(1 + (x * 31 + y * 17) % 100); }

} // namespace

TEST_CASE(clip_studio_brushes_come_through_with_their_tips_and_textures) {
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
    REQUIRE(compress(packed.data(), &packedSize, pixels.data(), uLong(pixels.size())) == Z_OK);
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
    REQUIRE(encodePngImage(texture, png));
    const std::vector<uint8_t> textureLayer = layerFile([&](sqlite3* db) {
        sqlite3_stmt* insert = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO Rows(a, b) VALUES (1, ?1)", -1, &insert, nullptr);
        bindBlob(insert, 1, png);
        sqlite3_step(insert);
        sqlite3_finalize(insert);
    });
    const auto tipMaterial = tar({{"icedata/layerData.xml", text("<data>BrushPattern</data>")}, {"data/material_0.layer", tipLayer}});
    const auto textureMaterial = tar({{"icedata/layerData.xml", text("<data>PaperTexture</data>")}, {"data/material.layer", textureLayer}});

    const fs::path path = fs::temp_directory_path() / "compositor-test.sut";
    fs::remove(path);
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    const char* script =
        "CREATE TABLE Node(_PW_ID INTEGER PRIMARY KEY AUTOINCREMENT, NodeName TEXT, NodeVariantID INTEGER);"
        "CREATE TABLE Variant(_PW_ID INTEGER PRIMARY KEY AUTOINCREMENT, VariantID INTEGER, BrushSize REAL, BrushSizeUnit INTEGER,"
        " BrushHardness INTEGER, BrushInterval REAL, BrushThickness INTEGER, BrushRotation REAL, BrushFlow INTEGER, BrushUseSpray INTEGER,"
        " BrushSpraySize REAL, BrushSpraySizeUnit INTEGER, BrushSprayDensity INTEGER, BrushUsePatternImage INTEGER, BrushUseWaterColor INTEGER,"
        " UseDualBrush INTEGER, BrushPatternImageArray BLOB, TextureImage BLOB, TextureDensity INTEGER, TextureScale2 REAL, TextureReverseDensity INTEGER);"
        "CREATE TABLE MaterialFile(_PW_ID INTEGER PRIMARY KEY AUTOINCREMENT, FileData BLOB);"
        "INSERT INTO Node(NodeName, NodeVariantID) VALUES ('Soft Pencil', 11), ('Spray', 12);"
        "INSERT INTO Variant(VariantID, BrushSize, BrushSizeUnit, BrushHardness, BrushInterval, BrushThickness, BrushRotation, BrushFlow,"
        " BrushUseSpray, BrushSpraySize, BrushSpraySizeUnit, BrushSprayDensity, BrushUsePatternImage, BrushUseWaterColor, UseDualBrush)"
        " VALUES (11, 12.0, 0, 40, 5.0, 100, 0.0, 80, 0, 0, 0, 0, 0, 0, 0), (12, 40.0, 0, 100, 25.0, 50, 45.0, 100, 1, 20.0, 0, 6, 1, 0, 0);";
    REQUIRE(sqlite3_exec(db, script, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(db, "UPDATE Variant SET TextureImage = ?1, TextureDensity = 40, TextureScale2 = 50, TextureReverseDensity = 1 WHERE VariantID = 11", -1, &statement, nullptr);
    bindBlob(statement, 1, reference(".:Install:Paint002:1234:data:material.layer"));
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    sqlite3_prepare_v2(db, "UPDATE Variant SET BrushPatternImageArray = ?1 WHERE VariantID = 12", -1, &statement, nullptr);
    bindBlob(statement, 1, reference(".:Install:Paint110:5678:data:material_0.layer"));
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    sqlite3_prepare_v2(db, "INSERT INTO MaterialFile(FileData) VALUES (?1), (?2)", -1, &statement, nullptr);
    bindBlob(statement, 1, textureMaterial);
    bindBlob(statement, 2, tipMaterial);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    sqlite3_close(db);

    std::string error;
    auto import = importBrushFile(path.string(), &error);
    REQUIRE(import.has_value());
    REQUIRE(import->brushes.size() == 2);
    const TipPreset& pencil = import->brushes[0];
    CHECK(pencil.name == "Soft Pencil");
    CHECK_EQ(pencil.diameter, 12.0);
    CHECK_EQ(pencil.tip.spacing, 0.05);
    CHECK_EQ(pencil.tip.flow, 0.8);
    CHECK_EQ(pencil.tip.scatter, 0.0);
    // The texture: its alpha stretched so 200 is 255, then reversed; at 50% scale and 40% density.
    REQUIRE(pencil.tip.grain != nullptr);
    CHECK_EQ(pencil.tip.grain->width(), 40);
    CHECK_EQ(pencil.tip.grain->height(), 30);
    CHECK_EQ(int(pencil.tip.grain->at(3, 2)), 255 - (17 * 255 + 100) / 200);
    CHECK_EQ(pencil.tip.grainScale, 2.0);
    CHECK_EQ(pencil.tip.grainDepth, 0.4);
    const TipPreset& spray = import->brushes[1];
    CHECK_EQ(spray.tip.roundness, 0.5);
    CHECK_EQ(spray.tip.angle, 45.0);
    CHECK_EQ(spray.tip.scatter, 0.5);   // 20 px of spray on a 40 px brush
    CHECK_EQ(spray.tip.count, 6);
    // The tip: tile 1's first 44 columns (the image is 300 wide), stretched so 100 is 255, cropped to what paints.
    REQUIRE(spray.tip.shape != nullptr);
    CHECK_EQ(spray.tip.shape->width(), 44);
    CHECK_EQ(spray.tip.shape->height(), 200);
    CHECK_EQ(int(spray.tip.shape->at(10, 20)), (tileValue(10, 20) * 255 + 50) / 100);
    CHECK(spray.tip.grain == nullptr);
    fs::remove(path);
}

#else

TEST_CASE(clip_studio_needs_sqlite) { std::fprintf(stderr, "  (skipped: built without SQLite)\n"); }

#endif

TEST_CASE(clip_studio_sample_file_when_available) {
    const char* sample = std::getenv("COMPOSITOR_SUT_SAMPLE");
    if (!sample) { std::fprintf(stderr, "  (skipped: set COMPOSITOR_SUT_SAMPLE to a .sut file)\n"); return; }
    std::string error;
    auto import = importBrushFile(sample, &error);
    REQUIRE(import.has_value());
    for (const TipPreset& brush : import->brushes)
        std::fprintf(stderr, "  %s: %.0f px, tip %dx%d, grain %dx%d\n", brush.name.c_str(), brush.diameter, brush.tip.shape->width(), brush.tip.shape->height(),
            brush.tip.grain ? brush.tip.grain->width() : 0, brush.tip.grain ? brush.tip.grain->height() : 0);
    for (const std::string& note : import->notes) std::fprintf(stderr, "  note: %s\n", note.c_str());
    CHECK(!import->brushes.empty());
}

TEST_MAIN()
