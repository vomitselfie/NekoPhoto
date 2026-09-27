#include "compositor/brushsample.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

namespace compositor {

namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double fallbackDt = 1.0 / 120;   // events without usable times count as a tablet's 120 a second
constexpr double speedSmoothing = 0.02;    // seconds: the time constant of the speed's smoothing

} // namespace

BrushSample mouseSample(Point position, double time) {
    BrushSample s;
    s.position = position;
    s.time = time;
    return s;
}

double unwrapAngle(double previous, double angle) {
    if (!std::isfinite(previous) || !std::isfinite(angle)) return angle;
    return angle + 2 * pi * std::round((previous - angle) / (2 * pi));
}

BrushSample BrushSampleTrack::add(BrushSample s) {
    const double tiltX = std::isfinite(s.tiltX) ? s.tiltX : 0, tiltY = std::isfinite(s.tiltY) ? s.tiltY : 0;
    s.tiltMagnitude = std::min(1.0, std::hypot(tiltX, tiltY) / 60);
    const double azimuth = std::atan2(tiltY, tiltX);
    const double twist = std::isfinite(s.twist) ? s.twist * pi / 180 : 0;
    if (!last_) {
        s.dt = fallbackDt;
        s.speed = s.acceleration = s.distance = 0;
        s.direction = 0;
        s.tiltAzimuth = azimuth;
        s.twistAngle = twist;
    } else {
        const BrushSample& p = *last_;
        const double dt = s.time - p.time;
        s.dt = std::isfinite(dt) && dt > 0 ? dt : fallbackDt;
        const double dx = s.position.x - p.position.x, dy = s.position.y - p.position.y;
        const double step = std::isfinite(dx) && std::isfinite(dy) ? std::hypot(dx, dy) : 0;
        s.distance = p.distance + step;
        s.direction = step > 1e-9 ? unwrapAngle(p.direction, std::atan2(dy, dx)) : p.direction;
        const double raw = step / s.dt;
        s.speed = p.speed + (raw - p.speed) * (1 - std::exp(-s.dt / speedSmoothing));
        s.acceleration = (s.speed - p.speed) / s.dt;
        // With no tilt the azimuth means nothing: it keeps the last one rather than snapping to zero.
        s.tiltAzimuth = (tiltX == 0 && tiltY == 0) ? p.tiltAzimuth : unwrapAngle(p.tiltAzimuth, azimuth);
        s.twistAngle = unwrapAngle(p.twistAngle, twist);
    }
    s.progress = -1;
    last_ = s;
    return s;
}

void deriveStroke(std::vector<BrushSample>& samples) {
    BrushSampleTrack track;
    for (BrushSample& s : samples) s = track.add(s);
    const double total = samples.empty() ? 0 : samples.back().distance;
    for (BrushSample& s : samples) s.progress = total > 0 ? s.distance / total : 0;
}

BrushSample interpolate(const BrushSample& a, const BrushSample& b, double t) {
    auto mix = [t](double x, double y) { return x + (y - x) * t; };
    BrushSample s = b;
    s.position = {mix(a.position.x, b.position.x), mix(a.position.y, b.position.y)};
    s.time = mix(a.time, b.time);
    s.pressure = mix(a.pressure, b.pressure);
    s.tiltX = mix(a.tiltX, b.tiltX);
    s.tiltY = mix(a.tiltY, b.tiltY);
    s.tangentialPressure = mix(a.tangentialPressure, b.tangentialPressure);
    s.speed = mix(a.speed, b.speed);
    s.acceleration = mix(a.acceleration, b.acceleration);
    s.tiltMagnitude = mix(a.tiltMagnitude, b.tiltMagnitude);
    s.tiltAzimuth = mix(a.tiltAzimuth, b.tiltAzimuth);
    s.twistAngle = mix(a.twistAngle, b.twistAngle);
    s.twist = std::remainder(s.twistAngle * 180 / pi, 360.0);
    s.distance = mix(a.distance, b.distance);
    s.progress = a.progress >= 0 && b.progress >= 0 ? mix(a.progress, b.progress) : -1;
    return s;   // the direction is the segment's, b's
}

std::optional<RecordedStroke> recordedStrokeFromJson(const std::string& text, std::string* error) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) { if (error) *error = "not JSON"; return std::nullopt; }
    RecordedStroke out;
    const nlohmann::json* list = &j;
    if (j.is_object()) {
        auto it = j.find("samples");
        if (it == j.end() || !it->is_array()) { if (error) *error = "no samples array"; return std::nullopt; }
        list = &*it;
        if (auto s = j.find("stylus"); s != j.end() && s->is_boolean()) out.stylus = s->get<bool>();
        if (auto n = j.find("name"); n != j.end() && n->is_string()) out.name = n->get<std::string>();
    }
    if (!list->is_array()) { if (error) *error = "samples must be an array"; return std::nullopt; }
    for (const auto& item : *list) {
        if (!item.is_object()) { if (error) *error = "a sample is not an object"; return std::nullopt; }
        auto number = [&](const char* key, double fallback) {
            auto it = item.find(key);
            const double v = it != item.end() && it->is_number() ? it->get<double>() : fallback;
            return std::isfinite(v) ? v : fallback;
        };
        BrushSample s;
        s.time = number("t", 0);
        s.position = {number("x", 0), number("y", 0)};
        s.stylus = out.stylus;
        if (out.stylus) {
            s.pressure = std::clamp(number("pressure", 1), 0.0, 1.0);
            s.tiltX = std::clamp(number("tiltX", 0), -90.0, 90.0);
            s.tiltY = std::clamp(number("tiltY", 0), -90.0, 90.0);
            s.twist = number("twist", 0);
            s.tangentialPressure = std::clamp(number("tangentialPressure", 0), -1.0, 1.0);
        }
        s.eraser = item.contains("eraser") && item["eraser"].is_boolean() && item["eraser"].get<bool>();
        out.samples.push_back(s);
    }
    if (out.samples.empty()) { if (error) *error = "no samples"; return std::nullopt; }
    return out;
}

std::string recordedStrokeToJson(const RecordedStroke& stroke) {
    nlohmann::json samples = nlohmann::json::array();
    for (const BrushSample& s : stroke.samples) {
        nlohmann::json o = {{"t", s.time}, {"x", s.position.x}, {"y", s.position.y}};
        if (stroke.stylus) {
            o["pressure"] = s.pressure;
            o["tiltX"] = s.tiltX;
            o["tiltY"] = s.tiltY;
            o["twist"] = s.twist;
            o["tangentialPressure"] = s.tangentialPressure;
        }
        if (s.eraser) o["eraser"] = true;
        samples.push_back(std::move(o));
    }
    nlohmann::json j = {{"format", "nekophoto-stroke"}, {"version", 1}, {"name", stroke.name}, {"stylus", stroke.stylus}, {"samples", samples}};
    return j.dump(1);
}

} // namespace compositor
