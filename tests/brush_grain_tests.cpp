// Moving grain under torture (docs/brush-engine.md, "Moving grain"): an asymmetric grain (checkerboard, one-way
// stripes, an L in one corner) in Stroke mode, painted along a straight line, a right-angled corner, an S curve, a
// circle past a full turn, a tight spiral and a stroke drawn right to left, each slow and fast, at 100% and 200% and
// with the view turned. The checks read each dab's grain frame from TipStroke::trace:
//
//   phase     between two neighbouring dabs the grain under the same document point only turns about the dab before
//             (it never slides across the stroke): under a tenth of a pixel on a straight path, under 2 pixels round a
//             corner, an S or a circle, and on the spiral, which ends tighter than the brush, no faster than the path;
//   spin      the grain's tangent never swings outside the directions the path has taken (no spin at a corner or at
//             the start of a stroke), and settles on the path's direction within the smoothing window;
//   wrap      no step where the direction crosses +-180 degrees (the circle, the spiral, the reversed stroke);
//   sampling  the same path reported every 2 or every 16 points lays the grain the same way;
//   rotation  a turned view turns the grain's frame by exactly the turn and changes nothing else;
//   zoom      a size that follows the speed on the screen resolves the same at 200% as at 100%;
//   depth     8 and 16 bits agree within the tip brushes' usual few levels;
//   threads   the worker pool and a serial run paint the same pixels.
#include "check.h"
#include "brush_harness.h"
#include "compositor/parallel.h"
#include <cmath>
#include <cstdio>
#include <map>

using namespace brushharness;

namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double degree = pi / 180;

Preset presetNamed(const std::vector<Preset>& presets, const std::string& name) {
    for (const Preset& p : presets) if (p.name == name) return p;
    std::fprintf(stderr, "no preset %s\n", name.c_str());
    std::abort();
}

/// The grain coordinate (document pixels in the grain's frame, before its scale) that dab `d` puts under point `p`, as
/// TipStroke::dab computes it in Stroke mode.
Point grainAt(const TipDab& d, Point p) {
    const double ox = p.x - d.center.x, oy = p.y - d.center.y, c = std::cos(d.grainTangent), s = std::sin(d.grainTangent);
    return {c * ox + s * oy + d.grainOffset.x, -s * ox + c * oy + d.grainOffset.y};
}

struct Phase {
    double jump = 0;    // the largest move of the grain under a point near a dab (its centre, rings at a quarter and half
                        // its size) between it and the dab before, document pixels
    double slide = 0;   // the largest move under a dab's centre beyond what the frame's turn about the dab before explains
    double spare = 0;   // the largest move under any probe beyond that turn: zero when the grain only turns
    size_t at = 0;      // the dab of the largest jump
};

Phase phaseOf(const std::vector<TipDab>& dabs) {
    Phase out;
    for (size_t k = 1; k < dabs.size(); k++) {
        const double turn = std::fabs(dabs[k].grainTangent - dabs[k - 1].grainTangent);
        const double step = std::hypot(dabs[k].center.x - dabs[k - 1].center.x, dabs[k].center.y - dabs[k - 1].center.y);
        for (double r : {0.0, 0.25, 0.5})
            for (int a = 0; a < 8; a++) {
                const Point p{dabs[k].center.x + r * dabs[k].size * std::cos(a * pi / 4), dabs[k].center.y + r * dabs[k].size * std::sin(a * pi / 4)};
                const Point g0 = grainAt(dabs[k - 1], p), g1 = grainAt(dabs[k], p);
                const double jump = std::hypot(g1.x - g0.x, g1.y - g0.y);
                // A turn by `turn` about the dab before moves the grain under p by at most its distance from there
                // times the turn.
                const double turned = std::hypot(p.x - dabs[k - 1].center.x, p.y - dabs[k - 1].center.y) * turn;
                if (jump > out.jump) { out.jump = jump; out.at = k; }
                out.spare = std::max(out.spare, jump - turned);
                if (r == 0) out.slide = std::max(out.slide, jump - step * turn);
            }
    }
    return out;
}

