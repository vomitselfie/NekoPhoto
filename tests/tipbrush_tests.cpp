// Tip brushes: stamping, spacing, pressure, jitter, the selection, and presets on disk.
#include "check.h"
#include "compositor/brushimport.h"
#include "compositor/png.h"
#include "compositor/tipbrush.h"
#include <cmath>
#include <cstring>
#include <filesystem>

using namespace compositor;
namespace fs = std::filesystem;

namespace {

Layer paper(int w, int h) {
    auto image = std::make_shared<Image>(w, h);
    image->fill(255, 255, 255, 255);
    return Layer(Asset::make(image, "Paper"), Point(0, 0));
}

/// A hard square tip with a notch cut out of its top-left quarter, so orientation shows.
std::shared_ptr<GrayImage> notchedSquare(int side) {
    auto tip = std::make_shared<GrayImage>(side, side, 255);
    for (int y = 0; y < side / 2; y++) for (int x = 0; x < side / 2; x++) tip->at(x, y) = 0;
    return tip;
}

BrushSettings black(double diameter) {
    BrushSettings s;
    s.diameter = diameter;
    s.red = s.green = s.blue = 0;
    return s;
}

struct Painting {
    BrushStroke grid;
    TipStroke tip;
    Painting(const Layer& layer, const BrushTip& t, double diameter, const GrayImage* selection = nullptr)
        : grid(layer, false, black(diameter), Size(layer.pixelWidth(), layer.pixelHeight()), selection), tip(grid, t, diameter) {}
    std::shared_ptr<const Image> finish() { grid.flush(); return grid.commit().asset->image; }
};

int dark(const Image& image, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) n += image.pixel(x, y)[0] < 128;
    return n;
}

} // namespace

TEST_CASE(a_single_dab_stamps_the_tip_at_its_size_and_orientation) {
    BrushTip tip;
    tip.shape = notchedSquare(64);
    Layer layer = paper(200, 200);
    Painting p(layer, tip, 40);
    REQUIRE(p.tip.isValid());
    p.tip.strokeTo({{100, 100}, 1});
    auto image = p.finish();
    // 40 px square centred on (100, 100), with its top-left quarter empty.
    CHECK(image->pixel(110, 110)[0] < 16);
    CHECK(image->pixel(90, 110)[0] < 16);
    CHECK(image->pixel(110, 90)[0] < 16);
    CHECK(image->pixel(90, 90)[0] > 240);
    CHECK(image->pixel(125, 100)[0] > 240);   // outside the 40 px square
    // Rotated 90 degrees counterclockwise, the notch moves to the bottom-left.
    tip.angle = 90;
    Layer again = paper(200, 200);
    Painting q(again, tip, 40);
    q.tip.strokeTo({{100, 100}, 1});
    auto turned = q.finish();
    CHECK(turned->pixel(90, 110)[0] > 240);
    CHECK(turned->pixel(90, 90)[0] < 16);
}

TEST_CASE(spacing_places_separate_dabs_and_tight_spacing_joins_them) {
    BrushTip tip;
    tip.shape = std::make_shared<GrayImage>(32, 32, 255);
    tip.spacing = 2;   // twice the size: 20 px dabs every 40 px
    Layer layer = paper(300, 60);
    Painting p(layer, tip, 20);
    p.tip.strokeTo({{20, 30}, 1});
    p.tip.strokeTo({{280, 30}, 1});
    auto image = p.finish();
    CHECK(image->pixel(20, 30)[0] < 16);
    CHECK(image->pixel(60, 30)[0] < 16);
    CHECK(image->pixel(40, 30)[0] > 240);   // the gap between two dabs
    tip.spacing = 0.2;
    Layer joined = paper(300, 60);
    Painting q(joined, tip, 20);
    q.tip.strokeTo({{20, 30}, 1});
    q.tip.strokeTo({{280, 30}, 1});
    auto line = q.finish();
    for (int x = 25; x < 275; x += 10) CHECK(line->pixel(x, 30)[0] < 16);
}

TEST_CASE(pressure_drives_size_and_the_selection_clips) {
    BrushTip tip;
    tip.shape = std::make_shared<GrayImage>(32, 32, 255);
    tip.spacing = 0.1;
    tip.pressureSize = 1;
    tip.minimumSize = 0.1;
    Layer layer = paper(300, 100);
    Painting p(layer, tip, 60);
    p.tip.strokeTo({{30, 50}, 0});
    p.tip.strokeTo({{270, 50}, 1});
    auto image = p.finish();
    // Thin where the pen was light, full width where it pressed.
    CHECK(dark(*image, 40, 25, 50, 75) < dark(*image, 250, 25, 260, 75) / 3);

    GrayImage selection(300, 100, 0);
    for (int y = 0; y < 100; y++) for (int x = 150; x < 300; x++) selection.at(x, y) = 255;
    tip.pressureSize = 0;
    Layer clipped = paper(300, 100);
    Painting q(clipped, tip, 30, &selection);
    q.tip.strokeTo({{30, 50}, 1});
    q.tip.strokeTo({{270, 50}, 1});
    auto half = q.finish();
    CHECK_EQ(dark(*half, 0, 0, 148, 100), 0);
    CHECK(dark(*half, 152, 40, 260, 60) > 1000);
}

