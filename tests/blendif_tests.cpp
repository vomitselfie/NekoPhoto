// Blend If (blendif.h): the ranges' bytes, the gates' arithmetic, editing, and the render of Patchy's Photoshop fixture
// against Photoshop's own render of it (photoshop-blend-if-4b-render.bmp), when Patchy is beside this checkout; and
// Advanced Blending's Channels (excluded channels) the same way (photoshop-channel-restrictions).
#include "check.h"
#include "compositor/adjustments.h"
#include "compositor/blendif.h"
#include "compositor/colormgmt.h"
#include "compositor/document.h"
#include "compositor/layerstyle.h"
#include "compositor/psd.h"
#include "compositor/project.h"
#include "compositor/psd_carry.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace compositor;

namespace {

std::string fixture(const char* name) {
    const char* dir = std::getenv("PATCHY_FIXTURES");
    return std::string(dir ? dir : PATCHY_FIXTURES) + "/" + name;
}

/// A 24-bit bottom-up BMP as straight RGB rows; empty when it cannot be read.
std::vector<uint8_t> readBmp24(const std::string& path, int& w, int& h) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (f.size() < 54 || f[0] != 'B' || f[1] != 'M') return {};
    auto u32 = [&](size_t at) { return uint32_t(f[at]) | uint32_t(f[at + 1]) << 8 | uint32_t(f[at + 2]) << 16 | uint32_t(f[at + 3]) << 24; };
    const uint32_t offset = u32(10);
    w = int(int32_t(u32(18)));
    const int rawH = int(int32_t(u32(22)));
    h = std::abs(rawH);
    if (f[28] != 24 || w <= 0 || h <= 0) return {};
    const size_t stride = (size_t(w) * 3 + 3) / 4 * 4;
    if (f.size() < offset + stride * size_t(h)) return {};
    std::vector<uint8_t> rgb(size_t(w) * size_t(h) * 3);
    for (int y = 0; y < h; y++) {
        const uint8_t* row = f.data() + offset + stride * size_t(rawH > 0 ? h - 1 - y : y);
        for (int x = 0; x < w; x++) for (int k = 0; k < 3; k++) rgb[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(k)] = row[x * 3 + 2 - k];
    }
    return rgb;
}

} // namespace

TEST_CASE(ranges_read_and_write_back) {
    // Photoshop's RGB layout: Gray, R, G, B, then an identity transparency pair.
    std::vector<uint8_t> bytes(40);
    for (size_t i = 0; i < 5; i++) { bytes[i * 8 + 2] = bytes[i * 8 + 3] = bytes[i * 8 + 6] = bytes[i * 8 + 7] = 255; }
    bytes[0] = 7; bytes[1] = 27; bytes[2] = 213; bytes[3] = 243;
    bytes[8 + 4] = 13; bytes[8 + 5] = 41;
    auto b = parseBlendIf(bytes, ColorMode::RGB);
    REQUIRE(b.has_value());
    CHECK(b->channels[0].thisLayer == (BlendIfRange{7, 27, 213, 243}));
    CHECK(b->channels[1].underlying == (BlendIfRange{13, 41, 255, 255}));
    CHECK(b->channels[2].thisLayer.identity());
    CHECK(encodeBlendIf(*b, bytes, ColorMode::RGB) == bytes);
    // A tail the model does not hold (a non-identity transparency pair) survives an edit.
    bytes[32] = 9;
    BlendIf edited = *parseBlendIf(bytes, ColorMode::RGB);
    edited.channels[3].thisLayer = {0, 0, 100, 140};
    auto written = encodeBlendIf(edited, bytes, ColorMode::RGB);
    CHECK_EQ(written.size(), size_t(40));
    CHECK_EQ(int(written[32]), 9);
    CHECK_EQ(int(written[24 + 2]), 100);
    // Nothing set and nothing there: no bytes. Something set and nothing there: a whole block (CMYK: 6 pairs).
    CHECK(encodeBlendIf(BlendIf{}, {}, ColorMode::RGB).empty());
    CHECK_EQ(encodeBlendIf(edited, {}, ColorMode::CMYK).size(), size_t(48));
    CHECK(!parseBlendIf(std::vector<uint8_t>(13), ColorMode::RGB));
}

