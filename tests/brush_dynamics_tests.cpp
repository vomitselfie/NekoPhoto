// Brush dynamics (brushdynamics.h): curves, the combination rule, circular targets, and presets saved before mappings.
#include "check.h"
#include "compositor/brushdynamics.h"
#include "compositor/png.h"
#include "compositor/tipbrush.h"
#include "compositor/warpstroke.h"
#include <memory>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace compositor;
namespace fs = std::filesystem;

namespace {

constexpr double pi = 3.14159265358979323846;

BrushSample pressed(double pressure) {
    BrushSample s;
    s.pressure = pressure;
    s.stylus = true;
    return s;
}

} // namespace

TEST_CASE(curves_are_identity_linear_or_monotone_without_overshoot) {
    DynamicsCurve identity;
    for (double x : {0.0, 0.3, 0.77, 1.0}) CHECK_EQ(identity.evaluate(x), x);
    DynamicsCurve linear{DynamicsCurve::Kind::Linear, {{0, 0.2}, {0.5, 0.4}, {1, 1}}};
    CHECK_NEAR(linear.evaluate(0.25), 0.3, 1e-12);
    CHECK_NEAR(linear.evaluate(0.75), 0.7, 1e-12);
    CHECK_EQ(linear.evaluate(-1), 0.2);
    CHECK_EQ(linear.evaluate(2), 1.0);
    // A plateau and a steep rise: a free cubic spline would overshoot around both; PCHIP stays in range and monotone.
    DynamicsCurve smooth{DynamicsCurve::Kind::Smooth, {{0, 0}, {0.2, 0.7}, {0.5, 0.7}, {0.6, 1}, {1, 1}}};
    double previous = -1;
    bool monotone = true, inRange = true;
    for (int i = 0; i <= 1000; i++) {
        const double y = smooth.evaluate(i / 1000.0);
        monotone = monotone && y >= previous - 1e-12;
        inRange = inRange && y >= 0 && y <= 1;
        previous = y;
    }
    CHECK(monotone);
    CHECK(inRange);
    for (const auto& [x, y] : smooth.points) CHECK_NEAR(smooth.evaluate(x), y, 1e-12);
    for (int i = 0; i <= 100; i++) CHECK_NEAR(smooth.evaluate(0.2 + 0.3 * i / 100), 0.7, 1e-12);   // the plateau stays flat
    // A falling curve falls without dipping below its end.
    DynamicsCurve falling{DynamicsCurve::Kind::Smooth, {{0, 1}, {0.1, 0.2}, {1, 0}}};
    for (int i = 0; i <= 100; i++) CHECK(falling.evaluate(i / 100.0) >= 0);
    // Normalizing sorts, clamps and drops what is not a number.
    DynamicsCurve messy{DynamicsCurve::Kind::Linear, {{1.5, 2}, {0.5, 0.5}, {NAN, 0}, {-1, -1}}};
    messy.normalize();
    REQUIRE(messy.points.size() == 3);
    CHECK(messy.points.front() == std::make_pair(0.0, 0.0));
    CHECK(messy.points.back() == std::make_pair(1.0, 1.0));
}

TEST_CASE(scalar_targets_multiply_their_mappings_and_clamp) {
    BrushDynamics d = {dynamicsMapping(DynamicsInput::Pressure, DynamicsTarget::Size, 0.2, 0.8),
                       dynamicsMapping(DynamicsInput::Random, DynamicsTarget::Size, 1, -0.5),
                       dynamicsMapping(DynamicsInput::Pressure, DynamicsTarget::Flow, 0.5, 1.5)};
    // 40 x (0.2 + 0.8 x 0.5) x (1 - 0.5 x 1)
    CHECK_NEAR(applyDynamics(d, DynamicsTarget::Size, 40, pressed(0.5), 40, 1.0), 40 * 0.6 * 0.5, 1e-12);
    // Without the random part: what the spacing is measured in.
    CHECK_NEAR(applyDynamics(d, DynamicsTarget::Size, 40, pressed(0.5), 40, 1.0, false), 40 * 0.6, 1e-12);
    // Flow may not pass 1, whatever a mapping says.
    CHECK_EQ(applyDynamics(d, DynamicsTarget::Flow, 0.9, pressed(1), 40), 1.0);
    // A target without mappings keeps its base.
    CHECK_EQ(applyDynamics(d, DynamicsTarget::Roundness, 0.7, pressed(1), 40), 0.7);
    // A curve shapes the input before the range: pressure squared.
    DynamicsMapping squared = dynamicsMapping(DynamicsInput::Pressure, DynamicsTarget::Opacity, 0, 1);
    squared.curve = {DynamicsCurve::Kind::Smooth, {{0, 0}, {0.5, 0.25}, {1, 1}}};
    CHECK_NEAR(applyDynamics({squared}, DynamicsTarget::Opacity, 1, pressed(0.5), 40), 0.25, 1e-12);
}

