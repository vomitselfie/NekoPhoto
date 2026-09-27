// 16-bit documents in and out of files (docs/bit-depth.md): PSD (a 16-bit file opens as a 16-bit document and an
// unedited layer's channels go back byte for byte), 16-bit PNG, and the project package's version 8 with its
// sampleType. The 16-bit PSD is built here the way Photoshop lays one out (layers in an 'Lr16' block, zip with
// prediction and raw channels, odd 16-bit values that 0..32768 cannot hold).
#include "check.h"
#include "compositor/depth.h"
#include "compositor/png.h"
#include "compositor/project.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <unistd.h>
#include <zlib.h>

using namespace compositor;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

struct Put {
    Bytes b;
    void u8(unsigned v) { b.push_back(uint8_t(v)); }
    void u16(unsigned v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xffff); }
    void str(const char* s) { b.insert(b.end(), s, s + std::strlen(s)); }
    void bytes(const Bytes& v) { b.insert(b.end(), v.begin(), v.end()); }
};

/// One channel as a record stores it: compression 0 (raw) or 3 (zip with prediction), 16-bit big-endian samples.
Bytes channel16(const std::vector<uint16_t>& samples, int w, int h, int compression) {
    Put p;
    p.u16(unsigned(compression));
    Bytes raw(samples.size() * 2);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const size_t i = size_t(y) * w + x;
            const uint16_t v = compression == 3 ? uint16_t(samples[i] - (x ? samples[i - 1] : 0)) : samples[i];
            raw[i * 2] = uint8_t(v >> 8); raw[i * 2 + 1] = uint8_t(v);
        }
    if (compression == 3) {
        uLongf size = compressBound(uLong(raw.size()));
        Bytes packed(size);
        compress2(packed.data(), &size, raw.data(), uLong(raw.size()), 9);
        packed.resize(size);
        p.bytes(packed);
    } else p.bytes(raw);
    return p.b;
}

struct FixtureLayer {
    std::string name;
    int left, top, width, height;
    int compression;
    std::map<int, std::vector<uint16_t>> planes;   // -1 alpha, 0..2 colour, -2 mask (over the layer's rectangle)
};

/// A 16-bit RGB PSD as Photoshop writes one: the layers in 'Lr16', the merged image raw.
Bytes makePsd16(int width, int height, const std::vector<FixtureLayer>& layers) {
    Put f;
    f.str("8BPS"); f.u16(1); for (int i = 0; i < 6; i++) f.u8(0);
    f.u16(3); f.u32(uint32_t(height)); f.u32(uint32_t(width)); f.u16(16); f.u16(3);
    f.u32(0);   // colour mode data
    f.u32(0);   // image resources
    Put info;
    info.u16(uint16_t(int16_t(-int(layers.size()))));
    std::vector<std::vector<std::pair<int, Bytes>>> data;
    for (const FixtureLayer& l : layers) {
        std::vector<std::pair<int, Bytes>> channels;
        for (auto& [id, samples] : l.planes) channels.push_back({id, channel16(samples, l.width, l.height, id == -2 ? 0 : l.compression)});
        info.u32(uint32_t(l.top)); info.u32(uint32_t(l.left)); info.u32(uint32_t(l.top + l.height)); info.u32(uint32_t(l.left + l.width));
        info.u16(unsigned(channels.size()));
        for (auto& [id, bytes] : channels) { info.u16(uint16_t(int16_t(id))); info.u32(uint32_t(bytes.size())); }
        info.str("8BIM"); info.str("norm");
        info.u8(255); info.u8(0); info.u8(0x08); info.u8(0);
        Put extra;
        if (l.planes.count(-2)) {
            extra.u32(20);
            extra.u32(uint32_t(l.top)); extra.u32(uint32_t(l.left)); extra.u32(uint32_t(l.top + l.height)); extra.u32(uint32_t(l.left + l.width));
            extra.u8(0); extra.u8(0); extra.u16(0);
        } else extra.u32(0);
        extra.u32(0);   // blending ranges
        extra.u8(unsigned(l.name.size())); extra.str(l.name.c_str());
        for (size_t used = 1 + l.name.size(); used % 4; used++) extra.u8(0);
        info.u32(uint32_t(extra.b.size()));
        info.bytes(extra.b);
        data.push_back(std::move(channels));
    }
    for (auto& channels : data) for (auto& [id, bytes] : channels) info.bytes(bytes);
    while (info.b.size() % 4) info.u8(0);
    Put section;
    section.u32(0);   // layer information: empty, the layers are in Lr16
    section.u32(0);   // global layer mask information
    section.str("8BIM"); section.str("Lr16"); section.u32(uint32_t(info.b.size())); section.bytes(info.b);
    f.u32(uint32_t(section.b.size()));
    f.bytes(section.b);
    f.u16(0);   // merged image, raw: three planes of mid grey
    for (int c = 0; c < 3; c++) for (int i = 0; i < width * height; i++) f.u16(0x8001);
    return f.b;
}

