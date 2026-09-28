// The brush parity harness by hand (not a test): paints the harness's fixtures with its presets and writes the images,
// the measurements and the fixtures themselves, for looking at a brush change rather than only hashing it.
//
//   brush_parity_tool list                            the fixtures and presets
//   brush_parity_tool dump <folder> [filter]          <fixture>__<preset>.png, metrics.txt and fixtures/*.json
//   brush_parity_tool render <stroke.json> <preset> <out.png>   one recorded stroke with one preset
//   brush_parity_tool grain <folder>                  the moving-grain torture scenes (brush_grain_tests), the grain
//                                                     itself, and each scene's dabs (centre, tangent, grain offset)
#include "brush_harness.h"
#include "compositor/png.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace brushharness;
namespace fs = std::filesystem;

namespace {

int usage() {
    std::fprintf(stderr, "usage: brush_parity_tool list | dump <folder> [filter] | render <stroke.json> <preset> <out.png> | grain <folder>\n");
    return 2;
}

std::vector<StrokeFixture> allFixtures() {
    std::vector<StrokeFixture> fixtures = standardFixtures();
    for (StrokeFixture& f : fileFixtures(BRUSH_FIXTURES_DIR)) fixtures.push_back(std::move(f));
    return fixtures;
}

std::string fileName(std::string name) {
    for (char& c : name) if (c == '/') c = '_';
    return name;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string command = argv[1];
    const std::vector<Preset> presets = standardPresets(MYPAINT_BRUSHES_DIR);
    if (command == "list") {
        for (const StrokeFixture& f : allFixtures()) std::printf("fixture %s (%zu samples%s)\n", f.name.c_str(), f.samples.size(), f.stylus ? "" : ", mouse");
        for (const Preset& p : presets) std::printf("preset  %s\n", p.name.c_str());
        return 0;
    }
    if (command == "dump" && argc >= 3) {
        const fs::path folder = argv[2];
        const std::string filter = argc > 3 ? argv[3] : "";
        fs::create_directories(folder / "fixtures");
        const std::vector<StrokeFixture> fixtures = allFixtures();
        for (const StrokeFixture& f : fixtures) std::ofstream(folder / "fixtures" / (f.name + ".json")) << fixtureToJson(f) << "\n";
        std::ofstream metrics(folder / "metrics.txt");
        int written = 0;
        for (const Scene& scene : scenes(fixtures, presets)) {
            if (!filter.empty() && scene.name.find(filter) == std::string::npos) continue;
            const Render r = render(*scene.fixture, *scene.preset);
            if (!r.image) { std::fprintf(stderr, "%s: nothing rendered\n", scene.name.c_str()); continue; }
            writePngImage((folder / (fileName(scene.name) + ".png")).string(), *r.image);
            metrics << scene.name << ' ' << hex(hashRender(r)) << ' ' << formatMetrics(measure(*scene.fixture, r)) << '\n';
            written++;
        }
        std::printf("%d renders in %s\n", written, folder.string().c_str());
        return 0;
    }
    if (command == "grain" && argc >= 3) {
        const fs::path folder = argv[2];
        fs::create_directories(folder);
        // The grain eight times over, so its asymmetry can be seen.
        const auto grain = tortureGrain();
        Image big(grain->width() * 8, grain->height() * 8);
        for (int y = 0; y < big.height(); y++)
            for (int x = 0; x < big.width(); x++) {
                uint8_t* p = big.pixel(x, y);
                p[0] = p[1] = p[2] = grain->at(x / 8, y / 8);
                p[3] = 255;
            }
        writePngImage((folder / "grain.png").string(), big);
        int written = 0;
        for (const Preset& preset : grainTorturePresets())
            for (const GrainStroke& g : grainTortureStrokes()) {
                std::vector<TipDab> dabs;
                const Render r = render(g.stroke, preset, &dabs);
                if (!r.image) continue;
                const std::string name = g.stroke.name + "__" + preset.name;
                writePngImage((folder / (name + ".png")).string(), *r.image);
                std::ofstream trace(folder / (name + ".txt"));
                for (const TipDab& d : dabs)
                    trace << d.distance << ' ' << d.center.x << ' ' << d.center.y << ' ' << d.size << ' ' << d.grainDirection << ' ' << d.grainTangent << ' '
                          << d.grainOffset.x << ' ' << d.grainOffset.y << '\n';
                written++;
            }
        std::printf("%d renders in %s (each with its dabs: distance, x, y, size, direction, tangent, grain offset x, y)\n", written, folder.string().c_str());
        return 0;
    }
    if (command == "render" && argc >= 5) {
        std::string error;
        auto fixture = loadFixture(argv[2], &error);
        if (!fixture) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        for (const Preset& p : presets) {
            if (p.name != argv[3]) continue;
            const Render r = render(*fixture, p);
            if (!r.image) { std::fprintf(stderr, "nothing rendered\n"); return 1; }
            writePngImage(argv[4], *r.image);
            std::printf("%s %s\n", hex(hashRender(r)).c_str(), formatMetrics(measure(*fixture, r)).c_str());
            return 0;
        }
        std::fprintf(stderr, "no preset %s; list shows them\n", argv[3]);
        return 1;
    }
    return usage();
}
