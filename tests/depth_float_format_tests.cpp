// 32-bit documents in and out of files (docs/bit-depth.md, "32 bits"): PSD (a 32-bit file opens as a 32-bit document,
// resource 1039 read as the profile its linear values are in, and an unedited layer's channels go back byte for byte)
// and the project package's float sidecars (.f32z). No Photoshop-saved 32-bit PSD was available (Patchy's fixtures
// have none): the file is built here the way Photoshop lays one out (layers in an 'Lr32' block, big-endian floats,
// zip with PSD's byte-planar prediction and raw channels, a float mask), which is weaker evidence than a real file.
#include "check.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/project.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
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
    void f32(float v) { uint32_t bits; std::memcpy(&bits, &v, 4); u32(bits); }
    void str(const char* s) { b.insert(b.end(), s, s + std::strlen(s)); }
    void bytes(const Bytes& v) { b.insert(b.end(), v.begin(), v.end()); }
};

/// One channel as a record stores it: compression 0 (raw big-endian floats) or 3 (zip with the byte-planar predictor).
Bytes channel32(const std::vector<float>& samples, int w, int h, int compression) {
    Put p;
    p.u16(unsigned(compression));
    if (compression == 3) {
        Bytes raw(samples.size() * 4);
        for (int y = 0; y < h; y++) {
            // Written out by hand (not predictFloatRow), so the reader is checked against the layout itself.
            uint8_t* row = raw.data() + size_t(y) * size_t(w) * 4;
            for (int x = 0; x < w; x++) {
                uint32_t bits;
                std::memcpy(&bits, &samples[size_t(y) * w + x], 4);
                for (int b = 0; b < 4; b++) row[size_t(b) * w + x] = uint8_t(bits >> (24 - 8 * b));
            }
            for (size_t i = size_t(w) * 4; i-- > 1;) row[i] = uint8_t(row[i] - row[i - 1]);
        }
        uLongf size = compressBound(uLong(raw.size()));
        Bytes packed(size);
        compress2(packed.data(), &size, raw.data(), uLong(raw.size()), 9);
        packed.resize(size);
        p.bytes(packed);
    } else for (float v : samples) p.f32(v);
    return p.b;
}

struct FixtureLayer {
    std::string name;
    int left, top, width, height;
    int compression;
    std::map<int, std::vector<float>> planes;   // -1 alpha, 0..2 straight linear colour, -2 mask (over the layer's rectangle)
};

