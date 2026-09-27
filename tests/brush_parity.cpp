// Brush parity: every standard motion and recorded stroke (brush_harness.h) painted with every harness preset, each
// render hashed and measured, and held to tests/brush_parity_baseline.txt. A change to the brush engines that is meant
// to change nothing must leave every line as it is; one that is meant to change something shows which strokes moved
// and how (width, peak alpha, edge, taper). Every scene is painted on the worker pool and serially, which must agree.
//
// COMPOSITOR_UPDATE_BRUSH_PARITY=1 rewrites the baseline after an intentional change.
#include "check.h"
#include "brush_harness.h"
#include "compositor/mypaint.h"
#include "compositor/parallel.h"
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

using namespace brushharness;

namespace {

/// Runs `body` with every nested parallel loop serial: the caller of parallelFor marks itself as inside a loop.
Render serially(const std::function<Render()>& body) {
    if (workerCount() <= 1) return body();
    Render result;
    parallelFor(0, 2, 1, [&](int y0, int) { if (y0 == 0) result = body(); });
    return result;
}

std::vector<StrokeFixture> allFixtures() {
    std::vector<StrokeFixture> fixtures = standardFixtures();
    for (StrokeFixture& f : fileFixtures(BRUSH_FIXTURES_DIR)) fixtures.push_back(std::move(f));
    return fixtures;
}

/// Every fixture with every preset, then the synthetic Procreate brushes on the strokes their manifest names.
std::vector<Scene> allScenes(const std::vector<StrokeFixture>& fixtures, const std::vector<Preset>& presets, const FixtureBrushes& synthetic) {
    std::vector<Scene> out = scenes(fixtures, presets);
    for (Scene& s : expectationScenes(fixtures, synthetic)) out.push_back(std::move(s));
    return out;
}

// The baseline file has two sections: the 8-bit scenes, then the 16-bit ones, named "u16/<fixture>/<preset>". Each
// test reads and rewrites its own section and keeps the other as it is.
const std::string deepPrefix = "u16/";

std::map<std::string, std::string> readSection(const std::string& path, bool deep) {
    std::map<std::string, std::string> lines;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const size_t space = line.find(' ');
        if (space == std::string::npos || (line.rfind(deepPrefix, 0) == 0) != deep) continue;
        lines[line.substr(0, space)] = line.substr(space + 1);
    }
    return lines;
}

void writeBaseline(const std::string& path, const std::map<std::string, std::string>& eight, const std::map<std::string, std::string>& deep) {
    std::ofstream out(path);
    out << "# Brush parity baseline: <fixture>/<preset> <FNV-1a 64 of the layer's pixels> <measurements>, from tests/brush_parity.cpp.\n"
           "# Regenerate after an intentional brush change: COMPOSITOR_UPDATE_BRUSH_PARITY=1 build/tests/brush_parity\n";
    for (auto& [name, line] : eight) out << name << ' ' << line << '\n';
    if (deep.empty()) return;
    out << "# 16 bits: u16/<fixture>/<preset> <FNV-1a 64 of the 16-bit layer's samples> <measurements of it reduced to 8 bits>\n"
           "# vs8=<largest difference from the 8-bit render, in 8-bit levels>,<share of samples more than a level apart>\n";
    for (auto& [name, line] : deep) out << name << ' ' << line << '\n';
}

/// Compares `actual` with the baseline's section; returns the number of scenes changed, new and missing.
/// The numbers of a baseline line's metrics ("box=30,40,259,120 total=5049.4 ..."), by key.
std::map<std::string, std::vector<double>> metricsOf(const std::string& line) {
    std::map<std::string, std::vector<double>> out;
    size_t at = line.find(' ');
    while (at != std::string::npos) {
        const size_t start = at + 1, end = line.find(' ', start), eq = line.find('=', start);
        if (eq != std::string::npos && eq < end) {
            std::vector<double>& values = out[line.substr(start, eq - start)];
            std::string rest = line.substr(eq + 1, end == std::string::npos ? std::string::npos : end - eq - 1);
            for (char& c : rest) if (c == ',' || c == '/' || c == '%') c = ' ';
            std::istringstream in(rest);
            for (double v; in >> v;) values.push_back(v);
        }
        at = end;
    }
    return out;
}

