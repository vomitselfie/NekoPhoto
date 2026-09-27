// The brush parity harness (docs/brush-engine.md): recorded strokes, the standard motions made from them, the presets
// they are painted with, and the measurements taken of the result. brush_parity (a test) holds every render to
// tests/brush_parity_baseline.txt; brush_parity_tool writes the images and measurements out for a look.
#pragma once
#include "compositor/brush.h"
#include "compositor/brushsample.h"
#include "compositor/tipbrush.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace brushharness {

using namespace compositor;

/// A recorded stroke (brushsample.h): raw samples, seconds since the stroke began, document positions and the pen.
/// `stylus` false plays it as a mouse (neutral pressure, no tilt or twist).
using StrokeFixture = RecordedStroke;

/// Every fixture is drawn for a canvas this size.
constexpr int canvasWidth = 320, canvasHeight = 200;

/// The standard motions: pressure ramp and sine, speed, tilt and twist sweeps, a straight line, a circle, an S curve,
/// corners, a fast flick and a long slow stroke.
std::vector<StrokeFixture> standardFixtures();
/// Reads a fixture: {"samples": [{t, x, y, pressure, tiltX, tiltY, twist, tangentialPressure}...]} or a bare array
/// of samples; missing fields take their defaults. Nullopt with `error` when the text is not one.
std::optional<StrokeFixture> fixtureFromJson(const std::string& text, const std::string& name, std::string* error = nullptr);
std::optional<StrokeFixture> loadFixture(const std::string& path, std::string* error = nullptr);
std::string fixtureToJson(const StrokeFixture& fixture);
/// The fixtures kept as files under tests/brush_fixtures/.
std::vector<StrokeFixture> fileFixtures(const std::string& folder);

/// A brush to paint fixtures with.
struct Preset {
    enum class Engine { Round, Tip, MyPaint };
    std::string name;
    Engine engine = Engine::Round;
    BrushSettings settings;          // diameter, hardness, opacity, colour, erasing
    std::optional<TipPreset> tip;    // Engine::Tip
    std::string myPaintJson;         // Engine::MyPaint
    uint32_t seed = 1234;            // the tip brushes' jitter
};

/// The round tip hard and soft, the eraser, tip brushes (made here and from the importer tests' files) and MyPaint
/// presets from `myPaintFolder` (skipped when the build has no MyPaint or the folder lacks them).
std::vector<Preset> standardPresets(const std::string& myPaintFolder);

/// A painted fixture: the layer's pixels as the stroke left them (a transparent layer, or an opaque grey one for the
/// eraser) and the alpha the stroke laid down (or took away), per pixel.
struct Render {
    std::shared_ptr<Image> image;
    std::vector<uint8_t> paint;   // canvasWidth x canvasHeight
};
Render render(const StrokeFixture& fixture, const Preset& preset);

/// The same fixture painted on a 16-bit layer (the same paper, widened to 0..32768) by the 16-bit stroke. `image` is the
/// layer at 16 bits; `eight` is it reduced to 8 bits, with its paint, for the measurements and the comparison with the
/// 8-bit render.
struct Render16 {
    std::shared_ptr<Image16> image;
    Render eight;
};
Render16 render16(const StrokeFixture& fixture, const Preset& preset);

/// How far a 16-bit render reduced to 8 bits is from the 8-bit render: the largest difference in any sample, and the
/// share of samples more than a level apart.
struct Calibration { int worst = 0; double beyondOne = 0; };
Calibration compare(const Render& eight, const Render16& deep);

/// Measurements of a render along the fixture's path.
struct Metrics {
    int boxX = 0, boxY = 0, boxWidth = 0, boxHeight = 0;   // where any paint landed
    double total = 0;          // the sum of the paint alpha, 0..1 per pixel
    double mean = 0;           // over the painted pixels
    std::vector<double> width; // across the stroke at 10 stations along it (pixels at 10% alpha or more)
    std::vector<double> peak;  // the strongest alpha at each station, 0..1
    double edge = 0;           // the mean distance from 90% to 10% of a station's peak, outwards
    double startTaper = 0, endTaper = 0;   // the width at 5% and 95% of the way over the median width
};
Metrics measure(const StrokeFixture& fixture, const Render& render);
std::string formatMetrics(const Metrics& metrics);

/// FNV-1a 64 over the layer's pixels.
uint64_t hashRender(const Render& render);
/// FNV-1a 64 over the 16-bit layer's samples (little-endian).
uint64_t hashRender(const Render16& render);
std::string hex(uint64_t value);

/// A scene: one fixture painted with one preset, named "<fixture>/<preset>".
struct Scene {
    std::string name;
    const StrokeFixture* fixture;
    const Preset* preset;
};
std::vector<Scene> scenes(const std::vector<StrokeFixture>& fixtures, const std::vector<Preset>& presets);

// ---- Brushes checked against expectations ---------------------------------------------------------------------------

/// What a brush is expected to do on one stroke against another brush (docs/brush-engine.md, "Fixture brushes"):
/// `measure` is width or peak at the stations where the stroke's input is high (4 and 5 of 10) over those where it is
/// low (0 and 9), or the render; `expect` is up, down, turns (the width swings more), continuous (no jump between
/// neighbouring stations) or differs (another render).
struct Expectation {
    std::string brush;        // a preset name
    std::string stroke;       // a fixture name
    std::string measure;
    std::string expect;
    std::string against;      // the preset it is compared with
    std::string weakerThan;   // optional: a preset whose change against `against` this one's stays inside
    std::string setting;      // what the brush sets, for messages
};

/// Brushes read from files, with their expectations.
struct FixtureBrushes {
    std::vector<Preset> presets;
    std::vector<Expectation> expectations;
    std::vector<std::string> notes;   // what the importer said about each file
};

/// The synthetic Procreate brushes in `folder` (tests/fixtures/brushes/procreate): every .brush its manifest.json
/// lists, compared with the manifest's baseline brush. Empty when the folder or manifest is missing.
FixtureBrushes syntheticProcreate(const std::string& folder);

/// Third-party brush sets kept locally under `folder` (tests/local-fixtures, git-ignored), when its manifest.json
/// exists: each listed brush is compared with itself without its mappings from the listed input. Nothing from these
/// goes into the baseline. Nullopt when there is no manifest.
std::optional<FixtureBrushes> localFixtures(const std::string& folder);

/// The scenes the expectations paint (each brush and what it is compared with, on its stroke), without repeats.
std::vector<Scene> expectationScenes(const std::vector<StrokeFixture>& fixtures, const FixtureBrushes& brushes);

/// Paints and checks one expectation; `why` says what was measured.
bool checkExpectation(const Expectation& expectation, const std::vector<StrokeFixture>& fixtures, const FixtureBrushes& brushes,
                      std::string* why);

} // namespace brushharness
