#include "compositor/brushdynamics.h"
#include <algorithm>
#include <cmath>

namespace compositor {

namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double defaultSpeedScale = 2000;   // document pixels (Speed) or screen points (ScreenSpeed) per second read as full speed
constexpr double defaultFadeDiameters = 25;  // Photoshop's default Fade: 25 steps

/// The fraction of a turn `angle` (radians) is past zero, 0..1.
double turn(double angle) {
    double t = std::fmod(angle / (2 * pi), 1.0);
    if (t < 0) t += 1;
    return t;
}

/// The end slope of a monotone cubic, from the one-sided three-point estimate, kept from overshooting.
double endSlope(double h0, double h1, double d0, double d1) {
    double m = ((2 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
    if (m * d0 <= 0) m = 0;
    else if (d0 * d1 <= 0 && std::fabs(m) > std::fabs(3 * d0)) m = 3 * d0;
    return m;
}

} // namespace

// ---- Curves ------------------------------------------------------------------------------------------------------

void DynamicsCurve::normalize() {
    std::vector<std::pair<double, double>> kept;
    for (auto [x, y] : points)
        if (std::isfinite(x) && std::isfinite(y)) kept.emplace_back(std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0));
    std::stable_sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    points.clear();
    for (const auto& p : kept) {
        if (!points.empty() && points.back().first == p.first) points.back() = p;
        else points.push_back(p);
    }
}

double DynamicsCurve::evaluate(double x) const {
    if (isIdentity()) return x;
    const auto& p = points;
    if (!(x > p.front().first)) return p.front().second;
    if (!(x < p.back().first)) return p.back().second;
    size_t k = 1;
    while (k + 1 < p.size() && x > p[k].first) k++;
    const double x0 = p[k - 1].first, x1 = p[k].first, y0 = p[k - 1].second, y1 = p[k].second;
    const double h = x1 - x0;
    if (h <= 0) return y1;
    const double t = (x - x0) / h;
    if (kind == Kind::Linear || p.size() < 3) return y0 + (y1 - y0) * t;
    // Fritsch-Carlson: secant slopes, tangents their weighted harmonic mean where they agree in sign, zero where
    // they do not, so each piece stays between its end values.
    auto secant = [&](size_t i) { return (p[i + 1].second - p[i].second) / (p[i + 1].first - p[i].first); };
    auto tangent = [&](size_t i) {
        const size_t n = p.size();
        if (i == 0) return endSlope(p[1].first - p[0].first, p[2].first - p[1].first, secant(0), secant(1));
        if (i == n - 1) return endSlope(p[n - 1].first - p[n - 2].first, p[n - 2].first - p[n - 3].first, secant(n - 2), secant(n - 3));
        const double d0 = secant(i - 1), d1 = secant(i);
        if (d0 * d1 <= 0) return 0.0;
        const double h0 = p[i].first - p[i - 1].first, h1 = p[i + 1].first - p[i].first;
        return 3 * (h0 + h1) / ((2 * h1 + h0) / d0 + (h1 + 2 * h0) / d1);
    };
    const double m0 = tangent(k - 1), m1 = tangent(k);
    const double t2 = t * t, t3 = t2 * t;
    const double y = (2 * t3 - 3 * t2 + 1) * y0 + (t3 - 2 * t2 + t) * h * m0 + (-2 * t3 + 3 * t2) * y1 + (t3 - t2) * h * m1;
    return std::clamp(y, std::min(y0, y1), std::max(y0, y1));
}

// ---- Mappings ----------------------------------------------------------------------------------------------------

bool isCircular(DynamicsTarget target) { return target == DynamicsTarget::Angle || target == DynamicsTarget::GrainRotation; }

std::pair<double, double> targetRange(DynamicsTarget target) {
    switch (target) {
    case DynamicsTarget::Size: return {0, 10000};
    case DynamicsTarget::Flow: case DynamicsTarget::Opacity: case DynamicsTarget::GrainDepth: return {0, 1};
    case DynamicsTarget::Roundness: return {0.01, 1};
    case DynamicsTarget::Spacing: return {0.01, 10};
    case DynamicsTarget::Scatter: return {0, 10};
    case DynamicsTarget::Angle: case DynamicsTarget::GrainRotation: return {-360, 360};
    }
    return {0, 1};
}

double dynamicsInput(const DynamicsMapping& m, const BrushSample& s, double diameter, double random) {
    switch (m.input) {
    case DynamicsInput::Pressure: return std::clamp(s.pressure, 0.0, 1.0);
    case DynamicsInput::Speed: return std::clamp(s.speed / (m.scale > 0 ? m.scale : defaultSpeedScale), 0.0, 1.0);
    case DynamicsInput::ScreenSpeed: return std::clamp(s.screenSpeed / (m.scale > 0 ? m.scale : defaultSpeedScale), 0.0, 1.0);
    case DynamicsInput::Tilt: return std::clamp(s.tiltMagnitude, 0.0, 1.0);
    case DynamicsInput::TiltDirection: return turn(s.tiltAzimuth);
    case DynamicsInput::Twist: return turn(s.twistAngle);
    case DynamicsInput::Roll: return turn(s.twistReported ? s.twistAngle : s.direction);
    case DynamicsInput::Random: return isCircular(m.target) ? std::clamp(random, -1.0, 1.0) : std::clamp(random, 0.0, 1.0);
    case DynamicsInput::StrokeRandom: return isCircular(m.target) ? std::clamp(s.strokeRandom * 2 - 1, -1.0, 1.0) : std::clamp(s.strokeRandom, 0.0, 1.0);
    case DynamicsInput::InitialDirection: return turn(s.initialDirection);
    case DynamicsInput::Wheel: return std::clamp((s.tangentialPressure + 1) / 2, 0.0, 1.0);
    case DynamicsInput::StrokeProgress: {
        const double size = std::max(diameter, 1e-6);
        if (m.scale > 0) return std::clamp(s.distance / (m.scale * size), 0.0, 1.0);
        if (s.progress >= 0) return std::clamp(s.progress, 0.0, 1.0);
        return std::clamp(s.distance / (defaultFadeDiameters * size), 0.0, 1.0);
    }
    }
    return 0;
}

double dynamicsOutput(const DynamicsMapping& m, double x) {
    // A centred random draw keeps its sign; the curve shapes its size.
    const double y = x < 0 ? -m.curve.evaluate(-x) : m.curve.evaluate(x);
    return m.offset + m.depth * y;
}

double applyDynamics(const BrushDynamics& dynamics, DynamicsTarget target, double base, const BrushSample& sample, double diameter,
                     double random, bool withRandom) {
    const bool circular = isCircular(target);
    double value = base;
    bool any = false;
    for (const DynamicsMapping& m : dynamics) {
        if (m.target != target || (!withRandom && m.input == DynamicsInput::Random)) continue;
        const double out = dynamicsOutput(m, dynamicsInput(m, sample, diameter, random));
        value = circular ? value + out : value * out;
        any = true;
    }
    if (!any || circular) return value;
    const auto [lo, hi] = targetRange(target);
    return std::clamp(value, lo, hi);
}

bool hasMapping(const BrushDynamics& dynamics, DynamicsTarget target, std::optional<DynamicsInput> input) {
    for (const DynamicsMapping& m : dynamics)
        if (m.target == target && (!input || m.input == *input)) return true;
    return false;
}

DynamicsMapping dynamicsMapping(DynamicsInput input, DynamicsTarget target, double offset, double depth, double scale) {
    DynamicsMapping m;
    m.input = input;
    m.target = target;
    m.offset = offset;
    m.depth = depth;
    m.scale = scale;
    return m;
}

const DynamicsMapping* findMapping(const BrushDynamics& dynamics, DynamicsInput input, DynamicsTarget target) {
    for (const DynamicsMapping& m : dynamics)
        if (m.input == input && m.target == target) return &m;
    return nullptr;
}

BrushDynamics legacyDynamics(const LegacyTipDynamics& l) {
    BrushDynamics out;
    auto add = [&](DynamicsInput input, DynamicsTarget target, double offset, double depth) {
        out.push_back(dynamicsMapping(input, target, offset, depth));
    };
    // In the order the old engine multiplied them: the pressure factor before the jitter.
    if (l.pressureSize > 0) {
        if (l.pressureSize >= 1) add(DynamicsInput::Pressure, DynamicsTarget::Size, l.minimumSize, 1 - l.minimumSize);
        else {
            // Part pressure: 1 - p + p x (minimum + (1 - minimum) x pressure).
            const double low = 1 - l.pressureSize + l.pressureSize * l.minimumSize;
            add(DynamicsInput::Pressure, DynamicsTarget::Size, low, 1 - low);
        }
    }
    if (l.sizeJitter > 0) add(DynamicsInput::Random, DynamicsTarget::Size, 1, -l.sizeJitter);
    if (l.pressureFlow > 0) add(DynamicsInput::Pressure, DynamicsTarget::Flow, 1 - l.pressureFlow, l.pressureFlow);
    if (l.flowJitter > 0) add(DynamicsInput::Random, DynamicsTarget::Flow, 1, -l.flowJitter);
    if (l.angleJitter > 0) add(DynamicsInput::Random, DynamicsTarget::Angle, 0, l.angleJitter);
    return out;
}

// ---- A tilted pencil ---------------------------------------------------------------------------------------------

namespace {
bool isTiltRoundness(const DynamicsMapping& m) {
    return m.input == DynamicsInput::Tilt && m.target == DynamicsTarget::Roundness && m.offset == 1 && m.depth <= 0 && m.curve.isIdentity();
}
bool isAzimuthAngle(const DynamicsMapping& m) {
    return m.input == DynamicsInput::TiltDirection && m.target == DynamicsTarget::Angle && m.offset == 0 && m.depth == -360 && m.curve.isIdentity();
}
} // namespace

BrushDynamics tiltShapesTip(double flattest) {
    const double f = std::clamp(std::isfinite(flattest) ? flattest : 1.0, 0.01, 1.0);
    return {dynamicsMapping(DynamicsInput::Tilt, DynamicsTarget::Roundness, 1, f - 1),
            dynamicsMapping(DynamicsInput::TiltDirection, DynamicsTarget::Angle, 0, -360)};
}

std::optional<double> tiltShapeOf(const BrushDynamics& dynamics) {
    const auto roundness = std::find_if(dynamics.begin(), dynamics.end(), isTiltRoundness);
    if (roundness == dynamics.end() || std::none_of(dynamics.begin(), dynamics.end(), isAzimuthAngle)) return std::nullopt;
    return 1 + roundness->depth;
}

void removeTiltShape(BrushDynamics& dynamics) {
    if (!tiltShapeOf(dynamics)) return;
    auto first = [&](auto predicate) { auto it = std::find_if(dynamics.begin(), dynamics.end(), predicate); if (it != dynamics.end()) dynamics.erase(it); };
    first(isTiltRoundness);
    first(isAzimuthAngle);
}

// ---- Names -------------------------------------------------------------------------------------------------------

namespace {
const char* const inputNames[] = {"pressure", "speed", "tilt", "tiltDirection", "twist", "random", "strokeProgress", "roll", "screenSpeed", "strokeRandom", "initialDirection", "wheel"};
const char* const targetNames[] = {"size", "flow", "opacity", "angle", "roundness", "spacing", "scatter", "grainDepth", "grainRotation"};
} // namespace

const char* dynamicsInputName(DynamicsInput input) { return inputNames[int(input)]; }
const char* dynamicsTargetName(DynamicsTarget target) { return targetNames[int(target)]; }

std::optional<DynamicsInput> dynamicsInputFromName(const std::string& name) {
    for (int i = 0; i < dynamicsInputCount; i++) if (name == inputNames[i]) return DynamicsInput(i);
    return std::nullopt;
}

std::optional<DynamicsTarget> dynamicsTargetFromName(const std::string& name) {
    for (int i = 0; i < dynamicsTargetCount; i++) if (name == targetNames[i]) return DynamicsTarget(i);
    return std::nullopt;
}

} // namespace compositor
