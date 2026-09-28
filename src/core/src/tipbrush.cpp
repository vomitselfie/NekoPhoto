#include "compositor/tipbrush.h"
#include "compositor/depth.h"
#include "compositor/parallel.h"
#include "compositor/png.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <type_traits>

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
    if (ix >= 0 && iy >= 0 && ix + 1 < image.width() && iy + 1 < image.height()) {
        // All four inside (most of a dab): the same sums without the edge tests.
        const uint8_t* r0 = image.row(iy) + ix;
        const uint8_t* r1 = r0 + image.width();
        const double top = double(r0[0]) * (1 - fx) + double(r0[1]) * fx;
        const double bottom = double(r1[0]) * (1 - fx) + double(r1[1]) * fx;
        return top * (1 - fy) + bottom * fy;
    }
    auto at = [&](int px, int py) -> double {
        return (px < 0 || py < 0 || px >= image.width() || py >= image.height()) ? 0.0 : image.at(px, py);
    };
    const double top = at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx;
    const double bottom = at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx;
    return top * (1 - fy) + bottom * fy;
}

/// std::lround for 0 <= v < 2^31, inline: the whole part plus one when the rest is a half or more (v - whole is exact).
inline unsigned roundHalfUp(double v) {
    const unsigned whole = unsigned(v);
    return whole + (v - whole >= 0.5 ? 1u : 0u);
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
    fix(grainMovement, 0, 1, 1);
    if (int(grainMode) < 0 || int(grainMode) > int(GrainMode::Dab)) grainMode = GrainMode::Canvas;
    fix(densityReference, 0.01, 10, 0.25);
    // Mappings with numbers that mean nothing go; the rest keep their order, which is the order they multiply in.
    BrushDynamics kept;
    for (DynamicsMapping m : dynamics) {
        if (!std::isfinite(m.offset) || !std::isfinite(m.depth) || int(m.input) < 0 || int(m.input) >= dynamicsInputCount
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
    if (auto it = j.find("grainMode"); it != j.end() && it->is_string())
        t.grainMode = *it == "stroke" ? BrushTip::GrainMode::Stroke : *it == "dab" ? BrushTip::GrainMode::Dab : BrushTip::GrainMode::Canvas;
    t.grainMovement = number("grainMovement", t.grainMovement);
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
        {"grainScale", t.grainScale}, {"grainDepth", t.grainDepth},
        {"grainMode", t.grainMode == BrushTip::GrainMode::Stroke ? "stroke" : t.grainMode == BrushTip::GrainMode::Dab ? "dab" : "canvas"},
        {"grainMovement", t.grainMovement}, {"dynamics", dynamics},
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
    if (!grid_.isValid() || !(grid_.gridCoverage() || grid_.gridCoverage16()) || !tip_.normalize() || !(diameter_ > 0)) return;
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
    // Spacing-independent density: the alpha a dab would have at the reference spacing, spread over this one. The
    // table (an entry per level: 256 at 8 bits, one per 15-bit level at 16) is kept while the spacing ratio stays the
    // same; one replaced while dabs still wait to be drawn with it is kept until they are.
    const bool compensate = tip_.densityBySpacing && spacing > 0 && std::fabs(spacing / tip_.densityReference - 1) > 1e-9;
    Gray16* coverage16 = grid_.gridCoverage16();
    const void* density = nullptr;
    if (compensate) {
        const double k = spacing / tip_.densityReference;
        if (!densityTable_ || densityK_ != k) {
            if (densityTable_) retiredDensity_.push_back(std::move(densityTable_));
            auto table = std::make_shared<std::vector<uint16_t>>(coverage16 ? size_t(one16) + 1 : 256);
            if (coverage16)
                for (uint32_t v = 0; v <= one16; v++) (*table)[v] = uint16_t(std::lround(one16 * (1 - std::pow(1 - v / double(one16), k))));
            else
                for (int v = 0; v < 256; v++) (*table)[size_t(v)] = uint16_t(std::lround(255 * (1 - std::pow(1 - v / 255.0, k))));
            densityTable_ = std::move(table);
            densityK_ = k;
        }
        density = densityTable_->data();
    }
    // Stroke grain turns with the stroke's tangent smoothed over about two diameters, so a corner or a jittered dab
    // does not spin it. The direction is unwrapped against the last dab's direction, not the lagging tangent, so a
    // path that turns more than half a turn within the window (a tight spiral) keeps turning the grain forwards. The
    // grain travels with each step between dabs in its own frame, so as the frame catches up with a bend the grain
    // under the paper turns about the dab a little and never slides across the stroke.
    if (tip_.grain && tip_.grainMode == BrushTip::GrainMode::Stroke) {
        if (!grainTangentSet_) {
            grainDirection_ = grainTangent_ = direction;
            grainOffset_ = {tip_.grainMovement * pen.distance, 0};
            grainTangentSet_ = true;
        } else {
            grainDirection_ = unwrapAngle(grainDirection_, direction);
            const double walked = std::max(0.0, pen.distance - grainTangentAt_);
            grainTangent_ += (grainDirection_ - grainTangent_) * (1 - std::exp(-walked / std::max(1.0, 2 * diameter_)));
            const double c = std::cos(grainTangent_), s = std::sin(grainTangent_);
            const double dx = center.x - grainCenter_.x, dy = center.y - grainCenter_.y;
            grainOffset_ = {grainOffset_.x + tip_.grainMovement * (c * dx + s * dy), grainOffset_.y + tip_.grainMovement * (-s * dx + c * dy)};
        }
        grainTangentAt_ = pen.distance;
        grainCenter_ = center;
    }
    const double tc = std::cos(grainTangent_), ts = std::sin(grainTangent_);
    const Point grainOffset = grainOffset_;
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
        const unsigned ceiling = unsigned(std::lround((coverage16 ? one16 : 255) * std::clamp(opacity, 0.0, 1.0)));
        if (!ceiling) continue;
        const double roundness = applyDynamics(dynamics, DynamicsTarget::Roundness, tip_.roundness, pen, diameter_, roundnessRandom);

        dabCount_++;
        if (trace_) trace_->push_back({center, at, pen.time, pen.distance, size, flow, opacity, roundness, spacing, rotation, grainTangent_, grainOffset, grainDirection_});
        const GrayImage& shape = *tip_.shape;
        const double scale = size / std::max(shape.width(), shape.height());   // document pixels per tip pixel
        const double sx = scale, sy = scale * roundness;
        const double c = std::cos(rotation), s = std::sin(rotation);
        const double hw = shape.width() * sx / 2, hh = shape.height() * sy / 2;
        const double ex = std::fabs(c) * hw + std::fabs(s) * hh, ey = std::fabs(s) * hw + std::fabs(c) * hh;
        Rect docBox(at.x - ex - 1, at.y - ey - 1, 2 * ex + 2, 2 * ey + 2);
        docBox = docBox.intersection(grid_.canvasRect().insetBy(-size, -size));
        const Affine& toGrid = grid_.documentToGrid();
        const int gridWidth = coverage16 ? coverage16->width() : grid_.gridCoverage()->width();
        const int gridHeight = coverage16 ? coverage16->height() : grid_.gridCoverage()->height();
        Rect box = toGrid.mapBounds(docBox).integral().intersection(Rect(0, 0, gridWidth, gridHeight));
        if (box.isEmpty()) continue;
        const Point step = grid_.gridToDocument().applyVector({1, 0});
        const double footprint = std::hypot(step.x, step.y);
        const Level& level = levelFor(footprint / scale);
        const bool grain = tip_.grain != nullptr;
        // The dab as it will be drawn (drawPending), with everything that varies from one dab to the next.
        Stamp st;
        st.center = center;
        st.at = at;
        st.c = c; st.s = s; st.sx = sx; st.sy = sy;
        st.flipX = flipX; st.flipY = flipY;
        st.level = &level;
        st.lx = level.image.width() / double(shape.width());
        st.ly = level.image.height() / double(shape.height());
        st.flow = flow;
        st.ceiling = ceiling;
        st.grainStrength = grain ? applyDynamics(dynamics, DynamicsTarget::GrainDepth, tip_.grainDepth, pen, diameter_, grainDepthRandom) : 0;
        // The grain turned about the document's origin, when a mapping turns it.
        st.grainTurn = grain ? applyDynamics(dynamics, DynamicsTarget::GrainRotation, 0, pen, diameter_, grainRotationRandom) * pi / 180 : 0;
        st.gc = std::cos(st.grainTurn); st.gs = std::sin(st.grainTurn);
        if (grain) {
            // What each grain level multiplies the dab by, as a table when it pays: the depth held from the last dab
            // (no mapping moves it) or a dab large enough to outnumber the table's 256 entries.
            const bool held = grainFactor_ && grainFactorDepth_ == st.grainStrength;
            if (!held && double(box.width) * box.height >= 1024) {
                if (grainFactor_) retiredGrain_.push_back(std::move(grainFactor_));
                auto table = std::make_shared<std::array<double, 256>>();
                for (int g = 0; g < 256; g++) (*table)[size_t(g)] = 1 - st.grainStrength + st.grainStrength * uint8_t(g) / 255.0;
                grainFactor_ = std::move(table);
                grainFactorDepth_ = st.grainStrength;
            }
            if (grainFactor_ && grainFactorDepth_ == st.grainStrength) st.grainFactor = grainFactor_->data();
        }
        st.tc = tc; st.ts = ts;
        st.grainOffset = grainOffset;
        st.density = static_cast<const uint16_t*>(density);
        st.x0 = int(box.minX()); st.x1 = int(box.maxX()); st.y0 = int(box.minY()); st.y1 = int(box.maxY());
        pending_.push_back(st);
        changed = changed.unionWith(box);
    }
}

template <typename Coverage>
void TipStroke::stampRows(const Stamp& st, Coverage& coverage, int ya, int yb) const {
    // The coverage at the grid's depth: 0..255 or 0..32768, the same sampling either way.
    constexpr bool deep = std::is_same_v<Coverage, Gray16>;
    const GrayImage& shape = *tip_.shape;
    const GrayImage& levelImage = st.level->image;
    const GrayImage* grain = tip_.grain.get();
    const Affine& toDocument = grid_.gridToDocument();
    const Point step = toDocument.applyVector({1, 0});
    const Point center = st.center, at = st.at, grainOffset = st.grainOffset;
    const double c = st.c, s = st.s, sx = st.sx, sy = st.sy, lx = st.lx, ly = st.ly, flow = st.flow;
    const double tc = st.tc, ts = st.ts, gc = st.gc, gs = st.gs, grainTurn = st.grainTurn, grainStrength = st.grainStrength;
    const bool flipX = st.flipX, flipY = st.flipY;
    const unsigned ceiling = st.ceiling;
    const uint16_t* density = st.density;
    const int x0 = st.x0, x1 = st.x1;
    const double* grainFactor = st.grainFactor;
    const int grainMaskX = grain && (grain->width() & (grain->width() - 1)) == 0 ? grain->width() - 1 : -1;
    const int grainMaskY = grain && (grain->height() & (grain->height() - 1)) == 0 ? grain->height() - 1 : -1;
    // Along a row the tip's u and v are linear in x, so the span where the tip's image can land is an interval: the
    // pixels outside it (a turned or flattened dab's box is mostly outside) are passed over, with a margin of two
    // pixels, and the test below still decides each one inside, so the same pixels are drawn.
    const double uStep = (c * step.x + s * step.y) / sx, vStep = (-s * step.x + c * step.y) / sy;
    const double uLow = -1 / lx - shape.width() / 2.0, uHigh = (levelImage.width() + 1) / lx - shape.width() / 2.0;
    const double vLow = -1 / ly - shape.height() / 2.0, vHigh = (levelImage.height() + 1) / ly - shape.height() / 2.0;
    const int n = x1 - x0;
    // The steps k in [0, n) where lo <= start + k * delta <= hi (the flips mirror the range), widened by two.
    auto span = [n](double start, double delta, double lo, double hi, bool flip, int& k0, int& k1) {
        if (flip) { const double t = lo; lo = -hi; hi = -t; }
        if (delta == 0 || !std::isfinite(delta)) {
            if (!(start >= lo - 1e-6 * (1 + std::fabs(lo)) && start <= hi + 1e-6 * (1 + std::fabs(hi)))) { k0 = n; k1 = n; }
            return;
        }
        double a = (lo - start) / delta, b = (hi - start) / delta;
        if (a > b) std::swap(a, b);
        a = std::floor(a) - 2;
        b = std::ceil(b) + 2;
        k0 = std::max(k0, a <= 0 ? 0 : a >= n ? n : int(a));
        k1 = std::min(k1, b < 0 ? 0 : b >= n ? n : int(b) + 1);
    };
    for (int y = ya; y < yb; y++) {
        auto* row = coverage.row(y);
        Point d = toDocument.apply({x0 + 0.5, y + 0.5});
        int k0 = 0, k1 = n;
        span((c * (d.x - at.x) + s * (d.y - at.y)) / sx, uStep, uLow, uHigh, flipX, k0, k1);
        span((-s * (d.x - at.x) + c * (d.y - at.y)) / sy, vStep, vLow, vHigh, flipY, k0, k1);
        if (k0 >= k1) continue;
        for (int k = 0; k < k0; k++) d = d + step;   // the same sums the loop would have made
        for (int x = x0 + k0; x < x0 + k1; x++, d = d + step) {
            // Into the tip's frame: undo the rotation, then the scale, then the flips.
            const double vx = d.x - at.x, vy = d.y - at.y;
            double u = (c * vx + s * vy) / sx, v = (-s * vx + c * vy) / sy;
            if (flipX) u = -u;
            if (flipY) v = -v;
            const double tx = (u + shape.width() / 2.0) * lx, ty = (v + shape.height() / 2.0) * ly;
            if (tx < -1 || ty < -1 || tx > levelImage.width() + 1 || ty > levelImage.height() + 1) continue;
            double value = sample(levelImage, tx, ty) * flow;
            if (grain && value > 0) {
                // The grain's frame: the document, the stroke (along its smoothed tangent, travelling with it)
                // or the dab (its turn and flips).
                double px = d.x, py = d.y;
                if (tip_.grainMode == BrushTip::GrainMode::Stroke) {
                    const double ox = d.x - center.x, oy = d.y - center.y;
                    px = tc * ox + ts * oy + grainOffset.x;
                    py = -ts * ox + tc * oy + grainOffset.y;
                } else if (tip_.grainMode == BrushTip::GrainMode::Dab) {
                    px = (flipX ? -1 : 1) * (c * vx + s * vy);
                    py = (flipY ? -1 : 1) * (-s * vx + c * vy);
                }
                const double gxd = grainTurn != 0 ? gc * px + gs * py : px, gyd = grainTurn != 0 ? -gs * px + gc * py : py;
                const int fx = int(std::floor(gxd * tip_.grainScale)), fy = int(std::floor(gyd * tip_.grainScale));
                int gx, gy;
                if (grainMaskX >= 0) gx = fx & grainMaskX;   // a power-of-two width wraps as the remainder would
                else if ((gx = fx % grain->width()) < 0) gx += grain->width();
                if (grainMaskY >= 0) gy = fy & grainMaskY;
                else if ((gy = fy % grain->height()) < 0) gy += grain->height();
                const uint8_t g = grain->at(gx, gy);
                value *= grainFactor ? grainFactor[g] : 1 - grainStrength + grainStrength * g / 255.0;
            }
            if constexpr (deep) {
                uint32_t add = roundHalfUp(std::clamp(value, 0.0, 255.0) * (one16 / 255.0));
                if (density) add = density[add];
                if (!add) continue;
                const uint32_t old = row[x];
                if (old >= ceiling) continue;
                row[x] = uint16_t(old + ((add * (ceiling - old) + one16 / 2) >> 15));
            } else {
                unsigned add = roundHalfUp(std::clamp(value, 0.0, 255.0));
                if (density) add = density[add];
                if (!add) continue;
                // Build up towards the dab's opacity (all the way, without an Opacity mapping).
                const unsigned old = row[x];
                if (old >= ceiling) continue;
                row[x] = uint8_t(old + (add * (ceiling - old) + 127) / 255);
            }
        }
    }
}

void TipStroke::drawPending() {
    if (pending_.empty()) return;
    // The dabs placed since the last call, drawn in the order they were placed. A pixel's value depends only on the
    // dabs over it and their order, so bands of rows can be drawn on every core at once, each band running through
    // all the dabs: the same pixels as one dab after another, with one hand-out to the workers for the lot instead of
    // one per dab.
    int y0 = pending_.front().y0, y1 = pending_.front().y1;
    double area = 0;
    for (const Stamp& st : pending_) {
        y0 = std::min(y0, st.y0);
        y1 = std::max(y1, st.y1);
        area += double(st.x1 - st.x0) * (st.y1 - st.y0);
    }
    auto drawOn = [&](auto& coverage) {
        auto rows = [&](int ya, int yb) {
            for (const Stamp& st : pending_) {
                const int a = std::max(ya, st.y0), b = std::min(yb, st.y1);
                if (a < b) stampRows(st, coverage, a, b);
            }
        };
        if (area >= 65536) parallelRows(y0, y1, rows, 4);
        else rows(y0, y1);
    };
    if (Gray16* coverage16 = grid_.gridCoverage16()) drawOn(*coverage16);
    else drawOn(*grid_.gridCoverage());
    pending_.clear();
    retiredDensity_.clear();
    retiredGrain_.clear();
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
        last_ = input;
        carried_ = 0;
        // Stroke grain turns with the stroke from its first dab: that dab waits until the stroke has a direction.
        if (tip_.grain && tip_.grainMode == BrushTip::GrainMode::Stroke) { firstPending_ = true; return; }
        dab(input.position, input, 0, tip_.spacing, changed);
    } else {
        const Point from = last_->position, to = input.position;
        const double dx = to.x - from.x, dy = to.y - from.y, length = std::hypot(dx, dy);
        if (length <= 0) return;
        const double direction = std::atan2(dy, dx);
        if (firstPending_) {
            dab(from, *last_, direction, tip_.spacing, changed);
            firstPending_ = false;
        }
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
    drawPending();
    if (!changed.isEmpty()) grid_.recomposeCovered(changed);
}

void TipStroke::finish() {
    if (!valid_ || !firstPending_ || !last_) return;
    firstPending_ = false;
    Rect changed;
    dab(last_->position, *last_, 0, tip_.spacing, changed);
    drawPending();
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
    stroke.finish();
    grid.flush();
    return std::make_shared<Image>(*grid.previewImage());
}

} // namespace compositor
