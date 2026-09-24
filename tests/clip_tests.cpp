// Clip Studio project import: a .clip written here byte by byte (the CSFCHUNK container, tile streams laid
// out as Clip Studio writes them, an SQLite database made here), when the build has SQLite; and a real file
// with the PSD Clip Studio exported from it, when COMPOSITOR_CLIP_SAMPLE and COMPOSITOR_CLIP_PSD name them.
#include "check.h"
#include "compositor/clip.h"
#include "compositor/render.h"
#include <array>
#include <cmath>
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

using Bytes = std::vector<uint8_t>;
void be16(Bytes& out, uint32_t v) { const uint8_t b[2] = {uint8_t(v >> 8), uint8_t(v)}; out.insert(out.end(), b, b + 2); }
void be32(Bytes& out, uint32_t v) { const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; out.insert(out.end(), b, b + 4); }
void le32(Bytes& out, uint32_t v) { const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; out.insert(out.end(), b, b + 4); }
void be64(Bytes& out, uint64_t v) { be32(out, uint32_t(v >> 32)); be32(out, uint32_t(v)); }
void label(Bytes& out, const std::string& text) { be32(out, uint32_t(text.size())); for (char c : text) { out.push_back(0); out.push_back(uint8_t(c)); } }

/// A layer bitmap as Clip Studio tiles it: `pixel(x, y)` gives straight RGBA (or gray in [0] when `gray`).
struct Bitmap {
    int width, height;
    bool gray = false;
    std::function<std::array<uint8_t, 4>(int, int)> pixel;
};

/// The Offscreen Attribute: sizes, then the Parameter section (size, tile grid, sixteen values), then
/// InitColor (a default fill of transparent / black).
Bytes attribute(const Bitmap& b) {
    Bytes out;
    be32(out, 16); be32(out, 102); be32(out, 42); be32(out, 70);
    label(out, "Parameter");
    be32(out, uint32_t(b.width)); be32(out, uint32_t(b.height));
    be32(out, uint32_t((b.width + 255) / 256)); be32(out, uint32_t((b.height + 255) / 256));
    for (uint32_t v : {33u, 1u, 4u, 5u, 65536u, 4u, 1024u, 1u, 256u, 65536u, 256u, 256u, 8u, 8u, 0u, 0u}) be32(out, v);
    label(out, "InitColor");
    be32(out, 0); be32(out, 0); be32(out, 0); be32(out, 0); be32(out, 0);
    return out;
}

/// The tile stream: a BlockDataBeginChunk per tile, those with any paint carrying zlib data (colour as an
/// alpha plane then B, G, R, unused; gray as one plane), each closed by BlockDataEndChunk.
Bytes tiles(const Bitmap& b) {
    Bytes out;
    const int columns = (b.width + 255) / 256, rows = (b.height + 255) / 256, channels = b.gray ? 1 : 5;
    for (int t = 0; t < columns * rows; t++) {
        Bytes raw(size_t(256 * 256 * channels), 0);
        bool any = false;
        for (int y = 0; y < 256; y++)
            for (int x = 0; x < 256; x++) {
                const int bx = (t % columns) * 256 + x, by = (t / columns) * 256 + y;
                if (bx >= b.width || by >= b.height) continue;
                const auto p = b.pixel(bx, by);
                const size_t i = size_t(y) * 256 + size_t(x);
                if (b.gray) { raw[i] = p[0]; any |= p[0] != 0; continue; }
                raw[i] = p[3];
                uint8_t* bgra = &raw[65536 + i * 4];
                bgra[0] = p[2]; bgra[1] = p[1]; bgra[2] = p[0];
                any |= p[3] != 0;
            }
        Bytes block;
        label(block, "BlockDataBeginChunk");
        be32(block, uint32_t(t)); be16(block, uint32_t(channels)); be16(block, 0); be32(block, 256); be32(block, 256); be32(block, any ? 1 : 0);
        if (any) {
            uLongf packedSize = compressBound(uLong(raw.size()));
            Bytes packed(packedSize);
            compress(packed.data(), &packedSize, raw.data(), uLong(raw.size()));
            be32(block, uint32_t(packedSize + 4)); le32(block, uint32_t(packedSize));
            block.insert(block.end(), packed.begin(), packed.begin() + long(packedSize));
        }
        label(block, "BlockDataEndChunk");
        be32(out, uint32_t(block.size() + 4));
        out.insert(out.end(), block.begin(), block.end());
    }
    return out;
}