/// The same stroke by every measure (box within a pixel, total alpha within 0.5%, mean, widths, peaks, edge and
/// taper within a hair) though not byte for byte: what another platform's maths library (the last bit of hypot, atan2
/// and exp, which the pen samples' speed and direction use) can move a dab by.
bool sameStroke(const std::string& was, const std::string& now) {
    auto a = metricsOf(was), b = metricsOf(now);
    for (auto& [key, values] : a) {
        if (key == "vs8") continue;
        auto it = b.find(key);
        if (it == b.end() || it->second.size() != values.size()) return false;
        for (size_t i = 0; i < values.size(); i++) {
            const double x = values[i], y = it->second[i];
            const double allowed = key == "box" ? 1.0 : key == "total" ? 0.005 * std::max(1.0, std::abs(x)) : 0.02;
            if (std::abs(x - y) > allowed) return false;
        }
    }
    return true;
}

int compareSection(const std::map<std::string, std::string>& actual, const std::map<std::string, std::string>& expected, const std::string& path) {
    auto hashOf = [](const std::string& line) { return line.substr(0, line.find(' ')); };
    int changed = 0, added = 0, missing = 0;
    for (auto& [name, line] : actual) {
        auto it = expected.find(name);
        if (it == expected.end()) { std::fprintf(stderr, "  new      %s %s\n", name.c_str(), line.c_str()); added++; }
#if defined(_WIN32)
        // The baseline is written on Linux; the Windows C runtime rounds a few maths functions differently in the last
        // bit, so there a scene may differ by a sliver while measuring the same. Linux stays byte for byte.
        else if (hashOf(it->second) != hashOf(line) && sameStroke(it->second, line))
            std::fprintf(stderr, "  near     %s (not byte for byte, the same by every measure)\n", name.c_str());
#endif
        else if (hashOf(it->second) != hashOf(line)) {
            std::fprintf(stderr, "  changed  %s\n    was %s\n    now %s\n", name.c_str(), it->second.c_str(), line.c_str());
            changed++;
        }
    }
    for (auto& [name, line] : expected)
        if (!actual.count(name)) {
            // MyPaint and Clip Studio scenes exist only in builds with libmypaint and SQLite.
            if ((name.find("/mypaint_") != std::string::npos && !myPaintSupported()) || name.find("/sut_") != std::string::npos) continue;
            std::fprintf(stderr, "  missing  %s\n", name.c_str());
            missing++;
        }
    if (changed || added || missing)
        std::fprintf(stderr, "  %d changed, %d new, %d missing; COMPOSITOR_UPDATE_BRUSH_PARITY=1 rewrites %s\n", changed, added, missing, path.c_str());
    return changed + added + missing;
}

} // namespace

TEST_CASE(near_matches_allow_a_sliver_and_nothing_more) {
    // The one scene the Windows runtime moved (fast_flick with the eraser): a pixel wider, 0.2% more alpha.
    const std::string was = "2a10b5428773df24 box=30,40,259,120 total=5049.4 mean=0.9109 width=19.5/19.5/19.5 peak=1.00/1.00/1.00 edge=1.00 taper=1.00,1.00";
    const std::string now = "4213224e584837ef box=30,40,260,120 total=5059.0 mean=0.9105 width=19.5/19.5/19.5 peak=1.00/1.00/1.00 edge=1.00 taper=1.00,1.00";
    CHECK(sameStroke(was, now));
    CHECK(!sameStroke(was, "0 box=30,40,262,120 total=5059.0 mean=0.9105 width=19.5/19.5/19.5 peak=1.00/1.00/1.00 edge=1.00 taper=1.00,1.00"));
    CHECK(!sameStroke(was, "0 box=30,40,259,120 total=5200.0 mean=0.9105 width=19.5/19.5/19.5 peak=1.00/1.00/1.00 edge=1.00 taper=1.00,1.00"));
    CHECK(!sameStroke(was, "0 box=30,40,259,120 total=5049.4 mean=0.9109 width=19.5/21.0/19.5 peak=1.00/1.00/1.00 edge=1.00 taper=1.00,1.00"));
}