std::vector<uint16_t> noise(std::mt19937& rng, int n, bool odd) {
    std::vector<uint16_t> v(static_cast<size_t>(n));
    for (auto& s : v) s = uint16_t((rng() & 0xfffe) | (odd ? 1 : 0));
    return v;
}

std::vector<FixtureLayer> fixtureLayers() {
    std::mt19937 rng(65535);
    FixtureLayer a{"raw layer", 2, 3, 20, 12, 0, {}};
    for (int id : {-1, 0, 1, 2}) a.planes[id] = noise(rng, a.width * a.height, true);
    FixtureLayer b{"zip layer with mask", 8, 1, 16, 14, 3, {}};
    for (int id : {-1, 0, 1, 2}) b.planes[id] = noise(rng, b.width * b.height, id != 1);
    b.planes[-2] = noise(rng, b.width * b.height, true);
    return {a, b};
}

/// The records' channels, id -> the stored bytes (compression word and data), from a 16-bit PSD's 'Lr16'.
std::vector<std::map<int, Bytes>> channelsOf(const Bytes& file) {
    struct Get {
        const Bytes& b; size_t at;
        uint32_t u8() { return b.at(at++); }
        uint32_t u16() { uint32_t v = u8() << 8; return v | u8(); }
        uint32_t u32() { uint32_t v = u16() << 16; return v | u16(); }
        std::string key() { std::string k(b.begin() + long(at), b.begin() + long(at + 4)); at += 4; return k; }
    } g{file, 26};
    g.at += g.u32();   // colour mode data
    g.at += g.u32();   // resources
    const uint32_t sectionLength = g.u32();
    const size_t sectionEnd = g.at + sectionLength;
    g.at += g.u32();   // layer info (empty in a 16-bit file)
    g.at += g.u32();   // global mask info
    std::vector<std::map<int, Bytes>> out;
    while (g.at + 12 <= sectionEnd) {
        g.key();
        const std::string key = g.key();
        const uint32_t length = g.u32();
        const size_t end = g.at + length;
        if (key == "Lr16") {
            int count = int16_t(g.u16());
            count = count < 0 ? -count : count;
            std::vector<std::vector<std::pair<int, uint32_t>>> lengths(static_cast<size_t>(count));
            for (int i = 0; i < count; i++) {
                g.at += 16;
                const uint32_t channels = g.u16();
                for (uint32_t c = 0; c < channels; c++) { const int id = int16_t(g.u16()); lengths[size_t(i)].push_back({id, g.u32()}); }
                g.at += 12;
                g.at += g.u32();   // extra data
            }
            for (auto& record : lengths) {
                std::map<int, Bytes> channels;
                for (auto& [id, length] : record) { channels[id] = Bytes(file.begin() + long(g.at), file.begin() + long(g.at + length)); g.at += length; }
                out.push_back(std::move(channels));
            }
        }
        g.at = end + (4 - length % 4) % 4;
    }
    return out;
}

fs::path scratch(const std::string& name) { return fs::temp_directory_path() / ("depth_format_" + std::to_string(::getpid()) + "_" + name); }

} // namespace

TEST_CASE(sixteen_bit_psd_opens_as_a_sixteen_bit_document) {
    const auto layers = fixtureLayers();
    std::string error;
    auto imported = importPsdBytes(makePsd16(32, 20, layers), &error);
    REQUIRE(imported.has_value());
    const Document& doc = imported->document;
    CHECK(doc.sampleType == SampleType::U16);
    REQUIRE(doc.layers.size() == 2);
    for (const Layer& l : doc.layers) REQUIRE(l.asset && l.asset->image.u16());
    // A sample read at 16 bits: 0..65535 mapped to 0..32768 (straight colour times alpha).
    const FixtureLayer& a = layers[0];
    const uint16_t* p = doc.layers[0].asset->image.u16()->pixel(0, 0);
    const uint32_t alpha = from65535(a.planes.at(-1)[0]);
    CHECK_EQ(int(p[3]), int(alpha));
    CHECK_EQ(int(p[0]), int(mul15(from65535(a.planes.at(0)[0]), alpha)));
    REQUIRE(doc.layers[1].mask.has_value());
    REQUIRE(doc.layers[1].mask->asset.image.u16() != nullptr);
    CHECK_EQ(int(doc.layers[1].mask->asset.image.u16()->at(3, 2)), int(from65535(layers[1].planes.at(-2)[size_t(2 * 16 + 3)])));
    for (auto& note : imported->notes) CHECK(note.find("reduced to 8 bits") == std::string::npos);
}

