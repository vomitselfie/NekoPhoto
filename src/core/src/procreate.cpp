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

struct Notes { int bundledShapes = 0, bundledGrains = 0, movingGrain = 0; };

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
            if (number(s, "textureApplication", 1) == 0) notes.movingGrain++;
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
    tip.angleJitter = std::min(180.0, number(s, "shapeScatter", 0) * 180);
    tip.count = int(std::clamp(std::lround(number(s, "shapeCount", 0) * 16), 1L, 16L));
    tip.roundness = number(s, "shapeRoundness", 1);
    tip.randomFlipX = number(s, "shapeFlipXJitter", 0) != 0;
    tip.randomFlipY = number(s, "shapeFlipYJitter", 0) != 0;
    // Dynamics and the pencil.
    tip.sizeJitter = number(s, "dynamicsJitterSize", 0);
    tip.flowJitter = number(s, "dynamicsJitterOpacity", 0);
    const double pressureSize = number(s, "dynamicsPressureSize", 0);
    if (pressureSize > 0) { tip.pressureSize = 1; tip.minimumSize = 1 - std::min(1.0, pressureSize); }
    tip.pressureFlow = std::max(0.0, number(s, "dynamicsPressureOpacity", 0));
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
    note(notes.movingGrain, "brushes whose grain moves with the stroke (here it stays put on the canvas)");
    if (import.brushes.empty()) { if (error) *error = "the file holds no Procreate brush this reader can use"; return std::nullopt; }
    return import;
}

} // namespace compositor
