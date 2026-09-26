// TGA, ICO, animated GIF and Aseprite: TGA and ICO round-trip through the writers; GIF and Aseprite files are
// built here byte by byte (the GIF's LZW encoder follows Patchy's, MIT, src/formats/gif_document_io.cpp).
#include "check.h"
#include "compositor/aseprite.h"
#include "compositor/gif.h"
#include "compositor/ico.h"
#include "compositor/tga.h"
#include <cstring>
#include <map>
#include <zlib.h>

using namespace compositor;

namespace {

using Bytes = std::vector<uint8_t>;
void u8(Bytes& b, uint32_t v) { b.push_back(uint8_t(v)); }
void u16(Bytes& b, uint32_t v) { u8(b, v); u8(b, v >> 8); }
void u32(Bytes& b, uint32_t v) { u16(b, v); u16(b, v >> 16); }

/// A premultiplied test pattern: opaque colours, a clear pixel and a half-transparent one.
Image pattern(int w, int h) {
    Image image(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = image.pixel(x, y);
            p[0] = uint8_t(x * 40); p[1] = uint8_t(y * 50); p[2] = uint8_t((x + y) * 20); p[3] = 255;
        }
    std::memset(image.pixel(0, 0), 0, 4);
    uint8_t* half = image.pixel(1, 0);
    half[0] = 100; half[1] = 50; half[2] = 0; half[3] = 128;
    return image;
}

bool samePixels(const Image& a, const Image& b, int tolerance) {
    if (a.width() != b.width() || a.height() != b.height()) return false;
    for (int y = 0; y < a.height(); y++)
        for (int x = 0; x < a.width() * 4; x++)
            if (std::abs(int(a.row(y)[x]) - int(b.row(y)[x])) > tolerance) return false;
    return true;
}

} // namespace

TEST_CASE(tga_round_trip) {
    const Image source = pattern(5, 4);
    Bytes bytes;
    REQUIRE(encodeTgaImage(source, bytes));
    CHECK(isTgaData(bytes.data(), bytes.size()));
    CHECK_EQ(int(bytes[2]), 10);   // RLE truecolour
    auto back = decodeTgaImage(bytes.data(), bytes.size());
    REQUIRE(back);
    CHECK(samePixels(source, *back, 1));   // the half-transparent pixel goes through straight alpha
    CHECK(std::memcmp(source.pixel(2, 2), back->pixel(2, 2), 4) == 0);
}

TEST_CASE(tga_reads_bottom_up_grayscale_and_indexed) {
    // Uncompressed 8-bit grayscale, bottom-up (descriptor 0): the first stored row is the bottom one.
    Bytes gray = {0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 8, 0, 10, 20, 30, 40};
    auto g = decodeTgaImage(gray.data(), gray.size());
    REQUIRE(g);
    CHECK_EQ(int(g->pixel(0, 1)[0]), 10);
    CHECK_EQ(int(g->pixel(1, 0)[0]), 40);
    CHECK_EQ(int(g->pixel(1, 0)[3]), 255);
    // RLE colour-mapped, top-down: a run of three index-1 pixels and a literal index 0.
    Bytes indexed = {0, 1, 9, 0, 0, 2, 0, 24, 0, 0, 0, 0, 4, 0, 1, 0, 8, 0x20};
    for (uint8_t c : {0, 0, 255, 0, 255, 0}) indexed.push_back(c);   // BGR: red, green
    for (uint8_t c : {0x82, 1, 0x00, 0}) indexed.push_back(c);
    auto i = decodeTgaImage(indexed.data(), indexed.size());
    REQUIRE(i);
    CHECK_EQ(int(i->pixel(0, 0)[1]), 255);
    CHECK_EQ(int(i->pixel(2, 0)[1]), 255);
    CHECK_EQ(int(i->pixel(3, 0)[0]), 255);
    // Truncated data fails instead of reading past the end.
    Bytes cut(indexed.begin(), indexed.end() - 2);
    std::string error;
    CHECK(!decodeTgaImage(cut.data(), cut.size(), &error));
    CHECK(!error.empty());
}

