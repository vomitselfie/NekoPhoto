// Imported brushes behaving as they did in their own application (docs/brush-engine.md, "Brushes that feel the
// same"): the taper, the stroke's own inputs, and what each importer maps onto them, measured on painted strokes.
#include "check.h"
#include "brush_harness.h"
#include "brush_import_fixtures.h"
#include "compositor/brushimport.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace compositor;
using namespace brushharness;
namespace fs = std::filesystem;

namespace {

constexpr double pi = 3.14159265358979323846;

std::shared_ptr<GrayImage> disc(int side) {
    auto tip = std::make_shared<GrayImage>(side, side, 0);
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++)
            tip->at(x, y) = std::hypot(x + 0.5 - side / 2.0, y + 0.5 - side / 2.0) <= side / 2.0 ? 255 : 0;
    return tip;
}

/// A straight stroke left to right, `length` pixels, a sample every 2 pixels at 120 a second.
std::vector<BrushSample> line(double length, bool stylus, double pressure = 1) {
    std::vector<BrushSample> out;
    for (int i = 0; i * 2 <= length; i++) {
        BrushSample s;
        s.position = {20 + 2.0 * i, 100};
        s.time = i / 120.0;
        s.pressure = pressure;
        s.stylus = stylus;
        out.push_back(s);
    }
    return out;
}

struct Painted {
    std::vector<TipDab> dabs;
    size_t beforeFinish = 0;   // dabs stamped before the pen lifted
};

Painted paint(const BrushTip& tip, double diameter, const std::vector<BrushSample>& samples) {
    auto paper = std::make_shared<Image>(400, 200);
    Layer layer(Asset::make(paper, "Paper"), Point(0, 0));
    BrushSettings settings;
    settings.diameter = diameter;
    BrushStroke grid(layer, false, settings, Size(400, 200));
    TipStroke stroke(grid, tip, diameter, 3);
    Painted out;
    stroke.trace(&out.dabs);
    BrushSampleTrack track;
    for (const BrushSample& s : samples) stroke.strokeTo(track.add(s));
    out.beforeFinish = out.dabs.size();
    stroke.finish();
    return out;
}

/// The size of the dab nearest `distance` along the stroke.
double sizeAt(const Painted& p, double distance) {
    const TipDab* best = nullptr;
    for (const TipDab& d : p.dabs)
        if (!best || std::fabs(d.distance - distance) < std::fabs(best->distance - distance)) best = &d;
    return best ? best->size : 0;
}

} // namespace

// ---- The engine ----------------------------------------------------------------------------------------------------

TEST_CASE(taper_grows_in_over_its_start_and_out_over_its_end) {
    BrushTip tip;
    tip.shape = disc(32);
    tip.spacing = 0.1;
    tip.taper.start = 40;   // pixels: two diameters
    tip.taper.end = 60;
    tip.taper.size = 1;
    const double d = 20;
    const Painted p = paint(tip, d, line(300, true));
    REQUIRE(!p.dabs.empty());
    const double length = p.dabs.back().distance;
    // Linear from nothing: half the size halfway through the start, full past it, a third left a third from the end.
    CHECK(std::fabs(sizeAt(p, 0.5 * 2 * d) - 0.5 * d) < 0.06 * d);
    CHECK(std::fabs(sizeAt(p, 150) - d) < 1e-9);
    CHECK(std::fabs(sizeAt(p, length - 1.0 * d) - d / 3) < 0.06 * d);
    CHECK(p.dabs.back().size < 0.1 * d);
    // The end waits for the pen to lift: nothing within the end's length of the pen was stamped before it did.
    for (size_t i = 0; i < p.beforeFinish; i++) CHECK(p.dabs[i].distance <= length - 3 * d + 1e-6);
    // Smaller dabs sit closer: the spacing follows the tapered size.
    CHECK(p.dabs[1].distance - p.dabs[0].distance < 0.1 * d);
}

TEST_CASE(taper_opacity_fades_and_the_mouse_takes_its_own_taper) {
    BrushTip tip;
    tip.shape = disc(32);
    tip.spacing = 0.1;
    tip.taper.start = 40;
    tip.taper.size = 1;
    BrushTip::Taper touch;
    touch.end = 40;
    touch.size = 0;
    touch.opacity = 1;
    tip.mouseTaper = touch;
    const Painted pen = paint(tip, 20, line(300, true));
    const Painted mouse = paint(tip, 20, line(300, false));
    CHECK(pen.dabs.front().size < 2);                      // the pen's taper: size from nothing
    CHECK(std::fabs(mouse.dabs.front().size - 20) < 1e-9);  // the mouse's: no start taper
    CHECK(std::fabs(mouse.dabs.back().size - 20) < 1e-9);   // and its end fades instead of narrowing
    CHECK(mouse.dabs.back().opacity < 0.1);
    CHECK(std::fabs(sizeAt(mouse, 150) - 20) < 1e-9);
}