TEST_CASE(fixtures_survive_a_json_round_trip) {
    for (const StrokeFixture& f : allFixtures()) {
        auto back = fixtureFromJson(fixtureToJson(f), f.name);
        REQUIRE(back.has_value());
        CHECK(back->name == f.name);
        CHECK_EQ(back->stylus, f.stylus);
        REQUIRE(back->samples.size() == f.samples.size());
        for (size_t i = 0; i < f.samples.size(); i++) {
            const BrushSample& a = f.samples[i];
            const BrushSample& b = back->samples[i];
            CHECK(a.time == b.time && a.position.x == b.position.x && a.position.y == b.position.y && a.pressure == b.pressure && a.tiltX == b.tiltX && a.tiltY == b.tiltY
                  && a.twist == b.twist && a.twistReported == b.twistReported && a.tangentialPressure == b.tangentialPressure);
        }
    }
    // A bare array with missing fields takes the defaults.
    auto bare = fixtureFromJson(R"([{"x": 1, "y": 2}, {"t": 0.01, "x": 3, "y": 4, "pressure": 0.5}])", "bare");
    REQUIRE(bare.has_value());
    REQUIRE(bare->samples.size() == 2);
    CHECK_EQ(bare->samples[0].pressure, 1.0);
    CHECK_EQ(bare->samples[1].pressure, 0.5);
    CHECK(!fixtureFromJson("{}", "empty").has_value());
    CHECK(!fixtureFromJson("[1, 2]", "numbers").has_value());
}

TEST_CASE(brush_samples_derive_speed_direction_and_unwrapped_angles) {
    constexpr double pi = 3.14159265358979323846;
    CHECK_NEAR(unwrapAngle(3.1, -3.1), 2 * pi - 3.1, 1e-12);
    CHECK_NEAR(unwrapAngle(-3.1, 3.1), 3.1 - 2 * pi, 1e-12);
    CHECK_NEAR(unwrapAngle(0.2, 0.1), 0.1, 1e-12);
    const std::vector<StrokeFixture> fixtures = standardFixtures();
    auto named = [&](const std::string& name) {
        for (const StrokeFixture& f : fixtures) if (f.name == name) return f;
        return StrokeFixture{};
    };
    // A full barrel turn wraps from 180 to -180 degrees halfway; unwrapped, it climbs steadily to one turn.
    std::vector<BrushSample> twist = named("twist_sweep").samples;
    REQUIRE(!twist.empty());
    deriveStroke(twist);
    double biggest = 0;
    for (size_t i = 1; i < twist.size(); i++) biggest = std::max(biggest, std::fabs(twist[i].twistAngle - twist[i - 1].twistAngle));
    CHECK(biggest < 0.1);
    CHECK_NEAR(twist.back().twistAngle - twist.front().twistAngle, 2 * pi, 1e-9);
    // Round a circle, the direction turns once without a jump.
    std::vector<BrushSample> circle = named("circle").samples;
    deriveStroke(circle);
    CHECK_NEAR(circle.back().direction - circle[1].direction, 2 * pi - 2 * pi / 180, 0.05);
    CHECK_NEAR(circle.back().progress, 1.0, 1e-12);
    CHECK_EQ(circle.front().progress, 0.0);
    // Slow, fast, slow: the middle is several times the speed of the ends.
    std::vector<BrushSample> speed = named("speed_sweep").samples;
    deriveStroke(speed);
    CHECK(speed[speed.size() / 2].speed > 4 * speed[5].speed);
    CHECK(speed[speed.size() / 2].speed > 4 * speed[speed.size() - 2].speed);
    // Tilted 60 degrees one way and 25 the other is full tilt, leaning down-right of the x axis.
    std::vector<BrushSample> tilt = named("tilt_sweep").samples;
    deriveStroke(tilt);
    CHECK_NEAR(tilt[tilt.size() / 2].tiltMagnitude, 1.0, 1e-12);
    CHECK_NEAR(tilt[tilt.size() / 2].tiltAzimuth, std::atan2(-25.0, 60.0), 1e-9);
    // Between 170 and -170 degrees of twist lies 180, not 0.
    BrushSampleTrack track;
    BrushSample a, b;
    a.twist = 170; b.twist = -170; b.position = {10, 0};
    a = track.add(a); b = track.add(b);
    CHECK_NEAR(std::fabs(interpolate(a, b, 0.5).twist), 180.0, 1e-9);
    // A mouse is neutral; a replay derives the same values every time.
    const BrushSample mouse = mouseSample({1, 2}, 0.5);
    CHECK(!mouse.stylus && mouse.pressure == 0.5 && mouse.tiltX == 0 && mouse.twist == 0);
    std::vector<BrushSample> again = named("twist_sweep").samples;
    deriveStroke(again);
    for (size_t i = 0; i < again.size(); i++) CHECK(again[i].twistAngle == twist[i].twistAngle && again[i].speed == twist[i].speed);
}