TEST_CASE(angles_add_and_stay_continuous_across_a_turn) {
    // Jitter spreads both ways: a centred draw of -1 turns the full amount one way, +1 the other.
    BrushDynamics jitter = {dynamicsMapping(DynamicsInput::Random, DynamicsTarget::Angle, 0, 20)};
    CHECK_NEAR(applyDynamics(jitter, DynamicsTarget::Angle, 350, pressed(1), 40, -1), 330, 1e-12);
    CHECK_NEAR(applyDynamics(jitter, DynamicsTarget::Angle, 350, pressed(1), 40, 1), 370, 1e-12);
    // Following the tilt's direction: just either side of the wrap the tip points the same way.
    BrushDynamics follow = {dynamicsMapping(DynamicsInput::TiltDirection, DynamicsTarget::Angle, 0, 360)};
    BrushSample below = pressed(1), above = pressed(1);
    below.tiltAzimuth = 2 * pi - 1e-6;
    above.tiltAzimuth = 2 * pi + 1e-6;   // unwrapped past a turn
    const double a = applyDynamics(follow, DynamicsTarget::Angle, 0, below, 40) * pi / 180;
    const double b = applyDynamics(follow, DynamicsTarget::Angle, 0, above, 40) * pi / 180;
    CHECK_NEAR(std::cos(a), std::cos(b), 1e-5);
    CHECK_NEAR(std::sin(a), std::sin(b), 1e-5);
    // Twist the same way.
    BrushDynamics twist = {dynamicsMapping(DynamicsInput::Twist, DynamicsTarget::Angle, 0, 360)};
    BrushSample turned = pressed(1);
    turned.twistAngle = -pi / 2;   // -90 degrees reads as three quarters of a turn
    CHECK_NEAR(applyDynamics(twist, DynamicsTarget::Angle, 0, turned, 40), 270, 1e-9);
}

TEST_CASE(inputs_read_speed_tilt_and_progress) {
    BrushSample s = pressed(1);
    s.speed = 1000;
    s.tiltMagnitude = 0.5;
    s.distance = 100;
    CHECK_NEAR(dynamicsInput(dynamicsMapping(DynamicsInput::Speed, DynamicsTarget::Size, 0, 1), s, 20, 0), 0.5, 1e-12);
    CHECK_NEAR(dynamicsInput(dynamicsMapping(DynamicsInput::Speed, DynamicsTarget::Size, 0, 1, 4000), s, 20, 0), 0.25, 1e-12);
    // The screen's speed is its own field: the document's does not stand in for it.
    CHECK_NEAR(dynamicsInput(dynamicsMapping(DynamicsInput::ScreenSpeed, DynamicsTarget::Size, 0, 1), s, 20, 0), 0.0, 1e-12);
    s.screenSpeed = 500;
    CHECK_NEAR(dynamicsInput(dynamicsMapping(DynamicsInput::ScreenSpeed, DynamicsTarget::Size, 0, 1), s, 20, 0), 0.25, 1e-12);
    CHECK_NEAR(dynamicsInput(dynamicsMapping(DynamicsInput::ScreenSpeed, DynamicsTarget::Size, 0, 1, 1000), s, 20, 0), 0.5, 1e-12);
    CHECK_NEAR(dynamicsInput(dynamicsMapping(DynamicsInput::Tilt, DynamicsTarget::Size, 0, 1), s, 20, 0), 0.5, 1e-12);
    // Progress over 10 diameters of 20 pixels: 100 pixels is halfway; without a scale, the whole stroke's progress.
    CHECK_NEAR(dynamicsInput(dynamicsMapping(DynamicsInput::StrokeProgress, DynamicsTarget::Size, 0, 1, 10), s, 20, 0), 0.5, 1e-12);
    s.progress = 0.8;
    CHECK_NEAR(dynamicsInput(dynamicsMapping(DynamicsInput::StrokeProgress, DynamicsTarget::Size, 0, 1), s, 20, 0), 0.8, 1e-12);
    s.progress = -1;   // painting live: 25 diameters
    CHECK_NEAR(dynamicsInput(dynamicsMapping(DynamicsInput::StrokeProgress, DynamicsTarget::Size, 0, 1), s, 20, 0), 0.2, 1e-12);
}