TEST_CASE(no_taper_paints_as_before) {
    // A tip without a taper stamps exactly the dabs it did: the same count, places and sizes as one with an empty taper
    // set explicitly, and every dab before the pen lifts.
    BrushTip a;
    a.shape = disc(32);
    a.spacing = 0.15;
    a.dynamics = {dynamicsMapping(DynamicsInput::Random, DynamicsTarget::Size, 1, -0.5), dynamicsMapping(DynamicsInput::Pressure, DynamicsTarget::Flow, 0.2, 0.8)};
    BrushTip b = a;
    b.taper = BrushTip::Taper{};
    const Painted pa = paint(a, 18, line(200, true, 0.6)), pb = paint(b, 18, line(200, true, 0.6));
    REQUIRE(pa.dabs.size() == pb.dabs.size());
    CHECK(pa.beforeFinish == pa.dabs.size());
    for (size_t i = 0; i < pa.dabs.size(); i++) {
        CHECK(pa.dabs[i].size == pb.dabs[i].size);
        CHECK(pa.dabs[i].at.x == pb.dabs[i].at.x);
    }
}

TEST_CASE(stroke_random_and_initial_direction_hold_for_the_whole_stroke) {
    BrushTip tip;
    tip.shape = disc(16);
    tip.spacing = 0.2;
    tip.dynamics = {dynamicsMapping(DynamicsInput::StrokeRandom, DynamicsTarget::Angle, 0, 180)};
    const Painted p = paint(tip, 16, line(120, true));
    REQUIRE(p.dabs.size() > 3);
    for (const TipDab& d : p.dabs) CHECK(d.rotation == p.dabs.front().rotation);
    // The initial direction: a stroke going down (+y in the document) turns the tip a quarter turn, and keeps it.
    BrushTip initial;
    initial.shape = disc(16);
    initial.spacing = 0.2;
    initial.dynamics = {dynamicsMapping(DynamicsInput::InitialDirection, DynamicsTarget::Angle, 0, -360)};
    std::vector<BrushSample> down;
    for (int i = 0; i < 30; i++) {
        BrushSample s;
        s.position = {100, 20 + 2.0 * i};
        if (i > 15) s.position = {100 + 2.0 * (i - 15), 50};   // then turning right
        s.time = i / 120.0;
        s.stylus = true;
        down.push_back(s);
    }
    const Painted q = paint(initial, 16, down);
    REQUIRE(!q.dabs.empty());
    for (const TipDab& d : q.dabs) CHECK(std::fabs(std::remainder(d.rotation - pi / 2, 2 * pi)) < 1e-6);
}

TEST_CASE(dynamics_names_round_trip_the_new_inputs) {
    for (DynamicsInput input : {DynamicsInput::StrokeRandom, DynamicsInput::InitialDirection, DynamicsInput::Wheel}) {
        auto back = dynamicsInputFromName(dynamicsInputName(input));
        REQUIRE(back.has_value());
        CHECK(*back == input);
    }
    BrushSample s;
    s.tangentialPressure = 0;
    CHECK(std::fabs(dynamicsInput(dynamicsMapping(DynamicsInput::Wheel, DynamicsTarget::Size, 0, 1), s, 10, 0) - 0.5) < 1e-12);
}

TEST_CASE(a_taper_survives_saving_and_loading) {
    TipPreset preset;
    preset.name = "Tapered";
    preset.tip.shape = disc(16);
    preset.tip.taper = {1.5, 2.5, 0.8, 0.4};
    preset.tip.mouseTaper = BrushTip::Taper{0.5, 0, 1, 0};
    const std::string folder = (fs::temp_directory_path() / "nekophoto-taper-preset").string();
    REQUIRE(saveTipPreset(folder, preset));
    auto back = loadTipPreset(folder);
    REQUIRE(back.has_value());
    CHECK(back->tip.taper.start == 1.5);
    CHECK(back->tip.taper.end == 2.5);
    CHECK(back->tip.taper.size == 0.8);
    CHECK(back->tip.taper.opacity == 0.4);
    REQUIRE(back->tip.mouseTaper.has_value());
    CHECK(back->tip.mouseTaper->start == 0.5);
}

