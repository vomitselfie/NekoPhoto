// Photoshop .abr import: versions 2 and 6 written here byte by byte, and a real file when
// COMPOSITOR_ABR_SAMPLE names one.
#include "check.h"
#include "brush_import_fixtures.h"
#include "compositor/brushimport.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace compositor;
namespace fs = std::filesystem;

using namespace brushfixtures;

TEST_CASE(abr_version_2_reads_computed_raw_and_packed_brushes) {
    Out f;
    f.u16(2); f.u16(3);
    // A computed brush: 40 px, round, soft.
    Out computed;
    computed.u32(0); computed.u16(30); computed.u16(40); computed.u16(100); computed.u16(0); computed.u16(50);
    f.u16(1); f.u32(uint32_t(computed.b.size())); f.append(computed);
    // A sampled brush, raw.
    Out raw;
    raw.u32(0); raw.u16(20); raw.unicode("Block"); raw.u8(1);
    for (int i = 0; i < 4; i++) raw.u16(0);
    raw.u32(0); raw.u32(0); raw.u32(12); raw.u32(16); raw.u16(8); raw.u8(0);
    for (uint8_t v : tipPixels()) raw.u8(v);
    f.u16(2); f.u32(uint32_t(raw.b.size())); f.append(raw);
    // The same, PackBits.
    Out packed;
    packed.u32(0); packed.u16(50); packed.unicode("Packed"); packed.u8(1);
    for (int i = 0; i < 4; i++) packed.u16(0);
    packed.u32(0); packed.u32(0); packed.u32(12); packed.u32(16); packed.u16(8); packed.u8(1);
    packedTip(packed);
    f.u16(2); f.u32(uint32_t(packed.b.size())); f.append(packed);
    std::string error;
    auto import = importBrushFile(writeTemp("v2-test.abr", f.b), &error);
    REQUIRE(import.has_value());
    REQUIRE(import->brushes.size() == 3);
    CHECK_EQ(import->brushes[0].diameter, 40.0);
    CHECK_EQ(import->brushes[0].tip.spacing, 0.3);
    CHECK(import->brushes[1].name == "Block");
    CHECK(import->brushes[2].name == "Packed");
    for (int i : {1, 2}) {
        const auto& shape = *import->brushes[size_t(i)].tip.shape;
        CHECK_EQ(shape.width(), 16);
        CHECK_EQ(shape.height(), 12);
        CHECK_EQ(int(shape.at(3, 5)), 0);
        CHECK_EQ(int(shape.at(12, 5)), 255);
    }
    CHECK_EQ(import->brushes[2].tip.spacing, 0.5);
}

TEST_CASE(abr_version_6_ties_presets_to_their_samples_and_reads_dynamics) {
    Out f;
    f.b = abrVersion6File();
    std::string error;
    auto import = importBrushFile(writeTemp("v6-test.abr", f.b), &error);
    REQUIRE(import.has_value());
    REQUIRE(import->brushes.size() == 1);   // the one sample is used by the preset, so no extra tip
    const TipPreset& leaf = import->brushes[0];
    CHECK(leaf.name == "Leaf");
    CHECK_EQ(leaf.diameter, 64.0);
    CHECK_EQ(leaf.tip.angle, 30.0);
    CHECK_EQ(leaf.tip.roundness, 0.8);
    CHECK_EQ(leaf.tip.spacing, 0.4);
    // Pressure on size from 25% up, then 20% size jitter, as mappings in that order.
    REQUIRE(leaf.tip.dynamics.size() == 2);
    CHECK(leaf.tip.dynamics[0].input == DynamicsInput::Pressure && leaf.tip.dynamics[0].target == DynamicsTarget::Size);
    CHECK_EQ(leaf.tip.dynamics[0].offset, 0.25);
    CHECK_EQ(leaf.tip.dynamics[0].depth, 0.75);
    CHECK(leaf.tip.dynamics[1].input == DynamicsInput::Random && leaf.tip.dynamics[1].target == DynamicsTarget::Size);
    CHECK_EQ(leaf.tip.dynamics[1].depth, -0.2);
    CHECK_EQ(leaf.tip.scatter, 1.5);
    CHECK_EQ(leaf.tip.count, 3);
    REQUIRE(leaf.tip.shape != nullptr);
    CHECK_EQ(int(leaf.tip.shape->at(12, 5)), 255);
    // A damaged file is refused, not read past its end.
    Out cut;
    cut.b.assign(f.b.begin(), f.b.begin() + 200);
    CHECK(!importBrushFile(writeTemp("v6-cut.abr", cut.b), &error).has_value());
}

TEST_CASE(abr_sample_file_when_available) {
    const char* sample = std::getenv("COMPOSITOR_ABR_SAMPLE");
    if (!sample) { std::fprintf(stderr, "  (skipped: set COMPOSITOR_ABR_SAMPLE to a .abr file)\n"); return; }
    std::string error;
    auto import = importBrushFile(sample, &error);
    REQUIRE(import.has_value());
    std::fprintf(stderr, "  %zu brushes, %zu notes\n", import->brushes.size(), import->notes.size());
    CHECK(!import->brushes.empty());
    for (const auto& b : import->brushes) CHECK(b.tip.shape && b.tip.shape->width() > 0);
}

TEST_MAIN()
