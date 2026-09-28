// Smoothing (brushsmoothing.h) on noisy fixtures: how far the stroke strays from the clean path, how far the brush
// trails the pen, and what the stroke end does; the three filters kept apart, and angles filtered round the circle.
#include "brush_harness.h"
#include "check.h"
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace brushharness;

namespace {

BrushSmoothing inputOnly(double amount) { BrushSmoothing s; s.input = amount; return s; }
BrushSmoothing stabilizer(double amount) { BrushSmoothing s; s.stabilizer = amount; return s; }

const JitterFixture& fixture(const char* name) {
    static const std::vector<JitterFixture> all = jitterFixtures();
    for (const JitterFixture& f : all) if (f.name == name) return f;
    return all.front();
}

double circular(double degrees) { return std::fabs(std::remainder(degrees, 360.0)); }

} // namespace

TEST_CASE(the_numbers) {
    struct Setting { const char* name; BrushSmoothing smoothing; };
    std::vector<Setting> settings = {{"off", {}}};
    for (double a : {10.0, 25.0, 50.0, 100.0}) settings.push_back({nullptr, inputOnly(a)});
    for (double a : {10.0, 25.0, 50.0, 100.0}) settings.push_back({nullptr, stabilizer(a)});
    for (double a : {25.0, 50.0}) { BrushSmoothing s = stabilizer(a); s.strokeCatchUp = false; settings.push_back({nullptr, s}); }
    for (double a : {25.0, 50.0}) { BrushSmoothing s = stabilizer(a); s.pulledString = true; settings.push_back({nullptr, s}); }
    { BrushSmoothing s = stabilizer(50); s.catchUpOnEnd = true; settings.push_back({nullptr, s}); }
    { BrushSmoothing s; s.pressure = 50; settings.push_back({nullptr, s}); }
    { BrushSmoothing s = stabilizer(25); s.input = 25; s.pressure = 25; settings.push_back({nullptr, s}); }
    std::printf("%-34s %-10s %8s %8s %8s %8s %8s %8s %6s\n", "setting", "fixture", "dev", "dev95", "lag", "lagMax", "endGap", "pNoise", "n");
    for (const Setting& setting : settings) {
        const BrushSmoothing& s = setting.smoothing;
        char name[96];
        std::snprintf(name, sizeof name, "in%.0f st%.0f%s%s%s p%.0f", s.input, s.stabilizer, s.pulledString ? " pulled" : "",
                      s.strokeCatchUp ? "" : " no-catch-up", s.catchUpOnEnd ? " end" : "", s.pressure);
        for (const JitterFixture& f : jitterFixtures()) {
            const SmoothingMetrics m = measureSmoothing(f, s);
            std::printf("%-34s %-10s %8.2f %8.2f %8.2f %8.2f %8.2f %8.3f %6zu\n", name, f.name.c_str(), m.deviation, m.deviationP95, m.lag, m.lagMax,
                        m.endGap, m.pressureNoise, m.samples);
        }
    }
}

TEST_CASE(off_passes_every_sample_through_unchanged) {
    for (const JitterFixture& f : jitterFixtures()) {
        std::vector<BrushSample> out;
        measureSmoothing(f, {}, &out);
        CHECK_EQ(out.size(), f.noisy.samples.size());
        bool same = out.size() == f.noisy.samples.size();
        for (size_t i = 0; same && i < out.size(); i++) {
            const BrushSample &a = out[i], &b = f.noisy.samples[i];
            same = a.position.x == b.position.x && a.position.y == b.position.y && a.time == b.time && a.pressure == b.pressure
                && a.twist == b.twist && a.tiltX == b.tiltX && a.tiltY == b.tiltY;
        }
        CHECK(same);
    }
    CHECK(!BrushSmoothing{}.active());
}

TEST_CASE(input_smoothing_removes_jitter_with_little_lag) {
    for (const char* name : {"line", "s_curve", "circle"}) {
        const SmoothingMetrics raw = measureSmoothing(fixture(name), {});
        const SmoothingMetrics light = measureSmoothing(fixture(name), inputOnly(25));
        const SmoothingMetrics heavy = measureSmoothing(fixture(name), inputOnly(100));
        CHECK(light.deviation < raw.deviation * 0.8);
        CHECK(heavy.deviation < light.deviation);
        CHECK(light.lag < 4);   // at about 500 pixels a second
        CHECK(heavy.lag < 12);
    }
    // The cutoff rises with speed: a fast pen is followed closely.
    const SmoothingMetrics fast = measureSmoothing(fixture("fast_line"), inputOnly(25));
    CHECK(fast.lag < 8);   // at 2000 pixels a second
}

