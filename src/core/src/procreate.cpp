// Procreate brushes: a .brushset is a ZIP of brush folders (named by UUID, ordered and titled by an XML
// brushset.plist); a .brush is one such folder zipped. Each folder holds Brush.archive, an NSKeyedArchiver
// binary plist of the settings, and Shape.png and Grain.png (white paints). The setting names and their
// ranges follow the procreate-brush-decoder schema (aumlette-lab, MIT licence), checked against a real
// set (Catherine's Basic Procreate Brushes, CC0). Procreate's sliders show these raw values through curves;
// the raw values are the physical ones and are used as such.
#include "brushformats.h"
#include "compositor/png.h"
#include "plist.h"
#include "zip.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace compositor {

namespace {

constexpr double pi = 3.14159265358979323846;

/// A Procreate shape or grain image as a tip: its luminance (white paints) times its alpha, inverted on
/// request. A dark border is Procreate's black background, often a little above black: its level is taken
/// off so the square around each dab does not show.
std::shared_ptr<GrayImage> procreateTip(const Image& image, bool invert, bool clearBackground) {
    const int w = image.width(), h = image.height();
    auto out = std::make_shared<GrayImage>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t* p = image.pixel(x, y);
            // Premultiplied: the channels already carry the alpha, so luminance of them is luminance times alpha.
            unsigned v = (p[0] * 54u + p[1] * 183u + p[2] * 19u + 128) >> 8;
            if (invert) v = p[3] - std::min<unsigned>(v, p[3]);
            out->at(x, y) = uint8_t(std::min(255u, v));
        }
    if (clearBackground && w > 4 && h > 4) {
        std::vector<uint8_t> border;
        for (int x = 0; x < w; x++) { border.push_back(out->at(x, 0)); border.push_back(out->at(x, h - 1)); }
        for (int y = 0; y < h; y++) { border.push_back(out->at(0, y)); border.push_back(out->at(w - 1, y)); }
        std::nth_element(border.begin(), border.begin() + long(border.size() / 2), border.end());
        const int level = border[border.size() / 2];
        if (level > 0 && level <= 48)
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    const int v = out->at(x, y);
                    out->at(x, y) = uint8_t(v <= level ? 0 : std::lround((v - level) * 255.0 / (255 - level)));
                }
    }
    return out;
}

double number(const std::map<std::string, plist::Value>& s, const char* key, double fallback) {
    auto it = s.find(key);
    if (it == s.end()) return fallback;
    const auto k = it->second.kind;
    return (k == plist::Value::Kind::Real || k == plist::Value::Kind::Integer || k == plist::Value::Kind::Bool) ? it->second.number : fallback;
}

std::string text(const std::map<std::string, plist::Value>& s, const char* key) {
    auto it = s.find(key);
    return it != s.end() && it->second.kind == plist::Value::Kind::String && it->second.text != "$null" ? it->second.text : std::string();
}

struct Notes {
    int bundledShapes = 0, bundledGrains = 0;
    std::map<std::string, int> notCarried;   // setting -> brushes using it
};