TEST_CASE(split_handles_ramp_inclusively) {
    const BlendIfRange r{10, 14, 200, 203};
    CHECK_EQ(r.factor(9), 0.0f);
    CHECK(std::abs(r.factor(10) - 1.0f / 5) < 1e-6f);
    CHECK(std::abs(r.factor(13) - 4.0f / 5) < 1e-6f);
    CHECK_EQ(r.factor(14), 1.0f);
    CHECK_EQ(r.factor(200), 1.0f);
    CHECK(std::abs(r.factor(201) - 3.0f / 4) < 1e-6f);
    CHECK(std::abs(r.factor(203) - 1.0f / 4) < 1e-6f);
    CHECK_EQ(r.factor(204), 0.0f);
    // Joined handles cut hard.
    const BlendIfRange hard{50, 50, 100, 100};
    CHECK_EQ(hard.factor(49), 0.0f);
    CHECK_EQ(hard.factor(50), 1.0f);
    CHECK_EQ(hard.factor(100), 1.0f);
    CHECK_EQ(hard.factor(101), 0.0f);
}

TEST_CASE(gates_multiply_and_transparency_passes) {
    BlendIf b;
    b.channels[0].thisLayer = {0, 0, 128, 128};      // Gray at most 128
    b.channels[1].thisLayer = {100, 100, 255, 255};  // Red at least 100
    b.channels[0].underlying = {0, 0, 50, 50};
    BlendIfGate gate(b, ColorMode::RGB);
    const int ok[3] = {120, 120, 120}, bright[3] = {200, 200, 200}, dull[3] = {50, 120, 120};
    CHECK_EQ(gate.sourceFactor(ok), 1.0f);
    CHECK_EQ(gate.sourceFactor(bright), 0.0f);
    CHECK_EQ(gate.sourceFactor(dull), 0.0f);
    // Gray: (299 R + 590 G + 111 B) / 1000, rounded.
    const int pure[3] = {255, 0, 0};
    CHECK_EQ(gate.gray(pure), 76);
    const int dark[3] = {10, 10, 10};
    CHECK_EQ(gate.underFactor(bright, 0.0f), 1.0f);
    CHECK(std::abs(gate.underFactor(bright, 0.25f) - 0.75f) < 1e-6f);
    CHECK_EQ(gate.underFactor(dark, 1.0f), 1.0f);
}

TEST_CASE(editing_keeps_unchanged_bytes) {
    Layer layer("L", Size(4, 4));
    CHECK(!setLayerBlendIf(layer, BlendIf{}, ColorMode::RGB));
    CHECK(!layer.psdCarry);
    BlendIf b;
    b.channels[0].underlying = {0, 20, 255, 255};
    CHECK(setLayerBlendIf(layer, b, ColorMode::RGB));
    REQUIRE(layer.psdCarry);
    CHECK_EQ(layer.psdCarry->blendingRanges.size(), size_t(40));
    CHECK(layerBlendIf(layer, ColorMode::RGB).has_value());
    const auto carry = layer.psdCarry;
    CHECK(!setLayerBlendIf(layer, b, ColorMode::RGB));
    CHECK(layer.psdCarry == carry);   // the same bytes, not a copy
    CHECK(setLayerBlendIf(layer, BlendIf{}, ColorMode::RGB));
    CHECK(!layerBlendIf(layer, ColorMode::RGB).has_value());
}

