#include "brush_harness.h"
#include "brush_import_fixtures.h"
#include "compositor/brushimport.h"
#include "compositor/depth.h"
#include "compositor/mypaint.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace brushharness {

namespace fs = std::filesystem;

namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double tick = 1.0 / 120;   // a tablet's report rate

/// A stylus sample.
BrushSample at(double t, double x, double y, double pressure) {
    BrushSample s;
    s.time = t;
    s.position = {x, y};
    s.pressure = pressure;
    s.stylus = true;
    return s;
}

/// `n` + 1 samples along a straight line, `tick` apart, with `fill` setting the pen at u = 0..1.
StrokeFixture line(const std::string& name, int n, double x0, double y0, double x1, double y1,
                   const std::function<void(BrushSample&, double)>& fill) {
    StrokeFixture f;
    f.name = name;
    for (int i = 0; i <= n; i++) {
        const double u = double(i) / n;
        BrushSample s = at(i * tick, x0 + (x1 - x0) * u, y0 + (y1 - y0) * u, 1);
        fill(s, u);
        f.samples.push_back(s);
    }
    return f;
}

/// Samples every `step` pixels along a polyline, `tick` apart, at `pressure`.
StrokeFixture polyline(const std::string& name, const std::vector<Point>& corners, double step, double pressure) {
    StrokeFixture f;
    f.name = name;
    int i = 0;
    for (size_t k = 0; k + 1 < corners.size(); k++) {
        const Point a = corners[k], b = corners[k + 1];
        const double length = std::hypot(b.x - a.x, b.y - a.y);
        const int n = std::max(1, int(std::ceil(length / step)));
        for (int j = (k == 0 ? 0 : 1); j <= n; j++) {
            const double u = double(j) / n;
            f.samples.push_back(at(i++ * tick, a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, pressure));
        }
    }
    return f;
}

std::shared_ptr<GrayImage> radialTip(int side) {
    auto tip = std::make_shared<GrayImage>(side, side, 0);
    const double r = side / 2.0;
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++) {
            const double d = std::hypot(x + 0.5 - r, y + 0.5 - r) / r;
            tip->at(x, y) = uint8_t(std::lround(255 * std::clamp(1.3 - 1.3 * d * d, 0.0, 1.0)));
        }
    return tip;
}

uint32_t mix(uint32_t v) { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v; }

std::string readText(const std::string& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

BrushSettings ink(double diameter, double hardness = 1) {
    BrushSettings s;
    s.diameter = diameter;
    s.hardness = hardness;
    s.red = 0.1; s.green = 0.15; s.blue = 0.35;
    return s;
}

Preset tipPreset(const std::string& name, const TipPreset& tip, double maxDiameter) {
    Preset p;
    p.name = name;
    p.engine = Preset::Engine::Tip;
    p.tip = tip;
    p.settings = ink(std::min(tip.diameter, maxDiameter));
    return p;
}

} // namespace

// ---- Fixtures ----------------------------------------------------------------------------------------------------