// ---- Procreate -----------------------------------------------------------------------------------------------------

TEST_CASE(procreate_pressure_curve_shapes_the_size) {
    auto import = importBrushFile(std::string(PROCREATE_FIXTURES_DIR) + "/syn_15_pressure_curve.brush");
    REQUIRE(import.has_value());
    REQUIRE(import->brushes.size() == 1);
    const BrushTip& tip = import->brushes[0].tip;
    const DynamicsMapping* size = findMapping(tip.dynamics, DynamicsInput::Pressure, DynamicsTarget::Size);
    REQUIRE(size != nullptr);
    CHECK(size->curve.kind == DynamicsCurve::Kind::Smooth);
    REQUIRE(size->curve.points.size() == 3);   // sorted from Procreate's unordered list
    // Half the size at a quarter of the pressure, as the curve says (linear would give a quarter); never above the line's end.
    BrushSample s;
    s.pressure = 0.25;
    CHECK(std::fabs(applyDynamics(tip.dynamics, DynamicsTarget::Size, 100, s, 100) - 50) < 1e-6);
    s.pressure = 0.6;
    const double at60 = applyDynamics(tip.dynamics, DynamicsTarget::Size, 100, s, 100);
    CHECK(at60 > 60 && at60 < 100);
    // Painted: the dab at a quarter pressure is half the full one.
    BrushTip copy = tip;
    const Painted quarter = paint(copy, 20, line(100, true, 0.25)), full = paint(copy, 20, line(100, true, 1));
    CHECK(std::fabs(sizeAt(quarter, 50) / sizeAt(full, 50) - 0.5) < 1e-6);
}

TEST_CASE(procreate_taper_reads_the_pencil_and_touch_tapers) {
    auto import = importBrushFile(std::string(PROCREATE_FIXTURES_DIR) + "/syn_16_taper.brush");
    REQUIRE(import.has_value());
    const BrushTip& tip = import->brushes[0].tip;
    // A third of the slider is a third of 300 pixels (procreate.cpp's scaling::taperFullLength).
    CHECK(std::fabs(tip.taper.start - 100) < 1e-6);
    CHECK(std::fabs(tip.taper.end - 100) < 1e-6);
    CHECK(tip.taper.size == 1);
    CHECK(tip.taper.opacity == 0);
    REQUIRE(tip.mouseTaper.has_value());
    CHECK(std::fabs(tip.mouseTaper->start - 75) < 1e-6);
    CHECK(tip.mouseTaper->end == 0);
    CHECK(tip.mouseTaper->opacity == 1);
    // The baseline brush has none.
    auto baseline = importBrushFile(std::string(PROCREATE_FIXTURES_DIR) + "/syn_00_baseline.brush");
    REQUIRE(baseline.has_value());
    CHECK(baseline->brushes[0].tip.taper.isNone());
    CHECK(!baseline->brushes[0].tip.mouseTaper);
}

// ---- Photoshop -----------------------------------------------------------------------------------------------------

namespace {

/// A version 6 file of one computed brush whose angle has the control `control` (bVTy).
std::vector<uint8_t> abrAngleControl(int control) {
    using brushfixtures::Out;
    Out d;
    d.u32(16); d.unicode(""); d.key("null"); d.u32(1);
    d.key("Brsh"); d.chars("VlLs"); d.u32(1);
    d.chars("Objc"); d.unicode(""); d.key("brushPreset"); d.u32(4);
    d.key("Nm  "); d.chars("TEXT"); d.unicode("Angle");
    d.key("Brsh"); d.chars("Objc"); d.unicode(""); d.key("computedBrush"); d.u32(2);
    d.key("Dmtr"); d.chars("UntF"); d.chars("#Pxl"); d.f64(20);
    d.key("Hrdn"); d.chars("UntF"); d.chars("#Prc"); d.f64(100);
    d.key("useTipDynamics"); d.chars("bool"); d.u8(1);
    d.key("angleDynamics"); d.chars("Objc"); d.unicode(""); d.key("brVr"); d.u32(1);
    d.key("bVTy"); d.chars("long"); d.u32(uint32_t(control));
    Out f;
    f.u16(6); f.u16(2);
    f.chars("8BIM"); f.chars("desc"); f.u32(uint32_t(d.b.size())); f.append(d);
    return f.b;
}

} // namespace