TEST_CASE(edited_ranges_survive_psd_export_and_projects) {
    Document doc(8, 8);
    Layer layer("L", doc.size());
    auto image = std::make_shared<Image>(8, 8);
    for (size_t i = 0; i < image->byteCount(); i++) image->data()[i] = uint8_t(i * 7);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) image->row(y)[x * 4 + 3] = 255;
    layer.asset = Asset::make(image, "L");
    BlendIf b;
    b.channels[0].underlying = {0, 0, 120, 200};
    b.channels[2].thisLayer = {5, 60, 255, 255};
    REQUIRE(setLayerBlendIf(layer, b, ColorMode::RGB));
    doc.layers.push_back(layer);
    // PSD: written into the record and read back.
    std::string error;
    const auto bytes = encodePsd(doc, PsdExportOptions(), nullptr, &error);
    REQUIRE(!bytes.empty());
    auto back = importPsdBytes(bytes, &error);
    REQUIRE(back.has_value());
    REQUIRE(!back->document.layers.empty());
    CHECK(editableBlendIf(back->document.layers.back(), ColorMode::RGB) == b);
    // A project keeps the carry.
    const std::string path = (std::filesystem::temp_directory_path() / "blendif_tests.comp").string();
    ProjectError projectError;
    REQUIRE(saveProject(doc, std::nullopt, path, projectError));
    auto loaded = loadProject(path, projectError);
    REQUIRE(loaded.has_value());
    CHECK(editableBlendIf(loaded->layers.back(), ColorMode::RGB) == b);
    std::filesystem::remove_all(path);
}

TEST_CASE(photoshop_fixture_matches_photoshops_render) {
    std::string error;
    auto imported = importPsd(fixture("photoshop-blend-if-4b-roundtrip.psd"), &error);
    int w = 0, h = 0;
    const auto ps = readBmp24(fixture("photoshop-blend-if-4b-render.bmp"), w, h);
    if (!imported || ps.empty()) { std::printf("  skipped: Patchy's fixtures are not beside this checkout\n"); return; }
    // A layer, a Levels adjustment layer and a folder with Blend If: Gray and per-channel split ranges.
    const auto ours = renderFlattened(imported->document);
    REQUIRE(ours->width() == w && ours->height() == h);
    double sum = 0;
    int worst = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int k = 0; k < 3; k++) {
                const int d = std::abs(int(ours->row(y)[x * 4 + k]) - int(ps[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(k)]));
                sum += d;
                worst = std::max(worst, d);
            }
    std::printf("  against Photoshop's render: mean %.3f, max %d levels\n", sum / (w * h * 3.0), worst);
    CHECK(worst <= 2);
    CHECK(sum / (w * h * 3.0) < 0.6);
}

namespace {

/// A gray backdrop under a layer whose left half is dark and right half bright; This Layer's Gray range hides the
/// bright half. Optionally a blue Color Overlay on the layer, and Blend Interior Effects as Group.
Document gatedOverlayDocument(bool overlay, bool interiorAsGroup) {
    Document doc(16, 8);
    Layer base("Base", doc.size());
    auto gray = std::make_shared<Image>(16, 8);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) { uint8_t* p = gray->row(y) + x * 4; p[0] = p[1] = p[2] = 100; p[3] = 255; }
    base.asset = Asset::make(gray, "Base");
    doc.layers.push_back(base);
    Layer top("Top", doc.size());
    auto pixels = std::make_shared<Image>(16, 8);
    for (int y = 2; y < 6; y++) for (int x = 2; x < 14; x++) { uint8_t* p = pixels->row(y) + x * 4; p[0] = p[1] = p[2] = x < 8 ? 50 : 200; p[3] = 255; }
    top.asset = Asset::make(pixels, "Top");
    BlendIf b;
    b.channels[0].thisLayer = {0, 0, 128, 128};
    setLayerBlendIf(top, b, ColorMode::RGB);
    if (overlay) {
        LayerStyle style;
        ColorOverlay o;
        o.color = {0, 0, 255};
        style.colorOverlays.push_back(o);
        style.blendInteriorAsGroup = interiorAsGroup;
        setLayerStyle(top, style);
    }
    doc.layers.push_back(top);
    return doc;
}

} // namespace