// ---- Procreate's dynamics, scaled in one place ---------------------------------------------------------------------
//
// Every Procreate setting this reader turns into a dynamics mapping (brushdynamics.h) is scaled here and nowhere
// else, so the one-setting reference brushes made in Procreate can tune each in one spot. The mapping table in
// docs/brush-engine.md lists the same; the assumptions it marks as pending are the ones those files will settle.
namespace scaling {

/// The speed, in document pixels per second, at which a speed setting has its full effect (assumed).
constexpr double fullSpeed = 1500;

/// dynamicsSpeedSize, -1..1: positive grows the size with speed, to 1 + amount at full speed; negative shrinks it.
DynamicsMapping speedSize(double amount) {
    return dynamicsMapping(DynamicsInput::Speed, DynamicsTarget::Size, 1, std::clamp(amount, -0.95, 1.0), fullSpeed);
}

/// dynamicsSpeedOpacity, -1..1: positive makes slow strokes lighter (from 1 - amount at rest to full at full speed);
/// negative makes fast strokes lighter (to 1 + amount at full speed). Opacity is the most a stroke builds up to.
DynamicsMapping speedOpacity(double amount) {
    const double a = std::clamp(amount, -1.0, 1.0);
    return a > 0 ? dynamicsMapping(DynamicsInput::Speed, DynamicsTarget::Opacity, 1 - a, a, fullSpeed)
                 : dynamicsMapping(DynamicsInput::Speed, DynamicsTarget::Opacity, 1, a, fullSpeed);
}

/// plotSpacingSpeed, 0 and up: the spacing widens with speed, to 1 + amount times at full speed.
DynamicsMapping speedSpacing(double amount) {
    return dynamicsMapping(DynamicsInput::Speed, DynamicsTarget::Spacing, 1, std::clamp(amount, 0.0, 10.0), fullSpeed);
}

// Tilt, as this reader takes Procreate's (assumed): the pen's angle from upright, which is what the Tilt input reads
// (BrushSample::tiltMagnitude, 0 upright to 1 at `tiltFullAt` degrees). A setting's tilt angle (sizeTiltAngle and the
// like, else dynamicsTiltAngle) is stored as a fraction of Procreate's 0..90 degree tilt graph and read as the lean
// from upright at which the tilt starts to count: below it the setting does nothing, beyond it the effect ramps up to
// full at `tiltFullAt`. If Procreate measures the angle from the screen instead, only tiltCurve changes.

/// Degrees a stored tilt angle of 1 stands for.
constexpr double tiltAngleRange = 90;
/// Degrees from upright at which the Tilt input is full (brushsample.cpp).
constexpr double tiltFullAt = 60;
/// The share of the flow that a full dynamicsTiltBleed takes away at full tilt (assumed).
constexpr double tiltBleedFlow = 0.5;

/// The Tilt input's curve for a stored tilt angle: zero up to the angle, then straight up to full.
DynamicsCurve tiltCurve(double storedAngle) {
    DynamicsCurve curve;
    const double start = std::clamp(storedAngle * tiltAngleRange / tiltFullAt, 0.0, 0.95);
    if (start > 0) curve.points = {{start, 0}, {1, 1}};
    return curve;
}

DynamicsMapping withCurve(DynamicsMapping m, const DynamicsCurve& curve) { m.curve = curve; return m; }

/// dynamicsTiltSize, -1..1: the size grows as the pen leans, to 1 + amount at full tilt (negative: shrinks).
DynamicsMapping tiltSize(double amount, double storedAngle) {
    return withCurve(dynamicsMapping(DynamicsInput::Tilt, DynamicsTarget::Size, 1, std::clamp(amount, -0.95, 1.0)), tiltCurve(storedAngle));
}

/// dynamicsTiltOpacity, 0..1: the stroke gets lighter as the pen leans, to 1 - amount at full tilt.
DynamicsMapping tiltOpacity(double amount, double storedAngle) {
    return withCurve(dynamicsMapping(DynamicsInput::Tilt, DynamicsTarget::Opacity, 1, -std::clamp(amount, 0.0, 1.0)), tiltCurve(storedAngle));
}

/// dynamicsTiltBleed, 0..1: each dab thins as the pen leans, its flow down by `tiltBleedFlow` x amount at full tilt.
DynamicsMapping tiltBleed(double amount, double storedAngle) {
    return withCurve(dynamicsMapping(DynamicsInput::Tilt, DynamicsTarget::Flow, 1, -tiltBleedFlow * std::clamp(amount, 0.0, 1.0)), tiltCurve(storedAngle));
}

/// dynamicsTiltShapeRoundness and its Minimum: the tip flattens as the pen leans, to the minimum at full tilt (with a
/// full amount); nothing while the minimum is 1, as in most brushes.
DynamicsMapping tiltRoundness(double amount, double minimum, double storedAngle) {
    const double depth = std::clamp(amount, 0.0, 1.0) * (1 - std::clamp(minimum, 0.0, 1.0));
    return withCurve(dynamicsMapping(DynamicsInput::Tilt, DynamicsTarget::Roundness, 1, -depth), tiltCurve(storedAngle));
}

// Orientation. The tip's angle is counterclockwise on screen while the pen's azimuth, twist and the stroke's direction
// turn clockwise in the document's y-down frame, so a tip that follows one of them takes a depth of -360: its x axis
// then points the way the pen leans, or turns with the barrel. The sum of angles stays continuous across a turn.

/// shapeAzimuth: the tip turns to the way the pen leans.
DynamicsMapping azimuthAngle() { return dynamicsMapping(DynamicsInput::TiltDirection, DynamicsTarget::Angle, 0, -360); }

/// shapeRoll: the tip turns with the barrel on a pen that reports its twist, and with the stroke on one that does not.
DynamicsMapping rollAngle() { return dynamicsMapping(DynamicsInput::Roll, DynamicsTarget::Angle, 0, -360); }

} // namespace scaling

