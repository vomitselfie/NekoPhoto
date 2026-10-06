// Hostile files, each built here in a few bytes: the reproducers from fuzzing the readers added since 1.4.4 (see
// docs/fuzzing.md). Each must be refused cleanly or give a bounded result quickly, without allocating what the file
// merely claims. Timings are generous; the point is that none of these take the seconds or gigabytes they once did.
#include "check.h"
#include "compositor/adjustments.h"
#include "compositor/aseprite.h"
#include "compositor/gif.h"
#include "compositor/ico.h"
#include "compositor/presets.h"
#include "compositor/project.h"
#include "compositor/smartobject.h"
#include "compositor/svg.h"
#include "compositor/tga.h"
#include "compositor/vectorlayer.h"
#include "compositor/zipfile.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <zlib.h>

using namespace compositor;

namespace {

using Bytes = std::vector<uint8_t>;
void u8(Bytes& b, uint32_t v) { b.push_back(uint8_t(v)); }
void u16(Bytes& b, uint32_t v) { u8(b, v); u8(b, v >> 8); }
void u32(Bytes& b, uint32_t v) { u16(b, v); u16(b, v >> 16); }
void be16(Bytes& b, uint32_t v) { u8(b, v >> 8); u8(b, v); }
void be32(Bytes& b, uint32_t v) { be16(b, v >> 16); be16(b, v); }
Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }

struct Timer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    double seconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
};

} // namespace

// ---- GIF: frames sized far past the canvas ---------------------------------------------------------------------

TEST_CASE(gif_frames_larger_than_the_canvas_are_clipped_not_allocated) {
    // A 1 x 1 canvas and 50 frames each claiming 65535 x 65535 with an empty LZW stream (once 4 GiB of indices each).
    Bytes gif = text("GIF89a");
    u16(gif, 1); u16(gif, 1); u8(gif, 0); u8(gif, 0); u8(gif, 0);
    for (int i = 0; i < 50; i++) {
        u8(gif, 0x2C); u16(gif, 0); u16(gif, 0); u16(gif, 65535); u16(gif, 65535); u8(gif, 0);
        u8(gif, 2); u8(gif, 0);   // minimum code size, no data
    }
    u8(gif, 0x3B);
    Timer timer;
    std::string error;
    auto imported = importGifBytes(gif, &error);
    REQUIRE(imported.has_value());
    CHECK_EQ(imported->document.width, 1);
    CHECK_EQ(imported->document.layers.size(), size_t(50));
    CHECK(timer.seconds() < 5);
}

TEST_CASE(gif_frame_pixels_outside_the_canvas_are_dropped) {
    // A 2 x 1 canvas; one 4 x 1 frame whose LZW stream (min code 2) is: clear, 1, 1, 1, 1, end.
    Bytes gif = text("GIF89a");
    u16(gif, 2); u16(gif, 1); u8(gif, 0x80 | 1); u8(gif, 0); u8(gif, 0);   // global palette, 4 entries
    for (int c = 0; c < 4; c++) { u8(gif, c * 60); u8(gif, 0); u8(gif, 0); }
    u8(gif, 0x2C); u16(gif, 0); u16(gif, 0); u16(gif, 4); u16(gif, 1); u8(gif, 0);
    u8(gif, 2);
    // Codes (3 bits each, LSB first): 4 (clear), 1, 1 (table grows: 6), 1, 1, 5 (end).
    const int codes[] = {4, 1, 1, 1, 1, 5};
    uint32_t bits = 0; int count = 0; Bytes data;
    int size = 3, next = 6;
    for (size_t i = 0; i < std::size(codes); i++) {
        bits |= uint32_t(codes[i]) << count; count += size;
        while (count >= 8) { data.push_back(uint8_t(bits)); bits >>= 8; count -= 8; }
        if (i >= 2 && codes[i] != 5) { next++; if (next == (1 << size) && size < 12) size++; }
    }
    if (count) data.push_back(uint8_t(bits));
    u8(gif, uint32_t(data.size())); gif.insert(gif.end(), data.begin(), data.end()); u8(gif, 0);
    u8(gif, 0x3B);
    auto imported = importGifBytes(gif);
    REQUIRE(imported.has_value());
    REQUIRE(imported->document.layers.size() == size_t(1));
    const Image& frame = *imported->document.layers[0].asset->image.u8();
    CHECK_EQ(frame.width(), 2);
    CHECK_EQ(int(frame.pixel(0, 0)[0]), 60);
    CHECK_EQ(int(frame.pixel(1, 0)[0]), 60);
    CHECK_EQ(int(frame.pixel(1, 0)[3]), 255);
}