struct Spin {
    double outside = 0;     // the furthest the tangent went beyond the directions the path had taken so far, radians
    double unsettled = 0;   // the largest gap to the path's direction where it had held for three smoothing windows
    double largestStep = 0; // the tangent's largest turn between neighbouring dabs, radians
    double pathStep = 0;    // the path's largest turn between neighbouring dabs
    double turned = 0;      // the tangent's last minus its first
    double pathTurned = 0;  // the path's
    double backwards = 0;   // the largest turn against the path's own sense of turning
    double fromStart = 0;   // the furthest the tangent strayed from the first dab's direction
};

Spin spinOf(const std::vector<TipDab>& dabs, double diameter) {
    Spin out;
    if (dabs.empty()) return out;
    double lo = dabs[0].grainDirection, hi = lo;
    const double sense = dabs.back().grainDirection >= dabs.front().grainDirection ? 1 : -1;
    for (size_t k = 0; k < dabs.size(); k++) {
        const double dir = dabs[k].grainDirection, tangent = dabs[k].grainTangent;
        lo = std::min(lo, dir);
        hi = std::max(hi, dir);
        out.outside = std::max({out.outside, lo - tangent, tangent - hi});
        out.fromStart = std::max(out.fromStart, std::fabs(tangent - dabs[0].grainDirection));
        // Held: the path's direction within a degree of this one for the last three windows (2 diameters each).
        bool held = dabs[k].distance - dabs[0].distance >= 6 * diameter;
        for (size_t j = k; held && j-- > 0 && dabs[k].distance - dabs[j].distance <= 6 * diameter;) held = std::fabs(dabs[j].grainDirection - dir) <= degree;
        if (held) out.unsettled = std::max(out.unsettled, std::fabs(tangent - dir));
        if (k) {
            out.largestStep = std::max(out.largestStep, std::fabs(tangent - dabs[k - 1].grainTangent));
            out.pathStep = std::max(out.pathStep, std::fabs(dir - dabs[k - 1].grainDirection));
            out.backwards = std::max(out.backwards, -sense * (tangent - dabs[k - 1].grainTangent));
        }
    }
    out.turned = dabs.back().grainTangent - dabs.front().grainTangent;
    out.pathTurned = dabs.back().grainDirection - dabs.front().grainDirection;
    return out;
}

/// The point of the polyline through `dabs`' centres nearest `p`: which segment, how far along it, and how far away.
struct Nearest { size_t k = 1; double t = 0, distance = 1e18; };
Nearest nearest(const std::vector<TipDab>& dabs, Point p) {
    Nearest out;
    for (size_t k = 1; k < dabs.size(); k++) {
        const Point a = dabs[k - 1].center, b = dabs[k].center;
        const double vx = b.x - a.x, vy = b.y - a.y, length2 = vx * vx + vy * vy;
        const double t = length2 > 0 ? std::clamp(((p.x - a.x) * vx + (p.y - a.y) * vy) / length2, 0.0, 1.0) : 0;
        const double d = std::hypot(a.x + vx * t - p.x, a.y + vy * t - p.y);
        if (d < out.distance) out = {k, t, d};
    }
    return out;
}

struct Painted {
    std::vector<TipDab> dabs;
    Render eight;
    Render16 deep;
};

Painted paintTraced(const GrainStroke& g, const Preset& preset) {
    Painted out;
    out.eight = render(g.stroke, preset, &out.dabs);
    std::vector<TipDab> deepDabs;
    out.deep = render16(g.stroke, preset, &deepDabs);
    return out;
}

template <class F>
auto serially(F body) {
    if (workerCount() <= 1) return body();
    decltype(body()) result;
    parallelFor(0, 2, 1, [&](int y0, int) { if (y0 == 0) result = body(); });
    return result;
}

} // namespace