/// Settings that change how a brush paints and have no mapping here yet. A setting counts when it is off its
/// neutral value; the import's notes list how many brushes use each. As a setting gets a mapping it leaves this list.
/// dynamicsPressure*Speed are not speed dynamics: they follow dynamicsPressureResponse (how quickly the size,
/// opacity and bleed catch up with the pressure), which has no counterpart in the engine.
const char* const notCarriedSettings[] = {
    "dynamicsPressureSizeSpeed", "dynamicsPressureOpacitySpeed", "dynamicsPressureBleedSpeed",
    "dynamicsTiltCompression", "dynamicsTiltGradation", "shapeRollMode",
    "dynamicsTiltHue", "dynamicsTiltSaturation", "dynamicsTiltBrightness", "dynamicsTiltSecondaryColor",
    "dynamicsPressureHue", "dynamicsPressureSaturation", "dynamicsPressureBrightness", "dynamicsPressureSecondaryColor",
    "dynamicsPressureBleed", "dynamicsPressureShapeRoundness"};

bool inUse(const std::map<std::string, plist::Value>& s, const std::string& key) {
    if (std::fabs(number(s, key.c_str(), 0)) < 1e-6) return false;
    // Roundness by pressure or tilt does nothing while its minimum is the full roundness.
    if (key.size() > 14 && key.compare(key.size() - 14, 14, "ShapeRoundness") == 0) return number(s, (key + "Minimum").c_str(), 1) < 1;
    // Compression only qualifies a tilt on the size.
    if (key == "dynamicsTiltCompression") return std::fabs(number(s, "dynamicsTiltSize", 0)) >= 1e-6;
    return true;
}

