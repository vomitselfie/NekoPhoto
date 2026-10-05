// The brush parity harness by hand (not a test): paints the harness's fixtures with its presets and writes the images,
// the measurements and the fixtures themselves, for looking at a brush change rather than only hashing it.
//
//   brush_parity_tool list                            the fixtures and presets
//   brush_parity_tool dump <folder> [filter]          <fixture>__<preset>.png, metrics.txt and fixtures/*.json
//   brush_parity_tool render <stroke.json> <preset> <out.png>   one recorded stroke with one preset
//   brush_parity_tool sheet <brush file> <out.png> [filter]     every imported brush on the same synthetic strokes
//   brush_parity_tool grain <folder>                  the moving-grain torture scenes (brush_grain_tests), the grain
//                                                     itself, and each scene's dabs (centre, tangent, grain offset)
#include "brush_harness.h"
#include "compositor/brushimport.h"
#include "compositor/png.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace brushharness;
namespace fs = std::filesystem;

namespace {

int usage() {
    std::fprintf(stderr, "usage: brush_parity_tool list | dump <folder> [filter] | render <stroke.json> <preset> <out.png> | sheet <brush file> <out.png> [filter] | grain <folder>\n");
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
    if (command == "sheet" && argc >= 4) {
        // Every brush of an imported file on the same synthetic strokes: a row per brush, a column per stroke (pressure
        // ramp, tilt sweep, speed sweep and S curve with a pen, then a straight line with a mouse), on white.
        std::string error;
        auto import = importBrushFile(argv[2], &error);
        if (!import) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        const std::string filter = argc > 4 ? argv[4] : "";
        std::vector<StrokeFixture> strokes;
        for (const StrokeFixture& f : standardFixtures())
            if (f.name == "pressure_ramp" || f.name == "tilt_sweep" || f.name == "speed_sweep" || f.name == "s_curve") strokes.push_back(f);
        for (const StrokeFixture& f : standardFixtures())
            if (f.name == "straight_line") { strokes.push_back(f); strokes.back().name = "mouse_line"; strokes.back().stylus = false; }
        std::vector<const TipPreset*> brushes;
        for (const TipPreset& b : import->brushes)
            if (filter.empty() || b.name.find(filter) != std::string::npos) brushes.push_back(&b);
        if (brushes.empty()) { std::fprintf(stderr, "no brush matches\n"); return 1; }
        Image sheet(int(strokes.size()) * canvasWidth, int(brushes.size()) * canvasHeight);
        for (int y = 0; y < sheet.height(); y++)
            for (int x = 0; x < sheet.width(); x++) { uint8_t* p = sheet.pixel(x, y); p[0] = p[1] = p[2] = p[3] = 255; }
        for (size_t row = 0; row < brushes.size(); row++) {
            Preset preset;
            preset.name = brushes[row]->name;
            preset.engine = Preset::Engine::Tip;
            preset.tip = *brushes[row];
            preset.settings.diameter = std::clamp(brushes[row]->diameter, 4.0, 40.0);
            preset.settings.red = preset.settings.green = preset.settings.blue = 0;
            std::printf("row %zu: %s (diameter %.1f, %zu mappings)\n", row, preset.name.c_str(), brushes[row]->diameter, brushes[row]->tip.dynamics.size());
            for (size_t column = 0; column < strokes.size(); column++) {
                const Render r = render(strokes[column], preset);
                if (!r.image) continue;
                const Metrics m = measure(strokes[column], r);
                std::printf("  %-14s %s\n", strokes[column].name.c_str(), formatMetrics(m).c_str());
                for (int y = 0; y < canvasHeight; y++)
                    for (int x = 0; x < canvasWidth; x++) {
                        const unsigned a = r.paint[size_t(y) * canvasWidth + size_t(x)];
                        uint8_t* p = sheet.pixel(int(column) * canvasWidth + x, int(row) * canvasHeight + y);
                        // A hairline between cells.
                        const uint8_t v = uint8_t(255 - a);
                        p[0] = p[1] = p[2] = (x == 0 || y == 0) ? uint8_t(200) : v;
                    }
            }
        }
        writePngImage(argv[3], sheet);
        for (const std::string& note : import->notes) std::printf("note: %s\n", note.c_str());
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