// ---- Aseprite: a compressed cel claiming a huge size ----------------------------------------------------------

namespace {
Bytes asepriteWithCel(int celWidth, int celHeight) {
    Bytes layer;
    u16(layer, 1); u16(layer, 0); u16(layer, 0); u16(layer, 0); u16(layer, 0); u16(layer, 0); u8(layer, 255); u8(layer, 0); u8(layer, 0); u8(layer, 0);
    u16(layer, 1); u8(layer, 'L');
    Bytes cel;
    u16(cel, 0); u16(cel, 0); u16(cel, 0); u8(cel, 255); u16(cel, 2); u16(cel, 0); for (int i = 0; i < 5; i++) u8(cel, 0);
    u16(cel, uint32_t(celWidth)); u16(cel, uint32_t(celHeight));
    Bytes zeros(16, 0), packed(64);
    uLongf packedSize = uLongf(packed.size());
    compress(packed.data(), &packedSize, zeros.data(), uLong(zeros.size()));
    cel.insert(cel.end(), packed.begin(), packed.begin() + std::ptrdiff_t(packedSize));
    Bytes chunks;
    for (auto [type, body] : {std::pair{0x2004u, &layer}, std::pair{0x2005u, &cel}}) { u32(chunks, uint32_t(body->size() + 6)); u16(chunks, type); chunks.insert(chunks.end(), body->begin(), body->end()); }
    Bytes frame;
    u32(frame, uint32_t(16 + chunks.size())); u16(frame, 0xF1FA); u16(frame, 2); u16(frame, 100); u16(frame, 0); u32(frame, 2);
    frame.insert(frame.end(), chunks.begin(), chunks.end());
    Bytes file;
    u32(file, uint32_t(128 + frame.size())); u16(file, 0xA5E0); u16(file, 1); u16(file, 16); u16(file, 16); u16(file, 32); u32(file, 1); u16(file, 100);
    file.resize(128, 0);
    file.insert(file.end(), frame.begin(), frame.end());
    return file;
}
} // namespace

TEST_CASE(aseprite_cel_sizes_are_checked_before_inflating) {
    Timer timer;
    std::string error;
    CHECK(!importAsepriteBytes(asepriteWithCel(65535, 65535), &error));   // once a 16 GiB buffer
    CHECK(!error.empty());
    CHECK(!importAsepriteBytes(asepriteWithCel(20000, 20000), &error));   // in bounds, but the stream holds 16 bytes
    CHECK(importAsepriteBytes(asepriteWithCel(2, 2), &error).has_value());   // 2 x 2 x 4 bytes: exactly what it holds
    CHECK(timer.seconds() < 5);
}

// ---- TGA: an RLE header with no data ---------------------------------------------------------------------------

TEST_CASE(tga_rle_header_without_data_is_refused) {
    Bytes tga;
    u8(tga, 0); u8(tga, 0); u8(tga, 10); u16(tga, 0); u16(tga, 0); u8(tga, 0); u16(tga, 0); u16(tga, 0);
    u16(tga, 10000); u16(tga, 10000); u8(tga, 32); u8(tga, 0x20);
    std::string error;
    CHECK(!decodeTgaImage(tga.data(), tga.size(), &error));
    CHECK(!error.empty());
}