namespace {

/// A hard disc as a tip.
std::shared_ptr<GrayImage> disc(int side) {
    auto tip = std::make_shared<GrayImage>(side, side, 0);
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++)
            if (std::hypot(x + 0.5 - side / 2.0, y + 0.5 - side / 2.0) <= side / 2.0) tip->at(x, y) = 255;
    return tip;
}

/// Paints `tip` along `samples` on a transparent 400 x 100 layer and returns the layer.
std::shared_ptr<Image> paintTip(const BrushTip& tip, double diameter, const std::vector<BrushSample>& samples) {
    auto image = std::make_shared<Image>(400, 100);
    Layer layer(Asset::make(image, "Paper"), Point(0, 0));
    BrushSettings settings;
    settings.diameter = diameter;
    BrushStroke grid(layer, false, settings, Size(400, 100));
    TipStroke stroke(grid, tip, diameter, 5);
    BrushSampleTrack track;
    for (const BrushSample& s : samples) stroke.strokeTo(track.add(s));
    grid.flush();
    return std::make_shared<Image>(*grid.previewImage());
}

/// The same on a 16-bit layer.
std::shared_ptr<Image16> paintTip16(const BrushTip& tip, double diameter, const std::vector<BrushSample>& samples) {
    Layer layer(Asset::make(Image16Ptr(std::make_shared<Image16>(400, 100)), "Paper"), Point(0, 0));
    BrushSettings settings;
    settings.diameter = diameter;
    BrushStroke grid(layer, false, settings, Size(400, 100), SampleType::U16, nullptr);
    TipStroke stroke(grid, tip, diameter, 5);
    BrushSampleTrack track;
    for (const BrushSample& s : samples) stroke.strokeTo(track.add(s));
    grid.flush();
    return std::make_shared<Image16>(*grid.previewImage16());
}

/// The mean alpha (0..1) over a rectangle.
double meanAlpha(const Image& image, int x0, int y0, int x1, int y1) {
    double sum = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) sum += image.pixel(x, y)[3];
    return sum / (255.0 * (x1 - x0) * (y1 - y0));
}
double meanAlpha(const Image16& image, int x0, int y0, int x1, int y1) {
    double sum = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) sum += image.pixel(x, y)[3];
    return sum / (32768.0 * (x1 - x0) * (y1 - y0));
}

/// A straight pen stroke across the layer's middle.
std::vector<BrushSample> straightStroke(bool stylus) {
    std::vector<BrushSample> out;
    for (int i = 0; i <= 180; i++) {
        BrushSample s = stylus ? BrushSample{} : mouseSample({}, 0);
        s.position = {20 + 2.0 * i, 50};
        s.time = i / 120.0;
        s.pressure = stylus ? 1 : 0.5;
        s.stylus = stylus;
        out.push_back(s);
    }
    return out;
}

} // namespace

