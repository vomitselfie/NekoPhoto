// Brush parity: every standard motion and recorded stroke (brush_harness.h) painted with every harness preset, each
// render hashed and measured, and held to tests/brush_parity_baseline.txt. A change to the brush engines that is meant
// to change nothing must leave every line as it is; one that is meant to change something shows which strokes moved
// and how (width, peak alpha, edge, taper). Every scene is painted on the worker pool and serially, which must agree.
//
// COMPOSITOR_UPDATE_BRUSH_PARITY=1 rewrites the baseline after an intentional change.
#include "check.h"
#include "brush_harness.h"
#include "compositor/mypaint.h"
#include "compositor/parallel.h"
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

using namespace brushharness;

namespace {

/// Runs `body` with every nested parallel loop serial: the caller of parallelFor marks itself as inside a loop.
Render serially(const std::function<Render()>& body) {
    if (workerCount() <= 1) return body();
    Render result;
    parallelFor(0, 2, 1, [&](int y0, int) { if (y0 == 0) result = body(); });
    return result;
}

std::vector<StrokeFixture> allFixtures() {
    std::vector<StrokeFixture> fixtures = standardFixtures();
    for (StrokeFixture& f : fileFixtures(BRUSH_FIXTURES_DIR)) fixtures.push_back(std::move(f));
    return fixtures;
}

} // namespace

TEST_CASE(fixtures_survive_a_json_round_trip) {
    for (const StrokeFixture& f : allFixtures()) {
        auto back = fixtureFromJson(fixtureToJson(f), f.name);
        REQUIRE(back.has_value());
        CHECK(back->name == f.name);
        CHECK_EQ(back->stylus, f.stylus);
        REQUIRE(back->samples.size() == f.samples.size());
        for (size_t i = 0; i < f.samples.size(); i++) {
            const StrokeSample& a = f.samples[i];
            const StrokeSample& b = back->samples[i];
            CHECK(a.t == b.t && a.x == b.x && a.y == b.y && a.pressure == b.pressure && a.tiltX == b.tiltX && a.tiltY == b.tiltY
                  && a.twist == b.twist && a.tangentialPressure == b.tangentialPressure);
        }
    }
    // A bare array with missing fields takes the defaults.
    auto bare = fixtureFromJson(R"([{"x": 1, "y": 2}, {"t": 0.01, "x": 3, "y": 4, "pressure": 0.5}])", "bare");
    REQUIRE(bare.has_value());
    REQUIRE(bare->samples.size() == 2);
    CHECK_EQ(bare->samples[0].pressure, 1.0);
    CHECK_EQ(bare->samples[1].pressure, 0.5);
    CHECK(!fixtureFromJson("{}", "empty").has_value());
    CHECK(!fixtureFromJson("[1, 2]", "numbers").has_value());
}

TEST_CASE(every_fixture_and_preset_matches_the_baseline) {
    const std::vector<StrokeFixture> fixtures = allFixtures();
    const std::vector<Preset> presets = standardPresets(MYPAINT_BRUSHES_DIR);
    CHECK(fixtures.size() >= 12);
    std::map<std::string, std::string> actual;
    int threadMismatch = 0, empty = 0;
    for (const Scene& scene : scenes(fixtures, presets)) {
        const Render pooled = render(*scene.fixture, *scene.preset);
        const Render serial = serially([&] { return render(*scene.fixture, *scene.preset); });
        const uint64_t hash = hashRender(pooled);
        if (!pooled.image) { std::fprintf(stderr, "  %s: nothing rendered\n", scene.name.c_str()); empty++; continue; }
        if (hash != hashRender(serial)) {
            std::fprintf(stderr, "  %s: %s on %d threads, %s serially\n", scene.name.c_str(), hex(hash).c_str(), workerCount(), hex(hashRender(serial)).c_str());
            threadMismatch++;
        }
        actual[scene.name] = hex(hash) + " " + formatMetrics(measure(*scene.fixture, pooled));
    }
    CHECK_EQ(empty, 0);
    CHECK_EQ(threadMismatch, 0);
    std::fprintf(stderr, "  %zu scenes (%zu fixtures x %zu presets, %d worker threads)\n", actual.size(), fixtures.size(), presets.size(), workerCount());

    const std::string path = BRUSH_PARITY_BASELINE;
    if (std::getenv("COMPOSITOR_UPDATE_BRUSH_PARITY")) {
        std::ofstream out(path);
        out << "# Brush parity baseline: <fixture>/<preset> <FNV-1a 64 of the layer's pixels> <measurements>, from tests/brush_parity.cpp.\n"
               "# Regenerate after an intentional brush change: COMPOSITOR_UPDATE_BRUSH_PARITY=1 build/tests/brush_parity\n";
        for (auto& [name, line] : actual) out << name << ' ' << line << '\n';
        std::fprintf(stderr, "  wrote %s\n", path.c_str());
        return;
    }
    std::map<std::string, std::string> expected;
    {
        std::ifstream in(path);
        for (std::string line; std::getline(in, line);) {
            if (line.empty() || line[0] == '#') continue;
            const size_t space = line.find(' ');
            if (space != std::string::npos) expected[line.substr(0, space)] = line.substr(space + 1);
        }
    }
    REQUIRE(!expected.empty());
    auto hashOf = [](const std::string& line) { return line.substr(0, line.find(' ')); };
    int changed = 0, added = 0, missing = 0;
    for (auto& [name, line] : actual) {
        auto it = expected.find(name);
        if (it == expected.end()) { std::fprintf(stderr, "  new      %s %s\n", name.c_str(), line.c_str()); added++; }
        else if (hashOf(it->second) != hashOf(line)) {
            std::fprintf(stderr, "  changed  %s\n    was %s\n    now %s\n", name.c_str(), it->second.c_str(), line.c_str());
            changed++;
        }
    }
    for (auto& [name, line] : expected)
        if (!actual.count(name)) {
            // MyPaint and Clip Studio scenes exist only in builds with libmypaint and SQLite.
            if ((name.find("/mypaint_") != std::string::npos && !myPaintSupported()) || name.find("/sut_") != std::string::npos) continue;
            std::fprintf(stderr, "  missing  %s\n", name.c_str());
            missing++;
        }
    if (changed || added || missing)
        std::fprintf(stderr, "  %d changed, %d new, %d missing; COMPOSITOR_UPDATE_BRUSH_PARITY=1 rewrites %s\n", changed, added, missing, path.c_str());
    CHECK_EQ(changed, 0);
    CHECK_EQ(added, 0);
    CHECK_EQ(missing, 0);
}

TEST_MAIN()
