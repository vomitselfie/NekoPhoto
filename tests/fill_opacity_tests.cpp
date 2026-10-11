// Fill opacity in Photoshop's "special eight" blend modes (Color Burn, Linear Burn, Color Dodge, Linear Dodge, Vivid
// Light, Linear Light, Hard Mix, Difference). For every other mode a layer's Fill is just less coverage, like Opacity;
// for these eight, Photoshop applies Fill inside the blend, so a layer at Fill 50% does not look like the same layer at
// Opacity 50%. A PSD keeps Fill apart from Opacity ('iOpa'); NekoPhoto keeps it in the layer's carry (psd_carry.h).
//
// The expected colours are Photoshop's own composites of a solid layer over a solid Background, measured by two
// independent projects:
// - PhotoCraft (storytold/photocraft, MIT OR Apache-2.0), crates/compose/src/psblend.rs,
//   special_eight_fill_matches_photoshop, at commit 88ffd89: Photoshop 27.11, Fill 128/255, 64/255 and 191/255.
// - Patchy (MIT), tests/core/compositor_blend_if_tests.cpp, compositor_fill_opacity_matches_photoshop_modes:
//   Photoshop 2026, Fill 128/255.
// They used different colours, and agree: PhotoCraft's closed form (a source pulled toward the mode's neutral colour by
// Fill and blended at full strength; Hard Mix (b - f(1 - s)) / (1 - f)), fitted to its own measurements, predicts
// Patchy's within 2 levels, where treating Fill as opacity is off by up to 48.
//
// Each case goes the way a user's file does: the document is written as a PSD with Opacity 100% and the Fill, read back,
// and flattened. The controls (Normal, Multiply) check that Fill stays plain coverage in the other modes.
#include "check.h"
#include "compositor/document.h"
#include "compositor/psd.h"
#include "compositor/psd_carry.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace compositor;

namespace {

using RGB = std::array<int, 3>;

std::shared_ptr<Image> solid(const RGB& c) {
    auto image = std::make_shared<Image>(4, 4);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            uint8_t* p = image->row(y) + x * 4;
            p[0] = uint8_t(c[0]); p[1] = uint8_t(c[1]); p[2] = uint8_t(c[2]); p[3] = 255;
        }
    return image;
}

/// A Background of `base` under a layer of `top` in `mode` at Opacity 100% and Fill `fill` (0..255), through a PSD.
std::optional<RGB> flattenThroughPsd(const RGB& base, const RGB& top, BlendMode mode, int fill) {
    Document doc(4, 4);
    Layer background("Background", doc.size());
    background.asset = Asset::make(solid(base), "Background");
    doc.layers.push_back(background);
    Layer layer("Fill", doc.size());
    layer.asset = Asset::make(solid(top), "Fill");
    layer.blendMode = mode;
    auto carry = std::make_shared<PsdLayerCarry>();
    carry->opacity = 255;
    carry->fill = uint8_t(fill);
    layer.psdCarry = carry;
    layer.opacity = fill / 255.0;   // NekoPhoto's one opacity: Opacity x Fill
    doc.layers.push_back(layer);

    std::string error;
    const std::vector<uint8_t> bytes = encodePsd(doc, PsdExportOptions{}, nullptr, &error);
    if (bytes.empty()) { std::fprintf(stderr, "  encodePsd: %s\n", error.c_str()); return std::nullopt; }
    auto imported = importPsdBytes(bytes, &error);
    if (!imported) { std::fprintf(stderr, "  importPsdBytes: %s\n", error.c_str()); return std::nullopt; }
    // The file kept Fill apart from Opacity.
    const Layer& back = imported->document.layers.back();
    if (!back.psdCarry || back.psdCarry->fill != fill || back.psdCarry->opacity != 255) {
        std::fprintf(stderr, "  Fill did not survive the PSD\n");
        return std::nullopt;
    }
    auto flat = renderFlattened(imported->document);
    if (!flat) return std::nullopt;
    const uint8_t* p = flat->pixel(1, 1);
    return RGB{p[0], p[1], p[2]};
}

struct Measured {
    const char* source;
    BlendMode mode;
    RGB base, top;
    int fill;
    RGB photoshop;
    int tolerance;
};

void expect(const Measured& m) {
    const auto got = flattenThroughPsd(m.base, m.top, m.mode, m.fill);
    REQUIRE(got.has_value());
    int worst = 0;
    for (int k = 0; k < 3; k++) worst = std::max(worst, std::abs((*got)[k] - m.photoshop[k]));
    if (worst > m.tolerance)
        check::fail(__FILE__, __LINE__,
                    std::string(blendModeName(m.mode)) + " at Fill " + std::to_string(m.fill) + "/255 (" + m.source + "): ours " +
                        std::to_string((*got)[0]) + "," + std::to_string((*got)[1]) + "," + std::to_string((*got)[2]) + ", Photoshop " +
                        std::to_string(m.photoshop[0]) + "," + std::to_string(m.photoshop[1]) + "," + std::to_string(m.photoshop[2]) +
                        " (off by " + std::to_string(worst) + ")");
}