TEST_CASE(density_by_spacing_keeps_the_interior_alpha_across_spacings) {
    const double spacings[] = {0.02, 0.05, 0.1, 0.25, 0.5};
    std::vector<double> on, off, on16;
    for (double spacing : spacings) {
        BrushTip tip;
        tip.shape = disc(64);
        tip.flow = 0.3;
        tip.spacing = spacing;
        tip.densityBySpacing = true;
        on.push_back(meanAlpha(*paintTip(tip, 20, straightStroke(true)), 100, 46, 300, 54));
        on16.push_back(meanAlpha(*paintTip16(tip, 20, straightStroke(true)), 100, 46, 300, 54));
        tip.densityBySpacing = false;
        off.push_back(meanAlpha(*paintTip(tip, 20, straightStroke(true)), 100, 46, 300, 54));
        std::fprintf(stderr, "  spacing %4.0f%%: interior alpha %.3f with density by spacing (%.3f at 16 bits), %.3f without\n", spacing * 100, on.back(), on16.back(), off.back());
    }
    // On: within 8% of the reference spacing's (25%) interior alpha from 2% to 50%; at 16 bits, where a light dab is
    // not rounded to whole 8-bit levels, closer still.
    double spread8 = 0, spread16 = 0;
    for (size_t i = 0; i < on.size(); i++) {
        spread8 = std::max(spread8, std::fabs(on[i] - on[3]) / on[3]);
        spread16 = std::max(spread16, std::fabs(on16[i] - on16[3]) / on16[3]);
    }
    std::fprintf(stderr, "  interior alpha across spacings: within %.1f%% at 8 bits, %.1f%% at 16 bits\n", spread8 * 100, spread16 * 100);
    CHECK(spread8 < 0.08);
    CHECK(spread16 <= spread8);
    // At the reference spacing it changes nothing; off, tight spacing builds up far more paint.
    CHECK_EQ(on[3], off[3]);
    CHECK(off[0] > 1.5 * off[4]);
}

TEST_CASE(a_mouse_can_press_by_its_speed_when_asked) {
    // Evenly timed mouse events whose spacing follows a slow, fast, slow sweep.
    std::vector<BrushSample> samples;
    for (int i = 0; i <= 120; i++) {
        const double u = i / 120.0, eased = u - std::sin(2 * 3.14159265358979323846 * u) / (2 * 3.14159265358979323846);
        BrushSample s = mouseSample({30 + 340 * eased, 50}, i / 120.0);
        samples.push_back(s);
    }
    BrushTip tip;
    tip.shape = disc(64);
    tip.spacing = 0.1;
    tip.dynamics = {dynamicsMapping(DynamicsInput::Pressure, DynamicsTarget::Size, 0.2, 0.8)};
    auto width = [](const Image& image, int x) { int n = 0; for (int y = 0; y < 100; y++) n += image.pixel(x, y)[3] > 128; return n; };
    // Off (the default), a mouse is full pressure: the same width all along.
    const auto plain = paintTip(tip, 24, samples);
    CHECK(std::abs(width(*plain, 200) - width(*plain, 60)) <= 1);
    // On, the fast middle is thinner than the slow ends; a stylus's pressure is never replaced.
    tip.mousePressureFromSpeed = true;
    const auto simulated = paintTip(tip, 24, samples);
    std::fprintf(stderr, "  mouse speed as pressure: %d px wide slow, %d px fast\n", width(*simulated, 350), width(*simulated, 200));
    CHECK(width(*simulated, 200) + 3 < width(*simulated, 350));
    const auto pen = paintTip(tip, 24, straightStroke(true));
    CHECK(std::abs(width(*pen, 200) - width(*pen, 60)) <= 1);
}

