// Clip Studio import: a .sut-shaped SQLite database made here (Node and Variant rows with the columns the
// reader uses), when the build has SQLite.
#include "check.h"
#include "compositor/brushimport.h"
#include <filesystem>
#include <string>

#ifdef COMPOSITOR_HAVE_SQLITE
#include <sqlite3.h>
#endif

using namespace compositor;
namespace fs = std::filesystem;

TEST_CASE(clip_studio_brush_settings_come_through_with_a_round_tip) {
#ifndef COMPOSITOR_HAVE_SQLITE
    std::fprintf(stderr, "  (skipped: built without SQLite)\n");
#else
    const fs::path path = fs::temp_directory_path() / "compositor-test.sut";
    fs::remove(path);
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    const char* script =
        "CREATE TABLE Node(_PW_ID INTEGER PRIMARY KEY AUTOINCREMENT, NodeName TEXT, NodeVariantID INTEGER);"
        "CREATE TABLE Variant(_PW_ID INTEGER PRIMARY KEY AUTOINCREMENT, VariantID INTEGER, BrushSize REAL, BrushSizeUnit INTEGER,"
        " BrushHardness INTEGER, BrushInterval REAL, BrushThickness INTEGER, BrushRotation REAL, BrushFlow INTEGER, BrushUseSpray INTEGER,"
        " BrushSpraySize REAL, BrushSpraySizeUnit INTEGER, BrushSprayDensity INTEGER, BrushUsePatternImage INTEGER, BrushUseWaterColor INTEGER,"
        " UseDualBrush INTEGER, TextureImage BLOB);"
        "INSERT INTO Node(NodeName, NodeVariantID) VALUES ('Soft Pencil', 11), ('Spray', 12);"
        "INSERT INTO Variant(VariantID, BrushSize, BrushSizeUnit, BrushHardness, BrushInterval, BrushThickness, BrushRotation, BrushFlow,"
        " BrushUseSpray, BrushSpraySize, BrushSpraySizeUnit, BrushSprayDensity, BrushUsePatternImage, BrushUseWaterColor, UseDualBrush, TextureImage)"
        " VALUES (11, 12.0, 0, 40, 5.0, 100, 0.0, 80, 0, 0, 0, 0, 0, 0, 0, NULL), (12, 40.0, 0, 100, 25.0, 50, 45.0, 100, 1, 20.0, 0, 6, 1, 0, 0, x'00');";
    REQUIRE(sqlite3_exec(db, script, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
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
    const TipPreset& spray = import->brushes[1];
    CHECK_EQ(spray.tip.roundness, 0.5);
    CHECK_EQ(spray.tip.angle, 45.0);
    CHECK_EQ(spray.tip.scatter, 0.5);   // 20 px of spray on a 40 px brush
    CHECK_EQ(spray.tip.count, 6);
    CHECK(import->notes.size() >= 2);   // the image tip and the texture, and the pressure settings
    fs::remove(path);
#endif
}

TEST_MAIN()
