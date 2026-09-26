// Affinity import: the pure helpers (stream predictors, the blend table, transform composition); a document
// written here byte by byte (the container, its stream table, a doc.dat tree with one translated RGBA8 pixel layer
// in Multiply, and its tile streams); and, when COMPOSITOR_AFFINITY_FIXTURES names a folder of real documents
// (Patchy's test-fixtures/af), every one of them opened and compared with the preview Affinity embedded.
#include "check.h"
#include "compositor/affinity.h"
#include "compositor/png.h"
#include "compositor/render.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <zlib.h>

using namespace compositor;
namespace fs = std::filesystem;

TEST_CASE(byte_delta_is_a_running_sum) {
    std::vector<uint8_t> v{10, 5, 250, 1};
    affinity_detail::undoByteDelta(v);
    CHECK((v == std::vector<uint8_t>{10, 15, 9, 10}));
}

TEST_CASE(u16_delta_is_a_running_little_endian_sum) {
    std::vector<uint8_t> v{0x00, 0x01, 0x01, 0x00, 0xFF, 0xFF};   // 256, +1, -1
    affinity_detail::undoU16Delta(v);
    CHECK((v == std::vector<uint8_t>{0x00, 0x01, 0x01, 0x01, 0x00, 0x01}));
}

TEST_CASE(tile_interleave_rebuilds_16_bit_pairs) {
    std::vector<uint8_t> v(0x10000, 0);
    v[0] = 0xA1; v[1] = 0xA3; v[0x8000] = 0xA0; v[0x8001] = 0xA2;
    affinity_detail::undoTileInterleave(v);
    CHECK_EQ(int(v[0]), 0xA0);
    CHECK_EQ(int(v[1]), 0xA1);
    CHECK_EQ(int(v[2]), 0xA2);
    CHECK_EQ(int(v[3]), 0xA3);
    std::vector<uint8_t> small{1, 2, 3};
    affinity_detail::undoTileInterleave(small);   // only whole 64 KiB tiles
    CHECK_EQ(small.size(), size_t(3));
}

TEST_CASE(blend_table_follows_both_numberings) {
    CHECK(affinity_detail::blendMode(0, 0) == BlendMode::Normal);
    CHECK(affinity_detail::blendMode(5, 0) == BlendMode::Screen);
    CHECK(affinity_detail::blendMode(7, 0) == BlendMode::LinearDodge);
    CHECK(affinity_detail::blendMode(20, 0) == BlendMode::Color);
    CHECK(!affinity_detail::blendMode(25, 0));   // Erase
    CHECK(affinity_detail::blendMode(5, 3) == BlendMode::LinearBurn);
    CHECK(affinity_detail::blendMode(21, 4) == BlendMode::Divide);
    CHECK(affinity_detail::blendMode(9, 6) == BlendMode::Screen);
    CHECK(affinity_detail::blendMode(4, 6) == BlendMode::Multiply);
    CHECK(!affinity_detail::blendMode(1, 6));    // Pigment
}

TEST_CASE(transforms_compose_parent_first) {
    const std::vector<double> scale{2, 0, 10, 0, 2, 20}, move{1, 0, 3, 0, 1, 4};
    const auto m = affinity_detail::composeTransforms(scale, move);
    CHECK((m == std::vector<double>{2, 0, 16, 0, 2, 28}));
}

namespace {

using Bytes = std::vector<uint8_t>;
void u8(Bytes& b, uint8_t v) { b.push_back(v); }
void u16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void u32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i))); }
void u64(Bytes& b, uint64_t v) { u32(b, uint32_t(v)); u32(b, uint32_t(v >> 32)); }
void f64(Bytes& b, double d) { uint64_t bits; std::memcpy(&bits, &d, 8); u64(b, bits); }
constexpr uint32_t tag(const char (&s)[5]) { return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint32_t(uint8_t(s[3])); }