std::vector<StrokeFixture> standardFixtures() {
    std::vector<StrokeFixture> out;
    out.push_back(line("pressure_ramp", 160, 30, 100, 290, 100, [](BrushSample& s, double u) { s.pressure = 1 - std::fabs(2 * u - 1); }));
    out.push_back(line("pressure_sine", 160, 30, 100, 290, 100, [](BrushSample& s, double u) { s.pressure = 0.5 + 0.45 * std::sin(4 * pi * u); }));
    {
        // Slow, fast, slow: evenly timed samples whose spacing follows the speed.
        StrokeFixture f;
        f.name = "speed_sweep";
        const int n = 120;
        std::vector<double> speed(size_t(n) + 1);
        double total = 0;
        for (int i = 0; i <= n; i++) { speed[size_t(i)] = 0.1 + std::pow(std::sin(pi * i / n), 2); if (i) total += speed[size_t(i)]; }
        double walked = 0;
        for (int i = 0; i <= n; i++) {
            if (i) walked += speed[size_t(i)];
            f.samples.push_back(at(i * tick, 30 + 260 * walked / total, 100, 0.7));
        }
        out.push_back(f);
    }
    out.push_back(line("tilt_sweep", 160, 30, 100, 290, 100, [](BrushSample& s, double u) {
        s.pressure = 0.7;
        s.tiltX = 60 * std::sin(pi * u);
        s.tiltY = -25 * std::sin(pi * u);
    }));
    out.push_back(line("twist_sweep", 160, 30, 100, 290, 100, [](BrushSample& s, double u) {
        s.pressure = 0.7;
        s.twist = std::remainder(360 * u, 360.0);   // a full turn, wrapping from 180 to -180 halfway
        s.twistReported = true;
    }));
    out.push_back(line("straight_line", 130, 30, 60, 290, 140, [](BrushSample& s, double) { s.pressure = 0.8; }));
    {
        StrokeFixture f;
        f.name = "circle";
        const int n = 180;
        for (int i = 0; i <= n; i++) {
            const double a = 2 * pi * i / n;
            f.samples.push_back(at(i * tick, 160 + 70 * std::cos(a), 100 + 70 * std::sin(a), 0.7));
        }
        out.push_back(f);
    }
    {
        StrokeFixture f;
        f.name = "s_curve";
        const int n = 150;
        for (int i = 0; i <= n; i++) {
            const double u = double(i) / n;
            f.samples.push_back(at(i * tick, 30 + 260 * u, 100 - 60 * std::sin(2 * pi * u), 0.6 + 0.3 * std::sin(pi * u)));
        }
        out.push_back(f);
    }
    out.push_back(polyline("corners", {{30, 160}, {100, 40}, {170, 160}, {240, 40}, {290, 160}}, 3, 0.75));
    {
        // Ten reports over 240 pixels, easing out as the pressure lifts.
        StrokeFixture f;
        f.name = "fast_flick";
        const int n = 9;
        for (int i = 0; i <= n; i++) {
            const double u = double(i) / n, eased = 1 - (1 - u) * (1 - u);
            f.samples.push_back(at(i * tick, 40 + 240 * eased, 150 - 100 * eased, 0.9 - 0.85 * u));
        }
        out.push_back(f);
    }
    {
        StrokeFixture f;
        f.name = "long_slow";
        const int n = 720;
        for (int i = 0; i <= n; i++) {
            const double u = double(i) / n;
            f.samples.push_back(at(i * tick, 20 + 280 * u, 100 + 50 * std::sin(3 * pi * u), 0.5 + 0.3 * std::sin(5 * pi * u)));
        }
        out.push_back(f);
    }
    // The pen held at 45 degrees from upright while the way it leans goes once round.
    out.push_back(line("azimuth_sweep", 160, 30, 100, 290, 100, [](BrushSample& s, double u) {
        s.pressure = 0.7;
        s.tiltX = 45 * std::cos(2 * pi * u);
        s.tiltY = 45 * std::sin(2 * pi * u);
    }));
    // The barrel turned from 340 through 359, 0 and 1 to 20 degrees, as a pen that reports 0..360 does.
    out.push_back(line("twist_wrap", 160, 30, 100, 290, 100, [](BrushSample& s, double u) {
        s.pressure = 0.7;
        s.twist = std::fmod(340 + 40 * u, 360.0);
        s.twistReported = true;
    }));
    return out;
}

std::optional<StrokeFixture> fixtureFromJson(const std::string& text, const std::string& name, std::string* error) {
    auto stroke = recordedStrokeFromJson(text, error);
    if (stroke && stroke->name.empty()) stroke->name = name;
    return stroke;
}

std::optional<StrokeFixture> loadFixture(const std::string& path, std::string* error) {
    if (!fs::exists(path)) { if (error) *error = "no file " + path; return std::nullopt; }
    return fixtureFromJson(readText(path), fs::path(path).stem().string(), error);
}

std::string fixtureToJson(const StrokeFixture& fixture) { return recordedStrokeToJson(fixture); }

std::vector<StrokeFixture> fileFixtures(const std::string& folder) {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec))
        if (entry.path().extension() == ".json") files.push_back(entry.path().string());
    std::sort(files.begin(), files.end());
    std::vector<StrokeFixture> out;
    for (const std::string& file : files)
        if (auto f = loadFixture(file)) out.push_back(*f);
    return out;
}

// ---- Presets -----------------------------------------------------------------------------------------------------