TEST_CASE(abr_angle_controls_follow_photoshop) {
    auto read = [](int control) -> BrushTip {
        auto import = importBrushFile(brushfixtures::writeTemp("angle-" + std::to_string(control) + ".abr", abrAngleControl(control)));
        if (!import || import->brushes.size() != 1) return {};
        return import->brushes[0].tip;
    };
    // 7 is Direction: the tip follows the stroke.
    const BrushTip direction = read(7);
    CHECK(direction.followStroke);
    CHECK(direction.dynamics.empty());
    // 6 is Initial Direction: the tip keeps the way the stroke set off.
    const BrushTip initial = read(6);
    CHECK(!initial.followStroke);
    CHECK(findMapping(initial.dynamics, DynamicsInput::InitialDirection, DynamicsTarget::Angle) != nullptr);
    // 5 is Rotation (the barrel), 4 the stylus wheel.
    CHECK(findMapping(read(5).dynamics, DynamicsInput::Twist, DynamicsTarget::Angle) != nullptr);
    CHECK(findMapping(read(4).dynamics, DynamicsInput::Wheel, DynamicsTarget::Angle) != nullptr);
}

// ---- Clip Studio ---------------------------------------------------------------------------------------------------

#ifdef COMPOSITOR_HAVE_SQLITE
TEST_CASE(clip_studio_pressure_curve_and_starting_and_ending) {
    const fs::path path = fs::temp_directory_path() / "nekophoto-feel.sut";
    fs::remove(path);
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    const char* script =
        "CREATE TABLE Node(_PW_ID INTEGER PRIMARY KEY AUTOINCREMENT, NodeName TEXT, NodeVariantID INTEGER);"
        "CREATE TABLE Variant(_PW_ID INTEGER PRIMARY KEY AUTOINCREMENT, VariantID INTEGER, BrushSize REAL, BrushSizeUnit INTEGER,"
        " BrushHardness INTEGER, BrushInterval REAL, BrushSizeEffector BLOB, BrushUseIn INTEGER, BrushInLength REAL,"
        " BrushUseOut INTEGER, BrushOutLength REAL);"
        "INSERT INTO Node(NodeName, NodeVariantID) VALUES ('G-pen', 1);"
        "INSERT INTO Variant(VariantID, BrushSize, BrushSizeUnit, BrushHardness, BrushInterval, BrushUseIn, BrushInLength, BrushUseOut, BrushOutLength)"
        " VALUES (1, 20.0, 0, 100, 10.0, 1, 40.0, 1, 60.0);";
    REQUIRE(sqlite3_exec(db, script, nullptr, nullptr, nullptr) == SQLITE_OK);
    // Size on pressure from 20%, through a curve with half the response at a quarter of the pressure: the header (eleven
    // words, the ninth the curve block's length), then the block: 12, the point count, 16, and the points as doubles.
    std::vector<uint8_t> blob;
    const std::vector<std::pair<double, double>> points = {{0, 0}, {0.25, 0.5}, {1, 1}};
    for (uint32_t v : {44u, 0xF0u, 0x90u, 20u, 100u, 0u, 0u, 0u, uint32_t(12 + 16 * points.size()), 0u, 0u}) brushfixtures::be32(blob, v);
    for (uint32_t v : {12u, uint32_t(points.size()), 16u}) brushfixtures::be32(blob, v);
    for (auto [x, y] : points)
        for (double v : {x, y}) { uint64_t bits; std::memcpy(&bits, &v, 8); brushfixtures::be32(blob, uint32_t(bits >> 32)); brushfixtures::be32(blob, uint32_t(bits)); }
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(db, "UPDATE Variant SET BrushSizeEffector = ?1", -1, &statement, nullptr);
    sqlite3_bind_blob(statement, 1, blob.data(), int(blob.size()), SQLITE_TRANSIENT);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    sqlite3_close(db);

    auto import = importBrushFile(path.string());
    REQUIRE(import.has_value());
    const BrushTip& tip = import->brushes[0].tip;
    const DynamicsMapping* size = findMapping(tip.dynamics, DynamicsInput::Pressure, DynamicsTarget::Size);
    REQUIRE(size != nullptr);
    CHECK(std::fabs(size->offset - 0.2) < 1e-12);
    BrushSample s;
    s.pressure = 0.25;
    // 20% + 80% of the curve's half.
    CHECK(std::fabs(applyDynamics(tip.dynamics, DynamicsTarget::Size, 100, s, 100) - 60) < 1e-6);
    // Starting and ending: 40 and 60 pixels, tapering the size.
    CHECK(std::fabs(tip.taper.start - 40) < 1e-12);
    CHECK(std::fabs(tip.taper.end - 60) < 1e-12);
    CHECK(tip.taper.size == 1);
}
#endif

TEST_MAIN()