TEST_CASE(roll_follows_the_barrel_or_the_stroke_without_a_jump_at_359_to_0) {
    // The barrel from 340 through 359, 0 and 1 to 20 degrees, a report every quarter degree: the tip's angle (mod 360)
    // never moves more than the barrel did between two reports.
    const BrushDynamics roll = {dynamicsMapping(DynamicsInput::Roll, DynamicsTarget::Angle, 0, -360)};
    BrushSampleTrack track;
    double previous = 0, largest = 0;
    bool first = true, passed359 = false, passed0 = false, passed1 = false;
    for (int i = 0; i <= 160; i++) {
        BrushSample s;
        s.position = {double(i), 0};
        s.time = i / 120.0;
        s.stylus = true;
        s.twist = std::fmod(340 + i * 0.25, 360.0);
        s.twistReported = true;
        passed359 |= s.twist >= 359 && s.twist < 359.25;
        passed0 |= s.twist == 0;
        passed1 |= s.twist == 1;
        const double angle = applyDynamics(roll, DynamicsTarget::Angle, 0, track.add(s), 20);
        if (!first) largest = std::max(largest, std::fabs(std::remainder(angle - previous, 360.0)));
        previous = angle;
        first = false;
    }
    CHECK(passed359 && passed0 && passed1);
    CHECK(largest <= 0.25 + 1e-9);
    // On a pen that reports no twist it follows the stroke: heading down the page (90 degrees in the y-down frame)
    // turns the tip a quarter turn clockwise, an angle of -90.
    BrushSampleTrack plain;
    BrushSample a, b;
    a.stylus = b.stylus = true;
    b.position = {0, 10};
    b.time = 0.01;
    plain.add(a);
    CHECK_NEAR(std::remainder(applyDynamics(roll, DynamicsTarget::Angle, 0, plain.add(b), 20), 360.0), -90.0, 1e-9);
    // With a twist reported, the twist wins over the direction.
    b.twist = 30;
    b.twistReported = true;
    BrushSampleTrack pen;
    pen.add(a);
    CHECK_NEAR(std::remainder(applyDynamics(roll, DynamicsTarget::Angle, 0, pen.add(b), 20), 360.0), -30.0, 1e-9);
    CHECK(dynamicsInputFromName("roll") == DynamicsInput::Roll);
    CHECK(dynamicsInputFromName("screenSpeed") == DynamicsInput::ScreenSpeed);
    CHECK(std::string(dynamicsInputName(DynamicsInput::ScreenSpeed)) == "screenSpeed");
    CHECK(std::string(dynamicsInputName(DynamicsInput::Speed)) == "speed");
    CHECK(std::string(dynamicsInputName(DynamicsInput::Roll)) == "roll");
}

TEST_CASE(a_tilted_pencil_flattens_and_turns_the_way_it_leans) {
    BrushDynamics d = {dynamicsMapping(DynamicsInput::Pressure, DynamicsTarget::Size, 0.2, 0.8)};
    CHECK(!tiltShapeOf(d).has_value());
    for (const DynamicsMapping& m : tiltShapesTip(0.3)) d.push_back(m);
    REQUIRE(tiltShapeOf(d).has_value());
    CHECK_NEAR(*tiltShapeOf(d), 0.3, 1e-12);
    // Leaning fully towards the bottom right: roundness at its flattest, the tip's x axis along the lean.
    BrushSampleTrack track;
    BrushSample s;
    s.stylus = true;
    s.tiltX = 60;
    s.tiltY = 60;
    const BrushSample leaning = track.add(s);
    CHECK_NEAR(applyDynamics(d, DynamicsTarget::Roundness, 1, leaning, 20), 0.3, 1e-12);
    CHECK_NEAR(std::remainder(applyDynamics(d, DynamicsTarget::Angle, 0, leaning, 20), 360.0), -45.0, 1e-9);
    // Upright, the tip keeps its shape.
    BrushSampleTrack still;
    CHECK_NEAR(applyDynamics(d, DynamicsTarget::Roundness, 1, still.add(BrushSample{}), 20), 1.0, 1e-12);
    removeTiltShape(d);
    CHECK_EQ(d.size(), size_t(1));
    CHECK(!tiltShapeOf(d).has_value());
}