std::vector<Preset> standardPresets(const std::string& myPaintFolder) {
    std::vector<Preset> out;
    auto round = [&](const std::string& name, double diameter, double hardness, bool erasing) {
        Preset p;
        p.name = name;
        p.settings = ink(diameter, hardness);
        p.settings.erasing = erasing;
        out.push_back(p);
    };
    round("round_hard", 16, 1, false);
    round("round_soft", 24, 0.2, false);
    round("eraser", 20, 0.5, true);
    {
        TipPreset square;
        auto shape = std::make_shared<GrayImage>(48, 48, 255);
        for (int y = 0; y < 24; y++) for (int x = 0; x < 24; x++) shape->at(x, y) = 0;
        square.tip.shape = shape;
        square.tip.spacing = 0.15;
        square.tip.followStroke = true;
        square.diameter = 20;
        out.push_back(tipPreset("tip_square", square, 40));
    }
    {
        TipPreset textured;
        textured.tip.shape = radialTip(64);
        auto grain = std::make_shared<GrayImage>(32, 32, 0);
        for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) grain->at(x, y) = uint8_t(mix(uint32_t(y * 32 + x)) & 255);
        textured.tip.grain = grain;
        textured.tip.spacing = 0.1;
        textured.tip.dynamics = legacyDynamics({.sizeJitter = 0.4, .flowJitter = 0.3, .angleJitter = 60, .pressureSize = 1, .minimumSize = 0.2, .pressureFlow = 0.5});
        textured.tip.scatter = 0.5;
        textured.tip.count = 2;
        textured.tip.roundness = 0.6;
        textured.diameter = 26;
        out.push_back(tipPreset("tip_textured", textured, 40));
    }
    {
        // Every pen input on something: tilt flattens the tip and turns it, the barrel turns it too, speed thins the
        // flow, the stroke fades in size along its length, and chance varies each dab's opacity.
        TipPreset pen;
        auto shape = std::make_shared<GrayImage>(48, 16, 255);
        pen.tip.shape = shape;
        pen.tip.spacing = 0.08;
        auto smooth = dynamicsMapping(DynamicsInput::Pressure, DynamicsTarget::Size, 0.3, 0.7);
        smooth.curve = {DynamicsCurve::Kind::Smooth, {{0, 0}, {0.5, 0.8}, {1, 1}}};
        pen.tip.dynamics = {smooth,
                            dynamicsMapping(DynamicsInput::StrokeProgress, DynamicsTarget::Size, 1, -0.5),
                            dynamicsMapping(DynamicsInput::Tilt, DynamicsTarget::Roundness, 1, -0.7),
                            dynamicsMapping(DynamicsInput::TiltDirection, DynamicsTarget::Angle, 0, 360),
                            dynamicsMapping(DynamicsInput::Twist, DynamicsTarget::Angle, 0, 360),
                            dynamicsMapping(DynamicsInput::Speed, DynamicsTarget::Flow, 1, -0.6, 600),
                            dynamicsMapping(DynamicsInput::Random, DynamicsTarget::Opacity, 1, -0.5)};
        pen.diameter = 24;
        out.push_back(tipPreset("tip_pen_dynamics", pen, 40));
        // Tight spacing at light flow with density by spacing, and a mouse's speed as its pressure.
        TipPreset dense;
        dense.tip.shape = radialTip(64);
        dense.tip.spacing = 0.04;
        dense.tip.flow = 0.35;
        dense.tip.densityBySpacing = true;
        dense.tip.mousePressureFromSpeed = true;
        dense.tip.dynamics = {dynamicsMapping(DynamicsInput::Pressure, DynamicsTarget::Size, 0.2, 0.8)};
        dense.diameter = 22;
        out.push_back(tipPreset("tip_density_speed", dense, 40));
    }
    // What the importers make of the files their own tests write.
    if (auto abr = importBrushFile(brushfixtures::writeTemp("parity-v6.abr", brushfixtures::abrVersion6File())); abr && !abr->brushes.empty())
        out.push_back(tipPreset("abr_leaf", abr->brushes[0], 28));
    if (auto set = importBrushFile(brushfixtures::writeTemp("parity.brushset", brushfixtures::procreateBrushsetFile())); set && set->brushes.size() == 2)
        out.push_back(tipPreset("procreate_soft_ink", set->brushes[1], 28));
#ifdef COMPOSITOR_HAVE_SQLITE
    {
        const fs::path path = fs::temp_directory_path() / "parity.sut";
        if (brushfixtures::writeClipStudioFile(path))
            if (auto sut = importBrushFile(path.string()); sut && sut->brushes.size() == 2) {
                out.push_back(tipPreset("sut_pencil", sut->brushes[0], 28));
                out.push_back(tipPreset("sut_spray", sut->brushes[1], 28));
            }
        fs::remove(path);
    }
