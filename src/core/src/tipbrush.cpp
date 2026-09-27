#include "compositor/tipbrush.h"
#include "compositor/parallel.h"
#include "compositor/png.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace compositor {

namespace fs = std::filesystem;

namespace {

constexpr double pi = 3.14159265358979323846;

/// Half the size, each pixel the mean of the 2x2 block it covers (the edge row or column repeats).
GrayImage halve(const GrayImage& src) {
    const int w = std::max(1, (src.width() + 1) / 2), h = std::max(1, (src.height() + 1) / 2);
    GrayImage out(w, h);
    for (int y = 0; y < h; y++) {
        const int y0 = std::min(2 * y, src.height() - 1), y1 = std::min(2 * y + 1, src.height() - 1);
        for (int x = 0; x < w; x++) {
            const int x0 = std::min(2 * x, src.width() - 1), x1 = std::min(2 * x + 1, src.width() - 1);
            out.at(x, y) = uint8_t((src.at(x0, y0) + src.at(x1, y0) + src.at(x0, y1) + src.at(x1, y1) + 2) / 4);
        }
    }
    return out;
}

/// Bilinear sample with transparent outside, at tip pixel coordinates (pixel centres at +0.5).
double sample(const GrayImage& image, double x, double y) {
    x -= 0.5; y -= 0.5;
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const double fx = x - ix, fy = y - iy;
    auto at = [&](int px, int py) -> double {
        return (px < 0 || py < 0 || px >= image.width() || py >= image.height()) ? 0.0 : image.at(px, py);
    };
    const double top = at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx;
    const double bottom = at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx;
    return top * (1 - fy) + bottom * fy;
}

} // namespace

bool BrushTip::normalize() {
    auto fix = [](double& v, double lo, double hi, double fallback) { v = std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; };
    fix(spacing, 0.01, 10, 0.25);
    fix(angle, -3600, 3600, 0);
    fix(roundness, 0.01, 1, 1);
    fix(scatter, 0, 10, 0);
    count = std::clamp(count, 1, 16);
    fix(flow, 0, 1, 1);
    fix(grainScale, 0.01, 100, 1);
    fix(grainDepth, 0, 1, 1);
    fix(densityReference, 0.01, 10, 0.25);
    // Mappings with numbers that mean nothing go; the rest keep their order, which is the order they multiply in.
    BrushDynamics kept;
    for (DynamicsMapping m : dynamics) {
        if (!std::isfinite(m.offset) || !std::isfinite(m.depth) || int(m.input) < 0 || int(m.input) > int(DynamicsInput::StrokeProgress)
            || int(m.target) < 0 || int(m.target) >= dynamicsTargetCount || kept.size() >= 64) continue;
        m.offset = std::clamp(m.offset, -3600.0, 3600.0);
        m.depth = std::clamp(m.depth, -3600.0, 3600.0);
        m.scale = std::isfinite(m.scale) ? std::clamp(m.scale, 0.0, 1e6) : 0;
        m.curve.normalize();
        kept.push_back(std::move(m));
    }
    dynamics = std::move(kept);
    if (grain && grain->isEmpty()) grain.reset();
    return shape && !shape->isEmpty() && shape->width() <= 8192 && shape->height() <= 8192;
}

// ---- Presets on disk ----------------------------------------------------------------------------

namespace {

nlohmann::json mappingToJson(const DynamicsMapping& m) {
    nlohmann::json j = {{"input", dynamicsInputName(m.input)}, {"target", dynamicsTargetName(m.target)}, {"offset", m.offset}, {"depth", m.depth}};
    if (m.scale > 0) j["scale"] = m.scale;
    if (!m.curve.isIdentity()) {
        nlohmann::json points = nlohmann::json::array();
        for (auto [x, y] : m.curve.points) points.push_back({x, y});
        j["curve"] = points;
        j["smooth"] = m.curve.kind == DynamicsCurve::Kind::Smooth;
    }
    return j;
}

std::optional<DynamicsMapping> mappingFromJson(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    auto text = [&](const char* key) { auto it = j.find(key); return it != j.end() && it->is_string() ? it->get<std::string>() : std::string(); };
    auto number = [&](const char* key, double fallback) { auto it = j.find(key); return it != j.end() && it->is_number() ? it->get<double>() : fallback; };
    const auto input = dynamicsInputFromName(text("input"));
    const auto target = dynamicsTargetFromName(text("target"));
    if (!input || !target) return std::nullopt;
    DynamicsMapping m;
    m.input = *input;
    m.target = *target;
    m.offset = number("offset", 0);
    m.depth = number("depth", 1);
    m.scale = number("scale", 0);
    if (auto it = j.find("curve"); it != j.end() && it->is_array())
        for (const auto& p : *it)
            if (p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number()) m.curve.points.emplace_back(p[0].get<double>(), p[1].get<double>());
    if (auto it = j.find("smooth"); it != j.end() && it->is_boolean() && it->get<bool>()) m.curve.kind = DynamicsCurve::Kind::Smooth;
    return m;
}

} // namespace