TEST_CASE(the_stabilizer_trails_the_pen_and_smooths_more) {
    for (const char* name : {"line", "s_curve", "circle"}) {
        const SmoothingMetrics raw = measureSmoothing(fixture(name), {});
        const SmoothingMetrics some = measureSmoothing(fixture(name), stabilizer(25));
        const SmoothingMetrics more = measureSmoothing(fixture(name), stabilizer(50));
        CHECK(some.deviation < raw.deviation);
        CHECK(some.lag < more.lag);
        CHECK(more.lag > 10);
    }
    // On a straight line the extra smoothing cannot cut a corner: the heavier setting strays less.
    CHECK(measureSmoothing(fixture("line"), stabilizer(50)).deviation < measureSmoothing(fixture("line"), stabilizer(25)).deviation);
}

TEST_CASE(pulled_string_trails_by_the_string_and_ignores_moves_inside_it) {
    BrushSmoothing pulled = stabilizer(50);
    pulled.pulledString = true;
    const double reach = stabilizerReach(pulled, 1);
    CHECK_NEAR(reach, 50, 1e-9);
    // Once taut on the line, the brush is the string's length behind the pen (give or take the noise).
    BrushStabilizer s(pulled);
    std::vector<BrushSample> out;
    const JitterFixture& line = fixture("line");
    double worst = 0;
    for (size_t i = 0; i < line.clean.samples.size(); i++) {
        s.add(line.clean.samples[i], out);
        if (line.clean.samples[i].position.x > 30 + reach + 5) {
            const Point b = s.brush(), p = line.clean.samples[i].position;
            worst = std::max(worst, std::fabs(std::hypot(p.x - b.x, p.y - b.y) - reach));
        }
    }
    CHECK(worst < 1e-6);
    // A pen wandering inside the string leaves no mark.
    BrushStabilizer still(pulled);
    out.clear();
    for (int i = 0; i < 100; i++) {
        BrushSample p;
        p.time = i / 120.0;
        p.position = {100 + 20 * std::cos(i * 0.7), 100 + 20 * std::sin(i * 1.3)};   // never 50 from the first
        still.add(p, out);
    }
    CHECK_EQ(out.size(), size_t(1));
    // Adjust For Zoom: the string is the same length on the screen, so half as long in the document at 200%.
    CHECK_NEAR(stabilizerReach(pulled, 2), 25, 1e-9);
    pulled.adjustForZoom = false;
    CHECK_NEAR(stabilizerReach(pulled, 2), 50, 1e-9);
}

TEST_CASE(catch_up_on_stroke_end_finishes_where_the_pen_lifted) {
    for (const char* name : {"line", "s_curve", "circle"}) {
        BrushSmoothing s = stabilizer(50);
        const SmoothingMetrics trailing = measureSmoothing(fixture(name), s);
        s.catchUpOnEnd = true;
        const SmoothingMetrics caught = measureSmoothing(fixture(name), s);
        CHECK(trailing.endGap > 10);
        CHECK(caught.endGap < 5);   // where the noisy pen lifted, within the noise of the clean end
        s.pulledString = true;
        CHECK(measureSmoothing(fixture(name), s).endGap < 5);
    }
}

TEST_CASE(stroke_catch_up_closes_on_a_paused_pen_only_when_on) {
    for (bool catchUp : {true, false}) {
        BrushSmoothing smoothing = stabilizer(50);
        smoothing.strokeCatchUp = catchUp;
        BrushStabilizer s(smoothing);
        std::vector<BrushSample> out;
        for (const BrushSample& p : fixture("line").clean.samples) s.add(p, out);
        const double before = std::hypot(s.pen().position.x - s.brush().x, s.pen().position.y - s.brush().y);
        CHECK(before > 5);
        const size_t given = out.size();
        s.tick(fixture("line").clean.samples.back().time + 1, out);
        const double after = std::hypot(s.pen().position.x - s.brush().x, s.pen().position.y - s.brush().y);
        if (catchUp) { CHECK(after < 0.5); CHECK(out.size() > given); }
        else { CHECK_NEAR(after, before, 1e-12); CHECK_EQ(out.size(), given); }
    }
}