// ---- ICO: repeated entries and oversized PNG entries ----------------------------------------------------------

namespace {
Bytes icoSharing(int entries) {
    const int w = 16, h = 16;
    Bytes bmp;
    u32(bmp, 40); u32(bmp, w); u32(bmp, h * 2); u16(bmp, 1); u16(bmp, 32); u32(bmp, 0); u32(bmp, 0); u32(bmp, 0); u32(bmp, 0); u32(bmp, 0); u32(bmp, 0);
    for (int i = 0; i < w * h; i++) { u8(bmp, 10); u8(bmp, 20); u8(bmp, 30); u8(bmp, 255); }
    for (int i = 0; i < 4 * h; i++) u8(bmp, 0);
    Bytes ico;
    u16(ico, 0); u16(ico, 1); u16(ico, uint32_t(entries));
    for (int i = 0; i < entries; i++) { u8(ico, w); u8(ico, h); u8(ico, 0); u8(ico, 0); u16(ico, 1); u16(ico, 32); u32(ico, uint32_t(bmp.size())); u32(ico, uint32_t(6 + 16 * entries)); }
    ico.insert(ico.end(), bmp.begin(), bmp.end());
    return ico;
}
} // namespace

TEST_CASE(ico_entries_sharing_a_payload_are_read_once) {
    Timer timer;
    auto imported = importIcoBytes(icoSharing(4000));
    REQUIRE(imported.has_value());
    CHECK_EQ(imported->document.layers.size(), size_t(1));
    CHECK(!imported->notes.empty());
    CHECK(timer.seconds() < 5);
    auto plain = importIcoBytes(icoSharing(1));
    REQUIRE(plain.has_value());
    CHECK(plain->notes.empty());
}

TEST_CASE(ico_png_entries_are_sized_before_decoding) {
    // A PNG entry whose header says 30000 x 3000: refused from its IHDR, before its pixels are inflated.
    Bytes png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    be32(png, 13); png.insert(png.end(), {'I', 'H', 'D', 'R'}); be32(png, 30000); be32(png, 3000);
    png.insert(png.end(), {8, 6, 0, 0, 0}); be32(png, 0);
    Bytes ico;
    u16(ico, 0); u16(ico, 1); u16(ico, 1);
    u8(ico, 0); u8(ico, 0); u8(ico, 0); u8(ico, 0); u16(ico, 1); u16(ico, 32); u32(ico, uint32_t(png.size())); u32(ico, 22);
    ico.insert(ico.end(), png.begin(), png.end());
    std::string error;
    CHECK(!importIcoBytes(ico, &error));
}

// ---- Patterns: many planes, hostile rectangles ------------------------------------------------------------------

namespace {
/// A .pat file with one 8-bit RGB 4096 x 4096 pattern whose VMA lists `slots` channel planes of junk, and whose
/// rectangle edges are `top`..`bottom` (big-endian throughout).
Bytes patFile(uint32_t slots, uint32_t top, uint32_t bottom, int written = -1) {
    Bytes rec;
    be32(rec, 1); be32(rec, 3); be16(rec, 4096); be16(rec, 4096);
    be32(rec, 1); be16(rec, 'P');   // name "P" (UTF-16, length 1)
    u8(rec, 2); u8(rec, 'i'); u8(rec, 'd');
    Bytes vma;
    be32(vma, top); be32(vma, 0); be32(vma, bottom); be32(vma, 4096); be32(vma, slots);
    for (int i = 0; i < (written < 0 ? int(slots) : written); i++) {
        be32(vma, 1); be32(vma, 23); be32(vma, 16);
        be32(vma, 0x80000000u); be32(vma, 0); be32(vma, 0x7FFFFFFFu); be32(vma, 1);   // edges that overflow a subtraction
        be16(vma, 16); u8(vma, 0);
    }
    be32(rec, 3); be32(rec, uint32_t(vma.size()));
    rec.insert(rec.end(), vma.begin(), vma.end());
    Bytes file = text("8BPT");
    be16(file, 1); be32(file, 1);
    file.insert(file.end(), rec.begin(), rec.end());
    return file;
}
} // namespace