TEST_CASE(grain_modes_fix_the_grain_to_the_canvas_the_stroke_or_the_dab) {
    // Vertical stripes 8 pixels apart on a square tip, painted along two parallel lines that start 3 pixels apart.
    auto shape = std::make_shared<GrayImage>(32, 32, 255);
    auto grain = std::make_shared<GrayImage>(8, 8, 0);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 4; x++) grain->at(x, y) = 255;
    auto paint = [&](BrushTip::GrainMode mode, double startX) {
        BrushTip tip;
        tip.shape = shape;
        tip.grain = grain;
        tip.spacing = 1;
        tip.grainMode = mode;
        auto image = std::make_shared<Image>(200, 40);
        Layer layer(Asset::make(image, "Paper"), Point(0, 0));
        BrushSettings settings;
        settings.diameter = 16;
        BrushStroke grid(layer, false, settings, Size(200, 40));
        TipStroke stroke(grid, tip, 16, 3);
        BrushSampleTrack track;
        for (int i = 0; i <= 1; i++) {
            BrushSample s;
            s.stylus = true;
            s.pressure = 1;
            s.position = {startX + 96.0 * i, 20};
            s.time = i / 60.0;
            stroke.strokeTo(track.add(s));
        }
        grid.flush();
        // The alpha of the first dab, relative to its centre.
        std::vector<int> row;
        for (int dx = -6; dx <= 6; dx++) row.push_back(grid.previewImage()->pixel(int(startX) + dx, 20)[3]);
        return row;
    };
    using M = BrushTip::GrainMode;
    // On the canvas the grain stays put, so the dab's pattern moves when the stroke starts elsewhere. Stroke and Dab
    // grain are not applied: the grain is one static mask anchored to the document (docs/legal-boundaries.md,
    // "Brushes"), so those modes paint exactly as Canvas does.
    CHECK(paint(M::Canvas, 40) != paint(M::Canvas, 43));
    CHECK(paint(M::Stroke, 40) == paint(M::Canvas, 40));
    CHECK(paint(M::Stroke, 43) == paint(M::Canvas, 43));
    CHECK(paint(M::Dab, 43) == paint(M::Canvas, 43));
    // A preset keeps its mode (it is stored, and only painting ignores it).
    BrushTip tip;
    tip.grainMode = M::Stroke;
    tip.grainMovement = 0.5;
    tip.shape = shape;
    TipPreset preset;
    preset.tip = tip;
    const auto folder = std::filesystem::temp_directory_path() / "nekophoto-grain-mode";
    REQUIRE(saveTipPreset(folder.string(), preset));
    auto back = loadTipPreset(folder.string());
    REQUIRE(back.has_value());
    CHECK(back->tip.grainMode == M::Stroke);
    CHECK_NEAR(back->tip.grainMovement, 0.5, 1e-12);
    { std::error_code cleanup_; std::filesystem::remove_all(folder, cleanup_); }
}

TEST_CASE(the_old_settings_are_mappings_that_paint_the_same) {
    // What the engine computed before mappings, for pressure on size (full) and flow (half), and the jitters.
    const LegacyTipDynamics legacy{.sizeJitter = 0.4, .flowJitter = 0.3, .angleJitter = 60, .pressureSize = 1, .minimumSize = 0.2, .pressureFlow = 0.5};
    const BrushDynamics d = legacyDynamics(legacy);
    for (double p : {0.0, 0.13, 0.5, 0.91, 1.0})
        for (double u : {0.0, 0.37, 0.999}) {
            const double oldSize = 30 * (1 - 1.0 + 1.0 * (0.2 + (1 - 0.2) * p)) * (1 - 0.4 * u);
            const double oldFlow = 0.8 * (1 - 0.5 + 0.5 * p) * (1 - 0.3 * u);
            CHECK_EQ(applyDynamics(d, DynamicsTarget::Size, 30, pressed(p), 30, u), oldSize);
            CHECK_EQ(applyDynamics(d, DynamicsTarget::Flow, 0.8, pressed(p), 30, u), oldFlow);
            const double s = 2 * u - 1;
            CHECK_EQ(applyDynamics(d, DynamicsTarget::Angle, 15, pressed(p), 30, s), 15 + 60 * s);
        }
    // A brush.json from before mappings opens with them.
    const fs::path dir = fs::temp_directory_path() / "compositor-legacy-tip";
    { std::error_code cleanup_; fs::remove_all(dir, cleanup_); }
    fs::create_directories(dir);
    std::ofstream(dir / "brush.json") << R"({"format": "compositor-tip-brush", "version": 1, "name": "Old", "diameter": 30,
        "spacing": 0.2, "sizeJitter": 0.4, "flowJitter": 0.3, "angleJitter": 60, "pressureSize": 1, "minimumSize": 0.2, "pressureFlow": 0.5})";
    REQUIRE(writePngGray((dir / "tip.png").string(), GrayImage(8, 8, 255)));
    auto preset = loadTipPreset(dir.string());
    REQUIRE(preset.has_value());
    REQUIRE(preset->tip.dynamics.size() == d.size());
    for (size_t i = 0; i < d.size(); i++) {
        CHECK(preset->tip.dynamics[i].input == d[i].input && preset->tip.dynamics[i].target == d[i].target);
        CHECK_EQ(preset->tip.dynamics[i].offset, d[i].offset);
        CHECK_EQ(preset->tip.dynamics[i].depth, d[i].depth);
    }
    // Saved again, it keeps the mappings and no longer the old fields.
    REQUIRE(saveTipPreset(dir.string(), *preset));
    std::string text;
    {
        // Closed before the folder is removed: Windows will not delete a file that is still open.
        std::ifstream in(dir / "brush.json");
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(text.find("\"dynamics\"") != std::string::npos);
    CHECK(text.find("sizeJitter") == std::string::npos);
    auto again = loadTipPreset(dir.string());
    REQUIRE(again.has_value());
    CHECK_EQ(again->tip.dynamics.size(), d.size());
    { std::error_code cleanup_; fs::remove_all(dir, cleanup_); }
}