std::optional<TipPreset> readBrush(const ZipArchive& zip, const std::string& folder, Notes& notes) {
    auto archive = zip.read(folder + "Brush.archive", 16u << 20);
    if (!archive) return std::nullopt;
    auto binary = plist::Binary::parse(archive->data(), archive->size());
    if (!binary) return std::nullopt;
    auto settings = plist::keyedRoot(*binary);
    if (!settings) return std::nullopt;
    const auto& s = *settings;
    TipPreset preset;
    preset.name = text(s, "name");
    BrushTip& tip = preset.tip;

    if (auto png = zip.read(folder + "Shape.png")) {
        if (auto image = decodePngImage(png->data(), png->size())) tip.shape = procreateTip(*image, number(s, "shapeInverted", 0) != 0, true);
    }
    if (!tip.shape) {
        // A shape from Procreate's own library is not in the file: a soft round tip stands in.
        if (!text(s, "bundledShapePath").empty()) notes.bundledShapes++;
        auto round = std::make_shared<GrayImage>(128, 128);
        for (int y = 0; y < 128; y++)
            for (int x = 0; x < 128; x++) {
                const double d = std::hypot(x + 0.5 - 64, y + 0.5 - 64) / 64;
                round->at(x, y) = uint8_t(d >= 1 ? 0 : std::lround(255 * (1 - d * d)));
            }
        tip.shape = round;
    }
    if (auto png = zip.read(folder + "Grain.png")) {
        if (auto image = decodePngImage(png->data(), png->size())) {
            tip.grain = procreateTip(*image, number(s, "textureInverted", 0) != 0, false);
            tip.grainDepth = std::clamp(number(s, "grainDepth", 1), 0.0, 1.0);
            tip.grainScale = 1 / std::clamp(number(s, "textureScale", 1), 0.05, 16.0);
            // Moving grain (textureApplication 0) rolls with the stroke, as far as its Movement says; texturized grain
            // stays on the canvas.
            if (number(s, "textureApplication", 1) == 0) {
                tip.grainMode = BrushTip::GrainMode::Stroke;
                tip.grainMovement = std::clamp(number(s, "textureMovement", 1), 0.0, 1.0);
            }
        }
    } else if (!text(s, "bundledGrainPath").empty()) notes.bundledGrains++;

    // Stroke path: spacing and jitter, as fractions of the dab's size.
    tip.spacing = number(s, "plotSpacing", 0.1);
    const double lateral = number(s, "plotJitter", 0), longitudinal = number(s, "plotJitterLongitudinal", 0);
    tip.scatter = std::max(lateral, longitudinal);
    tip.scatterBothAxes = longitudinal > 0;
    // Shape: rotation follows the stroke at 100% (and against it at -100%); scatter turns each dab at random.
    const double rotation = number(s, "shapeRotation", 0);
    tip.followStroke = std::fabs(rotation) >= 0.5;
    tip.angle = number(s, "shapeAngle", 0) * 180 / pi + (rotation <= -0.5 ? 180 : 0);
    const double angleJitter = std::clamp(number(s, "shapeScatter", 0) * 180, 0.0, 180.0);
    tip.count = int(std::clamp(std::lround(number(s, "shapeCount", 0) * 16), 1L, 16L));
    tip.roundness = number(s, "shapeRoundness", 1);
    tip.randomFlipX = number(s, "shapeFlipXJitter", 0) != 0;
    tip.randomFlipY = number(s, "shapeFlipYJitter", 0) != 0;
    // Dynamics and the pencil, as mappings (brushdynamics.h). Procreate's opacity is per dab, so it maps to flow;
    // its size pressure is how much of the size pressure takes away at its lightest.
    auto add = [&](DynamicsInput input, DynamicsTarget target, double offset, double depth) { tip.dynamics.push_back(dynamicsMapping(input, target, offset, depth)); };
    const double pressureSize = number(s, "dynamicsPressureSize", 0);
    if (pressureSize > 0) { const double minimum = 1 - std::min(1.0, pressureSize); add(DynamicsInput::Pressure, DynamicsTarget::Size, minimum, 1 - minimum); }
    const double sizeJitter = std::clamp(number(s, "dynamicsJitterSize", 0), 0.0, 1.0);
    if (sizeJitter > 0) add(DynamicsInput::Random, DynamicsTarget::Size, 1, -sizeJitter);
    const double pressureOpacity = std::clamp(number(s, "dynamicsPressureOpacity", 0), 0.0, 1.0);
    if (pressureOpacity > 0) add(DynamicsInput::Pressure, DynamicsTarget::Flow, 1 - pressureOpacity, pressureOpacity);
    const double opacityJitter = std::clamp(number(s, "dynamicsJitterOpacity", 0), 0.0, 1.0);
    if (opacityJitter > 0) add(DynamicsInput::Random, DynamicsTarget::Flow, 1, -opacityJitter);
    if (angleJitter > 0) add(DynamicsInput::Random, DynamicsTarget::Angle, 0, angleJitter);
    // Speed (scaling above).
    if (const double v = number(s, "dynamicsSpeedSize", 0); std::fabs(v) >= 1e-6) tip.dynamics.push_back(scaling::speedSize(v));
    if (const double v = number(s, "dynamicsSpeedOpacity", 0); std::fabs(v) >= 1e-6) tip.dynamics.push_back(scaling::speedOpacity(v));
    if (const double v = number(s, "plotSpacingSpeed", 0); v >= 1e-6) tip.dynamics.push_back(scaling::speedSpacing(v));
    // Tilt (scaling above), each from its own tilt angle when the brush has one.
    const double tiltAngle = number(s, "dynamicsTiltAngle", 0);
    auto angleFor = [&](const char* key) { return number(s, key, tiltAngle); };
    if (const double v = number(s, "dynamicsTiltSize", 0); std::fabs(v) >= 1e-6) tip.dynamics.push_back(scaling::tiltSize(v, angleFor("sizeTiltAngle")));
    if (const double v = number(s, "dynamicsTiltOpacity", 0); v >= 1e-6) tip.dynamics.push_back(scaling::tiltOpacity(v, angleFor("opacityTiltAngle")));
    if (const double v = number(s, "dynamicsTiltBleed", 0); v >= 1e-6) tip.dynamics.push_back(scaling::tiltBleed(v, angleFor("bleedTiltAngle")));
    if (inUse(s, "dynamicsTiltShapeRoundness"))
        tip.dynamics.push_back(scaling::tiltRoundness(number(s, "dynamicsTiltShapeRoundness", 0), number(s, "dynamicsTiltShapeRoundnessMinimum", 1),
                                                      angleFor("shapeRoundnessTiltAngle")));
    // Orientation (scaling above). Roll already follows the stroke where the pen has no twist, so a brush with both
    // stops following the stroke on its own rather than turning twice.
    if (number(s, "shapeAzimuth", 0) != 0) tip.dynamics.push_back(scaling::azimuthAngle());
    if (number(s, "shapeRoll", 0) != 0) {
        tip.dynamics.push_back(scaling::rollAngle());
        tip.followStroke = false;
    }
    for (const char* key : notCarriedSettings)
        if (inUse(s, key)) notes.notCarried[key]++;
    tip.flow = number(s, "maxOpacity", 1);
    // Size: Procreate's are relative, with no pixel size in the file. 200 pixels for a maximum of 1 matches the
    // proportions of Procreate's own thumbnails (a 0.04 ink is a fine line, a 0.4 velvet a broad stroke).
    preset.diameter = std::clamp(number(s, "maxSize", 0.1) * 200, 2.0, 500.0);
    if (!tip.normalize()) return std::nullopt;
    return preset;
}

} // namespace