TEST_CASE(pattern_planes_are_bounded) {
    Timer timer;
    std::string error;
    std::vector<std::string> notes;
    (void)readPat(patFile(2000, 0, 4096), &error, &notes);   // once 2000 planes of 16 MiB each
    (void)readPat(patFile(4, 0x80000000u, 0x7FFFFFFFu), &error, &notes);   // edges whose difference overflows int
    (void)readPat(patFile(0x7FFFFFFFu, 0, 4096, 8), &error, &notes);   // a slot count that overflows int + 2
    CHECK(timer.seconds() < 5);
}

// ---- Colour lookup: a .3dl whose shaper line is far too long -------------------------------------------------

TEST_CASE(threedl_grid_size_is_capped) {
    std::string lut;
    for (int i = 0; i < 3000; i++) lut += std::to_string(i) + " ";
    lut += "\n0 0 0\n";
    ColorLookupSettings s;
    s.format = "3dl";
    s.data = lut;
    CHECK(!colorLookupReadable(s));
}

// ---- SVG ----------------------------------------------------------------------------------------------------------

TEST_CASE(svg_entity_expansion_has_a_document_budget) {
    // Each attribute stays under the per-string cap, but 400 of them would expand to over 3 GB.
    std::string entities = "<!ENTITY a \"" + std::string(1000, 'x') + "\">";
    std::string prev = "a";
    for (int i = 1; i <= 3; i++) {
        std::string refs;
        for (int k = 0; k < 10; k++) refs += "&" + prev + ";";
        entities += "<!ENTITY e" + std::to_string(i) + " \"" + refs + "\">";
        prev = "e" + std::to_string(i);
    }
    std::string body;
    for (int i = 0; i < 400; i++) body += "<g data-x=\"&e3;&e3;&e3;&e3;&e3;&e3;&e3;&e3;\"/>";
    const std::string svg = "<?xml version=\"1.0\"?><!DOCTYPE svg [" + entities + "]><svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\">" + body + "</svg>";
    Timer timer;
    std::string error;
    CHECK(!importSvg(text(svg), &error));
    CHECK(error.find("entity") != std::string::npos);
    CHECK(timer.seconds() < 5);
}

TEST_CASE(svg_use_and_folders_count_towards_the_layer_limit) {
    std::string inner, uses;
    for (int i = 0; i < 5000; i++) inner += "<g/>";
    for (int i = 0; i < 20000; i++) uses += "<use href=\"#a\"/>";
    const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><defs><g id=\"a\">" + inner + "</g></defs>" + uses + "</svg>";
    Timer timer;
    std::string error;
    CHECK(!importSvg(text(svg), &error));
    CHECK(error.find("layers") != std::string::npos);
    CHECK(timer.seconds() < 10);
}

TEST_CASE(svg_use_still_places_its_target) {
    const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"20\" height=\"20\">"
                            "<defs><rect id=\"r\" width=\"2\" height=\"2\" fill=\"red\"/><symbol id=\"s\"><rect width=\"3\" height=\"3\"/></symbol></defs>"
                            "<use xlink:href=\"#r\" x=\"5\" y=\"6\"/><use href=\"#s\" x=\"1\"/></svg>";
    auto imported = importSvg(text(svg));
    REQUIRE(imported.has_value());
    CHECK_EQ(imported->shapeLayers, 2);
}