TEST_CASE(ico_round_trip) {
    Image source(64, 32);   // wider than tall: fitted and centred in each square
    source.fill(0, 0, 200, 200);
    Bytes bytes;
    REQUIRE(encodeIco(source, defaultIcoSizes, bytes));
    CHECK(isIcoData(bytes.data(), bytes.size()));
    auto imported = importIcoBytes(bytes);
    REQUIRE(imported);
    const Document& doc = imported->document;
    CHECK_EQ(doc.width, 256);
    CHECK_EQ(doc.height, 256);
    REQUIRE(doc.layers.size() == 4);
    const char* names[] = {"16x16", "32x32", "48x48", "256x256"};
    for (size_t i = 0; i < 4; i++) {
        CHECK_EQ(doc.layers[i].name, std::string(names[i]));
        CHECK_EQ(doc.layers[i].visible, i == 3);
    }
    // 32 px: the 64x32 source becomes 32x16, rows 8..23; the rest is clear.
    const Image& s32 = *doc.layers[1].asset->image;
    CHECK_EQ(s32.width(), 32);
    CHECK_EQ(int(s32.pixel(0, 7)[3]), 0);
    CHECK_EQ(int(s32.pixel(0, 8)[3]), 200);
    CHECK_EQ(int(s32.pixel(31, 23)[2]), 200);
    CHECK_EQ(int(s32.pixel(31, 24)[3]), 0);
    // The 256 px entry is PNG and keeps partial alpha exactly.
    const Image& s256 = *doc.layers[3].asset->image;
    CHECK_EQ(int(s256.pixel(128, 128)[3]), 200);

    // An opened icon exports its own sizes back unchanged.
    Bytes again;
    REQUIRE(encodeIco(source, {16, 32}, again, nullptr, &doc));
    auto reopened = importIcoBytes(again);
    REQUIRE(reopened && reopened->document.layers.size() == 2);
    CHECK(*reopened->document.layers[1].asset->image == s32);
}

namespace {

/// Patchy's GIF LZW encoder: codes LSB first in 255-byte sub-blocks.
void encodeLzw(Bytes& out, const Bytes& indexes, int minCodeSize) {
    Bytes block;
    uint32_t buffer = 0;
    int bits = 0;
    auto flush = [&] { if (!block.empty()) { u8(out, uint32_t(block.size())); out.insert(out.end(), block.begin(), block.end()); block.clear(); } };
    auto push = [&](uint8_t b) { block.push_back(b); if (block.size() == 255) flush(); };
    auto write = [&](uint32_t code, int width) {
        buffer |= code << bits;
        bits += width;
        while (bits >= 8) { push(uint8_t(buffer & 0xff)); buffer >>= 8; bits -= 8; }
    };
    const uint32_t clear = 1u << minCodeSize, end = clear + 1;
    std::map<uint32_t, uint32_t> dict;
    uint32_t next = clear + 2;
    int width = minCodeSize + 1;
    write(clear, width);
    uint32_t prefix = indexes[0];
    for (size_t i = 1; i < indexes.size(); i++) {
        const uint32_t key = prefix << 8 | indexes[i];
        auto found = dict.find(key);
        if (found != dict.end()) { prefix = found->second; continue; }
        write(prefix, width);
        dict[key] = next;
        if (next == (1u << width) && width < 12) width++;
        if (++next >= 4096) { write(clear, width); dict.clear(); next = clear + 2; width = minCodeSize + 1; }
        prefix = indexes[i];
    }
    write(prefix, width);
    write(end, width);
    if (bits > 0) push(uint8_t(buffer & 0xff));
    flush();
    u8(out, 0);
}

void gifFrame(Bytes& g, int x, int y, int w, int h, int delay, int transparent, int disposal, const Bytes& indexes) {
    u8(g, 0x21); u8(g, 0xF9); u8(g, 4);
    u8(g, uint32_t(disposal << 2 | (transparent >= 0 ? 1 : 0)));
    u16(g, uint32_t(delay)); u8(g, uint32_t(transparent >= 0 ? transparent : 0)); u8(g, 0);
    u8(g, 0x2C); u16(g, uint32_t(x)); u16(g, uint32_t(y)); u16(g, uint32_t(w)); u16(g, uint32_t(h)); u8(g, 0);
    u8(g, 2);
    encodeLzw(g, indexes, 2);
}

} // namespace