TEST_CASE(moving_grain_stays_attached_on_every_torture_path) {
    const std::vector<Preset> presets = grainTorturePresets();
    const std::vector<GrainStroke> strokes = grainTortureStrokes();
    int failures = 0;
    for (const Preset& preset : presets)
        for (const GrainStroke& g : strokes) {
            const Painted p = paintTraced(g, preset);
            REQUIRE(p.dabs.size() > 10);
            const Phase phase = phaseOf(p.dabs);
            const Spin spin = spinOf(p.dabs, preset.settings.diameter);
            // Everywhere: the grain only turns about the dab before, never slides (spare); the tangent stays within the
            // directions the path has taken (no spin at a corner or at the start) and settles on a steady direction.
            bool ok = phase.spare <= 1e-6 && spin.outside <= 1e-6 && spin.unsettled <= 3 * degree;
            // Straight strokes, the reversed one wobbling across +-180 degrees at every report included: the grain moves
            // under the paper by under a tenth of a pixel and its frame stays within 2 degrees of where it started.
            if (g.path == "straight" || g.path == "reversed") ok = ok && phase.jump <= 0.1 && spin.fromStart <= 2 * degree;
            // A corner, an S, a circle: the tangent turns at most 6 degrees between dabs (its window is two diameters
            // at 10% spacing), so the grain under the edge of a dab moves by under 2 pixels.
            else if (g.path != "spiral") ok = ok && spin.largestStep <= 6 * degree && phase.jump <= 2.0;
            // The spiral ends tighter than the brush: the grain never turns faster than the path itself.
            else ok = ok && spin.largestStep <= spin.pathStep + 1e-9;
            // Round and round: never a step back, above all where the direction passes +-180 degrees.
            if (g.path == "circle" || g.path == "spiral") ok = ok && spin.backwards <= 1e-9 && std::fabs(spin.pathTurned) > 2 * pi && spin.turned > 0.5 * spin.pathTurned;
            std::fprintf(stderr, "  %-4s %-19s %-30s %4zu dabs  jump %.3f px (dab %3zu) slide %+.1e spare %+.1e  outside %.1e  unsettled %.2f deg  step %.2f deg (path %.2f)  turned %.0f of %.0f deg\n",
                         ok ? "ok" : "FAIL", preset.name.c_str(), g.stroke.name.c_str(), p.dabs.size(), phase.jump, phase.at, phase.slide, phase.spare, spin.outside / degree,
                         spin.unsettled / degree, spin.largestStep / degree, spin.pathStep / degree, spin.turned / degree, spin.pathTurned / degree);
            failures += !ok;
        }
    CHECK_EQ(failures, 0);
}

