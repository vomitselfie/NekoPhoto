#include "brush_harness.h"
#include "brush_import_fixtures.h"
#include "compositor/brushimport.h"
#include "compositor/mypaint.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace brushharness {

namespace fs = std::filesystem;

namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double tick = 1.0 / 120;   // a tablet's report rate

/// `n` + 1 samples along a straight line, `tick` apart, with `fill` setting the pen at u = 0..1.
StrokeFixture line(const std::string& name, int n, double x0, double y0, double x1, double y1,
                   const std::function<void(StrokeSample&, double)>& fill) {
    StrokeFixture f;
    f.name = name;
    for (int i = 0; i <= n; i++) {
        const double u = double(i) / n;
        StrokeSample s;
        s.t = i * tick;
        s.x = x0 + (x1 - x0) * u;
        s.y = y0 + (y1 - y0) * u;
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
            f.samples.push_back({i++ * tick, a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, pressure});
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
    out.push_back(line("pressure_ramp", 160, 30, 100, 290, 100, [](StrokeSample& s, double u) { s.pressure = 1 - std::fabs(2 * u - 1); }));
    out.push_back(line("pressure_sine", 160, 30, 100, 290, 100, [](StrokeSample& s, double u) { s.pressure = 0.5 + 0.45 * std::sin(4 * pi * u); }));
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
            f.samples.push_back({i * tick, 30 + 260 * walked / total, 100, 0.7});
        }
        out.push_back(f);
    }
    out.push_back(line("tilt_sweep", 160, 30, 100, 290, 100, [](StrokeSample& s, double u) {
        s.pressure = 0.7;
        s.tiltX = 60 * std::sin(pi * u);
        s.tiltY = -25 * std::sin(pi * u);
    }));
    out.push_back(line("twist_sweep", 160, 30, 100, 290, 100, [](StrokeSample& s, double u) {
        s.pressure = 0.7;
        s.twist = std::remainder(360 * u, 360.0);   // a full turn, wrapping from 180 to -180 halfway
    }));
    out.push_back(line("straight_line", 130, 30, 60, 290, 140, [](StrokeSample& s, double) { s.pressure = 0.8; }));
    {
        StrokeFixture f;
        f.name = "circle";
        const int n = 180;
        for (int i = 0; i <= n; i++) {
            const double a = 2 * pi * i / n;
            f.samples.push_back({i * tick, 160 + 70 * std::cos(a), 100 + 70 * std::sin(a), 0.7});
        }
        out.push_back(f);
    }
    {
        StrokeFixture f;
        f.name = "s_curve";
        const int n = 150;
        for (int i = 0; i <= n; i++) {
            const double u = double(i) / n;
            f.samples.push_back({i * tick, 30 + 260 * u, 100 - 60 * std::sin(2 * pi * u), 0.6 + 0.3 * std::sin(pi * u)});
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
            f.samples.push_back({i * tick, 40 + 240 * eased, 150 - 100 * eased, 0.9 - 0.85 * u});
        }
        out.push_back(f);
    }
    {
        StrokeFixture f;
        f.name = "long_slow";
        const int n = 720;
        for (int i = 0; i <= n; i++) {
            const double u = double(i) / n;
            f.samples.push_back({i * tick, 20 + 280 * u, 100 + 50 * std::sin(3 * pi * u), 0.5 + 0.3 * std::sin(5 * pi * u)});
        }
        out.push_back(f);
    }
    return out;
}

std::optional<StrokeFixture> fixtureFromJson(const std::string& text, const std::string& name, std::string* error) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) { if (error) *error = "not JSON"; return std::nullopt; }
    StrokeFixture f;
    f.name = name;
    const nlohmann::json* list = &j;
    if (j.is_object()) {
        auto it = j.find("samples");
        if (it == j.end() || !it->is_array()) { if (error) *error = "no samples array"; return std::nullopt; }
        list = &*it;
        if (auto s = j.find("stylus"); s != j.end() && s->is_boolean()) f.stylus = s->get<bool>();
        if (auto n = j.find("name"); n != j.end() && n->is_string()) f.name = n->get<std::string>();
    }
    if (!list->is_array()) { if (error) *error = "samples must be an array"; return std::nullopt; }
    for (const auto& item : *list) {
        if (!item.is_object()) { if (error) *error = "a sample is not an object"; return std::nullopt; }
        auto number = [&](const char* key, double fallback) { auto it = item.find(key); return it != item.end() && it->is_number() ? it->get<double>() : fallback; };
        StrokeSample s;
        s.t = number("t", 0);
        s.x = number("x", 0);
        s.y = number("y", 0);
        s.pressure = number("pressure", 1);
        s.tiltX = number("tiltX", 0);
        s.tiltY = number("tiltY", 0);
        s.twist = number("twist", 0);
        s.tangentialPressure = number("tangentialPressure", 0);
        f.samples.push_back(s);
    }
    if (f.samples.empty()) { if (error) *error = "no samples"; return std::nullopt; }
    return f;
}