TEST_CASE(screen_speed_is_the_document_step_at_the_view_scale_smoothed_the_same) {
    // Two strokes a report every 8 ms: one 4 pixels a step at 100%, one 1 pixel a step at 400%. The same hand motion:
    // the screen speed agrees at every sample, the document speed is four times apart.
    BrushSampleTrack atFull, atFour;
    for (int i = 0; i < 40; i++) {
        BrushSample a, b;
        a.time = b.time = i * 0.008;
        a.position = {4.0 * i * (1 + 0.01 * i), 0};
        b.position = {a.position.x / 4, 0};
        b.viewScale = 4;
        a = atFull.add(a);
        b = atFour.add(b);
        CHECK_NEAR(a.screenSpeed, b.screenSpeed, 1e-9 * std::max(1.0, a.screenSpeed));
        CHECK_NEAR(a.speed, 4 * b.speed, 1e-9 * std::max(1.0, a.speed));
        CHECK_NEAR(a.screenSpeed, a.speed, 1e-9 * std::max(1.0, a.speed));   // at 100% the two are one
    }
    // A missing or broken scale reads as 100%.
    BrushSampleTrack broken;
    BrushSample p, q;
    q.position = {10, 0};
    q.time = 0.01;
    q.viewScale = 0;
    broken.add(p);
    const BrushSample r = broken.add(q);
    CHECK_NEAR(r.screenSpeed, r.speed, 1e-12);
    // Between two samples the screen speed and scale are mixed like the rest.
    BrushSample x, y;
    x.screenSpeed = 100; y.screenSpeed = 300; x.viewScale = 1; y.viewScale = 3;
    CHECK_NEAR(interpolate(x, y, 0.5).screenSpeed, 200, 1e-12);
    CHECK_NEAR(interpolate(x, y, 0.5).viewScale, 2, 1e-12);
}

TEST_CASE(a_recorded_stroke_keeps_its_view_scale) {
    RecordedStroke stroke;
    stroke.stylus = true;
    BrushSample a, b;
    a.viewScale = 2.5;
    b.position = {5, 5};
    b.time = 0.01;
    stroke.samples = {a, b};
    auto back = recordedStrokeFromJson(recordedStrokeToJson(stroke));
    REQUIRE(back.has_value());
    CHECK_NEAR(back->samples[0].viewScale, 2.5, 1e-12);
    CHECK_NEAR(back->samples[1].viewScale, 1.0, 1e-12);   // not written at 100%, and read back as 100%
    auto negative = recordedStrokeFromJson("[{\"x\": 1, \"y\": 2, \"viewScale\": -3}]");
    REQUIRE(negative.has_value());
    CHECK_EQ(negative->samples[0].viewScale, 1.0);
}