TEST_CASE(gif_frames_become_layers) {
    Bytes g = {'G', 'I', 'F', '8', '9', 'a'};
    u16(g, 20); u16(g, 20); u8(g, 0x80 | 0x70 | 1); u8(g, 0); u8(g, 0);
    for (uint8_t c : {255, 0, 0, 0, 0, 255, 0, 255, 0, 0, 0, 0}) u8(g, c);   // red, blue, green, black
    // Frame 1: the whole canvas in a pattern long enough to grow the LZW table past a few code widths.
    Bytes first(400);
    for (size_t i = 0; i < first.size(); i++) first[i] = uint8_t((i / 3 + i / 20) % 3);
    gifFrame(g, 0, 0, 20, 20, 10, -1, 1, first);
    // Frame 2: a 2x2 patch at (5, 5): blue, then index 3 (transparent: frame 1 shows through), green, blue.
    gifFrame(g, 5, 5, 2, 2, 25, 3, 2, {1, 3, 2, 1});
    // Frame 3 after frame 2's patch was cleared.
    gifFrame(g, 5, 5, 1, 1, 0, -1, 0, {2});
    u8(g, 0x3B);

    CHECK_EQ(gifFrameCount(g), 3);
    auto imported = importGifBytes(g);
    REQUIRE(imported);
    const Document& doc = imported->document;
    CHECK_EQ(doc.width, 20);
    REQUIRE(doc.layers.size() == 3);
    CHECK_EQ(doc.layers[0].name, std::string("Frame 1 (100 ms)"));
    CHECK_EQ(doc.layers[1].name, std::string("Frame 2 (250 ms)"));
    CHECK_EQ(doc.layers[2].name, std::string("Frame 3 (0 ms)"));
    CHECK(doc.layers[0].visible && !doc.layers[1].visible && !doc.layers[2].visible);
    CHECK(imported->notes.empty());
    const Image& f1 = *doc.layers[0].asset->image;
    bool matches = true;
    const uint8_t colours[3][3] = {{255, 0, 0}, {0, 0, 255}, {0, 255, 0}};
    for (int i = 0; i < 400; i++) {
        const uint8_t* p = f1.pixel(i % 20, i / 20);
        const uint8_t* c = colours[first[size_t(i)]];
        matches = matches && p[0] == c[0] && p[1] == c[1] && p[2] == c[2] && p[3] == 255;
    }
    CHECK(matches);
    const Image& f2 = *doc.layers[1].asset->image;
    CHECK_EQ(int(f2.pixel(5, 5)[2]), 255);
    CHECK(std::memcmp(f2.pixel(6, 5), f1.pixel(6, 5), 4) == 0);
    CHECK_EQ(int(f2.pixel(5, 6)[1]), 255);
    // Frame 2 disposed to background: its patch is clear under frame 3, apart from frame 3's own pixel.
    const Image& f3 = *doc.layers[2].asset->image;
    CHECK_EQ(int(f3.pixel(5, 5)[1]), 255);
    CHECK_EQ(int(f3.pixel(6, 6)[3]), 0);
    CHECK(std::memcmp(f3.pixel(0, 0), f1.pixel(0, 0), 4) == 0);

    CHECK_EQ(gifFrameCount(Bytes{'n', 'o', 'p', 'e'}), 0);
}