std::optional<StrokeFixture> loadFixture(const std::string& path, std::string* error) {
    if (!fs::exists(path)) { if (error) *error = "no file " + path; return std::nullopt; }
    return fixtureFromJson(readText(path), fs::path(path).stem().string(), error);
}

std::string fixtureToJson(const StrokeFixture& fixture) {
    nlohmann::json samples = nlohmann::json::array();
    for (const StrokeSample& s : fixture.samples)
        samples.push_back({{"t", s.t}, {"x", s.x}, {"y", s.y}, {"pressure", s.pressure}, {"tiltX", s.tiltX}, {"tiltY", s.tiltY},
                           {"twist", s.twist}, {"tangentialPressure", s.tangentialPressure}});
    nlohmann::json j = {{"format", "nekophoto-stroke"}, {"version", 1}, {"name", fixture.name}, {"stylus", fixture.stylus}, {"samples", samples}};
    return j.dump(1);
}

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
        textured.tip.angleJitter = 60;
        textured.tip.sizeJitter = 0.4;
        textured.tip.scatter = 0.5;
        textured.tip.count = 2;
        textured.tip.flowJitter = 0.3;
        textured.tip.roundness = 0.6;
        textured.tip.pressureSize = 1;
        textured.tip.minimumSize = 0.2;
        textured.tip.pressureFlow = 0.5;
        textured.diameter = 26;
        out.push_back(tipPreset("tip_textured", textured, 40));
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

Render render(const StrokeFixture& fixture, const Preset& preset) {
    Render out;
    const bool erasing = preset.settings.erasing;
    auto base = std::make_shared<Image>(canvasWidth, canvasHeight);
    if (erasing) base->fill(128, 128, 128, 255);
    Layer layer(Asset::make(base, "Paper"), Point(0, 0));
    BrushStroke grid(layer, false, preset.settings, Size(canvasWidth, canvasHeight));
    if (!grid.isValid() || fixture.samples.empty()) return out;
    const bool stylus = fixture.stylus;
    switch (preset.engine) {
    case Preset::Engine::Round:
        for (const StrokeSample& s : fixture.samples) grid.append({s.x, s.y});
        break;
    case Preset::Engine::Tip: {
        TipStroke stroke(grid, preset.tip->tip, preset.settings.diameter, preset.seed);
        if (!stroke.isValid()) return out;
        for (const StrokeSample& s : fixture.samples) stroke.strokeTo({{s.x, s.y}, stylus ? s.pressure : 1.0});
        break;
    }
    case Preset::Engine::MyPaint: {
        MyPaintStroke stroke(grid, preset.myPaintJson, preset.settings);
        if (!stroke.isValid()) return out;
        for (size_t i = 0; i < fixture.samples.size(); i++) {
            const StrokeSample& s = fixture.samples[i];
            MyPaintInput input;
            input.document = {s.x, s.y};
            input.pressure = stylus ? s.pressure : 0.5;
            input.xtilt = stylus ? std::clamp(s.tiltX / 60.0, -1.0, 1.0) : 0;
            input.ytilt = stylus ? std::clamp(s.tiltY / 60.0, -1.0, 1.0) : 0;
            const double dt = i ? s.t - fixture.samples[i - 1].t : 0;
            input.seconds = dt > 0 ? dt : 1.0 / 120;
            stroke.strokeTo(input);
        }
        stroke.finish();
        break;
    }
    }
    grid.flush();
    const ImagePtr preview = grid.previewImage();
    if (!preview || preview->width() != canvasWidth || preview->height() != canvasHeight) return out;
    out.image = std::make_shared<Image>(*preview);
    out.paint.assign(size_t(canvasWidth) * canvasHeight, 0);
    for (int y = 0; y < canvasHeight; y++)
        for (int x = 0; x < canvasWidth; x++) {
            const uint8_t a = out.image->pixel(x, y)[3];
            out.paint[size_t(y) * canvasWidth + size_t(x)] = erasing ? uint8_t(255 - a) : a;
        }
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
    for (size_t i = 1; i < s.size(); i++) along[i] = along[i - 1] + std::hypot(s[i].x - s[i - 1].x, s[i].y - s[i - 1].y);
    const double length = along.back();
    auto pointAt = [&](double d) {
        d = std::clamp(d, 0.0, length);
        size_t i = size_t(std::upper_bound(along.begin(), along.end(), d) - along.begin());
        i = std::clamp<size_t>(i, 1, s.size() - 1);
        const double span = along[i] - along[i - 1], u = span > 0 ? (d - along[i - 1]) / span : 0;
        return Point{s[i - 1].x + (s[i].x - s[i - 1].x) * u, s[i - 1].y + (s[i].y - s[i - 1].y) * u};
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

std::string hex(uint64_t value) { char b[17]; std::snprintf(b, sizeof b, "%016" PRIx64, value); return b; }

std::vector<Scene> scenes(const std::vector<StrokeFixture>& fixtures, const std::vector<Preset>& presets) {
    std::vector<Scene> out;
    for (const StrokeFixture& f : fixtures)
        for (const Preset& p : presets) out.push_back({f.name + "/" + p.name, &f, &p});
    return out;
}

} // namespace brushharness