namespace {

/// Degrees apart on the circle, -180..180.
double turnDegrees(double from, double to) { return std::remainder(to - from, 360.0); }

/// A stroke of five reports a pixel apart, the pen's barrel, lean or travel at `degrees[i]`.
enum class Turning { Twist, Azimuth, Travel };
std::vector<BrushSample> turningStroke(const std::vector<double>& degrees, Turning what) {
    std::vector<BrushSample> out;
    Point at{100, 50};
    for (size_t i = 0; i < degrees.size(); i++) {
        const double a = degrees[i] * pi / 180;
        BrushSample s;
        s.stylus = true;
        s.pressure = 1;
        s.time = double(i) / 120;
        if (what == Turning::Travel) {
            // Each step heads the way given: the stroke's direction turns through the angles.
            if (i) at = {at.x + 6 * std::cos(a), at.y + 6 * std::sin(a)};
            s.position = at;
        } else s.position = {100 + 6.0 * double(i), 50};
        if (what == Turning::Twist) { s.twist = degrees[i]; s.twistReported = true; }
        if (what == Turning::Azimuth) { s.tiltX = 45 * std::cos(a); s.tiltY = 45 * std::sin(a); }
        out.push_back(s);
    }
    return out;
}

struct Painted {
    std::vector<TipDab> dabs;
    std::shared_ptr<Image> image;
};

/// Paints `samples` with a flat tip whose angle follows `dynamics` (and the stroke, with `follow`), dabs every pixel.
Painted paintTurning(const std::vector<BrushSample>& samples, const BrushDynamics& dynamics, bool follow) {
    BrushTip tip;
    tip.shape = std::make_shared<GrayImage>(40, 10, 255);
    tip.spacing = 0.05;
    tip.followStroke = follow;
    tip.dynamics = dynamics;
    auto image = std::make_shared<Image>(200, 100);
    Layer layer(Asset::make(image, "Paper"), Point(0, 0));
    BrushSettings settings;
    settings.diameter = 20;
    BrushStroke grid(layer, false, settings, Size(200, 100));
    TipStroke stroke(grid, tip, 20, 11);
    Painted out;
    stroke.trace(&out.dabs);
    BrushSampleTrack track;
    for (const BrushSample& s : samples) stroke.strokeTo(track.add(s));
    stroke.finish();
    grid.flush();
    out.image = std::make_shared<Image>(*grid.previewImage());
    return out;
}

} // namespace

TEST_CASE(twist_roll_azimuth_and_the_tip_angle_pass_358_359_0_1_2_without_a_jump) {
    // The pen through 358, 359, 0, 1 and 2 degrees, as a pen reporting 0..360 gives them, as Qt's -180..180 gives them
    // (-2, -1, 0, 1, 2), and back the other way: every derived angle moves a degree a report, and so does every angle
    // the dynamics resolve from it, and the tip's own angle between the dabs in between.
    const std::vector<std::vector<double>> sequences = {{358, 359, 0, 1, 2}, {-2, -1, 0, 1, 2}, {2, 1, 0, 359, 358}};
    for (const std::vector<double>& degrees : sequences) {
        const double sense = turnDegrees(degrees[0], degrees[1]);   // +1 or -1 a report
        for (Turning what : {Turning::Twist, Turning::Azimuth, Turning::Travel}) {
            std::vector<BrushSample> samples = turningStroke(degrees, what);
            BrushSampleTrack track;
            for (BrushSample& s : samples) s = track.add(s);
            // The derived angle, unwrapped: exactly a degree a report, never 359.
            for (size_t i = 1; i < samples.size(); i++) {
                const double step = what == Turning::Twist ? samples[i].twistAngle - samples[i - 1].twistAngle
                                  : what == Turning::Azimuth ? samples[i].tiltAzimuth - samples[i - 1].tiltAzimuth
                                                             : samples[i].direction - samples[i - 1].direction;
                // The travel's first step has no direction before it to turn from.
                if (what == Turning::Travel && i == 1) continue;
                CHECK_NEAR(step * 180 / pi, sense, 1e-9);
            }
            // What a mapping resolves from it, on the angle (a sum, so continuous across the turn).
            const DynamicsInput input = what == Turning::Twist ? DynamicsInput::Twist : what == Turning::Azimuth ? DynamicsInput::TiltDirection : DynamicsInput::Roll;
            BrushDynamics dynamics = {dynamicsMapping(input, DynamicsTarget::Angle, 0, input == DynamicsInput::Twist ? 360 : -360)};
            for (size_t i = (what == Turning::Travel ? 2 : 1); i < samples.size(); i++) {
                const double a = applyDynamics(dynamics, DynamicsTarget::Angle, 0, samples[i - 1], 20);
                const double b = applyDynamics(dynamics, DynamicsTarget::Angle, 0, samples[i], 20);
                CHECK_NEAR(std::fabs(turnDegrees(a, b)), 1.0, 1e-9);
            }
            // Roll reads the barrel when the pen reports one.
            if (what == Turning::Twist) {
                const BrushDynamics roll = {dynamicsMapping(DynamicsInput::Roll, DynamicsTarget::Angle, 0, -360)};
                for (size_t i = 1; i < samples.size(); i++)
                    CHECK_NEAR(turnDegrees(applyDynamics(roll, DynamicsTarget::Angle, 0, samples[i - 1], 20), applyDynamics(roll, DynamicsTarget::Angle, 0, samples[i], 20)), -sense, 1e-9);
            }
            // The tip's resolved angle, dab by dab (twenty between reports): never more than a report's degree apart,
            // and a stroke's worth of them adds up to the pen's four degrees, not a turn. Following the travel, the
            // first dab waits for the stroke's direction, so it too is within a degree of the next.
            const Painted painted = paintTurning(turningStroke(degrees, what), dynamics, false);
            REQUIRE(painted.dabs.size() > 10);
            double largest = 0, total = 0;
            for (size_t k = 1; k < painted.dabs.size(); k++) {
                const double step = std::remainder(painted.dabs[k].rotation - painted.dabs[k - 1].rotation, 2 * pi) * 180 / pi;
                largest = std::max(largest, std::fabs(step));
                total += step;
            }
            CHECK(largest <= 1.0 + 1e-9);
            CHECK(std::fabs(total) <= 4.0 + 1e-9);
        }
    }
    // A tip that follows the stroke passes 359 -> 0 the same way.
    const Painted follow = paintTurning(turningStroke({358, 359, 0, 1, 2}, Turning::Travel), {}, true);
    double largest = 0;
    for (size_t k = 1; k < follow.dabs.size(); k++)
        largest = std::max(largest, std::fabs(std::remainder(follow.dabs[k].rotation - follow.dabs[k - 1].rotation, 2 * pi)) * 180 / pi);
    CHECK(largest <= 1.0 + 1e-9);
}