void bindBlob(sqlite3_stmt* s, int i, const Bytes& b) { sqlite3_bind_blob(s, i, b.data(), int(b.size()), SQLITE_TRANSIENT); }

} // namespace

TEST_CASE(clip_layers_folders_clipping_masks_and_placement_come_through) {
    // A 300 x 260 canvas. Base: blue over the left 200 columns. A folder holding Moved (a red 40 x 40 square,
    // its bitmap 556 wide and starting at x = -256 because the layer was moved past the edge, with a mask
    // hiding its right half) and Shade (green at half opacity, Multiply, clipped to Moved). Hidden: invisible.
    std::vector<std::pair<std::string, Bytes>> externals;
    struct Mip { int id; Bitmap bitmap; };
    std::vector<Mip> mips;
    auto mip = [&](int id, Bitmap b) { mips.push_back({id, std::move(b)}); };
    mip(10, {300, 260, false, [](int x, int) { return x < 200 ? std::array<uint8_t, 4>{0, 0, 255, 255} : std::array<uint8_t, 4>{0, 0, 0, 0}; }});
    mip(11, {556, 260, false, [](int x, int y) { return x >= 300 && x < 340 && y >= 50 && y < 90 ? std::array<uint8_t, 4>{255, 0, 0, 255} : std::array<uint8_t, 4>{0, 0, 0, 0}; }});
    mip(12, {300, 260, true, [](int x, int) { return std::array<uint8_t, 4>{uint8_t(x < 64 ? 255 : 0), 0, 0, 0}; }});
    mip(13, {300, 260, false, [](int x, int y) { return x < 150 && y < 150 ? std::array<uint8_t, 4>{0, 255, 0, 255} : std::array<uint8_t, 4>{0, 0, 0, 0}; }});
    mip(14, {300, 260, false, [](int, int) { return std::array<uint8_t, 4>{255, 255, 255, 255}; }});

    const fs::path dbPath = fs::temp_directory_path() / "compositor-test-clip.sqlite";
    fs::remove(dbPath);
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(dbPath.string().c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db,
        "CREATE TABLE Canvas(MainId INTEGER, CanvasWidth INTEGER, CanvasHeight INTEGER, CanvasResolution REAL, CanvasRootFolder INTEGER, CanvasChannelBytes INTEGER);"
        "CREATE TABLE Layer(MainId INTEGER, LayerName TEXT, LayerUuid TEXT, LayerType INTEGER, LayerComposite INTEGER, LayerOpacity INTEGER, LayerVisibility INTEGER,"
        " LayerFolder INTEGER, LayerLock INTEGER, LayerClip INTEGER, LayerFirstChildIndex INTEGER, LayerNextIndex INTEGER, LayerRenderMipmap INTEGER, LayerLayerMaskMipmap INTEGER,"
        " LayerOffsetX INTEGER, LayerOffsetY INTEGER, LayerRenderOffscrOffsetX INTEGER, LayerRenderOffscrOffsetY INTEGER,"
        " LayerMaskOffsetX INTEGER, LayerMaskOffsetY INTEGER, LayerMaskOffscrOffsetX INTEGER, LayerMaskOffscrOffsetY INTEGER);"
        "CREATE TABLE Mipmap(MainId INTEGER, BaseMipmapInfo INTEGER);"
        "CREATE TABLE MipmapInfo(MainId INTEGER, Offscreen INTEGER);"
        "CREATE TABLE Offscreen(MainId INTEGER, Attribute BLOB, BlockData BLOB);"
        "INSERT INTO Canvas VALUES (1, 300, 260, 144, 2, 1);"
        // Root (type 256) -> Base -> Group (folder: Moved -> Shade) -> Hidden, bottom to top.
        "INSERT INTO Layer VALUES (2, '', 'r', 256, 0, 256, 1, 1, 0, 0, 3, 0, 0, 0, 0,0,0,0, 0,0,0,0);"
        "INSERT INTO Layer VALUES (3, 'Base', 'a', 1, 0, 256, 1, 0, 0, 0, 0, 4, 10, 0, 0,0,0,0, 0,0,0,0);"
        "INSERT INTO Layer VALUES (4, 'Group', 'b', 0, 30, 256, 1, 1, 0, 0, 5, 7, 0, 0, 0,0,0,0, 0,0,0,0);"
        "INSERT INTO Layer VALUES (5, 'Moved', 'c', 1, 0, 256, 3, 0, 0, 0, 0, 6, 11, 12, -100,0,-156,0, 0,0,0,0);"
        "INSERT INTO Layer VALUES (6, 'Shade', 'd', 1, 2, 128, 1, 0, 0, 1, 0, 0, 13, 0, 0,0,0,0, 0,0,0,0);"
        "INSERT INTO Layer VALUES (7, 'Hidden', 'e', 1, 0, 256, 0, 0, 0, 0, 0, 0, 14, 0, 0,0,0,0, 0,0,0,0);",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    for (const Mip& m : mips) {
        const std::string id = "extrnlid" + std::to_string(m.id);
        externals.emplace_back(id, tiles(m.bitmap));
        sqlite3_stmt* s = nullptr;
        sqlite3_exec(db, ("INSERT INTO Mipmap VALUES (" + std::to_string(m.id) + ", " + std::to_string(m.id) + "); INSERT INTO MipmapInfo VALUES (" + std::to_string(m.id) + ", " + std::to_string(m.id) + ");").c_str(), nullptr, nullptr, nullptr);
        sqlite3_prepare_v2(db, "INSERT INTO Offscreen VALUES (?1, ?2, ?3)", -1, &s, nullptr);
        sqlite3_bind_int(s, 1, m.id);
        bindBlob(s, 2, attribute(m.bitmap));
        sqlite3_bind_blob(s, 3, id.data(), int(id.size()), SQLITE_TRANSIENT);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    sqlite3_close(db);
    std::ifstream dbIn(dbPath, std::ios::binary);
    const Bytes database((std::istreambuf_iterator<char>(dbIn)), std::istreambuf_iterator<char>());
    fs::remove(dbPath);

    // The container: CSFCHUNK, its length and the first chunk's offset, then chunks of [tag][be64 length][payload].
    Bytes file = {'C', 'S', 'F', 'C', 'H', 'U', 'N', 'K'};
    be64(file, 0); be64(file, 24);
    auto chunk = [&](const char* tag, const Bytes& payload) { file.insert(file.end(), tag, tag + 8); be64(file, payload.size()); file.insert(file.end(), payload.begin(), payload.end()); };
    chunk("CHNKHead", Bytes(40, 0));
    for (const auto& [id, body] : externals) {
        Bytes payload;
        be64(payload, id.size());
        payload.insert(payload.end(), id.begin(), id.end());
        be64(payload, body.size());
        payload.insert(payload.end(), body.begin(), body.end());
        chunk("CHNKExta", payload);
    }
    chunk("CHNKSQLi", database);
    chunk("CHNKFoot", {});
    const fs::path path = fs::temp_directory_path() / "compositor-test.clip";
    { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(file.data()), long(file.size())); }

    CHECK(isClipFile(path.string()));
    std::string error;
    auto imported = importClip(path.string(), &error);
    REQUIRE(imported.has_value());
    const Document& doc = imported->document;
    CHECK_EQ(doc.width, 300);
    CHECK_EQ(doc.height, 260);
    CHECK_EQ(doc.resolution, 144.0);
    REQUIRE(doc.layers.size() == 5);
    const Layer &base = doc.layers[0], &group = doc.layers[1], &moved = doc.layers[2], &shade = doc.layers[3], &hidden = doc.layers[4];
    CHECK(base.name == "Base" && group.name == "Group" && moved.name == "Moved" && shade.name == "Shade" && hidden.name == "Hidden");
    // Base, cropped to its blue 200 columns.
    REQUIRE(base.asset.has_value());
    CHECK_EQ(base.asset->image->width(), 200);
    CHECK_EQ(base.asset->image->height(), 260);
    CHECK_EQ(int(base.asset->image->pixel(10, 10)[2]), 255);
    // The folder and its children.
    CHECK(group.isGroup);
    CHECK(moved.parentId == group.id && shade.parentId == group.id && !base.parentId && !hidden.parentId);
    // Moved: the bitmap at x = -100 + -156 = -256, so the square lands at canvas (44, 50).
    REQUIRE(moved.asset.has_value());
    CHECK_EQ(moved.transform.origin.x, 44.0);
    CHECK_EQ(moved.transform.origin.y, 50.0);
    CHECK_EQ(moved.asset->image->width(), 40);
    CHECK_EQ(int(moved.asset->image->pixel(5, 5)[0]), 255);
    // Its mask covers its pixels: canvas x < 64 shows (the square's first 20 columns), the rest is hidden.
    REQUIRE(moved.mask.has_value());
    CHECK(moved.mask->enabled);
    CHECK_EQ(moved.mask->asset.image->width(), 40);
    CHECK_EQ(int(moved.mask->asset.image->at(10, 5)), 255);
    CHECK_EQ(int(moved.mask->asset.image->at(30, 5)), 0);
    // Shade: clipped to Moved, Multiply, half opacity.
    CHECK(shade.maskSourceId == moved.id);
    CHECK(shade.blendMode == BlendMode::Multiply);
    CHECK_EQ(shade.opacity, 0.5);
    CHECK(!hidden.visible);
    CHECK(imported->notes.empty());
    // And it renders: blue base, the square's left half red where the mask shows it, darkened by the green.
    auto flat = renderFlattened(doc);
    CHECK_EQ(int(flat->pixel(10, 200)[2]), 255);
    CHECK(flat->pixel(50, 60)[0] > 100 && flat->pixel(50, 60)[1] < 50);
    if (!std::getenv("COMPOSITOR_KEEP_TEST_CLIP")) fs::remove(path);   // kept, it seeds a fuzzer
}

TEST_CASE(clip_files_that_lie_are_refused) {
    const fs::path path = fs::temp_directory_path() / "compositor-test-bad.clip";
    Bytes file = {'C', 'S', 'F', 'C', 'H', 'U', 'N', 'K'};
    be64(file, 0); be64(file, 24);
    file.insert(file.end(), {'C', 'H', 'N', 'K', 'S', 'Q', 'L', 'i'});
    be64(file, 1ull << 40);   // a chunk longer than the file
    { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(file.data()), long(file.size())); }
    std::string error;
    CHECK(!importClip(path.string(), &error).has_value());
    CHECK(!error.empty());
    fs::remove(path);
}

