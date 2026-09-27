// Procreate import: a .brushset and a .brush written here byte by byte (a stored ZIP, an XML brushset.plist,
// an NSKeyedArchiver binary plist, PNG shapes), and a real file when COMPOSITOR_PROCREATE_SAMPLE names one.
#include "check.h"
#include "brush_import_fixtures.h"
#include "compositor/brushimport.h"
#include "compositor/png.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <variant>
#include <vector>

using namespace compositor;
namespace fs = std::filesystem;

using namespace brushfixtures;

TEST_CASE(procreate_brushset_reads_its_brushes_in_order_with_their_settings) {
    const auto file = procreateBrushsetFile();
    std::string error;
    auto import = importBrushFile(writeTemp("test.brushset", file), &error);
    REQUIRE(import.has_value());
    CHECK(import->set == "Test & Set");
    REQUIRE(import->brushes.size() == 2);
    CHECK(import->brushes[0].name == "Bars");   // the list's order, not the archive's
    const TipPreset& ink = import->brushes[1];
    CHECK(ink.name == "Soft Ink");
    CHECK_EQ(ink.tip.spacing, 0.08);
    CHECK_EQ(ink.tip.scatter, 0.5);
    CHECK(ink.tip.followStroke);
    const DynamicsMapping* angle = findMapping(ink.tip.dynamics, DynamicsInput::Random, DynamicsTarget::Angle);
    REQUIRE(angle != nullptr);
    CHECK_EQ(angle->depth, 90.0);
    CHECK_EQ(ink.tip.count, 4);
    const DynamicsMapping* size = findMapping(ink.tip.dynamics, DynamicsInput::Pressure, DynamicsTarget::Size);
    REQUIRE(size != nullptr);
    CHECK_EQ(size->offset, 0.25);
    CHECK_EQ(ink.tip.flow, 0.8);
    CHECK_EQ(ink.diameter, 20.0);
    // White paints; the near-black background is taken off.
    REQUIRE(ink.tip.shape != nullptr);
    CHECK_EQ(int(ink.tip.shape->at(5, 5)), 0);
    CHECK_EQ(int(ink.tip.shape->at(24, 5)), 255);
    CHECK(!import->notes.empty());   // Bars' grain is Procreate's own
}

TEST_CASE(a_single_procreate_brush_and_damaged_archives) {
    const auto archive = keyedArchive({{"name", std::string("Solo")}, {"plotSpacing", 0.1}});
    std::string error;
    auto single = importBrushFile(writeTemp("solo.brush", zip({{"Brush.archive", archive}, {"Shape.png", shapePng()}})), &error);
    REQUIRE(single.has_value());
    REQUIRE(single->brushes.size() == 1);
    CHECK(single->brushes[0].name == "Solo");
    // A damaged plist, or a ZIP whose file does not match its checksum, imports nothing.
    std::vector<uint8_t> broken = archive;
    broken.resize(broken.size() - 10);
    CHECK(!importBrushFile(writeTemp("broken.brush", zip({{"Brush.archive", broken}})), &error).has_value());
    auto file = zip({{"Brush.archive", archive}});
    file[30 + 13 + 20] ^= 0xFF;   // inside the stored archive, past its 30-byte header and name
    CHECK(!importBrushFile(writeTemp("corrupt.brush", file), &error).has_value());
}

TEST_CASE(procreate_sample_file_when_available) {
    const char* sample = std::getenv("COMPOSITOR_PROCREATE_SAMPLE");
    if (!sample) { std::fprintf(stderr, "  (skipped: set COMPOSITOR_PROCREATE_SAMPLE to a .brushset file)\n"); return; }
    std::string error;
    auto import = importBrushFile(sample, &error);
    REQUIRE(import.has_value());
    std::fprintf(stderr, "  %s: %zu brushes, %zu notes\n", import->set.c_str(), import->brushes.size(), import->notes.size());
    CHECK(!import->brushes.empty());
}

TEST_MAIN()