TEST_CASE(svg_raster_parts_take_only_the_definitions_they_use) {
    std::string defs;
    for (int i = 0; i < 50; i++) defs += "<linearGradient id=\"g" + std::to_string(i) + "\"><stop offset=\"0\" stop-color=\"red\"/></linearGradient>";
    defs += "<linearGradient id=\"base\"><stop offset=\"1\" stop-color=\"blue\"/></linearGradient>";
    defs += "<linearGradient id=\"used\" xlink:href=\"#base\"/>";
    const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"10\" height=\"10\">"
                            "<style id=\"sheet\">.c{opacity:1}</style><defs>" + defs + "</defs><rect class=\"c\" width=\"5\" height=\"5\" fill=\"url(#used)\"/></svg>";
    auto imported = importSvg(text(svg));
    REQUIRE(imported.has_value());
    REQUIRE(imported->rasterParts.size() == size_t(1));
    const std::string& part = imported->rasterParts[0].svg;
    CHECK(part.find("id=\"used\"") != std::string::npos);
    CHECK(part.find("id=\"base\"") != std::string::npos);    // followed from the gradient's own reference
    CHECK(part.find(".c{opacity:1}") != std::string::npos);  // style sheets always come along
    CHECK(part.find("id=\"g7\"") == std::string::npos);
}

TEST_CASE(svg_shape_pixels_stay_near_the_canvas) {
    std::string paths;
    for (int i = 0; i < 4; i++) paths += "<path d=\"M0 0L90000 0L90000 90000Z\"/>";
    auto imported = importSvg(text("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\">" + paths + "</svg>"));
    REQUIRE(imported.has_value());
    REQUIRE(imported->document.layers.size() == size_t(4));
    for (const Layer& l : imported->document.layers) {
        REQUIRE(l.asset.has_value());
        CHECK(l.asset->image.u8()->width() <= 40);    // once 30000 x 30000 each
        CHECK(l.asset->image.u8()->height() <= 40);
    }
}

TEST_CASE(vector_shapes_with_non_finite_geometry_get_a_small_place) {
    Document doc(100, 80);
    Layer layer("Shape", doc.size());
    VectorShape shape;
    VectorPath::Subpath sub;
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    VectorPath::Knot a{}, b{};
    a.x = a.inX = a.outX = nan; a.y = a.inY = a.outY = 1;
    b.x = b.inX = b.outX = 1e300; b.y = b.inY = b.outY = -inf;
    sub.knots = {a, b};
    shape.path.subpaths.push_back(sub);
    shape.stroke.enabled = true;
    shape.stroke.width = inf;
    setVectorShape(layer, doc, shape);
    REQUIRE(layer.asset.has_value());
    CHECK(layer.asset->image.u8()->width() <= 30000);
    CHECK((long long)layer.asset->image.u8()->width() * layer.asset->image.u8()->height() <= Document::pixelBudget);
}

// ---- .comp: smart objects that are each valid but too many or too large together ---------------------------------

TEST_CASE(project_smart_objects_have_aggregate_limits) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("nekophoto-hostile-" + std::to_string(std::rand()));
    fs::create_directories(dir);
    Document doc(10, 10);
    doc.layers.emplace_back(Asset::make(ImagePtr(std::make_shared<Image>(10, 10)), "Pixels"), Point{0, 0});
    for (int i = 0; i < 3; i++) {
        auto source = std::make_shared<SmartObjectSource>();
        source->id = "source-" + std::to_string(i);
        source->fileName = "Art.png";
        source->fileType = "png ";
        source->bytes = std::make_shared<const std::vector<uint8_t>>(1000, uint8_t(i));
        source->image = std::make_shared<Image>(64, 64);
        source->width = source->height = 64;
        doc.smartObjects[source->id] = source;
    }
    const std::string path = (dir / "Sources.comp").string();
    ProjectError error;
    REQUIRE(saveProject(doc, std::nullopt, path, error));
    auto loaded = loadProject(path, error);
    REQUIRE(loaded.has_value());
    CHECK_EQ(int(loaded->smartObjects.size()), 3);
    // Each is fine alone; together they pass a limit.
    ProjectLoadLimits pixels;
    pixels.smartObjectBytes = 2 * 64 * 64 * 4;
    CHECK(!loadProject(path, error, pixels));
    CHECK(error.kind == ProjectError::TooLarge);
    ProjectLoadLimits enough;
    enough.smartObjectBytes = 3 * 64 * 64 * 4;
    CHECK(loadProject(path, error, enough).has_value());
    ProjectLoadLimits count;
    count.smartObjects = 2;
    CHECK(!loadProject(path, error, count));
    CHECK(error.kind == ProjectError::TooLarge);
    ProjectLoadLimits bytes;
    bytes.sidecarBytes = 2500;
    CHECK(!loadProject(path, error, bytes));
    CHECK(error.kind == ProjectError::TooLarge);
    // And with the real limits: more sources than a package may hold, each a few bytes.
    const std::vector<uint8_t> one = serializeSmartObjectSource(*doc.smartObjects.begin()->second);
    for (int i = 0; i < ProjectLoadLimits{}.smartObjects + 1; i++) {
        std::ofstream out(dir / "Sources.comp" / "smartobjects" / ("extra-" + std::to_string(i) + ".source"), std::ios::binary);
        out.write(reinterpret_cast<const char*>(one.data()), std::streamsize(one.size()));
    }
    Timer timer;
    CHECK(!loadProject(path, error));
    CHECK(error.kind == ProjectError::TooLarge);
    CHECK(timer.seconds() < 10);
    { std::error_code cleanup_; fs::remove_all(dir, cleanup_); }
}