TEST_CASE(interior_effects_are_not_gated) {
    // Photoshop gates the layer's own pixels; its interior effects land on the gated result inside the layer's shape.
    // Plain: the bright half is gated away and shows the backdrop.
    auto plain = renderFlattened(gatedOverlayDocument(false, false));
    CHECK_EQ(int(plain->pixel(4, 4)[0]), 50);
    CHECK_EQ(int(plain->pixel(11, 4)[0]), 100);
    // With a Color Overlay: blue over the whole shape, gated pixels included, at 8 and 16 bits.
    for (SampleType depth : {SampleType::U8, SampleType::U16}) {
        Document doc = gatedOverlayDocument(true, false);
        if (depth != SampleType::U8) REQUIRE(convertSampleType(doc, depth));
        auto styled = renderFlattened(doc);
        for (int x : {4, 11}) {
            const uint8_t* p = styled->pixel(x, 4);
            CHECK_EQ(int(p[0]), 0);
            CHECK_EQ(int(p[2]), 255);
        }
        // Outside the shape the backdrop is untouched.
        CHECK_EQ(int(styled->pixel(0, 0)[0]), 100);
    }
    // Blend Interior Effects as Group: the overlay joins the pixels and is gated with them.
    auto grouped = renderFlattened(gatedOverlayDocument(true, true));
    CHECK_EQ(int(grouped->pixel(4, 4)[2]), 255);
    CHECK_EQ(int(grouped->pixel(11, 4)[0]), 100);
    CHECK_EQ(int(grouped->pixel(11, 4)[2]), 100);
}

// ---- Advanced Blending: Channels ----------------------------------------------------------------------------------

TEST_CASE(channel_list_reads_and_writes) {
    // Photoshop's 'brst': the excluded channels' indices, big-endian u32, ascending.
    CHECK_EQ(int(*parseBlendChannels({0, 0, 0, 1}, ColorMode::RGB)), 0b010);
    CHECK_EQ(int(*parseBlendChannels({0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 2}, ColorMode::RGB)), 0b111);
    CHECK_EQ(int(*parseBlendChannels({}, ColorMode::RGB)), 0);
    CHECK(!parseBlendChannels({0, 0, 1}, ColorMode::RGB));
    // An index the mode does not have is not drawn.
    CHECK_EQ(int(*parseBlendChannels({0, 0, 0, 3}, ColorMode::RGB)), 0);
    CHECK_EQ(int(*parseBlendChannels({0, 0, 0, 3}, ColorMode::CMYK)), 0b1000);
    CHECK(encodeBlendChannels(0b101, ColorMode::RGB) == (std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 2}));
    CHECK(encodeBlendChannels(0, ColorMode::RGB).empty());
    CHECK_EQ(int(allBlendChannels(ColorMode::CMYK)), 0b1111);
    CHECK_EQ(int(allBlendChannels(ColorMode::Lab)), 0b111);

    Layer layer("L", Size(4, 4));
    CHECK(!setLayerExcludedChannels(layer, 0, ColorMode::RGB));
    CHECK(!layer.psdCarry);
    PsdLayerCarry carry;
    carry.blocks = {{"lspf", {0, 0, 0, 0}}, {"lclr", {0, 0, 0, 0, 0, 0, 0, 0}}};
    layer.psdCarry = std::make_shared<PsdLayerCarry>(carry);
    CHECK(setLayerExcludedChannels(layer, 0b010, ColorMode::RGB));
    REQUIRE(layer.psdCarry->blocks.size() == 3);
    CHECK_EQ(layer.psdCarry->blocks[1].key, std::string("brst"));   // where Photoshop writes it
    CHECK_EQ(int(layerExcludedChannels(layer, ColorMode::RGB)), 0b010);
    const auto kept = layer.psdCarry;
    CHECK(!setLayerExcludedChannels(layer, 0b010, ColorMode::RGB));
    CHECK(layer.psdCarry == kept);   // the same bytes, not a copy
    CHECK(setLayerExcludedChannels(layer, 0, ColorMode::RGB));
    CHECK_EQ(layer.psdCarry->blocks.size(), size_t(2));
}