std::optional<BrushImport> readProcreate(const uint8_t* data, size_t size, const std::string& name, std::string* error) {
    auto zip = ZipArchive::open(data, size);
    if (!zip) { if (error) *error = "not a ZIP archive"; return std::nullopt; }
    BrushImport import;
    import.set = name;
    std::vector<std::string> folders;
    if (auto list = zip->read("brushset.plist", 1u << 20)) {
        if (auto dict = plist::parseXmlDict(std::string(list->begin(), list->end()))) {
            if (auto it = dict->strings.find("name"); it != dict->strings.end() && !it->second.empty()) import.set = it->second;
            if (auto it = dict->arrays.find("brushes"); it != dict->arrays.end())
                for (const std::string& uuid : it->second) folders.push_back(uuid + "/");
        }
    }
    if (folders.empty()) {
        // A single .brush, or a set without its list: every Brush.archive outside a Reset copy.
        for (const std::string& entry : zip->names()) {
            if (entry.size() < 13 || entry.compare(entry.size() - 13, 13, "Brush.archive") != 0 || entry.find("Reset/") != std::string::npos) continue;
            folders.push_back(entry.substr(0, entry.size() - 13));
        }
    }
    Notes notes;
    for (const std::string& folder : folders) {
        auto preset = readBrush(*zip, folder, notes);
        if (!preset) continue;
        if (preset->name.empty()) preset->name = name + " " + std::to_string(import.brushes.size() + 1);
        import.brushes.push_back(std::move(*preset));
    }
    auto note = [&](int count, const char* what) { if (count) import.notes.push_back(what + std::string(": ") + std::to_string(count)); };
    note(notes.bundledShapes, "brushes whose shape is from Procreate's own library, not in the file (a soft round tip stands in)");
    note(notes.bundledGrains, "brushes whose grain is from Procreate's own library, not in the file (they paint without grain)");
    if (!notes.notCarried.empty()) {
        std::string list;
        for (const auto& [key, count] : notes.notCarried) list += (list.empty() ? "" : ", ") + key + " " + std::to_string(count);
        import.notes.push_back("settings not carried over, with the brushes using each: " + list);
    }
    if (import.brushes.empty()) { if (error) *error = "the file holds no Procreate brush this reader can use"; return std::nullopt; }
    return import;
}

} // namespace compositor