// A 16-bit source is counted at its depth: twice the bytes of an 8-bit one of the same size.
TEST_CASE(project_smart_object_budget_counts_bytes_at_depth) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("nekophoto-hostile16-" + std::to_string(std::rand()));
    fs::create_directories(dir);
    Document doc(10, 10);
    doc.layers.emplace_back(Asset::make(ImagePtr(std::make_shared<Image>(10, 10)), "Pixels"), Point{0, 0});
    for (int i = 0; i < 2; i++) {
        auto source = std::make_shared<SmartObjectSource>();
        source->id = "deep-" + std::to_string(i);
        source->fileName = "Art.png";
        source->fileType = "png ";
        source->bytes = std::make_shared<const std::vector<uint8_t>>(100, uint8_t(i));
        auto image = std::make_shared<Image16>(64, 64);
        const uint16_t grey[4] = {16384, 16384, 16384, 32768};
        image->fill(grey);
        source->image = Image16Ptr(image);
        source->width = source->height = 64;
        doc.smartObjects[source->id] = source;
    }
    const std::string path = (dir / "Deep.comp").string();
    ProjectError error;
    REQUIRE(saveProject(doc, std::nullopt, path, error));
    auto loaded = loadProject(path, error);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->smartObjects.size() == 2);
    for (auto& [id, s] : loaded->smartObjects) {
        REQUIRE(s->image.u16() != nullptr);
        CHECK_EQ(int(s->image.u16()->pixel(5, 5)[0]), 16384);
    }
    // What two 8-bit sources would fit in does not hold two 16-bit ones.
    ProjectLoadLimits eightBitSized;
    eightBitSized.smartObjectBytes = 2 * 64 * 64 * 4;
    CHECK(!loadProject(path, error, eightBitSized));
    CHECK(error.kind == ProjectError::TooLarge);
    ProjectLoadLimits atDepth;
    atDepth.smartObjectBytes = 2 * 64 * 64 * 8;
    CHECK(loadProject(path, error, atDepth).has_value());
    { std::error_code cleanup_; fs::remove_all(dir, cleanup_); }
}

// ---- .nekophoto: the ZIP container ------------------------------------------------------------------------------------

