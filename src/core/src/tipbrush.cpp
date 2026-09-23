#include "compositor/tipbrush.h"
#include "compositor/parallel.h"
#include "compositor/png.h"
#include <nlohmann/json.hpp>
#include <algorithm>
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
    fix(angleJitter, 0, 180, 0);
    fix(roundness, 0.01, 1, 1);
    fix(sizeJitter, 0, 1, 0);
    fix(scatter, 0, 10, 0);
    count = std::clamp(count, 1, 16);
    fix(flow, 0, 1, 1);
    fix(flowJitter, 0, 1, 0);
    fix(pressureSize, 0, 1, 0);
    fix(minimumSize, 0, 1, 0);
    fix(pressureFlow, 0, 1, 0);
    fix(grainScale, 0.01, 100, 1);
    fix(grainDepth, 0, 1, 1);
    if (grain && grain->isEmpty()) grain.reset();
    return shape && !shape->isEmpty() && shape->width() <= 8192 && shape->height() <= 8192;
}

// ---- Presets on disk ----------------------------------------------------------------------------

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
    t.angleJitter = number("angleJitter", t.angleJitter);
    t.roundness = number("roundness", t.roundness);
    t.sizeJitter = number("sizeJitter", t.sizeJitter);
    t.scatter = number("scatter", t.scatter);
    t.scatterBothAxes = boolean("scatterBothAxes", t.scatterBothAxes);
    t.count = int(number("count", t.count));
    t.flow = number("flow", t.flow);
    t.flowJitter = number("flowJitter", t.flowJitter);
    t.pressureSize = number("pressureSize", t.pressureSize);
    t.minimumSize = number("minimumSize", t.minimumSize);
    t.pressureFlow = number("pressureFlow", t.pressureFlow);
    t.flipX = boolean("flipX", t.flipX);
    t.flipY = boolean("flipY", t.flipY);
    t.randomFlipX = boolean("randomFlipX", t.randomFlipX);
    t.randomFlipY = boolean("randomFlipY", t.randomFlipY);
    t.grainScale = number("grainScale", t.grainScale);
    t.grainDepth = number("grainDepth", t.grainDepth);
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
    nlohmann::json j = {
        {"format", "compositor-tip-brush"}, {"version", 1}, {"name", preset.name}, {"diameter", preset.diameter},
        {"spacing", t.spacing}, {"angle", t.angle}, {"followStroke", t.followStroke}, {"angleJitter", t.angleJitter},
        {"roundness", t.roundness}, {"sizeJitter", t.sizeJitter}, {"scatter", t.scatter}, {"scatterBothAxes", t.scatterBothAxes},
        {"count", t.count}, {"flow", t.flow}, {"flowJitter", t.flowJitter}, {"pressureSize", t.pressureSize},
        {"minimumSize", t.minimumSize}, {"pressureFlow", t.pressureFlow}, {"flipX", t.flipX}, {"flipY", t.flipY},
        {"randomFlipX", t.randomFlipX}, {"randomFlipY", t.randomFlipY}, {"grainScale", t.grainScale}, {"grainDepth", t.grainDepth}};
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
    valid_ = true;
}

const TipStroke::Level& TipStroke::levelFor(double tipPixelsPerGridPixel) const {
    // The smallest level still at least as detailed as the grid.
    size_t index = 0;
    while (index + 1 < levels_.size() && tipPixelsPerGridPixel * levels_[index + 1].scale >= 1) index++;
    return levels_[index];
}

double TipStroke::sizeAt(double pressure) const {
    const double fromPressure = tip_.minimumSize + (1 - tip_.minimumSize) * std::clamp(pressure, 0.0, 1.0);
    return diameter_ * (1 - tip_.pressureSize + tip_.pressureSize * fromPressure);
}