TEST_CASE(synthetic_procreate_brushes_change_the_way_their_setting_says) {
    // One setting at a time against the baseline brush (tests/fixtures/brushes/procreate/manifest.json): the direction
    // of the change where the stroke's input is high, the tip turning with the pen, and no jump where the twist wraps.
    const std::vector<StrokeFixture> fixtures = allFixtures();
    const FixtureBrushes synthetic = syntheticProcreate(PROCREATE_FIXTURES_DIR);
    REQUIRE(!synthetic.expectations.empty());
    for (const std::string& note : synthetic.notes) std::fprintf(stderr, "  note %s\n", note.c_str());
    int failed = 0;
    for (const Expectation& e : synthetic.expectations) {
        std::string why;
        const bool ok = checkExpectation(e, fixtures, synthetic, &why);
        std::fprintf(stderr, "  %-4s %-26s %-14s %-10s %s\n", ok ? "ok" : "FAIL", e.brush.c_str(), e.stroke.c_str(), e.expect.c_str(), why.c_str());
        failed += !ok;
    }
    CHECK_EQ(failed, 0);
}

TEST_CASE(local_third_party_brushes_when_present) {
    // tests/local-fixtures/ (git-ignored) holds brush sets that may not be shared; with its manifest.json this checks
    // them the same way against themselves without the input's mappings, and prints what the importer left out.
    const auto local = localFixtures(LOCAL_FIXTURES_DIR);
    if (!local) { std::fprintf(stderr, "  no tests/local-fixtures/manifest.json: skipped\n"); return; }
    const std::vector<StrokeFixture> fixtures = allFixtures();
    for (const std::string& note : local->notes) std::fprintf(stderr, "  note %s\n", note.c_str());
    int failed = 0;
    for (const Expectation& e : local->expectations) {
        std::string why;
        const bool ok = checkExpectation(e, fixtures, *local, &why);
        std::fprintf(stderr, "  %-4s %-28s %-14s %-10s %s\n", ok ? "ok" : "FAIL", e.brush.c_str(), e.stroke.c_str(), e.expect.c_str(), why.c_str());
        failed += !ok;
    }
    CHECK_EQ(failed, 0);
}

TEST_CASE(every_fixture_and_preset_matches_the_baseline) {
    const std::vector<StrokeFixture> fixtures = allFixtures();
    const std::vector<Preset> presets = standardPresets(MYPAINT_BRUSHES_DIR);
    CHECK(fixtures.size() >= 12);
    std::map<std::string, std::string> actual;
    int threadMismatch = 0, empty = 0;
    const FixtureBrushes synthetic = syntheticProcreate(PROCREATE_FIXTURES_DIR);
    for (const Scene& scene : allScenes(fixtures, presets, synthetic)) {
        const Render pooled = render(*scene.fixture, *scene.preset);
        const Render serial = serially([&] { return render(*scene.fixture, *scene.preset); });
        const uint64_t hash = hashRender(pooled);
        if (!pooled.image) { std::fprintf(stderr, "  %s: nothing rendered\n", scene.name.c_str()); empty++; continue; }
        if (hash != hashRender(serial)) {
            std::fprintf(stderr, "  %s: %s on %d threads, %s serially\n", scene.name.c_str(), hex(hash).c_str(), workerCount(), hex(hashRender(serial)).c_str());
            threadMismatch++;
        }
        actual[scene.name] = hex(hash) + " " + formatMetrics(measure(*scene.fixture, pooled));
    }
    CHECK_EQ(empty, 0);
    CHECK_EQ(threadMismatch, 0);
    std::fprintf(stderr, "  %zu scenes (%zu fixtures x %zu presets, %d worker threads)\n", actual.size(), fixtures.size(), presets.size(), workerCount());

    const std::string path = BRUSH_PARITY_BASELINE;
    if (std::getenv("COMPOSITOR_UPDATE_BRUSH_PARITY")) {
        writeBaseline(path, actual, readSection(path, true));
        std::fprintf(stderr, "  wrote %s\n", path.c_str());
        return;
    }
    const std::map<std::string, std::string> expected = readSection(path, false);
    REQUIRE(!expected.empty());
    CHECK_EQ(compareSection(actual, expected, path), 0);
}