TEST_CASE(sixteen_bit_psd_round_trips_byte_for_byte) {
    const Bytes original = makePsd16(32, 20, fixtureLayers());
    std::string error;
    auto imported = importPsdBytes(original, &error);
    REQUIRE(imported.has_value());
    PsdExportSummary summary;
    const Bytes out = encodePsd(imported->document, PsdExportOptions(), &summary, &error);
    REQUIRE(!out.empty());
    CHECK_EQ(int(out[22]), 0);
    CHECK_EQ(int(out[23]), 16);   // depth
    const auto before = channelsOf(original), after = channelsOf(out);
    REQUIRE(before.size() == 2);
    REQUIRE(after.size() == 2);
    for (size_t i = 0; i < before.size(); i++)
        for (auto& [id, bytes] : before[i]) {
            REQUIRE(after[i].count(id));
            CHECK(after[i].at(id) == bytes);
        }
    // Our file reads back to the same 16-bit pixels.
    auto again = importPsdBytes(out, &error);
    REQUIRE(again.has_value());
    CHECK(again->document.sampleType == SampleType::U16);
    for (size_t i = 0; i < 2; i++) CHECK(*again->document.layers[i].asset->image.u16() == *imported->document.layers[i].asset->image.u16());

    // One pixel edited: that layer's channels are written anew (15 bits widened), the other layer's stay as stored.
    Document edited = imported->document;
    auto changed = std::make_shared<Image16>(*edited.layers[0].asset->image.u16());
    changed->pixel(0, 0)[0] = 0;
    edited.layers[0].asset = Asset::make(Image16Ptr(changed), edited.layers[0].name);
    const auto rewritten = channelsOf(encodePsd(edited, PsdExportOptions(), &summary, &error));
    REQUIRE(rewritten.size() == 2);
    CHECK(rewritten[0].at(0) != before[0].at(0));
    for (auto& [id, bytes] : before[1]) CHECK(rewritten[1].at(id) == bytes);
    auto reread = importPsdBytes(encodePsd(edited, PsdExportOptions(), &summary, &error), &error);
    REQUIRE(reread.has_value());
    CHECK(*reread->document.layers[0].asset->image.u16() == *changed);   // 15-bit values survive 0..65535 exactly

    // Through a project (the carried planes are saved with it) and out again: still byte for byte.
    const fs::path project = scratch("carry.comp");
    ProjectError projectError;
    REQUIRE(saveProject(imported->document, std::nullopt, project.string(), projectError));
    auto loaded = loadProject(project.string(), projectError);
    REQUIRE(loaded.has_value());
    CHECK(loaded->sampleType == SampleType::U16);
    const auto viaProject = channelsOf(encodePsd(*loaded, PsdExportOptions(), &summary, &error));
    REQUIRE(viaProject.size() == 2);
    for (size_t i = 0; i < before.size(); i++) for (auto& [id, bytes] : before[i]) CHECK(viaProject[i].at(id) == bytes);
    fs::remove_all(project);
}

TEST_CASE(sixteen_bit_documents_export_as_sixteen_bit_psd) {
    // An 8-bit document converted to 16 bits: every layer written at 16 bits, read back exactly.
    Document doc(24, 16);
    auto image = std::make_shared<Image>(24, 16);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 24; x++) { uint8_t* p = image->pixel(x, y); p[0] = uint8_t(x * 10); p[1] = uint8_t(y * 15); p[2] = 7; p[3] = 255; }
    doc.layers.push_back(Layer(Asset::make(image, "base"), Point(0, 0)));
    Layer masked(Asset::make(image, "masked"), Point(4, 2));
    LayerMask m; m.asset = MaskAsset::make(std::make_shared<GrayImage>(24, 16, 90)); masked.mask = m;
    doc.layers.push_back(masked);
    REQUIRE(convertSampleType(doc, SampleType::U16));
    std::string error;
    PsdExportSummary summary;
    auto back = importPsdBytes(encodePsd(doc, PsdExportOptions(), &summary, &error), &error);
    REQUIRE(back.has_value());
    CHECK(back->document.sampleType == SampleType::U16);
    REQUIRE(back->document.layers.size() == 2);
    CHECK(*back->document.layers[0].asset->image.u16() == *doc.layers[0].asset->image.u16());
    REQUIRE(back->document.layers[1].mask.has_value());
    CHECK(*back->document.layers[1].mask->asset.image.u16() == *doc.layers[1].mask->asset.image.u16());
    // Raw (uncompressed) channels too.
    PsdExportOptions raw;
    raw.compress = false;
    auto rawBack = importPsdBytes(encodePsd(doc, raw, &summary, &error), &error);
    REQUIRE(rawBack.has_value());
    CHECK(*rawBack->document.layers[0].asset->image.u16() == *doc.layers[0].asset->image.u16());
}