std::optional<TipPreset> loadTipPreset(const std::string& folder, std::string* error) {
    const fs::path dir(folder);
    std::ifstream in(dir / "brush.json");
    if (!in) { if (error) *error = "no brush.json in " + folder; return std::nullopt; }
    std::stringstream text;
    text << in.rdbuf();
    nlohmann::json j = nlohmann::json::parse(text.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) { if (error) *error = "brush.json is not a JSON object"; return std::nullopt; }
    TipPreset preset;
    BrushTip& t = preset.tip;
    auto number = [&](const char* key, double fallback) { auto it = j.find(key); return it != j.end() && it->is_number() ? it->get<double>() : fallback; };
    auto boolean = [&](const char* key, bool fallback) { auto it = j.find(key); return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback; };
    if (auto it = j.find("name"); it != j.end() && it->is_string()) preset.name = it->get<std::string>();
    preset.diameter = std::clamp(number("diameter", 30), 1.0, 2000.0);
    t.spacing = number("spacing", t.spacing);
    t.angle = number("angle", t.angle);
    t.followStroke = boolean("followStroke", t.followStroke);
    t.roundness = number("roundness", t.roundness);
    t.scatter = number("scatter", t.scatter);
    t.scatterBothAxes = boolean("scatterBothAxes", t.scatterBothAxes);
    t.count = int(number("count", t.count));
    t.flow = number("flow", t.flow);
    t.flipX = boolean("flipX", t.flipX);
    t.flipY = boolean("flipY", t.flipY);
    t.randomFlipX = boolean("randomFlipX", t.randomFlipX);
    t.randomFlipY = boolean("randomFlipY", t.randomFlipY);
    t.grainScale = number("grainScale", t.grainScale);
    t.grainDepth = number("grainDepth", t.grainDepth);
    t.densityBySpacing = boolean("densityBySpacing", t.densityBySpacing);
    t.densityReference = number("densityReference", t.densityReference);
    t.mousePressureFromSpeed = boolean("mousePressureFromSpeed", t.mousePressureFromSpeed);
    if (auto it = j.find("dynamics"); it != j.end() && it->is_array()) {
        for (const auto& item : *it)
            if (auto m = mappingFromJson(item)) t.dynamics.push_back(std::move(*m));
    } else {
        // A preset saved before mappings (version 1): its pressure and jitter settings become mappings.
        auto unit = [&](const char* key, double hi) { const double v = number(key, 0); return std::isfinite(v) ? std::clamp(v, 0.0, hi) : 0.0; };
        LegacyTipDynamics legacy;
        legacy.sizeJitter = unit("sizeJitter", 1);
        legacy.flowJitter = unit("flowJitter", 1);
        legacy.angleJitter = unit("angleJitter", 180);
        legacy.pressureSize = unit("pressureSize", 1);
        legacy.minimumSize = unit("minimumSize", 1);
        legacy.pressureFlow = unit("pressureFlow", 1);
        t.dynamics = legacyDynamics(legacy);
    }
    t.shape = readPngGray((dir / "tip.png").string(), error);
    if (!t.shape) return std::nullopt;
    if (fs::exists(dir / "grain.png")) t.grain = readPngGray((dir / "grain.png").string());
    if (!t.normalize()) { if (error) *error = "the tip image is empty or larger than 8192 pixels"; return std::nullopt; }
    if (preset.name.empty()) preset.name = dir.filename().string();
    return preset;
}