/// The same scenes painted on a 16-bit layer (the same paper widened): each render's 16-bit samples held to the
/// baseline's 16-bit section, and each reduced to 8 bits held to its 8-bit render. Hard tips and MyPaint land within a
/// level (libmypaint paints in 15-bit fixed point, the depth of a 16-bit layer, so its tiles are the same at either
/// depth); soft tips and the tip brushes build their coverage up dab over dab, where the 8-bit coverage rounds at every
/// step (depth_paint_tests measures that against the exact coverage), so they may differ by a few levels in a small
/// share of samples.
TEST_CASE(every_fixture_and_preset_matches_the_baseline_at_16_bits) {
    const std::vector<StrokeFixture> fixtures = allFixtures();
    const std::vector<Preset> presets = standardPresets(MYPAINT_BRUSHES_DIR);
    std::map<std::string, std::string> actual;
    std::map<std::string, Calibration> worstByPreset;
    int threadMismatch = 0, empty = 0, apart = 0;
    const FixtureBrushes synthetic = syntheticProcreate(PROCREATE_FIXTURES_DIR);
    for (const Scene& scene : allScenes(fixtures, presets, synthetic)) {
        const Render16 pooled = render16(*scene.fixture, *scene.preset);
        const Render16 serial = [&] {
            if (workerCount() <= 1) return render16(*scene.fixture, *scene.preset);
            Render16 result;
            parallelFor(0, 2, 1, [&](int y0, int) { if (y0 == 0) result = render16(*scene.fixture, *scene.preset); });
            return result;
        }();
        if (!pooled.image) { std::fprintf(stderr, "  u16/%s: nothing rendered\n", scene.name.c_str()); empty++; continue; }
        const uint64_t hash = hashRender(pooled);
        if (hash != hashRender(serial)) { std::fprintf(stderr, "  u16/%s: differs between the worker pool and a serial run\n", scene.name.c_str()); threadMismatch++; }
        const Calibration c = compare(render(*scene.fixture, *scene.preset), pooled);
        Calibration& byPreset = worstByPreset[scene.preset->name];
        byPreset.worst = std::max(byPreset.worst, c.worst);
        byPreset.beyondOne = std::max(byPreset.beyondOne, c.beyondOne);
        // Density by spacing at a tight spacing spreads a light flow over many dabs, each laying down a level or two at
        // 8 bits, where the 256-entry table's rounding is a large share of the dab (measured: 5 levels, under 0.9%).
        const bool spread = scene.preset->tip && scene.preset->tip->tip.densityBySpacing;
        if (c.worst > (spread ? 6 : 4) || c.beyondOne >= 0.01) { std::fprintf(stderr, "  u16/%s: %d levels from the 8-bit render, %.3f%% beyond a level\n", scene.name.c_str(), c.worst, c.beyondOne * 100); apart++; }
        char vs8[64];
        std::snprintf(vs8, sizeof vs8, " vs8=%d,%.4f%%", c.worst, c.beyondOne * 100);
        actual[deepPrefix + scene.name] = hex(hash) + " " + formatMetrics(measure(*scene.fixture, pooled.eight)) + vs8;
    }
    CHECK_EQ(empty, 0);
    CHECK_EQ(threadMismatch, 0);
    CHECK_EQ(apart, 0);
    for (auto& [name, c] : worstByPreset)
        std::fprintf(stderr, "  %-22s at 16 bits against 8: worst %d levels, at most %.3f%% of samples beyond a level\n", name.c_str(), c.worst, c.beyondOne * 100);

    const std::string path = BRUSH_PARITY_BASELINE;
    if (std::getenv("COMPOSITOR_UPDATE_BRUSH_PARITY")) {
        writeBaseline(path, readSection(path, false), actual);
        std::fprintf(stderr, "  wrote the 16-bit section of %s\n", path.c_str());
        return;
    }
    const std::map<std::string, std::string> expected = readSection(path, true);
    REQUIRE(!expected.empty());
    CHECK_EQ(compareSection(actual, expected, path), 0);
}

TEST_MAIN()
