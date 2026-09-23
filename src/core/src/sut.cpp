// Clip Studio Paint brushes (.sut): an SQLite database. Each tool is a row of Node (its name and the Variant
// it uses), and each Variant row holds the settings in named columns. The tip images live in MaterialFile as
// archives whose images are in a format Clip Studio has not documented (the embedded thumbnails are a generic
// placeholder), so the import keeps the settings and stands a round tip in for the image. Column meanings are
// read from the column names and checked against a real file; the pressure and rotation "effectors" are
// coded blobs and numbers with no public description, and are left out rather than guessed.
#include "brushformats.h"
#include <algorithm>
#include <cmath>

#ifdef COMPOSITOR_HAVE_SQLITE
#include <sqlite3.h>
#endif

namespace compositor {

#ifdef COMPOSITOR_HAVE_SQLITE

std::optional<BrushImport> readClipStudio(const std::string& path, const std::string& name, std::string* error) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (error) *error = db ? sqlite3_errmsg(db) : "cannot open the database";
        sqlite3_close(db);
        return std::nullopt;
    }
    BrushImport import;
    import.set = name;
    const char* query =
        "SELECT n.NodeName, v.BrushSize, v.BrushSizeUnit, v.BrushHardness, v.BrushInterval, v.BrushThickness, v.BrushRotation,"
        " v.BrushFlow, v.BrushUseSpray, v.BrushSpraySize, v.BrushSpraySizeUnit, v.BrushSprayDensity, v.BrushUsePatternImage,"
        " v.BrushUseWaterColor, v.UseDualBrush, v.TextureImage IS NOT NULL"
        " FROM Node n JOIN Variant v ON v.VariantID = n.NodeVariantID ORDER BY n._PW_ID";
    sqlite3_stmt* rows = nullptr;
    if (sqlite3_prepare_v2(db, query, -1, &rows, nullptr) != SQLITE_OK) {
        if (error) *error = std::string("not a Clip Studio brush: ") + sqlite3_errmsg(db);
        sqlite3_close(db);
        return std::nullopt;
    }
    int images = 0, watercolour = 0, dual = 0, textured = 0;
    while (sqlite3_step(rows) == SQLITE_ROW && import.brushes.size() < 1000) {
        auto number = [&](int column, double fallback) { return sqlite3_column_type(rows, column) == SQLITE_NULL ? fallback : sqlite3_column_double(rows, column); };
        TipPreset preset;
        if (const unsigned char* title = sqlite3_column_text(rows, 0)) preset.name = reinterpret_cast<const char*>(title);
        if (preset.name.empty()) preset.name = name + " " + std::to_string(import.brushes.size() + 1);
        // Sizes in pixels when their unit is 0; the other units are relative and taken as pixels too.
        const double size = std::clamp(number(1, 20), 1.0, 2000.0);
        preset.diameter = size;
        BrushTip& tip = preset.tip;
        tip.shape = roundTipImage(std::min(size, 256.0), std::clamp(number(3, 100), 0.0, 100.0) / 100, 1);
        tip.spacing = std::clamp(number(4, 10), 1.0, 1000.0) / 100;
        tip.roundness = std::clamp(number(5, 100), 1.0, 100.0) / 100;
        tip.angle = number(6, 0);
        tip.flow = std::clamp(number(7, 100), 0.0, 100.0) / 100;
        if (number(8, 0) != 0) {
            tip.scatter = std::clamp(number(9, 0), 0.0, 20000.0) / size;
            tip.count = int(std::clamp(std::lround(number(11, 1)), 1L, 16L));
            tip.scatterBothAxes = true;   // a spray scatters all round the stroke
        }
        images += number(12, 0) != 0;
        watercolour += number(13, 0) != 0;
        dual += number(14, 0) != 0;
        textured += number(15, 0) != 0;
        if (tip.normalize()) import.brushes.push_back(std::move(preset));
    }
    sqlite3_finalize(rows);
    sqlite3_close(db);
    auto note = [&](int count, const char* what) { if (count) import.notes.push_back(what + std::string(": ") + std::to_string(count)); };
    note(images, "brushes whose tip image is in Clip Studio's undocumented material format (a round tip stands in)");
    note(watercolour, "brushes with watercolour or colour mixing, which is left out");
    note(dual, "brushes with a dual brush, which is left out");
    note(textured, "brushes with a paper texture, which is left out");
    if (!import.brushes.empty()) import.notes.push_back("pressure and rotation settings are Clip Studio's own coding and are left out");
    if (import.brushes.empty()) { if (error) *error = "the file holds no Clip Studio brush this reader can use"; return std::nullopt; }
    return import;
}

#else

std::optional<BrushImport> readClipStudio(const std::string&, const std::string&, std::string* error) {
    if (error) *error = "this build reads no Clip Studio brushes (it was made without SQLite)";
    return std::nullopt;
}

#endif

} // namespace compositor