#endif
    if (myPaintSupported())
        for (const char* name : {"classic/pencil", "classic/charcoal", "classic/dry_brush", "classic/calligraphy"}) {
            const std::string file = myPaintFolder + "/" + name + ".myb";
            if (!fs::exists(file)) continue;
            Preset p;
            p.name = std::string("mypaint_") + fs::path(name).filename().string();
            p.engine = Preset::Engine::MyPaint;
            p.myPaintJson = readText(file);
            p.settings = ink(16);
            out.push_back(p);
        }
    return out;
}

// ---- Rendering ---------------------------------------------------------------------------------------------------

namespace {

/// The paper a fixture is painted on: transparent, or an opaque grey for the eraser.
std::shared_ptr<Image> paper(const Preset& preset) {
    auto base = std::make_shared<Image>(canvasWidth, canvasHeight);
    if (preset.settings.erasing) base->fill(128, 128, 128, 255);
    return base;
}

/// Paints the fixture into `grid` with the preset's engine; false when the engine could not start.
bool paint(BrushStroke& grid, const StrokeFixture& fixture, const Preset& preset) {
    if (!grid.isValid() || fixture.samples.empty()) return false;
    // Every engine takes the same samples, derived as they arrive, as the canvas feeds them.
    BrushSampleTrack track;
    switch (preset.engine) {
    case Preset::Engine::Round:
        for (const BrushSample& s : fixture.samples) grid.append(track.add(s));
        break;
    case Preset::Engine::Tip: {
        TipStroke stroke(grid, preset.tip->tip, preset.settings.diameter, preset.seed);
        if (!stroke.isValid()) return false;
        for (const BrushSample& s : fixture.samples) stroke.strokeTo(track.add(s));
        break;
    }
    case Preset::Engine::MyPaint: {
        MyPaintStroke stroke(grid, preset.myPaintJson, preset.settings);
        if (!stroke.isValid()) return false;
        for (const BrushSample& s : fixture.samples) stroke.strokeTo(track.add(s));
        stroke.finish();
        break;
    }
    }
    grid.flush();
    return true;
}

/// The alpha a stroke laid down (or took away) per pixel of an 8-bit layer.
std::vector<uint8_t> paintOf(const Image& image, bool erasing) {
    std::vector<uint8_t> out(size_t(canvasWidth) * canvasHeight, 0);
    for (int y = 0; y < canvasHeight; y++)
        for (int x = 0; x < canvasWidth; x++) {
            const uint8_t a = image.pixel(x, y)[3];
            out[size_t(y) * canvasWidth + size_t(x)] = erasing ? uint8_t(255 - a) : a;
        }
    return out;
}

} // namespace

Render16 render16(const StrokeFixture& fixture, const Preset& preset) {
    Render16 out;
    Layer layer(Asset::make(Image16Ptr(widenImage(*paper(preset))), "Paper"), Point(0, 0));
    BrushStroke grid(layer, false, preset.settings, Size(canvasWidth, canvasHeight), SampleType::U16, nullptr);
    if (!paint(grid, fixture, preset)) return out;
    const Image16Ptr preview = grid.previewImage16();
    if (!preview || preview->width() != canvasWidth || preview->height() != canvasHeight) return out;
    out.image = std::make_shared<Image16>(*preview);
    out.eight.image = narrowImage(*preview);
    out.eight.paint = paintOf(*out.eight.image, preset.settings.erasing);
    return out;
}

Calibration compare(const Render& eight, const Render16& deep) {
    Calibration out;
    if (!eight.image || !deep.eight.image) return {256, 1};
    long beyond = 0, total = 0;
    for (int y = 0; y < canvasHeight; y++)
        for (int x = 0; x < canvasWidth; x++)
            for (int c = 0; c < 4; c++, total++) {
                const int d = std::abs(int(eight.image->pixel(x, y)[c]) - int(deep.eight.image->pixel(x, y)[c]));
                out.worst = std::max(out.worst, d);
                beyond += d > 1;
            }
    out.beyondOne = double(beyond) / double(total);
    return out;
}