TEST_CASE(slow_and_fast_reports_lay_the_grain_the_same_way) {
    // The same path reported every 2 or every 16 points. At each fast dab, against the slow stroke at the nearest point
    // of its path: how far apart the paths are (the fast one's chords cut the curves), the grain's tangent, and the grain
    // under that dab's centre. Where the fast reports turn by more than 20 degrees each (the tight end of the spiral,
    // where 16-point chords no longer follow the curve) the paths differ by construction and are left out.
    const Preset preset = presetNamed(grainTorturePresets(), "grain_moving");
    for (const std::string& path : grainTorturePaths()) {
        const GrainStroke slowStroke = grainTortureStroke(path, false, 1, 0), fastStroke = grainTortureStroke(path, true, 1, 0);
        std::vector<TipDab> slow, fast;
        const Render slowRender = render(slowStroke.stroke, preset, &slow);
        const Render fastRender = render(fastStroke.stroke, preset, &fast);
        const std::vector<BrushSample>& reports = fastStroke.stroke.samples;
        auto chord = [&](size_t j) { return std::atan2(reports[j].position.y - reports[j - 1].position.y, reports[j].position.x - reports[j - 1].position.x); };
        double apart = 0, tangent = 0, grain = 0;
        size_t compared = 0;
        for (const TipDab& d : fast) {
            size_t j = 1;
            while (j + 1 < reports.size() && reports[j].time < d.time) j++;
            double bend = 0;
            for (size_t i : {j - 1, j + 1})
                if (i >= 1 && i < reports.size()) bend = std::max(bend, std::fabs(std::remainder(chord(i) - chord(j), 2 * pi)));
            if (bend > 20 * degree) continue;
            // The nearest point of the slow path within 20 pixels of the same distance along it (not a lap before).
            std::vector<TipDab> window;
            for (const TipDab& s : slow) if (std::fabs(s.distance - d.distance) <= 20) window.push_back(s);
            if (window.size() < 2) continue;
            const Nearest n = nearest(window, d.center);
            const TipDab& a = window[n.k - 1];
            const TipDab& b = window[n.k];
            TipDab here = a;
            here.center = {a.center.x + (b.center.x - a.center.x) * n.t, a.center.y + (b.center.y - a.center.y) * n.t};
            here.grainTangent = a.grainTangent + (b.grainTangent - a.grainTangent) * n.t;
            here.grainOffset = {a.grainOffset.x + (b.grainOffset.x - a.grainOffset.x) * n.t, a.grainOffset.y + (b.grainOffset.y - a.grainOffset.y) * n.t};
            const Point gs = grainAt(here, d.center), gf = grainAt(d, d.center);
            apart = std::max(apart, n.distance);
            tangent = std::max(tangent, std::fabs(std::remainder(here.grainTangent - d.grainTangent, 2 * pi)));
            grain = std::max(grain, std::hypot(gs.x - gf.x, gs.y - gf.y));
            compared++;
        }
        double paintDiff = 0, painted = 0;
        for (size_t i = 0; i < slowRender.paint.size(); i++) {
            paintDiff += std::abs(int(slowRender.paint[i]) - int(fastRender.paint[i]));
            painted += std::max(slowRender.paint[i], fastRender.paint[i]) > 0;
        }
        const double meanDiff = painted > 0 ? paintDiff / painted / 255 : 0;
        std::fprintf(stderr, "  %-9s slow %3zu dabs, fast %3zu (%3zu compared): paths %.2f px apart, tangent within %.2f deg, grain within %.2f px; paint %.3f apart on average\n",
                     path.c_str(), slow.size(), fast.size(), compared, apart, tangent / degree, grain, meanDiff);
        CHECK(compared > fast.size() / 2);
        CHECK(apart <= 1.5);
        CHECK(tangent <= 8 * degree);
        // The grain's offset adds up the steps between dabs in the frame of the moment. A fast pen's first report is a
        // chord that cuts a curve, so its grain starts turned a few degrees from the slow one's and keeps the offset
        // that turn gives over the smoothing window (two diameters); beyond that it may drift by a small share of
        // the stroke's length.
        const double length = slow.back().distance, start = std::fabs(fast.front().grainTangent - slow.front().grainTangent);
        const double allowed = 1 + 0.005 * length + 2 * preset.settings.diameter * start;
        std::fprintf(stderr, "            starts %.2f deg apart; grain drift %.2f px of %.2f allowed over %.0f px\n", start / degree, grain, allowed, length);
        CHECK(grain <= allowed);
        CHECK(meanDiff <= 0.1);
    }
}

TEST_CASE(a_turned_view_turns_the_grain_and_nothing_else) {
    const Preset preset = presetNamed(grainTorturePresets(), "grain_moving");
    for (const std::string& path : grainTorturePaths()) {
        std::vector<TipDab> level, turned;
        render(grainTortureStroke(path, false, 1, 0).stroke, preset, &level);
        render(grainTortureStroke(path, false, 1, 30).stroke, preset, &turned);
        CHECK_EQ(level.size(), turned.size());
        double worst = 0, distance = 0;
        for (size_t k = 0; k < std::min(level.size(), turned.size()); k++) {
            worst = std::max(worst, std::fabs(std::remainder(turned[k].grainTangent - level[k].grainTangent - 30 * degree, 2 * pi)));
            distance = std::max(distance, std::fabs(turned[k].distance - level[k].distance));
        }
        std::fprintf(stderr, "  %-9s turned 30 degrees: the grain's frame %.2g degrees off the turn, distances %.2g px apart\n", path.c_str(), worst / degree, distance);
        CHECK(worst <= 1e-6);
        CHECK(distance <= 1e-6);
    }
}