namespace {

double sampleAt(const AnyImage& image, int x, int y, int k) {
    if (image.c8()) return image.c8()->pixel(x, y)[k];
    if (image.u8()) return image.u8()->pixel(x, y)[k];
    if (image.u16()) return image.u16()->pixel(x, y)[k];
    return image.f32()->pixel(x, y)[k];
}

/// A backdrop (opaque on the left 6 columns, clear on the right 2) under a layer of one colour over all of it, in
/// `blend`, at `depth` in `mode`. The top layer is the last one.
Document excludedChannelsDocument(SampleType depth, ColorMode mode, BlendMode blend) {
    Document doc(8, 4);
    auto back = std::make_shared<Image>(8, 4);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 6; x++) { uint8_t* p = back->row(y) + x * 4; p[0] = 90; p[1] = 140; p[2] = 60; p[3] = 255; }
    doc.layers.push_back(Layer(Asset::make(ImagePtr(back), "Back"), Point(0, 0)));
    auto top = std::make_shared<Image>(8, 4);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) { uint8_t* p = top->row(y) + x * 4; p[0] = 200; p[1] = 50; p[2] = 160; p[3] = 255; }
    Layer layer(Asset::make(ImagePtr(top), "Top"), Point(0, 0));
    layer.blendMode = blend;
    doc.layers.push_back(layer);
    std::string why;
    if (depth != SampleType::U8 && !convertSampleType(doc, depth, &why)) check::fail(__FILE__, __LINE__, "convertSampleType: " + why);
    if (mode != ColorMode::RGB && !convertDocumentMode(doc, mode, ColorProfile(), ConvertOptions(), &why)) check::fail(__FILE__, __LINE__, "convertDocumentMode: " + why);
    return doc;
}

/// The top layer with `excluded` against itself without (`full`) and hidden (`none`): every excluded channel reads as
/// `none` does, every other channel and alpha as `full` does.
void checkExcluded(Document doc, uint8_t excluded) {
    const AnyImage full = renderNative(doc);
    doc.layers.back().visible = false;
    const AnyImage none = renderNative(doc);
    doc.layers.back().visible = true;
    REQUIRE(setLayerExcludedChannels(doc.layers.back(), excluded, doc.colorMode));
    const AnyImage drawn = renderNative(doc);
    const int n = colorModeChannels(doc.colorMode);
    const bool all = excluded == allBlendChannels(doc.colorMode);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++)
            for (int k = 0; k < n; k++) {
                const bool kept = all || (k < n - 1 && (excluded & (1u << k)));
                CHECK_EQ(sampleAt(drawn, x, y, k), sampleAt(kept ? none : full, x, y, k));
            }
}

} // namespace

TEST_CASE(excluded_channels_keep_the_backdrop_at_every_depth_and_mode) {
    struct Case { SampleType depth; ColorMode mode; uint8_t excluded; };
    const Case cases[] = {
        {SampleType::U8, ColorMode::RGB, 0b010}, {SampleType::U16, ColorMode::RGB, 0b010}, {SampleType::F32, ColorMode::RGB, 0b101},
        {SampleType::U8, ColorMode::CMYK, 0b1010}, {SampleType::U16, ColorMode::CMYK, 0b0001},
        {SampleType::U8, ColorMode::Lab, 0b001}, {SampleType::U16, ColorMode::Lab, 0b110},
    };
    for (const Case& c : cases)
        for (BlendMode blend : {BlendMode::Normal, BlendMode::Multiply}) {
            checkExcluded(excludedChannelsDocument(c.depth, c.mode, blend), c.excluded);
            // Every channel excluded: the layer is gone, alpha too (the clear columns stay clear).
            checkExcluded(excludedChannelsDocument(c.depth, c.mode, blend), allBlendChannels(c.mode));
        }
}