bool saveTipPreset(const std::string& folder, const TipPreset& preset, std::string* error) {
    const fs::path dir(folder);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { if (error) *error = "cannot create " + folder + ": " + ec.message(); return false; }
    const BrushTip& t = preset.tip;
    if (!t.shape) { if (error) *error = "the preset has no tip"; return false; }
    nlohmann::json dynamics = nlohmann::json::array();
    for (const DynamicsMapping& m : t.dynamics) dynamics.push_back(mappingToJson(m));
    nlohmann::json j = {
        {"format", "compositor-tip-brush"}, {"version", 2}, {"name", preset.name}, {"diameter", preset.diameter},
        {"spacing", t.spacing}, {"angle", t.angle}, {"followStroke", t.followStroke}, {"roundness", t.roundness},
        {"scatter", t.scatter}, {"scatterBothAxes", t.scatterBothAxes}, {"count", t.count}, {"flow", t.flow},
        {"flipX", t.flipX}, {"flipY", t.flipY}, {"randomFlipX", t.randomFlipX}, {"randomFlipY", t.randomFlipY},
        {"grainScale", t.grainScale}, {"grainDepth", t.grainDepth}, {"dynamics", dynamics},
        {"densityBySpacing", t.densityBySpacing}, {"densityReference", t.densityReference},
        {"mousePressureFromSpeed", t.mousePressureFromSpeed}};
    std::ofstream out(dir / "brush.json");
    out << j.dump(2) << "\n";
    if (!out) { if (error) *error = "cannot write brush.json in " + folder; return false; }
    if (!writePngGray((dir / "tip.png").string(), *t.shape, error)) return false;
    if (t.grain && !writePngGray((dir / "grain.png").string(), *t.grain, error)) return false;
    return true;
}

// ---- Stamping ------------------------------------------------------------------------------------

TipStroke::TipStroke(BrushStroke& grid, BrushTip tip, double diameter, uint32_t seed)
    : grid_(grid), tip_(std::move(tip)), diameter_(diameter), rng_(seed) {
    if (!grid_.isValid() || !grid_.gridCoverage() || !tip_.normalize() || !(diameter_ > 0)) return;
    // The tip at every halving, so a large tip stamped small samples an image of about its size.
    levels_.push_back({GrayImage(*tip_.shape), 1.0});
    while (levels_.back().image.width() > 2 || levels_.back().image.height() > 2) {
        GrayImage next = halve(levels_.back().image);
        const double scale = double(next.width()) / tip_.shape->width();
        levels_.push_back({std::move(next), scale});
    }
    for (int i = 0; i < dynamicsTargetCount; i++) randomOn_[size_t(i)] = hasMapping(tip_.dynamics, DynamicsTarget(i), DynamicsInput::Random);
    valid_ = true;
}

const TipStroke::Level& TipStroke::levelFor(double tipPixelsPerGridPixel) const {
    // The smallest level still at least as detailed as the grid.
    size_t index = 0;
    while (index + 1 < levels_.size() && tipPixelsPerGridPixel * levels_[index + 1].scale >= 1) index++;
    return levels_[index];
}

double TipStroke::steadySize(const BrushSample& sample) const {
    return applyDynamics(tip_.dynamics, DynamicsTarget::Size, diameter_, sample, diameter_, 0, false);
}

