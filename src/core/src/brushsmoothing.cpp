#include "compositor/brushsmoothing.h"
#include <algorithm>
#include <cmath>

namespace compositor {

namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double fallbackDt = 1.0 / 120;

// ---- The constants, in one place ----
// Input smoothing (One Euro): the cutoff at rest falls from 25 Hz at 1% to 1 Hz at 100%, and rises with the pen's
// speed on the screen by `speedCutoff` Hz per point a second, so a moving pen is followed closely and a resting one
// is held still. The speed itself is smoothed at `speedCutoffHz`.
constexpr double restCutoffMax = 25, restCutoffMin = 1;
constexpr double speedCutoff = 0.05;
constexpr double speedCutoffHz = 1;
// The tilt and the twist: a plain low pass, 12 Hz at 1% down to 0.6 Hz at 100%. Slower than the position's at rest,
// since a pen's angles wander more slowly and read coarser than its position; there is no speed term.
constexpr double angleCutoffMax = 12, angleCutoffMin = 0.6;
// Pressure smoothing: a one-pole low pass whose time constant grows to 120 ms at 100%.
constexpr double pressureTauMax = 0.12;
// The stabiliser: its reach is the amount in screen points (100 at 100%); its time constant with Stroke Catch-Up
// grows to 250 ms at 100%.
constexpr double reachPerPercent = 1;
constexpr double stabilizerTauMax = 0.25;
// The stabiliser is stepped at least every `substep` document pixels of the pen's travel, so its curve does not
// depend on how often the pen reports; the brush gives the stroke a sample once it has moved `emitStep`.
constexpr double substep = 2, emitStep = 0.5;
constexpr int maxSubsteps = 64;

double fraction(double amount) { return std::clamp(amount, 0.0, 100.0) / 100; }

/// A first-order low pass's weight for the new value at cutoff `hz` after `dt` seconds.
double lowPass(double hz, double dt) { return 1 / (1 + 1 / (2 * pi * hz * dt)); }

double stabilizerTau(double amount) { const double a = fraction(amount); return std::max(1e-4, stabilizerTauMax * a * a); }

/// The raw fields of the pen a fraction `t` from `a` to `b` (the derived ones are left for the track).
BrushSample mixPen(const BrushSample& a, const BrushSample& b, double t) {
    auto mix = [t](double x, double y) { return x + (y - x) * t; };
    BrushSample s = b;
    s.position = {mix(a.position.x, b.position.x), mix(a.position.y, b.position.y)};
    s.time = mix(a.time, b.time);
    s.pressure = mix(a.pressure, b.pressure);
    s.tiltX = mix(a.tiltX, b.tiltX);
    s.tiltY = mix(a.tiltY, b.tiltY);
    s.tangentialPressure = mix(a.tangentialPressure, b.tangentialPressure);
    const double from = a.twist, to = unwrapAngle(from * pi / 180, b.twist * pi / 180) * 180 / pi;
    s.twist = std::remainder(mix(from, to), 360.0);
    s.viewScale = mix(a.viewScale, b.viewScale);
    return s;
}

} // namespace

double stabilizerReach(const BrushSmoothing& smoothing, double viewScale) {
    const double screen = reachPerPercent * std::clamp(smoothing.stabilizer, 0.0, 100.0);
    const double scale = std::isfinite(viewScale) && viewScale > 0 ? viewScale : 1;
    return smoothing.adjustForZoom ? screen / scale : screen;
}

void BrushStabilizer::reset() {
    started_ = false;
    speedX_ = speedY_ = brushSpeed_ = 0;
}

void BrushStabilizer::add(const BrushSample& raw, std::vector<BrushSample>& out) {
    if (!started_) {
        started_ = true;
        raw_ = pen_ = raw;
        twist_ = raw.twist;
        brush_ = emitted_ = raw.position;
        clock_ = emittedTime_ = raw.time;
        speedX_ = speedY_ = brushSpeed_ = 0;
        out.push_back(raw);
        return;
    }
    const double elapsed = raw.time - raw_.time;
    const double dt = std::isfinite(elapsed) && elapsed > 0 ? elapsed : fallbackDt;
    BrushSample f = raw;
    if (smoothing_.input > 0) {
        const double a = fraction(smoothing_.input);
        // One Euro on the position: the velocity (from the filtered position, smoothed), then a cutoff that rises
        // with it on the screen.
        const double vx = (raw.position.x - pen_.position.x) / dt, vy = (raw.position.y - pen_.position.y) / dt;
        const double av = lowPass(speedCutoffHz, dt);
        speedX_ += (vx - speedX_) * av;
        speedY_ += (vy - speedY_) * av;
        const double scale = std::isfinite(raw.viewScale) && raw.viewScale > 0 ? raw.viewScale : 1;
        const double rest = restCutoffMax * std::pow(restCutoffMin / restCutoffMax, a);
        const double ap = lowPass(rest + speedCutoff * std::hypot(speedX_, speedY_) * scale, dt);
        f.position = {pen_.position.x + (raw.position.x - pen_.position.x) * ap, pen_.position.y + (raw.position.y - pen_.position.y) * ap};
        // The angles: the tilt as a vector (its azimuth then turns the short way round), the twist on its unwrapped
        // angle, so 359 and 1 degrees average to 0, not 180.
        const double aa = lowPass(angleCutoffMax * std::pow(angleCutoffMin / angleCutoffMax, a), dt);
        f.tiltX = pen_.tiltX + (raw.tiltX - pen_.tiltX) * aa;
        f.tiltY = pen_.tiltY + (raw.tiltY - pen_.tiltY) * aa;
        const double twist = unwrapAngle(twist_ * pi / 180, raw.twist * pi / 180) * 180 / pi;
        twist_ += (twist - twist_) * aa;
        f.twist = std::remainder(twist_, 360.0);
    } else {
        twist_ = raw.twist;
    }
    if (smoothing_.pressure > 0) {
        const double a = fraction(smoothing_.pressure);
        const double k = 1 - std::exp(-dt / std::max(1e-4, pressureTauMax * std::pow(a, 1.5)));
        f.pressure = pen_.pressure + (raw.pressure - pen_.pressure) * k;
        f.tangentialPressure = pen_.tangentialPressure + (raw.tangentialPressure - pen_.tangentialPressure) * k;
    }
    const BrushSample before = pen_;
    pen_ = f;
    raw_ = raw;
    if (smoothing_.stabilizer <= 0) {
        brush_ = emitted_ = f.position;
        emittedTime_ = clock_ = f.time;
        out.push_back(f);
        return;
    }
    stabilize(before, f, out);
}