TEST_CASE(excluded_channels_on_adjustments_clips_and_folders) {
    auto rgbAt = [](const Document& doc, int x) { const uint8_t* p = renderFlattened(doc)->pixel(x, 1); return std::array<int, 4>{p[0], p[1], p[2], p[3]}; };
    // An Invert adjustment leaves an excluded channel as it was.
    {
        Document doc = excludedChannelsDocument(SampleType::U8, ColorMode::RGB, BlendMode::Normal);
        doc.layers.pop_back();
        Layer invert("Invert", doc.size());
        invert.adjustment = AdjustmentSettings::defaults(AdjustmentKind::Invert).toLayerAdjustment();
        setLayerExcludedChannels(invert, 0b001, ColorMode::RGB);
        doc.layers.push_back(invert);
        CHECK(rgbAt(doc, 1) == (std::array<int, 4>{90, 255 - 140, 255 - 60, 255}));
    }
    // A clipped layer's excluded channel keeps the base's colour; a base's exclusion keeps the backdrop's under the
    // whole clipped result.
    {
        Document doc = excludedChannelsDocument(SampleType::U8, ColorMode::RGB, BlendMode::Normal);
        auto small = std::make_shared<Image>(8, 4);
        for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) { uint8_t* p = small->row(y) + x * 4; p[0] = 10; p[1] = 20; p[2] = 30; p[3] = 255; }
        Layer clipped(Asset::make(ImagePtr(small), "Clipped"), Point(0, 0));
        clipped.maskSourceId = doc.layers.back().id;
        setLayerExcludedChannels(clipped, 0b100, ColorMode::RGB);
        doc.layers.push_back(clipped);
        CHECK(rgbAt(doc, 1) == (std::array<int, 4>{10, 20, 160, 255}));
        setLayerExcludedChannels(doc.layers[1], 0b001, ColorMode::RGB);
        CHECK(rgbAt(doc, 1) == (std::array<int, 4>{90, 20, 160, 255}));
    }
    // A Pass Through folder's exclusion holds over its children (a Multiply child still meets the backdrop).
    for (bool passThrough : {true, false}) {
        Document doc = excludedChannelsDocument(SampleType::U8, ColorMode::RGB, BlendMode::Multiply);
        Layer folder("Folder", doc.size());
        folder.isGroup = true;
        folder.passThrough = passThrough;
        setLayerExcludedChannels(folder, 0b010, ColorMode::RGB);
        doc.layers.back().parentId = folder.id;
        doc.layers.insert(doc.layers.begin() + 1, folder);
        const auto at = rgbAt(doc, 1);
        CHECK_EQ(at[1], 140);
        CHECK_EQ(at[0], passThrough ? (90 * 200 + 127) / 255 : 200);
    }
}

TEST_CASE(excluded_channels_survive_psd_export_projects_and_mode_changes) {
    Document doc = excludedChannelsDocument(SampleType::U8, ColorMode::RGB, BlendMode::Normal);
    REQUIRE(setLayerExcludedChannels(doc.layers.back(), 0b011, ColorMode::RGB));
    std::string error;
    const auto bytes = encodePsd(doc, PsdExportOptions(), nullptr, &error);
    REQUIRE(!bytes.empty());
    auto back = importPsdBytes(bytes, &error);
    REQUIRE(back.has_value());
    CHECK_EQ(int(layerExcludedChannels(back->document.layers.back(), ColorMode::RGB)), 0b011);
    const std::string path = (std::filesystem::temp_directory_path() / "blendchannels_tests.comp").string();
    ProjectError projectError;
    REQUIRE(saveProject(doc, std::nullopt, path, projectError));
    auto loaded = loadProject(path, projectError);
    REQUIRE(loaded.has_value());
    CHECK_EQ(int(layerExcludedChannels(loaded->layers.back(), ColorMode::RGB)), 0b011);
    std::filesystem::remove_all(path);
    // The channels mean something else in another mode: every channel takes part again.
    std::string why;
    REQUIRE(convertDocumentMode(doc, ColorMode::CMYK, ColorProfile(), ConvertOptions(), &why));
    CHECK_EQ(int(layerExcludedChannels(doc.layers.back(), ColorMode::CMYK)), 0);
}

TEST_CASE(channel_restrictions_fixture_matches_photoshops_render) {
    std::string error;
    auto imported = importPsd(fixture("photoshop-channel-restrictions.psd"), &error);
    int w = 0, h = 0;
    const auto ps = readBmp24(fixture("photoshop-channel-restrictions.bmp"), w, h);
    if (!imported || ps.empty()) { std::printf("  skipped: Patchy's fixtures are not beside this checkout\n"); return; }
    // Normal, Multiply, Linear Burn at Fill 50%, a layer with effects, and one with every channel excluded.
    const auto ours = renderFlattened(imported->document);
    REQUIRE(ours->width() == w && ours->height() == h);
    int worst = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int k = 0; k < 3; k++) {
                const int mine = int(ours->row(y)[x * 4 + k]) + 255 - int(ours->row(y)[x * 4 + 3]);
                worst = std::max(worst, std::abs(mine - int(ps[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(k)])));
            }
    std::printf("  against Photoshop's render: max %d levels\n", worst);
    CHECK(worst <= 1);
}

TEST_MAIN()