void TipStroke::dab(Point center, const BrushSample& pen, double direction, double spacing, Rect& changed) {
    std::uniform_real_distribution<double> unit(0.0, 1.0), signedUnit(-1.0, 1.0);
    const BrushDynamics& dynamics = tip_.dynamics;
    auto draw = [&](DynamicsTarget target) { return randomOn_[size_t(target)] ? unit(rng_) : 0.0; };
    // Spacing-independent density: the alpha a dab would have at the reference spacing, spread over this one.
    std::array<uint8_t, 256> density{};
    const bool compensate = tip_.densityBySpacing && spacing > 0 && std::fabs(spacing / tip_.densityReference - 1) > 1e-9;
    if (compensate) {
        const double k = spacing / tip_.densityReference;
        for (int v = 0; v < 256; v++) density[size_t(v)] = uint8_t(std::lround(255 * (1 - std::pow(1 - v / 255.0, k))));
    }
    for (int n = 0; n < tip_.count; n++) {
        // The draws every dab makes, in the order brushes were first painted with, so a seed paints the same.
        const double sizeRandom = unit(rng_), flowRandom = unit(rng_), angleRandom = signedUnit(rng_);
        const double size = applyDynamics(dynamics, DynamicsTarget::Size, diameter_, pen, diameter_, sizeRandom);
        const double flow = applyDynamics(dynamics, DynamicsTarget::Flow, tip_.flow, pen, diameter_, flowRandom);
        const double angle = applyDynamics(dynamics, DynamicsTarget::Angle, tip_.angle, pen, diameter_, angleRandom);
        // Angles in the document's y-down frame: a counterclockwise angle on screen is a negative rotation.
        double rotation = (tip_.followStroke ? direction : 0) - angle * pi / 180;
        const bool flipX = tip_.flipX != (tip_.randomFlipX && unit(rng_) < 0.5);
        const bool flipY = tip_.flipY != (tip_.randomFlipY && unit(rng_) < 0.5);
        double across = 0, along = 0;
        if (tip_.scatter > 0) {
            across = signedUnit(rng_);
            along = tip_.scatterBothAxes ? signedUnit(rng_) : 0;
        }
        // Draws only mappings on the other targets make, after those.
        const double opacityRandom = draw(DynamicsTarget::Opacity), roundnessRandom = draw(DynamicsTarget::Roundness);
        const double scatterRandom = draw(DynamicsTarget::Scatter), grainDepthRandom = draw(DynamicsTarget::GrainDepth);
        const double grainRotationRandom = randomOn_[size_t(DynamicsTarget::GrainRotation)] ? signedUnit(rng_) : 0.0;
        Point at = center;
        if (tip_.scatter > 0) {
            const double scatter = applyDynamics(dynamics, DynamicsTarget::Scatter, tip_.scatter, pen, diameter_, scatterRandom);
            across *= scatter * size;
            along *= scatter * size;
            at = {center.x - std::sin(direction) * across + std::cos(direction) * along, center.y + std::cos(direction) * across + std::sin(direction) * along};
        }
        if (size < 0.25 || flow <= 0) continue;
        const double opacity = applyDynamics(dynamics, DynamicsTarget::Opacity, 1, pen, diameter_, opacityRandom);
        const unsigned ceiling = unsigned(std::lround(255 * std::clamp(opacity, 0.0, 1.0)));
        if (!ceiling) continue;
        const double roundness = applyDynamics(dynamics, DynamicsTarget::Roundness, tip_.roundness, pen, diameter_, roundnessRandom);

        const GrayImage& shape = *tip_.shape;
        const double scale = size / std::max(shape.width(), shape.height());   // document pixels per tip pixel
        const double sx = scale, sy = scale * roundness;
        const double c = std::cos(rotation), s = std::sin(rotation);
        const double hw = shape.width() * sx / 2, hh = shape.height() * sy / 2;
        const double ex = std::fabs(c) * hw + std::fabs(s) * hh, ey = std::fabs(s) * hw + std::fabs(c) * hh;
        Rect docBox(at.x - ex - 1, at.y - ey - 1, 2 * ex + 2, 2 * ey + 2);
        docBox = docBox.intersection(grid_.canvasRect().insetBy(-size, -size));
        const Affine& toGrid = grid_.documentToGrid();
        GrayImage& coverage = *grid_.gridCoverage();
        Rect box = toGrid.mapBounds(docBox).integral().intersection(Rect(0, 0, coverage.width(), coverage.height()));
        if (box.isEmpty()) continue;
        const Affine& toDocument = grid_.gridToDocument();
        const Point step = toDocument.applyVector({1, 0});
        const double footprint = std::hypot(step.x, step.y);
        const Level& level = levelFor(footprint / scale);
        const double lx = level.image.width() / double(shape.width()), ly = level.image.height() / double(shape.height());
        const GrayImage* grain = tip_.grain.get();
        const double grainStrength = grain ? applyDynamics(dynamics, DynamicsTarget::GrainDepth, tip_.grainDepth, pen, diameter_, grainDepthRandom) : 0;
        // The grain turned about the document's origin, when a mapping turns it.
        const double grainTurn = grain ? applyDynamics(dynamics, DynamicsTarget::GrainRotation, 0, pen, diameter_, grainRotationRandom) * pi / 180 : 0;
        const double gc = std::cos(grainTurn), gs = std::sin(grainTurn);
        const int x0 = int(box.minX()), x1 = int(box.maxX()), y0 = int(box.minY()), y1 = int(box.maxY());
        auto rows = [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                uint8_t* row = coverage.row(y);
                Point d = toDocument.apply({x0 + 0.5, y + 0.5});
                for (int x = x0; x < x1; x++, d = d + step) {
                    // Into the tip's frame: undo the rotation, then the scale, then the flips.
                    const double vx = d.x - at.x, vy = d.y - at.y;
                    double u = (c * vx + s * vy) / sx, v = (-s * vx + c * vy) / sy;
                    if (flipX) u = -u;
                    if (flipY) v = -v;
                    const double tx = (u + shape.width() / 2.0) * lx, ty = (v + shape.height() / 2.0) * ly;
                    if (tx < -1 || ty < -1 || tx > level.image.width() + 1 || ty > level.image.height() + 1) continue;
                    double value = sample(level.image, tx, ty) * flow;
                    if (grain && value > 0) {
                        const double gxd = grainTurn != 0 ? gc * d.x + gs * d.y : d.x, gyd = grainTurn != 0 ? -gs * d.x + gc * d.y : d.y;
                        int gx = int(std::floor(gxd * tip_.grainScale)) % grain->width(), gy = int(std::floor(gyd * tip_.grainScale)) % grain->height();
                        if (gx < 0) gx += grain->width();
                        if (gy < 0) gy += grain->height();
                        value *= 1 - grainStrength + grainStrength * grain->at(gx, gy) / 255.0;
                    }
                    unsigned add = unsigned(std::lround(std::clamp(value, 0.0, 255.0)));
                    if (compensate) add = density[add];
                    if (!add) continue;
                    // Build up towards the dab's opacity (all the way, without an Opacity mapping).
                    const unsigned old = row[x];
                    if (old >= ceiling) continue;
                    row[x] = uint8_t(old + (add * (ceiling - old) + 127) / 255);
                }
            }
        };
        if (y1 - y0 >= 64) parallelRows(y0, y1, rows, 16);
        else rows(y0, y1);
        changed = changed.unionWith(box);
    }
}