namespace {

Bytes aseChunk(uint16_t type, const Bytes& body) {
    Bytes c;
    u32(c, uint32_t(body.size() + 6));
    u16(c, type);
    c.insert(c.end(), body.begin(), body.end());
    return c;
}

Bytes aseLayer(uint16_t flags, uint16_t type, uint16_t level, uint16_t blend, uint8_t opacity, const std::string& name) {
    Bytes b;
    u16(b, flags); u16(b, type); u16(b, level); u16(b, 0); u16(b, 0); u16(b, blend); u8(b, opacity); u8(b, 0); u8(b, 0); u8(b, 0);
    u16(b, uint32_t(name.size()));
    b.insert(b.end(), name.begin(), name.end());
    return aseChunk(0x2004, b);
}

Bytes aseCel(uint16_t layer, int x, int y, uint8_t opacity, bool compressed, int w, int h, const Bytes& rgba) {
    Bytes b;
    u16(b, layer); u16(b, uint32_t(x)); u16(b, uint32_t(y)); u8(b, opacity); u16(b, compressed ? 2 : 0);
    for (int i = 0; i < 7; i++) u8(b, 0);
    u16(b, uint32_t(w)); u16(b, uint32_t(h));
    if (compressed) {
        uLongf size = compressBound(uLong(rgba.size()));
        Bytes z(size);
        compress(z.data(), &size, rgba.data(), uLong(rgba.size()));
        b.insert(b.end(), z.begin(), z.begin() + std::ptrdiff_t(size));
    } else {
        b.insert(b.end(), rgba.begin(), rgba.end());
    }
    return aseChunk(0x2005, b);
}

} // namespace

TEST_CASE(aseprite_first_frame_layers) {
    Bytes chunks;
    auto add = [&](const Bytes& c) { chunks.insert(chunks.end(), c.begin(), c.end()); };
    add(aseLayer(1, 1, 0, 0, 255, "Folder"));            // 0: a group
    add(aseLayer(1, 0, 1, 1, 128, "Shade"));             // 1: in the group, Multiply at 128
    add(aseLayer(0, 0, 0, 8, 255, "Hidden hard light")); // 2: hidden, Hard Light
    add(aseLayer(1, 0, 0, 0, 255, "Empty"));             // 3: no cel
    add(aseCel(1, 3, -1, 255, true, 2, 2, {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 0, 0, 10, 20, 30, 255}));
    add(aseCel(2, 0, 0, 51, false, 1, 1, {0, 0, 255, 255}));
    Bytes frame;
    u32(frame, uint32_t(16 + chunks.size())); u16(frame, 0xF1FA); u16(frame, 6); u16(frame, 100); u16(frame, 0); u32(frame, 6);
    frame.insert(frame.end(), chunks.begin(), chunks.end());
    Bytes file;
    u32(file, uint32_t(128 + frame.size())); u16(file, 0xA5E0); u16(file, 2); u16(file, 8); u16(file, 6); u16(file, 32); u32(file, 3);
    file.resize(128, 0);
    file.insert(file.end(), frame.begin(), frame.end());

    std::string error;
    auto imported = importAsepriteBytes(file, &error);
    REQUIRE(imported);
    const Document& doc = imported->document;
    CHECK_EQ(doc.width, 8);
    CHECK_EQ(doc.height, 6);
    REQUIRE(doc.layers.size() == 4);
    const Layer& folder = doc.layers[0];
    const Layer& shade = doc.layers[1];
    CHECK(folder.isGroup);
    CHECK(shade.parentId && *shade.parentId == folder.id);
    CHECK(shade.blendMode == BlendMode::Multiply);
    CHECK_NEAR(shade.opacity, 128 / 255.0, 1e-9);
    CHECK_EQ(shade.origin().x, 3.0);
    CHECK_EQ(shade.origin().y, -1.0);
    const Image& pixels = *shade.asset->image;
    CHECK_EQ(int(pixels.pixel(0, 0)[0]), 255);
    CHECK_EQ(int(pixels.pixel(1, 0)[1]), 128);   // premultiplied
    CHECK_EQ(int(pixels.pixel(0, 1)[3]), 0);
    CHECK_EQ(int(pixels.pixel(1, 1)[2]), 30);
    const Layer& hard = doc.layers[2];
    CHECK(!hard.visible);
    CHECK(hard.blendMode == BlendMode::HardLight);
    CHECK_NEAR(hard.opacity, 51 / 255.0, 1e-9);
    CHECK(!hard.parentId);
    CHECK(!doc.layers[3].asset);
    CHECK_EQ(imported->notes.size(), size_t(1));   // the second frame
    std::string validation;
    CHECK(validateHierarchy(doc.layers, &validation));

    // Adobe's .ase swatch files are told apart.
    Bytes swatches = {'A', 'S', 'E', 'F', 0, 1, 0, 0};
    CHECK(!importAsepriteBytes(swatches, &error));
}

TEST_MAIN()