void TipStroke::dab(Point center, double pressure, double direction, Rect& changed) {
    std::uniform_real_distribution<double> unit(0.0, 1.0), signedUnit(-1.0, 1.0);
    for (int n = 0; n < tip_.count; n++) {
        double size = sizeAt(pressure) * (1 - tip_.sizeJitter * unit(rng_));
        double flow = tip_.flow * (1 - tip_.pressureFlow + tip_.pressureFlow * std::clamp(pressure, 0.0, 1.0)) * (1 - tip_.flowJitter * unit(rng_));
        // Angles in the document's y-down frame: a counterclockwise angle on screen is a negative rotation.
        double rotation = (tip_.followStroke ? direction : 0) - (tip_.angle + tip_.angleJitter * signedUnit(rng_)) * pi / 180;
        const bool flipX = tip_.flipX != (tip_.randomFlipX && unit(rng_) < 0.5);
        const bool flipY = tip_.flipY != (tip_.randomFlipY && unit(rng_) < 0.5);
        Point at = center;
        if (tip_.scatter > 0) {
            const double across = tip_.scatter * size * signedUnit(rng_);
            const double along = tip_.scatterBothAxes ? tip_.scatter * size * signedUnit(rng_) : 0;
            at = {center.x - std::sin(direction) * across + std::cos(direction) * along, center.y + std::cos(direction) * across + std::sin(direction) * along};
        }
        if (size < 0.25 || flow <= 0) continue;

        const GrayImage& shape = *tip_.shape;
        const double scale = size / std::max(shape.width(), shape.height());   // document pixels per tip pixel
        const double sx = scale, sy = scale * tip_.roundness;
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
        const double grainStrength = tip_.grain ? tip_.grainDepth : 0;
        const GrayImage* grain = tip_.grain.get();
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
                        int gx = int(std::floor(d.x * tip_.grainScale)) % grain->width(), gy = int(std::floor(d.y * tip_.grainScale)) % grain->height();
                        if (gx < 0) gx += grain->width();
                        if (gy < 0) gy += grain->height();
                        value *= 1 - grainStrength + grainStrength * grain->at(gx, gy) / 255.0;
                    }
                    const unsigned add = unsigned(std::lround(std::clamp(value, 0.0, 255.0)));
                    if (!add) continue;
                    const unsigned old = row[x];
                    row[x] = uint8_t(old + (add * (255 - old) + 127) / 255);
                }
            }
        };
        if (y1 - y0 >= 64) parallelRows(y0, y1, rows, 16);
        else rows(y0, y1);
        changed = changed.unionWith(box);
    }
}

void TipStroke::strokeTo(const TipInput& input) {
    if (!valid_ || !input.document.isFinite()) return;
    Rect changed;
    if (!last_) {
        dab(input.document, input.pressure, 0, changed);
        last_ = input;
        carried_ = 0;
    } else {
        const Point from = last_->document, to = input.document;
        const double dx = to.x - from.x, dy = to.y - from.y, length = std::hypot(dx, dy);
        if (length <= 0) return;
        const double direction = std::atan2(dy, dx);
        // Dabs every `spacing` of the dab's size, the size read at the point reached so far.
        double walked = 0;
        while (true) {
            const double t = walked / length;
            const double pressure = last_->pressure + (input.pressure - last_->pressure) * t;
            const double step = std::max(0.5, tip_.spacing * sizeAt(pressure));
            const double need = step - carried_;
            if (walked + need > length) { carried_ += length - walked; break; }
            walked += need;
            carried_ = 0;
            const double at = walked / length;
            dab({from.x + dx * at, from.y + dy * at}, last_->pressure + (input.pressure - last_->pressure) * at, direction, changed);
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
    for (int i = 0; i <= steps; i++) {
        const double t = double(i) / steps;
        stroke.strokeTo({{width * (0.08 + 0.84 * t), height * (0.5 - 0.22 * std::sin(t * 2 * pi))}, 0.35 + 0.65 * std::sin(t * pi)});
    }
    grid.flush();
    return std::make_shared<Image>(*grid.previewImage());
}

} // namespace compositor