TEST_CASE(the_first_dab_points_the_way_the_stroke_goes) {
    // A stroke drawn up and to the left (135 degrees on screen): a tip that follows the stroke, and Roll on a pen
    // without twist, start turned that way rather than along +x, as the first dab did up to 1.8.2.
    std::vector<BrushSample> samples;
    for (int i = 0; i < 6; i++) {
        BrushSample s;
        s.time = i / 120.0;
        s.position = {150 - 8.0 * i, 80 - 8.0 * i};
        s.pressure = 0.8;
        s.stylus = true;
        samples.push_back(s);
    }
    const double travel = std::atan2(-8.0, -8.0);
    const BrushDynamics roll = {dynamicsMapping(DynamicsInput::Roll, DynamicsTarget::Angle, 0, -360)};
    auto turned = [](double rotation, double expected) { return std::fabs(std::remainder(rotation - expected, 2 * pi)) < 1e-9; };
    const Painted follow = paintTurning(samples, {}, true), rolled = paintTurning(samples, roll, false);
    REQUIRE(follow.dabs.size() > 2 && rolled.dabs.size() > 2);
    CHECK(turned(follow.dabs[0].rotation, travel));
    CHECK(turned(follow.dabs[0].rotation, follow.dabs[1].rotation));
    CHECK(follow.dabs[0].center.x == 150 && follow.dabs[0].center.y == 80);   // where the pen went down
    CHECK(turned(rolled.dabs[0].rotation, rolled.dabs[1].rotation));
    // With the barrel reported, Roll reads it from the first report: no wait, the first dab turned by the barrel.
    std::vector<BrushSample> barrel = samples;
    for (BrushSample& s : barrel) { s.twist = 30; s.twistReported = true; }
    const Painted twisted = paintTurning(barrel, roll, false);
    REQUIRE(twisted.dabs.size() > 2);
    CHECK(turned(twisted.dabs[0].rotation, 30 * pi / 180));
    // A click that never moves still stamps its dab (on finish), facing +x as there is no direction.
    const Painted click = paintTurning({samples.front()}, {}, true);
    CHECK_EQ(click.dabs.size(), size_t(1));
    // A plain tip does not wait: its first dab is stamped at the press.
    BrushTip tip;
    tip.shape = std::make_shared<GrayImage>(10, 10, 255);
    auto image = std::make_shared<Image>(200, 100);
    Layer layer(Asset::make(image, "Paper"), Point(0, 0));
    BrushSettings settings;
    settings.diameter = 20;
    BrushStroke grid(layer, false, settings, Size(200, 100));
    TipStroke stroke(grid, tip, 20, 11);
    BrushSampleTrack track;
    stroke.strokeTo(track.add(samples.front()));
    CHECK_EQ(stroke.dabCount(), size_t(1));
}

