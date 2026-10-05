// Blend If (blendif.h): the ranges' bytes, the gates' arithmetic, editing, and the render of Patchy's Photoshop fixture
// against Photoshop's own render of it (photoshop-blend-if-4b-render.bmp), when Patchy is beside this checkout.
#include "check.h"
#include "compositor/blendif.h"
#include "compositor/document.h"
#include "compositor/psd.h"
#include "compositor/psd_carry.h"
#include "compositor/render.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

TEST_MAIN()