namespace {

/// A mouse's stand-in for pressure: slow presses harder, fast lifts, and the first two diameters ramp in.
double pressureFromSpeed(const BrushSample& s, double diameter) {
    const double fromSpeed = std::clamp(1.1 - s.speed / 1500, 0.25, 1.0);
    const double ramp = std::min(1.0, 0.3 + 0.7 * s.distance / std::max(1.0, 2 * diameter));
    return fromSpeed * ramp;
}

} // namespace

void TipStroke::strokeTo(const BrushSample& sample) {
    if (!valid_ || !sample.position.isFinite()) return;
    BrushSample input = sample;
    if (!input.stylus) input.pressure = tip_.mousePressureFromSpeed ? pressureFromSpeed(input, diameter_) : 1;
    Rect changed;
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const bool randomSpacing = randomOn_[size_t(DynamicsTarget::Spacing)];
    if (!last_) {
        dab(input.position, input, 0, tip_.spacing, changed);
        last_ = input;
        carried_ = 0;
    } else {
        const Point from = last_->position, to = input.position;
        const double dx = to.x - from.x, dy = to.y - from.y, length = std::hypot(dx, dy);
        if (length <= 0) return;
        const double direction = std::atan2(dy, dx);
        // Dabs every `spacing` of the dab's size, the size read at the point reached so far.
        double walked = 0;
        while (true) {
            const BrushSample here = interpolate(*last_, input, walked / length);
            const double size = steadySize(here);
            const double spacing = applyDynamics(tip_.dynamics, DynamicsTarget::Spacing, tip_.spacing, here, diameter_, randomSpacing ? unit(rng_) : 0.0);
            const double step = std::max(0.5, spacing * size);
            const double need = step - carried_;
            if (walked + need > length) { carried_ += length - walked; break; }
            walked += need;
            carried_ = 0;
            const double at = walked / length;
            dab({from.x + dx * at, from.y + dy * at}, interpolate(*last_, input, at), direction, size > 0 ? step / size : spacing, changed);
        }
        last_ = input;
    }
    if (!changed.isEmpty()) grid_.recomposeCovered(changed);
}

std::shared_ptr<Image> renderTipPreview(const TipPreset& preset, int width, int height) {
    auto paper = std::make_shared<Image>(width, height);
    Layer layer(Asset::make(paper, "Preview"), Point(0, 0));
    BrushSettings settings;
    settings.diameter = std::clamp(std::min(preset.diameter, height * 0.55), 2.0, 2000.0);
    settings.red = settings.green = settings.blue = 0;
    BrushStroke grid(layer, false, settings, Size(width, height));
    TipStroke stroke(grid, preset.tip, settings.diameter, 7);
    if (!grid.isValid() || !stroke.isValid()) return paper;
    // An S across the strip, pressing harder towards the middle.
    const int steps = 96;
    BrushSampleTrack track;
    for (int i = 0; i <= steps; i++) {
        const double t = double(i) / steps;
        BrushSample s;
        s.position = {width * (0.08 + 0.84 * t), height * (0.5 - 0.22 * std::sin(t * 2 * pi))};
        s.time = i / 120.0;
        s.pressure = 0.35 + 0.65 * std::sin(t * pi);
        s.stylus = true;
        stroke.strokeTo(track.add(s));
    }
    grid.flush();
    return std::make_shared<Image>(*grid.previewImage());
}

} // namespace compositor