// doc.dat fields: a type byte, the tag, the value.
void field(Bytes& b, uint8_t type, const char (&t)[5]) { u8(b, type); u32(b, tag(t)); }
void i32Field(Bytes& b, const char (&t)[5], int32_t v) { field(b, 0x07, t); u32(b, uint32_t(v)); }
void doubleField(Bytes& b, const char (&t)[5], double v) { field(b, 0x0A, t); f64(b, v); }
void stringField(Bytes& b, const char (&t)[5], const std::string& s) { field(b, 0x2B, t); u32(b, uint32_t(s.size())); b.insert(b.end(), s.begin(), s.end()); }
void enumField(Bytes& b, const char (&t)[5], uint16_t id, uint16_t version) { field(b, 0x2A, t); u16(b, id); u16(b, version); }
void boolField(Bytes& b, const char (&t)[5], bool v) { field(b, 0x29, t); u8(b, v); }
void classBegin(Bytes& b, const char (&t)[5], const char (&type)[5]) { field(b, 0x32, t); u8(b, 1); u32(b, tag(type)); u16(b, 0); }
void classListBegin(Bytes& b, const char (&t)[5], const char (&type)[5], uint32_t count) { field(b, 0x32 | 0x80, t); u32(b, count); u32(b, tag(type)); u16(b, 0); }
void end(Bytes& b) { u8(b, 0); }

/// A 3 x 2 RGBA8 bitmap in one 256 x 256 tile per channel, at (1, 1), in Multiply, 50% opacity, on a 5 x 4 canvas.
Bytes makeDocument(bool withText = false) {
    const int w = 3, h = 2;
    const uint8_t rgba[2][3][4] = {{{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}}, {{10, 20, 30, 128}, {40, 50, 60, 255}, {70, 80, 90, 0}}};
    Bytes tree;
    u32(tree, 0x534BFF00u); u16(tree, 2); u32(tree, tag("Pers")); u16(tree, 0); u32(tree, 30);
    classBegin(tree, "DocR", "Docu");
    {
        field(tree, 0x15, "DfSz"); u32(tree, 5); u32(tree, 4);
        classListBegin(tree, "Chld", "Sprd", 1);
        u8(tree, 1);   // the spread
        boolField(tree, "SprT", true);   // transparent: no background layer
        // (A class list shares one type, so the text document holds only the text node.)
        classListBegin(tree, "Chld", withText ? "TxtA" : "Rstr", 1);
        if (!withText) {
        u8(tree, 1);   // the pixel layer
        stringField(tree, "Desc", "Paint");
        doubleField(tree, "Opac", 0.5);
        enumField(tree, "Blnd", 2, 0);   // Multiply
        field(tree, 0x28, "Xfrm"); for (double v : {1.0, 0.0, 1.0, 0.0, 1.0, 1.0}) f64(tree, v);
        classBegin(tree, "Bitm", "DyBm");
        enumField(tree, "Frmt", 0, 0);   // RGBA8
        i32Field(tree, "BmpW", w);
        i32Field(tree, "BmpH", h);
        for (int ch = 1; ch <= 4; ch++) {
            const std::string n = std::to_string(ch);
            const char twi[5] = {'T', 'W', 'i', n[0], 0}, thi[5] = {'T', 'H', 'i', n[0], 0}, sta[5] = {'S', 't', 'a', n[0], 0}, idx[5] = {'I', 'd', 'x', n[0], 0};
            i32Field(tree, twi, 1);
            i32Field(tree, thi, 1);
            field(tree, 0x01 | 0x80, sta); u32(tree, 1); u8(tree, 4);   // one stored tile
            classListBegin(tree, idx, "Blck", 1);
            u8(tree, 1);
            field(tree, 0x33, "Data"); u32(tree, tag("Strm")); const std::string s = "d/" + n; u32(tree, uint32_t(s.size())); tree.insert(tree.end(), s.begin(), s.end());
            end(tree);
        }
        end(tree);   // DyBm
        end(tree);   // layer
        } else {
            // Artistic text "Hi\u2029yo": two blocks' worth of story in one, bold 20 px red then regular, at (2, 3).
            u8(tree, 1);
            stringField(tree, "Desc", "Words");
            field(tree, 0x28, "Xfrm"); for (double v : {1.0, 0.0, 2.0, 0.0, 1.0, 3.0}) f64(tree, v);
            classBegin(tree, "TxtH", "TxtH");
            field(tree, 0x26, "FrmB"); for (double v : {0.0, 0.0, 40.0, 30.0}) f64(tree, v);
            doubleField(tree, "ArtV", 16);
            end(tree);
            classBegin(tree, "StSt", "Stry");
            classListBegin(tree, "Blok", "Blok", 1);
            u8(tree, 1);
            classBegin(tree, "Glyp", "Glyp");
            stringField(tree, "Utf8", std::string("Hi\xE2\x80\xA9yo") + '\0');
            end(tree);
            classBegin(tree, "GAtt", "GAtt");
            classListBegin(tree, "Runs", "Run ", 2);
            u8(tree, 1);
            i32Field(tree, "Indx", 2);
            classBegin(tree, "Item", "Item");
            classBegin(tree, "DFnt", "DFnt"); stringField(tree, "Famy", "Test Sans"); i32Field(tree, "Wegt", 700); end(tree);
            field(tree, 0x0A | 0x80, "Doub"); u32(tree, 1); f64(tree, 20);
            end(tree);   // Item
            end(tree);   // run
            u8(tree, 1);
            i32Field(tree, "Indx", 6);
            classBegin(tree, "Item", "Item");
            classBegin(tree, "DFnt", "DFnt"); stringField(tree, "Famy", "Test Sans"); i32Field(tree, "Wegt", 400); end(tree);
            end(tree);   // Item
            end(tree);   // run
            end(tree);   // GAtt
            end(tree);   // block
            end(tree);   // story
            end(tree);   // text node
        }
        end(tree);   // spread
    }
    end(tree);   // Docu
    end(tree);   // root

    std::vector<std::pair<std::string, Bytes>> streams{{"doc.dat", tree}};
    for (int ch = 0; ch < 4; ch++) {
        Bytes tile(256 * 256, 0);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) tile[size_t(y) * 256 + size_t(x)] = rgba[y][x][ch];
        streams.push_back({"d/" + std::to_string(ch + 1), tile});
    }

    Bytes file;
    u32(file, 0x414BFF00u); u16(file, 11); u16(file, 0); u32(file, tag("Prsn"));
    u32(file, 0x666E4923u);   // #Inf
    const size_t fatAt = file.size();
    u64(file, 0); u64(file, 0);   // stream table, thumbnail
    for (int i = 0; i < 32; i++) u8(file, 0);
    u32(file, 0x746F7250u); u32(file, 1);   // Prot
    std::vector<uint64_t> offsets;
    for (auto& [name, data] : streams) {
        offsets.push_back(file.size());
        u32(file, 0x6C694623u);   // #Fil
        file.insert(file.end(), data.begin(), data.end());
    }
    const uint64_t fat = file.size();
    for (int i = 0; i < 8; i++) file[fatAt + size_t(i)] = uint8_t(fat >> (8 * i));
    u32(file, 0x33544623u);   // #FT3
    u64(file, 0);
    for (int i = 0; i < 32; i++) u8(file, 0);
    u32(file, uint32_t(streams.size()));
    u64(file, 0);
    u16(file, 0); u8(file, 0);
    for (size_t i = 0; i < streams.size(); i++) {
        const Bytes& data = streams[i].second;
        u32(file, uint32_t(i + 1)); u8(file, 0);
        u64(file, offsets[i]); u64(file, data.size()); u64(file, data.size());
        u32(file, uint32_t(crc32(0, data.data(), uInt(data.size()))));
        u8(file, 0);   // stored raw
        u32(file, 0);
        u16(file, uint16_t(streams[i].first.size()));
        file.insert(file.end(), streams[i].first.begin(), streams[i].first.end());
    }
    return file;
}

} // namespace

