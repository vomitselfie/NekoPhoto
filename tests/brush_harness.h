// The brush parity harness (docs/brush-engine.md): recorded strokes, the standard motions made from them, the presets
// they are painted with, and the measurements taken of the result. brush_parity (a test) holds every render to
// tests/brush_parity_baseline.txt; brush_parity_tool writes the images and measurements out for a look.
#pragma once
#include "compositor/brush.h"
#include "compositor/tipbrush.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace brushharness {

using namespace compositor;

/// One recorded pointer event, as a fixture stores it: seconds since the stroke began, the document position,
/// and the pen: pressure 0..1, tilt in degrees (-90..90 each way, as Qt reports it), barrel rotation (twist) in
/// degrees, and tangential (barrel wheel) pressure -1..1.
struct StrokeSample {
    double t = 0, x = 0, y = 0;
    double pressure = 1;
    double tiltX = 0, tiltY = 0, twist = 0, tangentialPressure = 0;
};

/// A recorded stroke. `stylus` false plays it as a mouse (no pressure, tilt or twist).
struct StrokeFixture {
    std::string name;
    std::vector<StrokeSample> samples;
    bool stylus = true;
};

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
std::string hex(uint64_t value);

/// A scene: one fixture painted with one preset, named "<fixture>/<preset>".
struct Scene {
    std::string name;
    const StrokeFixture* fixture;
    const Preset* preset;
};
std::vector<Scene> scenes(const std::vector<StrokeFixture>& fixtures, const std::vector<Preset>& presets);

} // namespace brushharness