Render render(const StrokeFixture& fixture, const Preset& preset) {
    Render out;
    Layer layer(Asset::make(paper(preset), "Paper"), Point(0, 0));
    BrushStroke grid(layer, false, preset.settings, Size(canvasWidth, canvasHeight));
    if (!paint(grid, fixture, preset)) return out;
    const ImagePtr preview = grid.previewImage();
    if (!preview || preview->width() != canvasWidth || preview->height() != canvasHeight) return out;
    out.image = std::make_shared<Image>(*preview);
    out.paint = paintOf(*out.image, preset.settings.erasing);
    return out;
}

// ---- Measuring ---------------------------------------------------------------------------------------------------

Metrics measure(const StrokeFixture& fixture, const Render& render) {
    Metrics m;
    if (render.paint.empty()) return m;
    auto at = [&](int x, int y) -> double {
        if (x < 0 || y < 0 || x >= canvasWidth || y >= canvasHeight) return 0;
        return render.paint[size_t(y) * canvasWidth + size_t(x)] / 255.0;
    };
    int x0 = canvasWidth, y0 = canvasHeight, x1 = -1, y1 = -1, painted = 0;
    for (int y = 0; y < canvasHeight; y++)
        for (int x = 0; x < canvasWidth; x++) {
            const double a = at(x, y);
            if (a <= 0) continue;
            m.total += a;
            painted++;
            x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
        }
    if (painted) { m.boxX = x0; m.boxY = y0; m.boxWidth = x1 - x0 + 1; m.boxHeight = y1 - y0 + 1; m.mean = m.total / painted; }

    // Stations along the path, by arc length.
    const auto& s = fixture.samples;
    std::vector<double> along(s.size(), 0);
    for (size_t i = 1; i < s.size(); i++) along[i] = along[i - 1] + std::hypot(s[i].position.x - s[i - 1].position.x, s[i].position.y - s[i - 1].position.y);
    const double length = along.back();
    auto pointAt = [&](double d) {
        d = std::clamp(d, 0.0, length);
        size_t i = size_t(std::upper_bound(along.begin(), along.end(), d) - along.begin());
        i = std::clamp<size_t>(i, 1, s.size() - 1);
        const double span = along[i] - along[i - 1], u = span > 0 ? (d - along[i - 1]) / span : 0;
        return Point{s[i - 1].position.x + (s[i].position.x - s[i - 1].position.x) * u, s[i - 1].position.y + (s[i].position.y - s[i - 1].position.y) * u};
    };
    double edgeSum = 0;
    int edgeCount = 0;
    for (int k = 0; k < 10; k++) {
        double width = 0, peak = 0;
        if (length > 0) {
            const double d = length * (0.05 + 0.1 * k);
            const Point c = pointAt(d), before = pointAt(d - 3), after = pointAt(d + 3);
            double tx = after.x - before.x, ty = after.y - before.y;
            const double tl = std::hypot(tx, ty);
            if (tl > 0) {
                tx /= tl; ty /= tl;
                const double nx = -ty, ny = tx;
                constexpr int reach = 80;   // half-pixel steps either way: 40 pixels
                std::vector<double> profile(2 * reach + 1);
                for (int j = -reach; j <= reach; j++)
                    profile[size_t(j + reach)] = at(int(std::floor(c.x + nx * j * 0.5)), int(std::floor(c.y + ny * j * 0.5)));
                // The run of 10% or more nearest the path.
                int seed = -1;
                for (int r = 0; r <= reach / 2 && seed < 0; r++)
                    for (int j : {reach + r, reach - r}) if (profile[size_t(j)] >= 0.1) { seed = j; break; }
                if (seed >= 0) {
                    int lo = seed, hi = seed;
                    while (lo > 0 && profile[size_t(lo - 1)] >= 0.1) lo--;
                    while (hi < 2 * reach && profile[size_t(hi + 1)] >= 0.1) hi++;
                    width = (hi - lo + 1) * 0.5;
                    int top = lo;
                    for (int j = lo; j <= hi; j++) if (profile[size_t(j)] > profile[size_t(top)]) top = j;
                    peak = profile[size_t(top)];
                    // The fall from 90% to 10% of the peak, on each side.
                    for (int dir : {-1, 1}) {
                        int j = top, j90 = -1, j10 = -1;
                        while (j >= 0 && j <= 2 * reach) {
                            if (j90 < 0 && profile[size_t(j)] <= 0.9 * peak) j90 = j;
                            if (profile[size_t(j)] <= 0.1 * peak) { j10 = j; break; }
                            j += dir;
                        }
                        if (j90 >= 0 && j10 >= 0) { edgeSum += std::abs(j10 - j90) * 0.5; edgeCount++; }
                    }
                }
            }
        }
        m.width.push_back(width);
        m.peak.push_back(peak);
    }
    if (edgeCount) m.edge = edgeSum / edgeCount;
    std::vector<double> sorted = m.width;
    std::sort(sorted.begin(), sorted.end());
    const double median = (sorted[4] + sorted[5]) / 2;
    if (median > 0) { m.startTaper = m.width.front() / median; m.endTaper = m.width.back() / median; }
    return m;
}