TEST_CASE(a_written_document_opens_with_its_layer) {
    std::string error;
    auto imported = importAffinityBytes(makeDocument(), &error);
    if (!imported) std::fprintf(stderr, "  error: %s\n", error.c_str());
    REQUIRE(imported.has_value());
    for (auto& n : imported->notes) std::fprintf(stderr, "  note: %s\n", n.c_str());
    const Document& doc = imported->document;
    CHECK_EQ(doc.width, 5);
    CHECK_EQ(doc.height, 4);
    REQUIRE(doc.layers.size() == size_t(1));
    const Layer& l = doc.layers[0];
    CHECK_EQ(l.name, std::string("Paint"));
    CHECK(l.blendMode == BlendMode::Multiply);
    CHECK_NEAR(l.opacity, 0.5, 1e-9);
    REQUIRE(l.asset.has_value());
    // The fully transparent corner is cropped away; what remains starts at the translation.
    CHECK_EQ(l.transform.origin.x, 1.0);
    CHECK_EQ(l.transform.origin.y, 1.0);
    const Image& img = *l.asset->image;
    CHECK_EQ(img.width(), 3);
    CHECK_EQ(img.height(), 2);
    CHECK_EQ(int(img.pixel(1, 0)[1]), 255);        // green, opaque
    CHECK_EQ(int(img.pixel(0, 1)[3]), 128);        // half alpha ...
    CHECK_EQ(int(img.pixel(0, 1)[0]), 5);          // ... premultiplied (10 * 128 / 255)
    CHECK_EQ(int(img.pixel(2, 1)[3]), 0);
    CHECK(imported->notes.empty());
}