/// A 32-bit RGB PSD as Photoshop writes one: resource 1039 with the working space's profile, the layers in 'Lr32', the
/// merged image raw.
Bytes makePsd32(int width, int height, const std::vector<FixtureLayer>& layers, const ColorProfile& profile) {
    Put f;
    f.str("8BPS"); f.u16(1); for (int i = 0; i < 6; i++) f.u8(0);
    f.u16(3); f.u32(uint32_t(height)); f.u32(uint32_t(width)); f.u16(32); f.u16(3);
    f.u32(0);   // colour mode data
    Put res;
    res.str("8BIM"); res.u16(1039); res.u8(0); res.u8(0);
    res.u32(uint32_t(profile.icc.size())); res.bytes(profile.icc);
    if (profile.icc.size() & 1) res.u8(0);
    f.u32(uint32_t(res.b.size())); f.bytes(res.b);
    Put info;
    info.u16(uint16_t(int16_t(-int(layers.size()))));
    std::vector<std::vector<std::pair<int, Bytes>>> data;
    for (const FixtureLayer& l : layers) {
        std::vector<std::pair<int, Bytes>> channels;
        for (auto& [id, samples] : l.planes) channels.push_back({id, channel32(samples, l.width, l.height, id == -2 ? 0 : l.compression)});
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
    section.u32(0);   // layer information: empty, the layers are in Lr32
    section.u32(0);   // global layer mask information
    section.str("8BIM"); section.str("Lr32"); section.u32(uint32_t(info.b.size())); section.bytes(info.b);
    f.u32(uint32_t(section.b.size()));
    f.bytes(section.b);
    f.u16(0);   // merged image, raw: three planes of a mid linear grey
    for (int c = 0; c < 3; c++) for (int i = 0; i < width * height; i++) f.f32(0.2140f);
    return f.b;
}

std::vector<float> noise(std::mt19937& rng, int n, float range, bool alpha) {
    std::uniform_real_distribution<float> d(0.0f, 1.0f);
    std::vector<float> v(static_cast<size_t>(n));
    for (auto& s : v) s = alpha ? (d(rng) < 0.1f ? 0.0f : d(rng)) : d(rng) * range;
    return v;
}

std::vector<FixtureLayer> fixtureLayers() {
    std::mt19937 rng(32);
    FixtureLayer a{"raw layer", 2, 3, 20, 12, 0, {}};
    a.planes[-1] = noise(rng, a.width * a.height, 1, true);
    for (int id : {0, 1, 2}) a.planes[id] = noise(rng, a.width * a.height, 6.0f, false);   // HDR: up to 6
    FixtureLayer b{"zip layer with mask", 8, 1, 16, 14, 3, {}};
    b.planes[-1] = noise(rng, b.width * b.height, 1, true);
    for (int id : {0, 1, 2}) b.planes[id] = noise(rng, b.width * b.height, 1.5f, false);
    b.planes[-2] = noise(rng, b.width * b.height, 1, true);
    return {a, b};
}

/// The records' channels, id -> the stored bytes, from a deep PSD's 'Lr16' or 'Lr32'.
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
    g.at += g.u32();   // layer info (empty in a deep file)
    g.at += g.u32();   // global mask info
    std::vector<std::map<int, Bytes>> out;
    while (g.at + 12 <= sectionEnd) {
        g.key();
        const std::string key = g.key();
        const uint32_t length = g.u32();
        const size_t end = g.at + length;
        if (key == "Lr16" || key == "Lr32") {
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

/// Resource 1039's bytes, or empty.
Bytes profileResourceOf(const Bytes& file) {
    size_t at = 26;
    auto u32 = [&](size_t p) { return uint32_t(file[p]) << 24 | uint32_t(file[p + 1]) << 16 | uint32_t(file[p + 2]) << 8 | file[p + 3]; };
    at += 4 + u32(at);
    const size_t end = at + 4 + u32(at);
    at += 4;
    while (at + 12 <= end) {
        const unsigned id = unsigned(file[at + 4]) << 8 | file[at + 5];
        const size_t nameLength = file[at + 6];
        size_t p = at + 6 + 1 + nameLength;
        if ((nameLength + 1) & 1) p++;
        const uint32_t length = u32(p);
        if (id == 1039) return Bytes(file.begin() + long(p + 4), file.begin() + long(p + 4 + length));
        at = p + 4 + length + (length & 1);
    }
    return {};
}

fs::path scratch(const std::string& name) { return fs::temp_directory_path() / ("depth_float_format_" + std::to_string(::getpid()) + "_" + name); }

bool near(const ImageF& a, const ImageF& b, float tolerance) {
    if (a.width() != b.width() || a.height() != b.height()) return false;
    for (size_t i = 0; i < size_t(a.width()) * a.height() * 4; i++)
        if (std::fabs(a.data()[i] - b.data()[i]) > tolerance * (1 + std::fabs(b.data()[i]))) return false;
    return true;
}

} // namespace

TEST_CASE(thirty_two_bit_psd_opens_as_a_float_document) {
    const auto layers = fixtureLayers();
    std::string error;
    auto imported = importPsdBytes(makePsd32(32, 20, layers, builtinProfile(WorkingSpace::AdobeRGB)), &error);
    REQUIRE(imported.has_value());
    const Document& doc = imported->document;
    CHECK(doc.sampleType == SampleType::F32);
    REQUIRE(doc.layers.size() == 2);
    for (const Layer& l : doc.layers) REQUIRE(l.asset && l.asset->image.f32());
    // The values as stored, premultiplied: colour above 1 kept.
    const FixtureLayer& a = layers[0];
    const float* p = doc.layers[0].asset->image.f32()->pixel(0, 0);
    CHECK(p[3] == a.planes.at(-1)[0]);
    CHECK(p[0] == a.planes.at(0)[0] * a.planes.at(-1)[0]);
    bool above = false;
    for (size_t i = 0; i < a.planes.at(0).size(); i++) above |= a.planes.at(0)[i] > 1 && a.planes.at(-1)[i] > 0;
    CHECK(above);
    REQUIRE(doc.layers[1].mask.has_value());
    REQUIRE(doc.layers[1].mask->asset.image.f32() != nullptr);
    CHECK(doc.layers[1].mask->asset.image.f32()->at(3, 2) == layers[1].planes.at(-2)[size_t(2 * 16 + 3)]);
    // Resource 1039 is the profile the linear values are in: the document holds its linear version and keeps the file's.
    CHECK(isLinearProfile(doc.profile));
    REQUIRE(doc.encodedProfile.has_value());
    CHECK(doc.encodedProfile->icc == builtinProfile(WorkingSpace::AdobeRGB).icc);
    REQUIRE(imported->compositeF != nullptr);
    CHECK(std::fabs(imported->compositeF->pixel(0, 0)[0] - 0.2140f) < 1e-6f);
    for (auto& note : imported->notes) CHECK(note.find("reduced to 8 bits") == std::string::npos);
}

TEST_CASE(thirty_two_bit_psd_round_trips_byte_for_byte) {
    const Bytes original = makePsd32(32, 20, fixtureLayers(), srgbProfile());
    std::string error;
    auto imported = importPsdBytes(original, &error);
    REQUIRE(imported.has_value());
    PsdExportSummary summary;
    const Bytes out = encodePsd(imported->document, PsdExportOptions(), &summary, &error);
    REQUIRE(!out.empty());
    CHECK_EQ(int(out[22]), 0);
    CHECK_EQ(int(out[23]), 32);   // depth
    CHECK(profileResourceOf(out) == profileResourceOf(original));
    const auto before = channelsOf(original), after = channelsOf(out);
    REQUIRE(before.size() == 2);
    REQUIRE(after.size() == 2);
    for (size_t i = 0; i < before.size(); i++)
        for (auto& [id, bytes] : before[i]) {
            REQUIRE(after[i].count(id));
            CHECK(after[i].at(id) == bytes);
        }
    // Our file reads back to the same float pixels.
    auto again = importPsdBytes(out, &error);
    REQUIRE(again.has_value());
    CHECK(again->document.sampleType == SampleType::F32);
    for (size_t i = 0; i < 2; i++) CHECK(*again->document.layers[i].asset->image.f32() == *imported->document.layers[i].asset->image.f32());
    CHECK(*again->document.layers[1].mask->asset.image.f32() == *imported->document.layers[1].mask->asset.image.f32());

    // One pixel edited: that layer's channels are written anew, the other layer's stay as stored.
    Document edited = imported->document;
    auto changed = std::make_shared<ImageF>(*edited.layers[0].asset->image.f32());
    changed->pixel(0, 0)[0] = 0.5f * changed->pixel(0, 0)[3];
    edited.layers[0].asset = Asset::make(ImageFPtr(changed), edited.layers[0].name);
    const Bytes rewrittenFile = encodePsd(edited, PsdExportOptions(), &summary, &error);
    const auto rewritten = channelsOf(rewrittenFile);
    REQUIRE(rewritten.size() == 2);
    CHECK(rewritten[0].at(0) != before[0].at(0));
    for (auto& [id, bytes] : before[1]) CHECK(rewritten[1].at(id) == bytes);
    auto reread = importPsdBytes(rewrittenFile, &error);
    REQUIRE(reread.has_value());
    CHECK(near(*reread->document.layers[0].asset->image.f32(), *changed, 1e-6f));

    // Through a project (the carried planes are saved with it, the pixels as .f32z) and out again: still byte for byte.
    const fs::path project = scratch("carry.comp");
    ProjectError projectError;
    REQUIRE(saveProject(imported->document, std::nullopt, project.string(), projectError));
    auto loaded = loadProject(project.string(), projectError);
    REQUIRE(loaded.has_value());
    CHECK(loaded->sampleType == SampleType::F32);
    CHECK(*loaded->layers[0].asset->image.f32() == *imported->document.layers[0].asset->image.f32());
    const Bytes viaProjectFile = encodePsd(*loaded, PsdExportOptions(), &summary, &error);
    const auto viaProject = channelsOf(viaProjectFile);
    REQUIRE(viaProject.size() == 2);
    for (size_t i = 0; i < before.size(); i++) for (auto& [id, bytes] : before[i]) CHECK(viaProject[i].at(id) == bytes);
    CHECK(profileResourceOf(viaProjectFile) == profileResourceOf(original));
    { std::error_code cleanup_; fs::remove_all(project, cleanup_); }
}

TEST_CASE(float_documents_export_as_thirty_two_bit_psd) {
    // An 8-bit document converted to 32 bits: written at 32 bits and read back; back at 8 bits, exactly the original.
    Document doc(24, 16);
    doc.profile = builtinProfile(WorkingSpace::ProPhoto);
    auto image = std::make_shared<Image>(24, 16);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 24; x++) {
        uint8_t* p = image->pixel(x, y);
        const uint8_t a = uint8_t(x == 0 ? 0 : 40 + x * 9);
        p[0] = uint8_t(std::min<int>(a, x * 10)); p[1] = uint8_t(std::min<int>(a, y * 15)); p[2] = uint8_t(std::min<int>(a, 7)); p[3] = a;
    }
    doc.layers.push_back(Layer(Asset::make(image, "base"), Point(0, 0)));
    Layer masked(Asset::make(image, "masked"), Point(4, 2));
    LayerMask m; m.asset = MaskAsset::make(std::make_shared<GrayImage>(24, 16, 90)); masked.mask = m;
    doc.layers.push_back(masked);
    Channel channel;
    channel.id = makeUuid();
    channel.name = "Alpha 1";
    channel.image = GrayPtr(std::make_shared<GrayImage>(24, 16, 200));
    doc.channels.push_back(channel);
    const Document eight = doc;
    REQUIRE(convertSampleType(doc, SampleType::F32));
    for (bool compress : {true, false}) {
        std::string error;
        PsdExportSummary summary;
        PsdExportOptions options;
        options.compress = compress;
        auto back = importPsdBytes(encodePsd(doc, options, &summary, &error), &error);
        REQUIRE(back.has_value());
        Document& read = back->document;
        CHECK(read.sampleType == SampleType::F32);
        REQUIRE(read.layers.size() == 2);
        CHECK(near(*read.layers[0].asset->image.f32(), *doc.layers[0].asset->image.f32(), 1e-6f));
        REQUIRE(read.layers[1].mask.has_value());
        CHECK(*read.layers[1].mask->asset.image.f32() == *doc.layers[1].mask->asset.image.f32());
        REQUIRE(read.channels.size() == 1);
        CHECK(*read.channels[0].image.f32() == *doc.channels[0].image.f32());
        CHECK(read.encodedProfile && read.encodedProfile->icc == builtinProfile(WorkingSpace::ProPhoto).icc);
        // Down to 8 bits: the document it came from.
        REQUIRE(convertSampleType(read, SampleType::U8));
        CHECK(*read.layers[0].asset->image.u8() == *eight.layers[0].asset->image.u8());
        CHECK(*read.layers[1].mask->asset.image.u8() == *eight.layers[1].mask->asset.image.u8());
        CHECK(*read.channels[0].image.u8() == *eight.channels[0].image.u8());
        CHECK(read.profile.icc == eight.profile.icc);
    }
    // A PSB too.
    std::string error;
    PsdExportSummary summary;
    PsdExportOptions large;
    large.large = true;
    auto psb = importPsdBytes(encodePsd(doc, large, &summary, &error), &error);
    REQUIRE(psb.has_value());
    CHECK(psb->document.sampleType == SampleType::F32);
    CHECK(near(*psb->document.layers[1].asset->image.f32(), *doc.layers[1].asset->image.f32(), 1e-6f));
}

TEST_CASE(projects_keep_float_pixels_in_sidecars) {
    std::mt19937 rng(5);
    Document doc(20, 14);
    doc.sampleType = SampleType::F32;
    doc.profile = linearProfile(builtinProfile(WorkingSpace::DisplayP3));
    doc.encodedProfile = builtinProfile(WorkingSpace::DisplayP3);
    auto image = std::make_shared<ImageF>(12, 9);
    std::uniform_real_distribution<float> d(0.0f, 1.0f);
    for (int i = 0; i < 12 * 9; i++) { float* p = image->data() + i * 4; p[3] = d(rng); for (int c = 0; c < 3; c++) p[c] = d(rng) * 9 * p[3]; }
    image->data()[5] = std::numeric_limits<float>::quiet_NaN();   // cleaned on the way in
    Layer layer(Asset::make(ImageFPtr(image), "hdr"), Point(3, 2));
    auto mask = std::make_shared<GrayF>(12, 9);
    for (int i = 0; i < 12 * 9; i++) mask->data()[i] = d(rng);
    LayerMask m; m.asset = MaskAsset::make(GrayFPtr(mask)); layer.mask = m;
    doc.layers.push_back(layer);
    Channel channel;
    channel.id = makeUuid();
    channel.name = "Alpha 1";
    channel.image = GrayFPtr(std::make_shared<GrayF>(20, 14, 0.25f));
    doc.channels.push_back(channel);
    const std::string manifest = manifestJson(doc, std::nullopt);
    CHECK(manifest.find("\"sampleType\": \"f32\"") != std::string::npos);
    CHECK(manifest.find("\"encodedProfile\": \"encoded.icc\"") != std::string::npos);
    CHECK(manifest.find(".f32z") != std::string::npos);
    const fs::path path = scratch("float.comp");
    ProjectError error;
    REQUIRE(saveProject(doc, std::nullopt, path.string(), error));
    CHECK(fs::exists(path / "images" / (layer.id + ".f32z")));
    CHECK(fs::exists(path / "images" / (layer.id + ".mask.f32z")));
    CHECK(fs::exists(path / "channels" / (channel.id + ".f32z")));
    auto loaded = loadProject(path.string(), error);
    REQUIRE(loaded.has_value());
    CHECK(loaded->sampleType == SampleType::F32);
    const ImageF& back = *loaded->layers[0].asset->image.f32();
    CHECK(back.data()[5] == 0.0f);
    for (int i = 0; i < 12 * 9 * 4; i++) if (i != 5) CHECK(back.data()[i] == image->data()[i]);
    CHECK(*loaded->layers[0].mask->asset.image.f32() == *mask);
    CHECK(*loaded->channels[0].image.f32() == *channel.image.f32());
    CHECK(loaded->profile.icc == doc.profile.icc);
    REQUIRE(loaded->encodedProfile.has_value());
    CHECK(loaded->encodedProfile->icc == doc.encodedProfile->icc);
    // An untagged source is remembered as untagged.
    doc.encodedProfile = ColorProfile{};
    REQUIRE(saveProject(doc, std::nullopt, path.string(), error));
    loaded = loadProject(path.string(), error);
    REQUIRE(loaded.has_value());
    CHECK(loaded->encodedProfile.has_value() && loaded->encodedProfile->empty());
    // A damaged sidecar is refused.
    {
        std::ofstream damaged(path / "images" / (layer.id + ".f32z"), std::ios::binary | std::ios::trunc);
        damaged << "NPF32Z";
    }
    CHECK(!loadProject(path.string(), error).has_value());
    { std::error_code cleanup_; fs::remove_all(path, cleanup_); }
}

TEST_MAIN()