constexpr RGB kBaseA{200, 150, 100}, kTopA{100, 180, 220};   // PhotoCraft's first measurement
constexpr RGB kBaseB{60, 120, 180}, kTopB{210, 90, 30};      // PhotoCraft's second
constexpr RGB kBaseC{40, 100, 180}, kTopC{200, 60, 120};     // Patchy's

} // namespace

TEST_CASE(other_modes_take_fill_as_coverage) {
    // Normal at Fill 50%: a plain 50% mix. Multiply likewise mixes its blend by Fill.
    expect({"control", BlendMode::Normal, kBaseC, kTopC, 128, {120, 80, 150}, 1});
    // Multiply (computed, not measured): 200x100/255 = 78, 150x180/255 = 106, 100x220/255 = 86; mixed 128/255 with the base.
    expect({"control", BlendMode::Multiply, kBaseA, kTopA, 128, {139, 128, 93}, 1});
}

TEST_CASE(special_eight_fill_matches_photoshop) {
    const char* pc = "PhotoCraft, Photoshop 27.11";
    const char* patchy = "Patchy, Photoshop 2026";
    const std::vector<Measured> cases{
        // Fill 50% (128/255).
        {pc, BlendMode::ColorBurn, kBaseA, kTopA, 128, {176, 132, 88}, 2},
        {pc, BlendMode::LinearBurn, kBaseA, kTopA, 128, {122, 112, 82}, 2},
        {pc, BlendMode::ColorDodge, kBaseA, kTopA, 128, {249, 232, 176}, 2},
        {pc, BlendMode::LinearDodge, kBaseA, kTopA, 128, {250, 240, 210}, 2},
        {pc, BlendMode::VividLight, kBaseA, kTopA, 128, {193, 188, 157}, 2},
        {pc, BlendMode::LinearLight, kBaseA, kTopA, 128, {171, 202, 192}, 2},
        {pc, BlendMode::HardMix, kBaseA, kTopA, 128, {245, 225, 165}, 2},
        {pc, BlendMode::Difference, kBaseA, kTopA, 128, {150, 60, 10}, 2},
        {patchy, BlendMode::ColorBurn, kBaseC, kTopC, 128, {13, 3, 153}, 2},
        {patchy, BlendMode::LinearBurn, kBaseC, kTopC, 128, {12, 2, 112}, 2},
        {patchy, BlendMode::ColorDodge, kBaseC, kTopC, 128, {66, 113, 235}, 2},
        {patchy, BlendMode::LinearDodge, kBaseC, kTopC, 128, {140, 130, 240}, 2},
        {patchy, BlendMode::Difference, kBaseC, kTopC, 128, {60, 70, 120}, 2},
        {patchy, BlendMode::VividLight, kBaseC, kTopC, 128, {56, 44, 178}, 2},
        {patchy, BlendMode::LinearLight, kBaseC, kTopC, 128, {112, 31, 171}, 2},
        {patchy, BlendMode::HardMix, kBaseC, kTopC, 128, {26, 6, 225}, 2},
        // Fill 25% (64/255) and 75% (191/255). Hard Mix at 75% divides by 1 - Fill, which magnifies Photoshop's 8-bit
        // rounding: PhotoCraft allows 3.5 levels there.
        {pc, BlendMode::ColorBurn, kBaseB, kTopB, 64, {51, 94, 159}, 2},
        {pc, BlendMode::LinearBurn, kBaseB, kTopB, 64, {49, 79, 124}, 2},
        {pc, BlendMode::ColorDodge, kBaseB, kTopB, 64, {76, 132, 186}, 2},
        {pc, BlendMode::LinearDodge, kBaseB, kTopB, 64, {113, 143, 188}, 2},
        {pc, BlendMode::VividLight, kBaseB, kTopB, 64, {71, 109, 162}, 2},
        {pc, BlendMode::LinearLight, kBaseB, kTopB, 64, {100, 100, 130}, 2},
        {pc, BlendMode::HardMix, kBaseB, kTopB, 64, {65, 105, 166}, 2},
        {pc, BlendMode::Difference, kBaseB, kTopB, 64, {7, 97, 172}, 2},
        {pc, BlendMode::ColorBurn, kBaseB, kTopB, 191, {30, 0, 33}, 2},
        {pc, BlendMode::LinearBurn, kBaseB, kTopB, 191, {26, 0, 11}, 2},
        {pc, BlendMode::ColorDodge, kBaseB, kTopB, 191, {156, 163, 197}, 2},
        {pc, BlendMode::LinearDodge, kBaseB, kTopB, 191, {217, 187, 202}, 2},
        {pc, BlendMode::VividLight, kBaseB, kTopB, 191, {117, 81, 80}, 2},
        {pc, BlendMode::LinearLight, kBaseB, kTopB, 191, {183, 63, 33}, 2},
        {pc, BlendMode::HardMix, kBaseB, kTopB, 191, {102, 0, 47}, 4},
        {pc, BlendMode::Difference, kBaseB, kTopB, 191, {97, 53, 158}, 2},
    };
    for (const Measured& m : cases) expect(m);
}

TEST_MAIN()