TEST_CASE(artistic_text_becomes_a_text_layer_to_draw) {
    std::string error;
    auto imported = importAffinityBytes(makeDocument(true), &error);
    if (!imported) std::fprintf(stderr, "  error: %s\n", error.c_str());
    REQUIRE(imported.has_value());
    const Document& doc = imported->document;
    REQUIRE(doc.layers.size() == size_t(1));
    const Layer& l = doc.layers[0];
    CHECK_EQ(l.name, std::string("Words"));
    REQUIRE(l.text.has_value());
    CHECK_EQ(l.text->text, std::string("Hi\nyo"));
    REQUIRE(l.text->runs.size() == size_t(2));
    CHECK_EQ(l.text->runs[0].length, 2);          // the paragraph break goes with the second run
    CHECK_EQ(l.text->runs[0].fontFamily, std::string("Test Sans"));
    CHECK(l.text->runs[0].bold);
    CHECK_NEAR(l.text->runs[0].fontSize, 20, 1e-9);
    CHECK(!l.text->runs[1].bold);
    CHECK(l.isLiveText());
    REQUIRE(imported->pendingTexts.size() == size_t(1));
    const auto& p = imported->pendingTexts[0];
    CHECK(p.layer == l.id);
    CHECK_NEAR(p.left, 2, 1e-9);
    CHECK_NEAR(p.top, 3, 1e-9);
    CHECK_NEAR(p.baseline, 19, 1e-9);            // the frame's top plus Affinity's ascent
    CHECK(!p.boxed);
}

TEST_CASE(damaged_documents_are_refused) {
    std::string error;
    Bytes bytes = makeDocument();
    bytes[0] = 1;
    CHECK(!importAffinityBytes(bytes, &error));
    bytes = makeDocument();
    bytes.resize(bytes.size() / 2);
    error.clear();
    CHECK(!importAffinityBytes(bytes, &error));
    CHECK(!error.empty());
}

TEST_CASE(real_documents_open_when_available) {
    const char* dir = std::getenv("COMPOSITOR_AFFINITY_FIXTURES");
    if (!dir) { std::fprintf(stderr, "  (skipped: set COMPOSITOR_AFFINITY_FIXTURES to a folder of Affinity documents, such as Patchy's test-fixtures/af)\n"); return; }
    int opened = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string ext = entry.path().extension().string();
        if (ext != ".af" && ext != ".afphoto" && ext != ".afdesign") continue;
        std::string error;
        auto imported = importAffinity(entry.path().string(), &error);
        if (!imported) { std::fprintf(stderr, "  %s: %s\n", entry.path().filename().c_str(), error.c_str()); CHECK(imported.has_value()); continue; }
        opened++;
        // The import, reduced to the preview's size, against the preview: loose, since left-out text and effects differ.
        if (!imported->composite) continue;
        auto flat = renderFlattened(imported->document);
        const int side = std::min(std::max(flat->width(), flat->height()), std::max(imported->composite->width(), imported->composite->height()));
        auto reduced = makeThumbnail(*flat, side);
        auto shrunk = makeThumbnail(*imported->composite, side);
        const Image& preview = *shrunk;
        if (std::abs(reduced->width() - preview.width()) > 2 || std::abs(reduced->height() - preview.height()) > 2) continue;
        const int cw = std::min(reduced->width(), preview.width()), ch = std::min(reduced->height(), preview.height());
        double total = 0;
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw * 4; x++) total += std::abs(int(reduced->row(y)[x]) - int(preview.row(y)[x]));
        const double mean = total / (double(cw) * ch * 4);
        std::fprintf(stderr, "  %-32s %3zu layers, %zu notes, %.2f from the preview\n", entry.path().filename().c_str(), imported->document.layers.size(), imported->notes.size(), mean);
        // Nothing left out and no text (which only the app draws): the import should look like Affinity's preview.
        if (imported->notes.empty() && imported->pendingTexts.empty()) CHECK(mean < 4.0);
    }
    CHECK(opened > 0);
}

TEST_MAIN()