#else

TEST_CASE(clip_needs_sqlite) { std::fprintf(stderr, "  (skipped: built without SQLite)\n"); }

#endif

TEST_CASE(clip_sample_matches_its_psd_when_available) {
    const char* sample = std::getenv("COMPOSITOR_CLIP_SAMPLE");
    const char* psdPath = std::getenv("COMPOSITOR_CLIP_PSD");
    if (!sample || !psdPath) { std::fprintf(stderr, "  (skipped: set COMPOSITOR_CLIP_SAMPLE to a .clip and COMPOSITOR_CLIP_PSD to its PSD export)\n"); return; }
    std::string error;
    auto clip = importClip(sample, &error);
    REQUIRE(clip.has_value());
    auto psd = importPsd(psdPath, &error);
    REQUIRE(psd.has_value());
    auto a = renderFlattened(clip->document), b = renderFlattened(psd->document);
    REQUIRE(a->width() == b->width() && a->height() == b->height());
    double total = 0;
    for (int y = 0; y < a->height(); y++)
        for (int x = 0; x < a->width() * 4; x++) total += std::abs(a->row(y)[x] - b->row(y)[x]);
    const double mean = total / (double(a->width()) * a->height() * 4);
    std::fprintf(stderr, "  %zu layers, render differs from the PSD's by %.4f on average\n", clip->document.layers.size(), mean);
    CHECK(mean < 0.05);
}

TEST_MAIN()