namespace {

/// One entry as a hostile file might describe it: what the directory claims may differ from what is there.
struct RawEntry {
    std::string name;
    Bytes data;                 // the bytes actually stored
    uint16_t method = 0;
    uint16_t flags = 0;
    uint32_t claimedSize = 0;   // 0: data.size()
    uint32_t claimedPacked = 0; // 0: data.size()
    uint32_t claimedOffset = 0xFFFFFFFE;   // the real offset unless set
};

Bytes rawZip(const std::vector<RawEntry>& entries, uint16_t claimedCount = 0) {
    Bytes out, directory;
    for (const RawEntry& e : entries) {
        const uint32_t offset = e.claimedOffset == 0xFFFFFFFE ? uint32_t(out.size()) : e.claimedOffset;
        const uint32_t crc = uint32_t(crc32(0, e.data.data(), uInt(e.data.size())));
        const uint32_t packed = e.claimedPacked ? e.claimedPacked : uint32_t(e.data.size());
        const uint32_t size = e.claimedSize ? e.claimedSize : uint32_t(e.data.size());
        u32(out, 0x04034b50); u16(out, 20); u16(out, e.flags); u16(out, e.method); u32(out, 0);
        u32(out, crc); u32(out, packed); u32(out, size); u16(out, uint32_t(e.name.size())); u16(out, 0);
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), e.data.begin(), e.data.end());
        u32(directory, 0x02014b50); u16(directory, 20); u16(directory, 20); u16(directory, e.flags); u16(directory, e.method); u32(directory, 0);
        u32(directory, crc); u32(directory, packed); u32(directory, size); u16(directory, uint32_t(e.name.size()));
        u16(directory, 0); u16(directory, 0); u16(directory, 0); u16(directory, 0); u32(directory, 0); u32(directory, offset);
        directory.insert(directory.end(), e.name.begin(), e.name.end());
    }
    const uint32_t at = uint32_t(out.size());
    out.insert(out.end(), directory.begin(), directory.end());
    const uint16_t count = claimedCount ? claimedCount : uint16_t(entries.size());
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, count); u16(out, count); u32(out, uint32_t(directory.size())); u32(out, at); u16(out, 0);
    return out;
}

bool zipOpens(const Bytes& bytes, const ZipLimits& limits = {}) {
    ZipFileReader zip;
    return zip.openMemory(bytes, nullptr, limits);
}

