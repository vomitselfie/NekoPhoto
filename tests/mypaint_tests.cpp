// The MyPaint engine against the presets the app ships (src/app/brushes/mypaint).
#include "check.h"
#include "compositor/mypaint.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace compositor;
namespace fs = std::filesystem;

namespace {

std::string readPreset(const std::string& name) {
    std::ifstream in(fs::path(BRUSHES_DIR) / (name + ".myb"));
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

Layer whiteLayer(int w, int h) {
    auto image = std::make_shared<Image>(w, h);
    image->fill(255, 255, 255, 255);
    return Layer(Asset::make(image, "Paper"), Point(0, 0));
}

BrushSettings black(double diameter) {
    BrushSettings s;
    s.diameter = diameter;
    s.red = s.green = s.blue = 0;
    return s;
}

/// A MyPaint stroke the way the session makes one: the grid stroke owns preview and commit, the engine paints.
struct Painting {
    BrushStroke grid;
    MyPaintStroke engine;
    Painting(const Layer& layer, const std::string& json, const BrushSettings& settings, Size canvas, const GrayImage* selection = nullptr)
        : grid(layer, false, settings, canvas, selection), engine(grid, json, settings) {}
    bool isValid() const { return engine.isValid(); }
    const std::string& error() const { return engine.error(); }
    bool touched() const { return grid.touched(); }
    BrushStroke::Commit commit() { engine.finish(); return grid.commit(); }
};

/// A horizontal stroke across the middle of the layer, one event every 4 pixels at 60 events a second.
void drawLine(Painting& stroke, double y, double x0, double x1, double pressure = 0.7) {
    for (double x = x0; x <= x1; x += 4) {
        MyPaintInput in;
        in.document = {x, y};
        in.pressure = pressure;
        in.seconds = 1.0 / 60;
        stroke.engine.strokeTo(in);
    }
    stroke.engine.finish();
}

int darkest(const Image& image, int x0, int y0, int x1, int y1) {
    int d = 255;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) d = std::min(d, int(image.pixel(x, y)[0]));
    return d;
}

bool same(const Image& a, const Image& b) {
    if (a.width() != b.width() || a.height() != b.height()) return false;
    for (int y = 0; y < a.height(); y++) if (std::memcmp(a.row(y), b.row(y), size_t(a.width()) * 4)) return false;
    return true;
}

} // namespace

TEST_CASE(every_shipped_preset_reads_and_paints) {
    std::ifstream order(fs::path(BRUSHES_DIR) / "order.conf");
    REQUIRE(order.good());
    int presets = 0, painted = 0;
    for (std::string line; std::getline(order, line);) {
        if (line.empty() || line[0] == '#' || line.rfind("Group:", 0) == 0) continue;
        std::string json = readPreset(line);
        MyPaintPresetInfo info = myPaintPresetInfo(json);
        CHECK(info.valid);
        CHECK(info.radius > 0);
        presets++;
        if (!myPaintSupported()) continue;
        Layer layer = whiteLayer(160, 120);
        Painting stroke(layer, json, black(std::min(60.0, std::max(4.0, info.radius * 2))), Size(160, 120));
        if (!stroke.isValid()) { std::fprintf(stderr, "  %s: %s\n", line.c_str(), stroke.error().c_str()); CHECK(stroke.isValid()); continue; }
        drawLine(stroke, 60, 20, 140);
        painted += stroke.touched();
        auto commit = stroke.commit();
        CHECK(commit.asset.has_value());
    }
    CHECK_EQ(presets, 196);
    if (myPaintSupported()) {
        // Smudge and blend presets have nothing to move on plain white; nearly all others leave marks.
        std::fprintf(stderr, "  %d of %d presets marked the paper\n", painted, presets);
        CHECK(painted > 150);
    } else std::fprintf(stderr, "  (painting skipped: this build has no libmypaint)\n");
}

TEST_CASE(mypaint_pencil_draws_along_the_path_and_repeats_exactly) {
    if (!myPaintSupported()) { std::fprintf(stderr, "  (skipped: no libmypaint)\n"); return; }
    const std::string pencil = readPreset("classic/pencil");
    auto paint = [&] {
        Layer layer = whiteLayer(200, 100);
        Painting stroke(layer, pencil, black(6), Size(200, 100));
        CHECK(stroke.isValid());
        drawLine(stroke, 50, 20, 180);
        return stroke.commit();
    };
    auto first = paint(), second = paint();
    REQUIRE(first.asset && first.asset->image);
    const Image& image = *first.asset->image;
    CHECK(darkest(image, 40, 44, 160, 57) < 200);     // on the line (graphite, so a light grey)
    CHECK(darkest(image, 40, 5, 160, 30) == 255);     // well away from it
    CHECK(same(image, *second.asset->image));         // libmypaint's jitter is seeded, so a stroke repeats
}

TEST_CASE(mypaint_respects_the_selection_and_erases) {
    if (!myPaintSupported()) { std::fprintf(stderr, "  (skipped: no libmypaint)\n"); return; }
    const std::string ink = readPreset("classic/pen");
    // Only the left half is selected.
    GrayImage selection(200, 100, 0);
    for (int y = 0; y < 100; y++) for (int x = 0; x < 100; x++) selection.at(x, y) = 255;
    Layer layer = whiteLayer(200, 100);
    Painting stroke(layer, ink, black(10), Size(200, 100), &selection);
    REQUIRE(stroke.isValid());
    drawLine(stroke, 50, 20, 180);
    auto commit = stroke.commit();
    REQUIRE(commit.asset && commit.asset->image);
    CHECK(darkest(*commit.asset->image, 20, 40, 95, 60) < 128);
    CHECK(darkest(*commit.asset->image, 105, 0, 200, 100) == 255);

    BrushSettings eraser = black(20);
    eraser.erasing = true;
    Layer paper = whiteLayer(200, 100);
    Painting erase(paper, ink, eraser, Size(200, 100));
    REQUIRE(erase.isValid());
    drawLine(erase, 50, 20, 180);
    auto erased = erase.commit();
    REQUIRE(erased.asset && erased.asset->image);
    CHECK(erased.asset->image->pixel(100, 50)[3] < 64);
    CHECK_EQ(int(erased.asset->image->pixel(100, 5)[3]), 255);
}

TEST_MAIN()