TEST_CASE(jitter_is_repeatable_for_a_seed) {
    BrushTip tip;
    tip.shape = notchedSquare(32);
    tip.spacing = 0.5;
    tip.scatter = 1;
    tip.angleJitter = 180;
    tip.sizeJitter = 0.5;
    tip.count = 3;
    auto paint = [&] {
        Layer layer = paper(300, 120);
        Painting p(layer, tip, 24);
        for (int x = 20; x <= 280; x += 20) p.tip.strokeTo({{double(x), 60}, 1});
        return p.finish();
    };
    auto a = paint(), b = paint();
    bool same = true;
    for (int y = 0; y < 120; y++) same = same && std::memcmp(a->row(y), b->row(y), size_t(a->width()) * 4) == 0;
    CHECK(same);
    // Scatter reaches well away from the line.
    CHECK(dark(*a, 20, 0, 280, 40) + dark(*a, 20, 80, 280, 120) > 0);
}

TEST_CASE(tip_presets_round_trip_through_their_folder) {
    TipPreset preset;
    preset.name = "Chalk";
    preset.diameter = 48;
    preset.tip.shape = notchedSquare(20);
    preset.tip.grain = std::make_shared<GrayImage>(16, 16, 200);
    preset.tip.spacing = 0.12;
    preset.tip.angle = 30;
    preset.tip.followStroke = true;
    preset.tip.scatter = 0.4;
    preset.tip.count = 2;
    preset.tip.pressureSize = 0.8;
    preset.tip.randomFlipX = true;
    fs::path dir = fs::temp_directory_path() / "compositor-tip-preset-test";
    fs::remove_all(dir);
    std::string error;
    REQUIRE(saveTipPreset(dir.string(), preset, &error));
    auto back = loadTipPreset(dir.string(), &error);
    REQUIRE(back.has_value());
    CHECK(back->name == "Chalk");
    CHECK_EQ(back->diameter, 48.0);
    CHECK_EQ(back->tip.spacing, 0.12);
    CHECK_EQ(back->tip.angle, 30.0);
    CHECK(back->tip.followStroke);
    CHECK_EQ(back->tip.count, 2);
    CHECK(back->tip.randomFlipX);
    REQUIRE(back->tip.shape && back->tip.grain);
    CHECK_EQ(int(back->tip.shape->at(2, 2)), 0);
    CHECK_EQ(int(back->tip.shape->at(15, 15)), 255);
    CHECK_EQ(int(back->tip.grain->at(3, 3)), 200);
    auto preview = renderTipPreview(*back, 256, 64);
    int alpha = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 256; x++) alpha += preview->pixel(x, y)[3] > 0;
    CHECK(alpha > 500);
    CHECK(!loadTipPreset((dir / "missing").string(), &error).has_value());
    fs::remove_all(dir);
}

TEST_CASE(images_become_tips_by_alpha_or_by_darkness_and_import_into_folders) {
    // A shape on a clear background paints by its alpha, cropped to what paints.
    Image clear(40, 30);
    for (int y = 10; y < 20; y++) for (int x = 5; x < 25; x++) { uint8_t* p = clear.pixel(x, y); p[0] = p[1] = p[2] = 0; p[3] = 200; }
    auto byAlpha = tipFromImage(clear);
    REQUIRE(byAlpha != nullptr);
    CHECK_EQ(byAlpha->width(), 20);
    CHECK_EQ(byAlpha->height(), 10);
    CHECK_EQ(int(byAlpha->at(3, 3)), 200);
    // An opaque image paints by its darkness: black on white.
    Image opaque(30, 30);
    opaque.fill(255, 255, 255, 255);
    for (int y = 12; y < 18; y++) for (int x = 12; x < 18; x++) { uint8_t* p = opaque.pixel(x, y); p[0] = p[1] = p[2] = 0; }
    auto byDarkness = tipFromImage(opaque);
    REQUIRE(byDarkness != nullptr);
    CHECK_EQ(byDarkness->width(), 6);
    CHECK_EQ(int(byDarkness->at(2, 2)), 255);
    Image blank(10, 10);
    blank.fill(255, 255, 255, 255);
    CHECK(tipFromImage(blank) == nullptr);

    fs::path dir = fs::temp_directory_path() / "compositor-brush-import-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string error;
    REQUIRE(writePngImage((dir / "Dab.png").string(), clear, 0, &error));
    auto import = importBrushFile((dir / "Dab.png").string(), &error);
    REQUIRE(import.has_value());
    CHECK(import->brushes.size() == 1);
    CHECK(import->brushes[0].name == "Dab");
    std::vector<std::string> written;
    REQUIRE(saveBrushImport(*import, (dir / "library").string(), &written, &error));
    REQUIRE(saveBrushImport(*import, (dir / "library").string(), &written, &error));   // again: a second folder
    REQUIRE(written.size() == 2);
    CHECK(written[0] != written[1]);
    CHECK(fs::exists(fs::path(written[1]) / "preview.png"));
    CHECK(loadTipPreset(written[1], &error).has_value());
    CHECK(!importBrushFile((dir / "library").string() + "/nothing.abr", &error).has_value());
    fs::remove_all(dir);
}

TEST_MAIN()