std::string formatMetrics(const Metrics& m) {
    char buffer[160];
    std::string out;
    std::snprintf(buffer, sizeof buffer, "box=%d,%d,%d,%d total=%.1f mean=%.4f width=", m.boxX, m.boxY, m.boxWidth, m.boxHeight, m.total, m.mean);
    out += buffer;
    for (size_t i = 0; i < m.width.size(); i++) { std::snprintf(buffer, sizeof buffer, "%s%.1f", i ? "/" : "", m.width[i]); out += buffer; }
    out += " peak=";
    for (size_t i = 0; i < m.peak.size(); i++) { std::snprintf(buffer, sizeof buffer, "%s%.2f", i ? "/" : "", m.peak[i]); out += buffer; }
    std::snprintf(buffer, sizeof buffer, " edge=%.2f taper=%.2f,%.2f", m.edge, m.startTaper, m.endTaper);
    out += buffer;
    return out;
}

uint64_t hashRender(const Render& render) {
    uint64_t h = 1469598103934665603ull;
    auto bytes = [&](const uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } };
    if (!render.image) return 0;
    const uint32_t size[2] = {uint32_t(render.image->width()), uint32_t(render.image->height())};
    for (uint32_t v : size) { const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; bytes(b, 4); }
    for (int y = 0; y < render.image->height(); y++) bytes(render.image->row(y), size_t(render.image->width()) * 4);
    return h;
}

uint64_t hashRender(const Render16& render) {
    uint64_t h = 1469598103934665603ull;
    auto bytes = [&](const uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } };
    if (!render.image) return 0;
    const uint32_t size[2] = {uint32_t(render.image->width()), uint32_t(render.image->height())};
    for (uint32_t v : size) { const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; bytes(b, 4); }
    for (int y = 0; y < render.image->height(); y++) {
        const uint16_t* row = render.image->row(y);
        for (int i = 0; i < render.image->width() * 4; i++) { const uint8_t b[2] = {uint8_t(row[i]), uint8_t(row[i] >> 8)}; bytes(b, 2); }
    }
    return h;
}

std::string hex(uint64_t value) { char b[17]; std::snprintf(b, sizeof b, "%016" PRIx64, value); return b; }

std::vector<Scene> scenes(const std::vector<StrokeFixture>& fixtures, const std::vector<Preset>& presets) {
    std::vector<Scene> out;
    for (const StrokeFixture& f : fixtures)
        for (const Preset& p : presets) out.push_back({f.name + "/" + p.name, &f, &p});
    return out;
}

// ---- Brushes checked against expectations ---------------------------------------------------------------------------