TEST_CASE(a_stroke_across_the_wrap_replays_the_same) {
    // The same recorded stroke across 359 -> 0 painted twice (and once from its JSON): the same dabs and pixels.
    std::vector<BrushSample> samples = turningStroke({356, 357, 358, 359, 0, 1, 2, 3, 4}, Turning::Twist);
    for (size_t i = 0; i < samples.size(); i++) { samples[i].tiltX = 45 * std::cos((350 + 2.0 * i) * pi / 180); samples[i].tiltY = 45 * std::sin((350 + 2.0 * i) * pi / 180); }
    const BrushDynamics dynamics = {dynamicsMapping(DynamicsInput::Roll, DynamicsTarget::Angle, 0, -360),
                                    dynamicsMapping(DynamicsInput::TiltDirection, DynamicsTarget::Angle, 0, -360),
                                    dynamicsMapping(DynamicsInput::Random, DynamicsTarget::Angle, 0, 15)};
    RecordedStroke recorded;
    recorded.samples = samples;
    const auto replayed = recordedStrokeFromJson(recordedStrokeToJson(recorded));
    REQUIRE(replayed.has_value());
    const Painted a = paintTurning(samples, dynamics, false), b = paintTurning(samples, dynamics, false), c = paintTurning(replayed->samples, dynamics, false);
    REQUIRE(a.dabs.size() == b.dabs.size() && a.dabs.size() == c.dabs.size());
    for (size_t k = 0; k < a.dabs.size(); k++) {
        CHECK(a.dabs[k].rotation == b.dabs[k].rotation && a.dabs[k].center.x == b.dabs[k].center.x);
        CHECK_NEAR(a.dabs[k].rotation, c.dabs[k].rotation, 1e-9);
    }
    CHECK(*a.image == *b.image);
    CHECK(*a.image == *c.image);
}

TEST_CASE(dynamics_never_drive_the_grain_or_tie_deposition_to_speed) {
    // The legal boundaries (docs/legal-boundaries.md, "Brushes"): the grain is static, and flow and opacity never follow
    // the pen's speed. Such mappings load but do nothing.
    BrushSample fast;
    fast.pressure = 0.5;
    fast.speed = fast.screenSpeed = 1e6;
    const BrushDynamics d = {dynamicsMapping(DynamicsInput::Pressure, DynamicsTarget::GrainDepth, 0, 1),
                             dynamicsMapping(DynamicsInput::Speed, DynamicsTarget::GrainRotation, 0, 90),
                             dynamicsMapping(DynamicsInput::ScreenSpeed, DynamicsTarget::Opacity, 0, 0.5),
                             dynamicsMapping(DynamicsInput::Speed, DynamicsTarget::Flow, 0, 0.5),
                             dynamicsMapping(DynamicsInput::Speed, DynamicsTarget::Size, 0, 0.5)};
    CHECK_EQ(applyDynamics(d, DynamicsTarget::GrainDepth, 0.7, fast, 10, 0), 0.7);
    CHECK_EQ(applyDynamics(d, DynamicsTarget::GrainRotation, 0, fast, 10, 0), 0.0);
    CHECK_EQ(applyDynamics(d, DynamicsTarget::Opacity, 1, fast, 10, 0), 1.0);
    CHECK_EQ(applyDynamics(d, DynamicsTarget::Flow, 1, fast, 10, 0), 1.0);
    CHECK(applyDynamics(d, DynamicsTarget::Size, 1, fast, 10, 0) < 1.0);   // speed on size (shape dynamics) stays
    CHECK(!mappingAllowed(DynamicsTarget::Opacity, DynamicsInput::Speed));
    CHECK(mappingAllowed(DynamicsTarget::Opacity, DynamicsInput::Pressure));
}

TEST_CASE(smudge_carries_one_colour_not_a_patch) {
    // The legal boundary (docs/legal-boundaries.md, "Brushes"): the smudge's pickup is one running average, so a
    // stroke that starts on a two-colour edge smears their mix, never a copy of the edge.
    auto img = std::make_shared<Image>(64, 32);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 64; x++) { uint8_t* p = img->pixel(x, y); const bool top = y < 16; p[0] = top ? 255 : 0; p[1] = 0; p[2] = top ? 0 : 255; p[3] = 255; }
    WarpStroke smudge(img, WarpMode::Smudge, 12, 0.9, 1.0);   // strength 1: the carried colour never changes
    smudge.append({8, 16});
    smudge.append({56, 16});
    // Far along the stroke, above and below its line: the same carried mix, not the red-over-blue edge it started on.
    const uint8_t* above = img->pixel(50, 13);
    const uint8_t* below = img->pixel(50, 19);
    CHECK(std::abs(int(above[0]) - int(below[0])) <= 40);
    CHECK(std::abs(int(above[2]) - int(below[2])) <= 40);
}

TEST_MAIN()