TEST_CASE(a_speed_on_screen_size_resolves_the_same_at_200_percent) {
    // With the size following the speed on the screen, each dab at 200% is as large as the 100% stroke at the same
    // moment: the same screen samples, derived, at that dab's time.
    const Preset preset = presetNamed(grainTorturePresets(), "grain_moving_speed");
    const BrushDynamics& dynamics = preset.tip->tip.dynamics;
    for (const std::string& path : grainTorturePaths())
        for (bool fast : {false, true}) {
            std::vector<BrushSample> reference = grainTortureStroke(path, fast, 1, 0).stroke.samples;
            deriveStroke(reference);
            std::vector<TipDab> zoomed;
            render(grainTortureStroke(path, fast, 2, 0).stroke, preset, &zoomed);
            double worst = 0, smallest = 1e9, largest = 0;
            for (const TipDab& d : zoomed) {
                size_t j = 1;
                while (j + 1 < reference.size() && reference[j].time < d.time) j++;
                const double span = reference[j].time - reference[j - 1].time;
                const BrushSample s = interpolate(reference[j - 1], reference[j], span > 0 ? std::clamp((d.time - reference[j - 1].time) / span, 0.0, 1.0) : 0);
                const double expected = applyDynamics(dynamics, DynamicsTarget::Size, preset.settings.diameter, s, preset.settings.diameter, 0, false);
                worst = std::max(worst, std::fabs(d.size - expected) / expected);
                smallest = std::min(smallest, d.size);
                largest = std::max(largest, d.size);
            }
            std::fprintf(stderr, "  %-9s %s at 200%%: dab sizes %.1f..%.1f px, within %.2g of the 100%% stroke's at the same moment\n",
                         path.c_str(), fast ? "fast" : "slow", smallest, largest, worst);
            CHECK(worst <= 1e-9);
            if (fast) CHECK(largest - smallest > 2);   // the speed does move the size
        }
}

TEST_CASE(moving_grain_agrees_across_depths_and_threads) {
    const std::vector<Preset> presets = grainTorturePresets();
    int apart = 0, threads = 0;
    Calibration worst;
    for (const Preset& preset : presets)
        for (const GrainStroke& g : grainTortureStrokes()) {
            const Render eight = render(g.stroke, preset);
            const Render16 deep = render16(g.stroke, preset);
            const Calibration c = compare(eight, deep);
            worst.worst = std::max(worst.worst, c.worst);
            worst.beyondOne = std::max(worst.beyondOne, c.beyondOne);
            if (c.worst > 4 || c.beyondOne >= 0.01) { std::fprintf(stderr, "  %s/%s: %d levels apart, %.3f%% beyond a level\n", g.stroke.name.c_str(), preset.name.c_str(), c.worst, c.beyondOne * 100); apart++; }
            const Render eightSerial = serially([&] { return render(g.stroke, preset); });
            const Render16 deepSerial = serially([&] { return render16(g.stroke, preset); });
            if (hashRender(eight) != hashRender(eightSerial) || hashRender(deep) != hashRender(deepSerial)) {
                std::fprintf(stderr, "  %s/%s: the worker pool and a serial run differ\n", g.stroke.name.c_str(), preset.name.c_str());
                threads++;
            }
        }
    std::fprintf(stderr, "  16 bits against 8: worst %d levels, at most %.3f%% of samples beyond a level (%d worker threads)\n", worst.worst, worst.beyondOne * 100, workerCount());
    CHECK_EQ(apart, 0);
    CHECK_EQ(threads, 0);
}

TEST_CASE(a_replay_lays_the_same_grain) {
    const Preset preset = presetNamed(grainTorturePresets(), "grain_moving_speed");
    const GrainStroke g = grainTortureStroke("spiral", true, 2, 30);
    std::vector<TipDab> a, b;
    const Render ra = render(g.stroke, preset, &a), rb = render(g.stroke, preset, &b);
    CHECK_EQ(hashRender(ra), hashRender(rb));
    REQUIRE(a.size() == b.size());
    for (size_t k = 0; k < a.size(); k++) CHECK(a[k].grainTangent == b[k].grainTangent && a[k].size == b[k].size && a[k].center.x == b[k].center.x);
}

TEST_MAIN()