namespace {

std::optional<nlohmann::json> readJson(const fs::path& path) {
    if (!fs::exists(path)) return std::nullopt;
    nlohmann::json j = nlohmann::json::parse(readText(path.string()), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    return j;
}

std::string field(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

const Preset* presetNamed(const FixtureBrushes& brushes, const std::string& name) {
    for (const Preset& p : brushes.presets) if (p.name == name) return &p;
    return nullptr;
}

const StrokeFixture* fixtureNamed(const std::vector<StrokeFixture>& fixtures, const std::string& name) {
    for (const StrokeFixture& f : fixtures) if (f.name == name) return &f;
    return nullptr;
}

/// A stroke's metric where its input is high (stations 4 and 5) over where it is low (0 and 9).
double highOverLow(const std::vector<double>& v) {
    if (v.size() < 10) return 0;
    return ((v[4] + v[5]) / 2 + 1e-3) / ((v[0] + v[9]) / 2 + 1e-3);
}

double range(const std::vector<double>& v) {
    if (v.empty()) return 0;
    return *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end());
}

double largestStep(const std::vector<double>& v) {
    double out = 0;
    for (size_t i = 1; i < v.size(); i++) out = std::max(out, std::fabs(v[i] - v[i - 1]));
    return out;
}

} // namespace

FixtureBrushes syntheticProcreate(const std::string& folder) {
    FixtureBrushes out;
    const auto manifest = readJson(fs::path(folder) / "manifest.json");
    if (!manifest) return out;
    const std::string baselineFile = field(*manifest, "baseline");
    auto stem = [](const std::string& file) { return fs::path(file).stem().string(); };
    auto load = [&](const std::string& file) {
        if (presetNamed(out, stem(file))) return;
        const std::string path = (fs::path(folder) / file).string();
        auto import = importBrushFile(path);
        if (!import || import->brushes.empty()) return;
        for (const std::string& note : import->notes) out.notes.push_back(file + ": " + note);
        Preset p = tipPreset(stem(file), import->brushes[0], 40);
        p.seed = 99;
        out.presets.push_back(std::move(p));
    };
    load(baselineFile);
    auto brushes = manifest->find("brushes");
    if (brushes == manifest->end() || !brushes->is_object()) return out;
    for (auto it = brushes->begin(); it != brushes->end(); ++it) {
        load(it.key());
        std::vector<const nlohmann::json*> checks = {&it.value()};
        if (auto also = it.value().find("also"); also != it.value().end() && also->is_array())
            for (const nlohmann::json& a : *also) if (a.is_object()) checks.push_back(&a);
        for (const nlohmann::json* c : checks) {
            Expectation e;
            e.brush = stem(it.key());
            e.stroke = field(*c, "stroke");
            e.measure = field(*c, "measure");
            e.expect = field(*c, "expect");
            e.against = stem(field(*c, "against").empty() ? baselineFile : field(*c, "against"));
            e.weakerThan = field(*c, "weakerThan").empty() ? std::string() : stem(field(*c, "weakerThan"));
            e.setting = field(it.value(), "setting");
            out.expectations.push_back(e);
        }
    }
    for (const Expectation& e : out.expectations) { load(e.against + ".brush"); if (!e.weakerThan.empty()) load(e.weakerThan + ".brush"); }
    return out;
}

std::optional<FixtureBrushes> localFixtures(const std::string& folder) {
    const auto manifest = readJson(fs::path(folder) / "manifest.json");
    if (!manifest) return std::nullopt;
    FixtureBrushes out;
    std::map<std::string, std::optional<BrushImport>> sets;
    auto brushes = manifest->find("brushes");
    if (brushes == manifest->end() || !brushes->is_array()) return out;
    for (const nlohmann::json& b : *brushes) {
        const std::string set = field(b, "set"), name = field(b, "name");
        if (!sets.count(set)) {
            const fs::path path = fs::path(folder) / set;
            sets[set] = fs::exists(path) ? importBrushFile(path.string()) : std::nullopt;
            if (sets[set]) for (const std::string& note : sets[set]->notes) out.notes.push_back(set + ": " + note);
            else out.notes.push_back(set + ": not found or not readable");
        }
        if (!sets[set]) continue;
        const TipPreset* found = nullptr;
        for (const TipPreset& t : sets[set]->brushes) if (t.name.find(name) != std::string::npos) { found = &t; break; }
        if (!found) { out.notes.push_back(set + ": no brush named " + name); continue; }
        const auto input = dynamicsInputFromName(field(b, "input"));
        // 28 pixels unless the entry asks for more: a spacing of a hundredth of the size needs a larger dab to step.
        auto size = b.find("diameter");
        const double diameter = size != b.end() && size->is_number() ? size->get<double>() : 28.0;
        Expectation e;
        e.brush = "local_" + name + (diameter != 28 ? "@" + std::to_string(int(diameter)) : std::string());
        e.stroke = field(b, "stroke");
        e.measure = field(b, "measure");
        e.expect = field(b, "expect");
        e.setting = field(b, "setting");
        e.against = e.brush + "~no_" + field(b, "input");
        if (!presetNamed(out, e.brush)) {
            Preset p = tipPreset(e.brush, *found, diameter);
            p.seed = 99;
            out.presets.push_back(p);
        }
        if (!presetNamed(out, e.against)) {
            // The control: the same brush without the mappings from that input.
            Preset control = *presetNamed(out, e.brush);
            control.name = e.against;
            auto& d = control.tip->tip.dynamics;
            if (input) d.erase(std::remove_if(d.begin(), d.end(), [&](const DynamicsMapping& m) { return m.input == *input; }), d.end());
            out.presets.push_back(control);
        }
        out.expectations.push_back(e);
    }
    return out;
}

std::vector<Scene> expectationScenes(const std::vector<StrokeFixture>& fixtures, const FixtureBrushes& brushes) {
    std::vector<Scene> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& stroke, const std::string& brush) {
        const StrokeFixture* f = fixtureNamed(fixtures, stroke);
        const Preset* p = presetNamed(brushes, brush);
        if (!f || !p || !seen.insert(stroke + "/" + brush).second) return;
        out.push_back({stroke + "/" + brush, f, p});
    };
    for (const Expectation& e : brushes.expectations) {
        add(e.stroke, e.brush);
        add(e.stroke, e.against);
        if (!e.weakerThan.empty()) add(e.stroke, e.weakerThan);
    }
    return out;
}