TEST_CASE(sixteen_bit_png_round_trips) {
    Image16 image(9, 7);
    std::mt19937 rng(7);
    for (int y = 0; y < 7; y++) for (int x = 0; x < 9; x++) {
        uint16_t* p = image.pixel(x, y);
        const uint32_t a = x == 0 ? one16 : x == 1 ? 0 : rng() % (one16 + 1);
        for (int c = 0; c < 3; c++) p[c] = uint16_t(mul15(rng() % (one16 + 1), a));
        p[3] = uint16_t(a);
    }
    const fs::path path = scratch("deep.png");
    REQUIRE(writePngImage16(path.string(), image));
    PngInfo info;
    REQUIRE(readPngInfo(path.string(), info));
    CHECK_EQ(info.bitDepth, 16);
    auto back = readPngImage16(path.string());
    REQUIRE(back != nullptr);
    CHECK(*back == image);
    // An 8-bit reader still opens it (reduced), and an 8-bit PNG opens at 16 bits widened.
    auto eight = readPngImage(path.string());
    REQUIRE(eight != nullptr);
    CHECK_EQ(eight->width(), 9);
    Gray16 mask(5, 4, 12345);
    REQUIRE(writePngGray16(path.string(), mask));
    auto maskBack = readPngGray16(path.string());
    REQUIRE(maskBack != nullptr);
    CHECK(*maskBack == mask);
    fs::remove(path);
}

TEST_CASE(project_version_eight_keeps_the_sample_type) {
    Document doc(16, 12);
    auto image = std::make_shared<Image16>(10, 8);
    for (int i = 0; i < 10 * 8 * 4; i++) image->data()[i] = uint16_t((i * 997) % 32769);
    for (int i = 0; i < 10 * 8; i++) { uint16_t* p = image->data() + i * 4; p[3] = std::max(p[3], std::max(p[0], std::max(p[1], p[2]))); }
    doc.sampleType = SampleType::U16;
    Layer layer(Asset::make(Image16Ptr(image), "deep"), Point(3, 2));
    LayerMask m; m.asset = MaskAsset::make(Gray16Ptr(std::make_shared<Gray16>(10, 8, 20001))); layer.mask = m;
    doc.layers.push_back(layer);
    const std::string manifest = manifestJson(doc, std::nullopt);
    CHECK(manifest.find("\"sampleType\": \"u16\"") != std::string::npos);
    CHECK(manifest.find("\"version\": 8") != std::string::npos);
    const fs::path path = scratch("deep.comp");
    ProjectError error;
    REQUIRE(saveProject(doc, std::nullopt, path.string(), error));
    PngInfo info;
    REQUIRE(readPngInfo((path / "images" / (layer.id + ".png")).string(), info));
    CHECK_EQ(info.bitDepth, 16);
    auto loaded = loadProject(path.string(), error);
    REQUIRE(loaded.has_value());
    CHECK(loaded->sampleType == SampleType::U16);
    CHECK(*loaded->layers[0].asset->image.u16() == *image);
    CHECK(*loaded->layers[0].mask->asset.image.u16() == *m.asset.image.u16());
    // An 8-bit document writes no sampleType, as before, and loads as 8-bit.
    Document eight(16, 12);
    eight.layers.push_back(Layer(Asset::make(std::make_shared<Image>(4, 4), "flat"), Point(0, 0)));
    const std::string plain = manifestJson(eight, std::nullopt);
    CHECK(plain.find("sampleType") == std::string::npos);
    CHECK(plain.find("\"version\": 7") != std::string::npos);
    auto parsed = parseManifest(plain, error);
    REQUIRE(parsed.has_value());
    CHECK(parsed->sampleType == SampleType::U8);
    // A sampleType before version 8, or an unknown one, is damage.
    std::string old = manifest;
    old.replace(old.find("\"version\": 8"), 12, "\"version\": 7");
    CHECK(!parseManifest(old, error).has_value());
    std::string unknown = manifest;
    unknown.replace(unknown.find("\"u16\""), 5, "\"u32\"");
    CHECK(!parseManifest(unknown, error).has_value());
    fs::remove_all(path);
}

TEST_MAIN()
