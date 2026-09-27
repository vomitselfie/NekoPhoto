// Clip Studio import: a .sut-shaped SQLite database made here (Node and Variant rows with the columns the
// reader uses, and MaterialFile rows holding tars of C2F layer files built from SQLite databases made here
// too), when the build has SQLite; and a real file when COMPOSITOR_SUT_SAMPLE names one.
#include "check.h"
#include "brush_import_fixtures.h"
#include "compositor/brushimport.h"
#include "compositor/png.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include <zlib.h>

#ifdef COMPOSITOR_HAVE_SQLITE
#include <sqlite3.h>
#endif

using namespace compositor;
namespace fs = std::filesystem;

#ifdef COMPOSITOR_HAVE_SQLITE

using namespace brushfixtures;

TEST_CASE(clip_studio_brushes_come_through_with_their_tips_and_textures) {
    const fs::path path = fs::temp_directory_path() / "compositor-test.sut";
    REQUIRE(writeClipStudioFile(path));

    std::string error;
    auto import = importBrushFile(path.string(), &error);
    REQUIRE(import.has_value());
    REQUIRE(import->brushes.size() == 2);
    const TipPreset& pencil = import->brushes[0];
    CHECK(pencil.name == "Soft Pencil");
    CHECK_EQ(pencil.diameter, 12.0);
    CHECK_EQ(pencil.tip.spacing, 0.05);
    CHECK_EQ(pencil.tip.flow, 0.8);
    CHECK_EQ(pencil.tip.scatter, 0.0);
    // The texture: its alpha stretched so 200 is 255, then reversed; at 50% scale and 40% density.
    REQUIRE(pencil.tip.grain != nullptr);
    CHECK_EQ(pencil.tip.grain->width(), 40);
    CHECK_EQ(pencil.tip.grain->height(), 30);
    CHECK_EQ(int(pencil.tip.grain->at(3, 2)), 255 - (17 * 255 + 100) / 200);
    CHECK_EQ(pencil.tip.grainScale, 2.0);
    CHECK_EQ(pencil.tip.grainDepth, 0.4);
    const TipPreset& spray = import->brushes[1];
    CHECK_EQ(spray.tip.roundness, 0.5);
    CHECK_EQ(spray.tip.angle, 45.0);
    CHECK_EQ(spray.tip.scatter, 0.5);   // 20 px of spray on a 40 px brush
    CHECK_EQ(spray.tip.count, 6);
    // The tip: tile 1's first 44 columns (the image is 300 wide), stretched so 100 is 255, cropped to what paints.
    REQUIRE(spray.tip.shape != nullptr);
    CHECK_EQ(spray.tip.shape->width(), 44);
    CHECK_EQ(spray.tip.shape->height(), 200);
    CHECK_EQ(int(spray.tip.shape->at(10, 20)), (tileValue(10, 20) * 255 + 50) / 100);
    CHECK(spray.tip.grain == nullptr);
    REQUIRE(spray.tip.dynamics.size() == 1);   // size on pressure down to 10%; opacity left alone
    CHECK(spray.tip.dynamics[0].input == DynamicsInput::Pressure && spray.tip.dynamics[0].target == DynamicsTarget::Size);
    CHECK_EQ(spray.tip.dynamics[0].offset, 0.1);
    CHECK(pencil.tip.dynamics.empty());
    fs::remove(path);
}

#else

TEST_CASE(clip_studio_needs_sqlite) { std::fprintf(stderr, "  (skipped: built without SQLite)\n"); }

#endif

TEST_CASE(clip_studio_sample_file_when_available) {
    const char* sample = std::getenv("COMPOSITOR_SUT_SAMPLE");
    if (!sample) { std::fprintf(stderr, "  (skipped: set COMPOSITOR_SUT_SAMPLE to a .sut file)\n"); return; }
    std::string error;
    auto import = importBrushFile(sample, &error);
    REQUIRE(import.has_value());
    for (const TipPreset& brush : import->brushes) {
        std::fprintf(stderr, "  %s: %.0f px, tip %dx%d, grain %dx%d\n", brush.name.c_str(), brush.diameter, brush.tip.shape->width(), brush.tip.shape->height(),
            brush.tip.grain ? brush.tip.grain->width() : 0, brush.tip.grain ? brush.tip.grain->height() : 0);
        for (const DynamicsMapping& m : brush.tip.dynamics)
            std::fprintf(stderr, "    %s -> %s: %.2f + %.2f\n", dynamicsInputName(m.input), dynamicsTargetName(m.target), m.offset, m.depth);
    }
    for (const std::string& note : import->notes) std::fprintf(stderr, "  note: %s\n", note.c_str());
    CHECK(!import->brushes.empty());
}

TEST_MAIN()