bool checkExpectation(const Expectation& e, const std::vector<StrokeFixture>& fixtures, const FixtureBrushes& brushes, std::string* why) {
    const StrokeFixture* stroke = fixtureNamed(fixtures, e.stroke);
    const Preset* brush = presetNamed(brushes, e.brush);
    const Preset* against = presetNamed(brushes, e.against);
    if (!stroke || !brush || !against) { if (why) *why = "missing stroke or brush"; return false; }
    const Render a = render(*stroke, *brush), b = render(*stroke, *against);
    const Metrics ma = measure(*stroke, a), mb = measure(*stroke, b);
    char buffer[256];
    if (e.measure == "render" || e.expect == "differs") {
        const bool differs = hashRender(a) != hashRender(b);
        if (why) *why = differs ? "the render differs" : "the render is the same";
        return differs;
    }
    const std::vector<double>& va = e.measure == "peak" ? ma.peak : ma.width;
    const std::vector<double>& vb = e.measure == "peak" ? mb.peak : mb.width;
    if (e.expect == "turns") {
        std::snprintf(buffer, sizeof buffer, "%s swings %.1f against %.1f", e.measure.c_str(), range(va), range(vb));
        if (why) *why = buffer;
        return range(va) > range(vb) + 1.5;
    }
    if (e.expect == "steady") {
        std::snprintf(buffer, sizeof buffer, "%s swings %.1f against %.1f", e.measure.c_str(), range(va), range(vb));
        if (why) *why = buffer;
        return range(va) + 1.5 < range(vb);
    }
    if (e.expect == "continuous") {
        std::snprintf(buffer, sizeof buffer, "largest %s step between stations %.2f (against %.2f)", e.measure.c_str(), largestStep(va), largestStep(vb));
        if (why) *why = buffer;
        return largestStep(va) <= largestStep(vb) + 2.0;
    }
    const double ra = highOverLow(va), rb = highOverLow(vb);
    std::snprintf(buffer, sizeof buffer, "%s high/low %.3f against %.3f", e.measure.c_str(), ra, rb);
    bool ok = e.expect == "up" ? ra > rb * 1.02 : e.expect == "down" ? ra < rb * 0.98 : false;
    std::string detail = buffer;
    if (ok && !e.weakerThan.empty()) {
        const Preset* stronger = presetNamed(brushes, e.weakerThan);
        if (!stronger) { if (why) *why = "missing " + e.weakerThan; return false; }
        const Metrics ms = measure(*stroke, render(*stroke, *stronger));
        // Where the input is low (stations 0 and 9) this brush stays nearer the one it is compared with.
        const std::vector<double>& vs = e.measure == "peak" ? ms.peak : ms.width;
        const double low = (va[0] + va[9]) / 2, lowAgainst = (vb[0] + vb[9]) / 2, lowStronger = (vs[0] + vs[9]) / 2;
        std::snprintf(buffer, sizeof buffer, "; where low %.2f against %.2f, %.2f for %s", low, lowAgainst, lowStronger, e.weakerThan.c_str());
        detail += buffer;
        ok = std::fabs(low - lowAgainst) < std::fabs(lowStronger - lowAgainst);
    }
    if (why) *why = detail;
    return ok;
}

} // namespace brushharness