TEST_CASE(the_stabilizer_does_not_depend_on_the_report_rate) {
    // The same clean S curve reported at 60 and at 240 a second: the stabilised paths stay within a pixel of each other.
    auto path = [](int rate) {
        std::vector<BrushSample> samples;
        const int n = rate * 3 / 4;
        for (int i = 0; i <= n; i++) {
            const double u = double(i) / n;
            BrushSample s;
            s.time = i / double(rate);
            s.position = {30 + 260 * u, 100 + 50 * std::sin(2 * 3.14159265358979323846 * u)};
            samples.push_back(s);
        }
        BrushStabilizer st(stabilizer(50));
        std::vector<BrushSample> out;
        for (const BrushSample& s : samples) st.add(s, out);
        return out;
    };
    const std::vector<BrushSample> slow = path(60), fast = path(240);
    double worst = 0;
    for (const BrushSample& s : slow) {
        double best = 1e9;
        for (size_t i = 1; i < fast.size(); i++) {
            const Point a = fast[i - 1].position, b = fast[i].position;
            const double dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy;
            const double t = l > 0 ? std::clamp(((s.position.x - a.x) * dx + (s.position.y - a.y) * dy) / l, 0.0, 1.0) : 0;
            best = std::min(best, std::hypot(s.position.x - a.x - dx * t, s.position.y - a.y - dy * t));
        }
        worst = std::max(worst, best);
    }
    std::printf("60 vs 240 reports a second: %.3f px apart at most\n", worst);
    CHECK(worst < 1);
}

TEST_CASE(pressure_smoothing_leaves_the_position_alone) {
    BrushSmoothing s;
    s.pressure = 50;
    for (const JitterFixture& f : jitterFixtures()) {
        std::vector<BrushSample> out;
        const SmoothingMetrics m = measureSmoothing(f, s, &out);
        const SmoothingMetrics raw = measureSmoothing(f, {});
        CHECK(m.pressureNoise < raw.pressureNoise * 0.6);
        bool samePositions = out.size() == f.noisy.samples.size();
        for (size_t i = 0; samePositions && i < out.size(); i++)
            samePositions = out[i].position.x == f.noisy.samples[i].position.x && out[i].position.y == f.noisy.samples[i].position.y;
        CHECK(samePositions);
    }
    // And position smoothing leaves the pressure alone.
    std::vector<BrushSample> out;
    measureSmoothing(fixture("line"), inputOnly(50), &out);
    bool samePressure = true;
    for (size_t i = 0; i < out.size(); i++) samePressure = samePressure && out[i].pressure == fixture("line").noisy.samples[i].pressure;
    CHECK(samePressure);
}

TEST_CASE(angles_are_smoothed_round_the_circle) {
    // A twist jittering either side of 0 (reported as 359, 1, -2 and so on) stays near 0, never near 180.
    std::vector<BrushSample> out;
    measureSmoothing(fixture("circle"), inputOnly(50), &out);
    double worst = 0;
    for (const BrushSample& s : out) worst = std::max(worst, circular(s.twist));
    CHECK(worst < 6);
    // A twist turning steadily through the wrap follows it the short way round.
    BrushStabilizer st(inputOnly(50));
    out.clear();
    for (int i = 0; i <= 40; i++) {
        BrushSample p;
        p.time = i / 120.0;
        p.position = {30.0 + i, 100};
        p.twist = std::remainder(340.0 + i, 360.0);
        p.twistReported = true;
        p.tiltX = 30 * std::cos((170 + i) * 3.14159265358979323846 / 180);   // the azimuth crosses 180 degrees
        p.tiltY = 30 * std::sin((170 + i) * 3.14159265358979323846 / 180);
        st.add(p, out);
    }
    double previous = out.front().twist;
    double biggest = 0;
    for (const BrushSample& s : out) { biggest = std::max(biggest, circular(s.twist - previous)); previous = s.twist; }
    CHECK(biggest < 2);
    for (const BrushSample& s : out) CHECK(std::hypot(s.tiltX, s.tiltY) > 25);   // the tilt vector does not collapse
}

TEST_CASE(each_filter_costs_microseconds_per_report) {
    // The latency budget: input smoothing must add well under a millisecond to a pen report. Each filter over the
    // circle's reports, repeated; the time per report printed, and held under 20 microseconds.
    const JitterFixture& circle = fixture("circle");
    struct Mode { const char* name; BrushSmoothing smoothing; };
    BrushSmoothing all = stabilizer(50);
    all.input = 50; all.pressure = 50; all.catchUpOnEnd = true;
    BrushSmoothing pulled = stabilizer(50);
    pulled.pulledString = true;
    BrushSmoothing pressure;
    pressure.pressure = 50;
    for (const Mode& mode : {Mode{"input", inputOnly(50)}, Mode{"stabilizer", stabilizer(50)}, Mode{"pulled", pulled}, Mode{"pressure", pressure}, Mode{"all", all}}) {
        std::vector<BrushSample> out;
        out.reserve(4096);
        size_t reports = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int round = 0; round < 200; round++) {
            BrushStabilizer s(mode.smoothing);
            out.clear();
            for (const BrushSample& p : circle.noisy.samples) { s.add(p, out); reports++; }
            s.finish(out);
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / double(reports);
        std::printf("%-10s %.3f us per report\n", mode.name, us);
        CHECK(us < 20);
    }
}

TEST_MAIN()