void BrushStabilizer::stabilize(const BrushSample& from, const BrushSample& to, std::vector<BrushSample>& out) {
    const double reach = std::max(1e-6, stabilizerReach(smoothing_, to.viewScale));
    const double dx = to.position.x - from.position.x, dy = to.position.y - from.position.y;
    const double travel = std::hypot(dx, dy);
    const int steps = std::clamp(int(std::ceil(travel / substep)), 1, maxSubsteps);
    const double t0 = clock_, t1 = std::max(clock_, to.time);
    const double tau = stabilizerTau(smoothing_.stabilizer);
    for (int i = 1; i <= steps; i++) {
        const double s = double(i) / steps;
        const Point target{from.position.x + dx * s, from.position.y + dy * s};
        const double vx = target.x - brush_.x, vy = target.y - brush_.y;
        if (smoothing_.pulledString) {
            // Photoshop's Pulled String Mode: the brush stays put while the pen moves inside the string's length, and
            // is dragged along behind it, the string taut, once the pen goes further.
            const double gap = std::hypot(vx, vy);
            if (gap > reach) brush_ = {target.x - vx * reach / gap, target.y - vy * reach / gap};
        } else {
            // With Stroke Catch-Up the brush closes on the pen with time, so it keeps going while the pen pauses;
            // without, only as the pen moves, and a steady pen is trailed by the reach.
            const double k = smoothing_.strokeCatchUp ? 1 - std::exp(-(t1 - t0) / steps / tau) : 1 - std::exp(-(travel / steps) / reach);
            brush_ = {brush_.x + vx * k, brush_.y + vy * k};
        }
        BrushSample pen = mixPen(from, to, s);
        pen.time = t0 + (t1 - t0) * s;
        give(pen, out, false);
    }
    clock_ = t1;
}

void BrushStabilizer::tick(double time, std::vector<BrushSample>& out) {
    if (!started_ || smoothing_.stabilizer <= 0 || smoothing_.pulledString || !smoothing_.strokeCatchUp) return;
    if (!std::isfinite(time) || time <= clock_) return;
    const double tau = stabilizerTau(smoothing_.stabilizer), total = time - clock_;
    const Point target = pen_.position;
    const double closing = std::hypot(target.x - brush_.x, target.y - brush_.y) * (1 - std::exp(-total / tau));
    const int steps = std::clamp(int(std::ceil(closing / substep)), 1, maxSubsteps);
    const double k = 1 - std::exp(-total / steps / tau);
    for (int i = 1; i <= steps; i++) {
        brush_ = {brush_.x + (target.x - brush_.x) * k, brush_.y + (target.y - brush_.y) * k};
        BrushSample pen = pen_;
        pen.time = clock_ + total * i / steps;
        give(pen, out, false);
    }
    clock_ = time;
}

void BrushStabilizer::finish(std::vector<BrushSample>& out) {
    if (!started_ || smoothing_.stabilizer <= 0 || !smoothing_.catchUpOnEnd) return;
    const double gap = std::hypot(pen_.position.x - emitted_.x, pen_.position.y - emitted_.y);
    if (gap < 1e-3) return;
    // Straight on to where the pen lifted, at the pace the brush had, so speed dynamics do not jump at the end.
    brush_ = pen_.position;
    BrushSample pen = pen_;
    pen.time = std::max(pen_.time, emittedTime_ + gap / std::max(brushSpeed_, 100.0));
    give(pen, out, true);
}

void BrushStabilizer::give(const BrushSample& pen, std::vector<BrushSample>& out, bool force) {
    const double moved = std::hypot(brush_.x - emitted_.x, brush_.y - emitted_.y);
    if (!force && moved < emitStep) return;
    BrushSample s = pen;
    s.position = brush_;
    s.time = std::max(pen.time, emittedTime_);
    const double dt = s.time - emittedTime_;
    if (dt > 0) brushSpeed_ += (moved / dt - brushSpeed_) * (1 - std::exp(-dt / 0.05));
    emitted_ = brush_;
    emittedTime_ = s.time;
    out.push_back(s);
}

} // namespace compositor