Bytes deflated(const Bytes& raw) {
    z_stream z{};
    deflateInit2(&z, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    Bytes out(deflateBound(&z, uLong(raw.size())));
    z.next_in = const_cast<Bytef*>(raw.data());
    z.avail_in = uInt(raw.size());
    z.next_out = out.data();
    z.avail_out = uInt(out.size());
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

} // namespace

TEST_CASE(zip_container_refuses_hostile_directories) {
    Timer timer;
    const Bytes small = text("hello");
    CHECK(zipOpens(rawZip({{"a.txt", small}})));
    // Names that climb out, are absolute, or hide a drive or a backslash.
    for (const char* name : {"../a.txt", "images/../../a.txt", "/etc/passwd", "C:/a.txt", "images\\..\\a.txt", "a//b", "./a"})
        CHECK(!zipOpens(rawZip({{name, small}})));
    // The same name twice, also in another case (one would overwrite the other when unpacked on Windows).
    CHECK(!zipOpens(rawZip({{"a.txt", small}, {"a.txt", small}})));
    CHECK(!zipOpens(rawZip({{"images/A.png", small}, {"images/a.png", small}})));
    // More entries than allowed.
    std::vector<RawEntry> many;
    for (int i = 0; i < 40; i++) many.push_back({"f" + std::to_string(i), small});
    ZipLimits few;
    few.entries = 32;
    CHECK(!zipOpens(rawZip(many), few));
    CHECK(zipOpens(rawZip(many)));
    // A count larger than the directory can hold, and the ZIP64 marker.
    CHECK(!zipOpens(rawZip({{"a.txt", small}}, 500)));
    CHECK(!zipOpens(rawZip({{"a.txt", small}}, 0xFFFF)));
    // Encrypted, or an unknown method.
    RawEntry encrypted{"a.txt", small};
    encrypted.flags = 1;
    CHECK(!zipOpens(rawZip({encrypted})));
    RawEntry bzip{"a.txt", small};
    bzip.method = 12;
    CHECK(!zipOpens(rawZip({bzip})));
    // A stored entry whose sizes disagree, and one whose data claims to run into the directory.
    RawEntry liar{"a.txt", small};
    liar.claimedSize = 9;
    CHECK(!zipOpens(rawZip({liar})));
    RawEntry overlap{"a.txt", small};
    overlap.claimedPacked = overlap.claimedSize = 4000;
    CHECK(!zipOpens(rawZip({overlap})));
    RawEntry elsewhere{"a.txt", small};
    elsewhere.claimedOffset = 1u << 30;
    CHECK(!zipOpens(rawZip({elsewhere})));
    // A deflate bomb: a few bytes that claim (and would inflate to) far more.
    const Bytes zeros(64u << 20, 0);
    RawEntry bomb{"bomb.bin", deflated(zeros)};
    bomb.method = 8;
    bomb.claimedSize = uint32_t(zeros.size());
    CHECK(!zipOpens(rawZip({bomb})));
    // Too much in total.
    ZipLimits tight;
    tight.totalBytes = 8;
    CHECK(!zipOpens(rawZip({{"a.txt", small}, {"b.txt", small}}), tight));
    // Truncated anywhere: refused, never read past the end.
    const Bytes whole = rawZip({{"a.txt", small}, {"b.txt", small}});
    for (size_t n = 0; n < whole.size(); n += 3) CHECK(!zipOpens(Bytes(whole.begin(), whole.begin() + std::ptrdiff_t(n))));
    // A CRC that does not match is caught on reading.
    Bytes damaged = rawZip({{"a.txt", small}});
    damaged[30 + 5] ^= 0x55;
    ZipFileReader zip;
    REQUIRE(zip.openMemory(damaged));
    Bytes out;
    CHECK(!zip.read(zip.entries()[0], out));
    CHECK(timer.seconds() < 10);
}

TEST_CASE(document_file_with_hostile_entries_is_refused) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("nekophoto-hostile-zip-" + std::to_string(std::rand()));
    fs::create_directories(dir);
    const std::string header = "{\"format\":\"NekoPhoto Document\",\"format_id\":\"org.nekophoto.document\",\"version\":1,\"minimum_reader_version\":1}";
    auto write = [&](const std::string& name, const Bytes& bytes) {
        std::ofstream out(dir / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    };
    const RawEntry mime{"mimetype", text("application/vnd.nekophoto.document")}, head{"nekophoto.json", text(header)};
    // A path out of the scratch folder never lands anywhere.
    write("Escape.nekophoto", rawZip({mime, head, {"images/../../escaped.txt", text("x")}}));
    ProjectError error;
    CHECK(!loadProject((dir / "Escape.nekophoto").string(), error));
    CHECK(!fs::exists(fs::temp_directory_path() / "escaped.txt"));
    // No manifest: invalid, not a crash.
    write("Empty.nekophoto", rawZip({mime, head}));
    CHECK(!loadProject((dir / "Empty.nekophoto").string(), error));
    CHECK(error.kind == ProjectError::Invalid);
    // A manifest naming an image that is not there.
    const std::string manifest = "{\"format\":\"com.compositor.project\",\"version\":7,\"documentID\":\"" + makeUuid() + "\",\"width\":10,\"height\":10,"
                                 "\"activeLayerID\":null,\"layers\":[{\"id\":\"" + makeUuid() + "\",\"name\":\"L\",\"imageFile\":\"x.png\",\"origin\":[0,0],"
                                 "\"size\":[10,10],\"visible\":true,\"opacity\":1}]}";
    write("Missing.nekophoto", rawZip({mime, head, {"manifest.json", text(manifest)}}));
    CHECK(!loadProject((dir / "Missing.nekophoto").string(), error));
    // Garbage with the right name.
    write("Garbage.nekophoto", text("PK\x03\x04 not really"));
    CHECK(!loadProject((dir / "Garbage.nekophoto").string(), error));
    { std::error_code cleanup_; fs::remove_all(dir, cleanup_); }
}

TEST_MAIN()
