#include "compositor/project.h"
#include "compositor/parallel.h"
#include "compositor/png.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace compositor {

namespace {

const char* formatIdentifier = "com.compositor.project";
constexpr size_t manifestLimit = 4 * 1024 * 1024;
constexpr uintmax_t assetLimit = uintmax_t(512) * 1024 * 1024;

ProjectError invalid() { return {ProjectError::Invalid, "This is not a valid Compositor project, or its metadata is damaged."}; }

/// Whether a parsed manifest nests deeper than any real one does. Parsing and destroying a json are
/// iterative, but dump() (which keeps unknown fields for saving) recurses, so a crafted manifest of deeply
/// nested arrays would overflow the stack; it is refused as damaged instead. Checked with an explicit stack.
bool nestsTooDeep(const json& root, size_t limit = 64) {
    std::vector<std::pair<const json*, size_t>> pending{{&root, 1}};
    while (!pending.empty()) {
        auto [value, depth] = pending.back();
        pending.pop_back();
        if (depth > limit) return true;
        if (value->is_structured())
            for (const json& child : *value) if (child.is_structured()) pending.emplace_back(&child, depth + 1);
    }
    return false;
}
ProjectError version(int v) { return {ProjectError::Version, "This project uses format version " + std::to_string(v) + ". This app supports versions 1-7.", v}; }
ProjectError missingImage() { return {ProjectError::MissingImage, "An image inside the project is missing or damaged. The current document has not been replaced."}; }
ProjectError tooLarge() { return {ProjectError::TooLarge, "This project exceeds the supported canvas, layer or file size, the 100-megapixel limit for one image, or the 1-gigapixel limit for all layers together."}; }
ProjectError encodeError() { return {ProjectError::Encode, "An image could not be saved. The previous project has not been replaced."}; }
ProjectError ioError(const std::string& what) { return {ProjectError::Io, what}; }

// Numbers: integral doubles print as integers, as Swift's encoder does.
json number(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) return json(static_cast<long long>(v));
    return json(v);
}

bool getDouble(const json& j, const char* key, double& out, bool required) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return !required;
    if (!it->is_number()) return false;
    out = it->get<double>();
    return std::isfinite(out);
}

bool getBool(const json& j, const char* key, bool& out, bool required) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return !required;
    if (!it->is_boolean()) return false;
    out = it->get<bool>();
    return true;
}

bool getString(const json& j, const char* key, std::string& out, bool required) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return !required;
    if (!it->is_string()) return false;
    out = it->get<std::string>();
    return true;
}

bool getUuid(const json& j, const char* key, std::optional<Uuid>& out, bool required) {
    std::string text;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return !required;
    if (!it->is_string()) return false;
    Uuid id;
    if (!parseUuid(it->get<std::string>(), id)) return false;
    out = id;
    return true;
}

bool parsePair(const json& j, double& a, double& b) {
    if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number()) return false;
    a = j[0].get<double>();
    b = j[1].get<double>();
    return std::isfinite(a) && std::isfinite(b);
}

bool parseTransform(const json& j, LayerTransform& t) {
    if (!j.is_object()) return false;
    auto origin = j.find("origin"), size = j.find("size");
    if (origin == j.end() || size == j.end()) return false;
    if (!parsePair(*origin, t.origin.x, t.origin.y) || !parsePair(*size, t.size.width, t.size.height)) return false;
    if (!getDouble(j, "rotation", t.rotation, false)) return false;
    if (!getBool(j, "flipX", t.flipX, false) || !getBool(j, "flipY", t.flipY, false)) return false;
    std::string sampling;
    if (!getString(j, "sampling", sampling, false)) return false;
    if (!sampling.empty() && !parseSampling(sampling, t.sampling)) return false;
    return true;
}

json transformJson(const LayerTransform& t) {
    json j;
    j["origin"] = {number(t.origin.x), number(t.origin.y)};
    j["size"] = {number(t.size.width), number(t.size.height)};
    j["rotation"] = number(t.rotation);
    j["flipX"] = t.flipX;
    j["flipY"] = t.flipY;
    j["sampling"] = samplingName(t.sampling);
    return j;
}

const std::set<std::string> knownLayerKeys = {"id", "name", "isVisible", "transform", "imageFile", "parentID", "isGroup", "opacity", "blendMode",
    "maskFile", "maskEnabled", "maskSourceID", "adjustment", "maskPlacement", "maskLinked", "shape", "text", "passThrough"};
const std::set<std::string> knownManifestKeys = {"format", "version", "colorSpace", "resolution", "documentID", "width", "height", "activeLayerID", "layers"};

struct Record {
    Layer layer;
    std::optional<std::string> imageFile, maskFile;
    std::optional<bool> maskEnabled;
    std::optional<LayerTransform> maskPlacement;
    std::optional<bool> maskLinked;
};

bool parseRecord(const json& j, Record& r) {
    if (!j.is_object()) return false;
    Layer& l = r.layer;
    std::optional<Uuid> id;
    if (!getUuid(j, "id", id, true)) return false;
    l.id = *id;
    if (!getString(j, "name", l.name, true)) return false;
    if (!getBool(j, "isVisible", l.visible, true)) return false;
    auto t = j.find("transform");
    if (t == j.end() || !parseTransform(*t, l.transform)) return false;
    std::string file;
    if (!getString(j, "imageFile", file, false)) return false;
    if (j.contains("imageFile") && !j["imageFile"].is_null()) r.imageFile = file;
    if (!getUuid(j, "parentID", l.parentId, false)) return false;
    bool isGroup = false;
    if (!getBool(j, "isGroup", isGroup, false)) return false;
    l.isGroup = isGroup;
    bool passThrough = true;
    if (!getBool(j, "passThrough", passThrough, false)) return false;
    l.passThrough = passThrough;
    double opacity = 1;
    if (!getDouble(j, "opacity", opacity, false)) return false;
    l.opacity = opacity;
    std::string blend;
    if (!getString(j, "blendMode", blend, false)) return false;
    if (!blend.empty() && !parseBlendMode(blend, l.blendMode)) return false;
    std::string maskFile;
    if (!getString(j, "maskFile", maskFile, false)) return false;
    if (j.contains("maskFile") && !j["maskFile"].is_null()) r.maskFile = maskFile;
    bool maskEnabled = true;
    if (j.contains("maskEnabled") && !j["maskEnabled"].is_null()) { if (!getBool(j, "maskEnabled", maskEnabled, true)) return false; r.maskEnabled = maskEnabled; }
    if (!getUuid(j, "maskSourceID", l.maskSourceId, false)) return false;
    auto adj = j.find("adjustment");
    if (adj != j.end() && !adj->is_null()) {
        if (!adj->is_object()) return false;
        LayerAdjustment a;
        std::string kind;
        if (!getString(*adj, "kind", kind, true) || !parseAdjustmentKind(kind, a.kind)) return false;
        a.json = adj->dump();
        l.adjustment = a;
    }
    auto mp = j.find("maskPlacement");
    if (mp != j.end() && !mp->is_null()) { LayerTransform p; if (!parseTransform(*mp, p)) return false; r.maskPlacement = p; }
    bool linked = true;
    if (j.contains("maskLinked") && !j["maskLinked"].is_null()) { if (!getBool(j, "maskLinked", linked, true)) return false; r.maskLinked = linked; }
    auto sh = j.find("shape");
    if (sh != j.end() && !sh->is_null()) {
        if (!sh->is_object()) return false;
        LayerShapeStyle s;
        std::string kind;
        if (!getString(*sh, "kind", kind, true)) return false;
        if (kind == "Rectangle") s.kind = ShapeKind::Rectangle; else if (kind == "Ellipse") s.kind = ShapeKind::Ellipse; else return false;
        if (!getDouble(*sh, "red", s.red, true) || !getDouble(*sh, "green", s.green, true) || !getDouble(*sh, "blue", s.blue, true) || !getDouble(*sh, "cornerRadius", s.cornerRadius, true)) return false;
        l.shape = s;
    }
    auto tx = j.find("text");
    if (tx != j.end() && !tx->is_null()) {
        if (!tx->is_object()) return false;
        LayerText t;
        if (!getString(*tx, "text", t.text, true)) return false;
        if (tx->contains("fontFamily") && !getString(*tx, "fontFamily", t.fontFamily, true)) return false;
        if (!getDouble(*tx, "fontSize", t.fontSize, true) || !getDouble(*tx, "red", t.red, true) || !getDouble(*tx, "green", t.green, true) || !getDouble(*tx, "blue", t.blue, true)) return false;
        if (tx->contains("bold") && !getBool(*tx, "bold", t.bold, true)) return false;
        if (tx->contains("italic") && !getBool(*tx, "italic", t.italic, true)) return false;
        double alignment = 0, lineSpacing = 1, letterSpacing = 0;
        if (tx->contains("alignment") && !getDouble(*tx, "alignment", alignment, true)) return false;
        if (tx->contains("lineSpacing") && !getDouble(*tx, "lineSpacing", lineSpacing, true)) return false;
        if (tx->contains("letterSpacing") && !getDouble(*tx, "letterSpacing", letterSpacing, true)) return false;
        t.alignment = std::clamp(int(alignment), 0, 2); t.lineSpacing = lineSpacing; t.letterSpacing = letterSpacing;
        if (tx->contains("boxWidth") && !getDouble(*tx, "boxWidth", t.boxWidth, true)) return false;
        if (tx->contains("boxHeight") && !getDouble(*tx, "boxHeight", t.boxHeight, true)) return false;
        if (!std::isfinite(t.boxWidth) || !std::isfinite(t.boxHeight) || t.boxWidth < 0 || t.boxHeight < 0) return false;
        if (auto wj = tx->find("warp"); wj != tx->end()) {
            if (!wj->is_object() || !getString(*wj, "style", t.warp.style, true)) return false;
            for (auto [key, field] : {std::pair{"bend", &t.warp.bend}, {"horizontal", &t.warp.horizontal}, {"vertical", &t.warp.vertical}})
                if (wj->contains(key) && (!getDouble(*wj, key, *field, true) || !std::isfinite(*field))) return false;
            if (wj->contains("verticalOrientation") && !getBool(*wj, "verticalOrientation", t.warp.verticalOrientation, true)) return false;
        }
        if (!(t.fontSize > 0) || !std::isfinite(t.fontSize) || !std::isfinite(t.lineSpacing) || !std::isfinite(t.letterSpacing)) return false;
        if (auto rs = tx->find("runs"); rs != tx->end()) {
            if (!rs->is_array() || rs->size() > 100000) return false;
            for (const json& rj : *rs) {
                if (!rj.is_object()) return false;
                TextRun r;
                double length = 0, caps = 0;
                if (!getDouble(rj, "length", length, true) || !getDouble(rj, "fontSize", r.fontSize, true)) return false;
                if (rj.contains("fontFamily") && !getString(rj, "fontFamily", r.fontFamily, true)) return false;
                for (auto [key, field] : {std::pair{"bold", &r.bold}, {"italic", &r.italic}, {"underline", &r.underline}, {"strikethrough", &r.strikethrough}})
                    if (rj.contains(key) && !getBool(rj, key, *field, true)) return false;
                for (auto [key, field] : {std::pair{"red", &r.red}, {"green", &r.green}, {"blue", &r.blue}, {"letterSpacing", &r.letterSpacing}, {"baselineShift", &r.baselineShift}, {"leading", &r.leading}})
                    if (rj.contains(key) && !getDouble(rj, key, *field, true)) return false;
                if (rj.contains("caps") && !getDouble(rj, "caps", caps, true)) return false;
                double weight = 0;
                if (rj.contains("weight") && !getDouble(rj, "weight", weight, true)) return false;
                r.weight = std::clamp(int(weight), 0, 1000);
                if (!(length >= 0 && length < 1e9) || !(r.fontSize > 0) || !std::isfinite(r.fontSize) || !std::isfinite(r.letterSpacing) || !std::isfinite(r.baselineShift) || !std::isfinite(r.leading)) return false;
                r.length = int(length);
                r.caps = caps == 1 ? TextRun::Caps::Small : caps == 2 ? TextRun::Caps::All : TextRun::Caps::Normal;
                t.runs.push_back(r);
            }
        }
        l.text = t;
    }
    json extra = json::object();
    for (auto& [key, value] : j.items()) if (!knownLayerKeys.count(key)) extra[key] = value;
    if (!extra.empty()) l.extraJson = extra.dump();
    return true;
}

json recordJson(const Layer& l) {
    json j;
    if (!l.extraJson.empty()) { auto extra = json::parse(l.extraJson, nullptr, false); if (extra.is_object()) j = extra; }
    j["id"] = l.id;
    j["name"] = l.name;
    j["isVisible"] = l.visible;
    j["transform"] = transformJson(l.transform);
    if (l.asset && l.asset->image) j["imageFile"] = l.id + ".png";
    if (l.parentId) j["parentID"] = *l.parentId;
    if (l.isGroup) j["isGroup"] = true;
    if (l.isGroup && !l.passThrough) j["passThrough"] = false;
    if (l.opacity != 1) j["opacity"] = number(l.opacity);
    if (l.blendMode != BlendMode::Normal) j["blendMode"] = blendModeName(l.blendMode);
    if (l.mask && l.mask->asset.image) {
        j["maskFile"] = l.id + ".mask.png";
        j["maskEnabled"] = l.mask->enabled;
        if (l.mask->placement) j["maskPlacement"] = transformJson(*l.mask->placement);
        j["maskLinked"] = l.mask->linked;
    }
    if (l.maskSourceId) j["maskSourceID"] = *l.maskSourceId;
    if (l.adjustment) { auto a = json::parse(l.adjustment->json, nullptr, false); if (a.is_object()) { a["kind"] = adjustmentKindName(l.adjustment->kind); j["adjustment"] = a; } }
    if (l.shape && l.isLiveShape()) {
        j["shape"] = {{"kind", l.shape->kind == ShapeKind::Ellipse ? "Ellipse" : "Rectangle"}, {"red", number(l.shape->red)}, {"green", number(l.shape->green)},
                      {"blue", number(l.shape->blue)}, {"cornerRadius", number(l.shape->cornerRadius)}};
    }
    if (l.text && l.isLiveText()) {
        const LayerText& t = *l.text;
        j["text"] = {{"text", t.text}, {"fontFamily", t.fontFamily}, {"fontSize", number(t.fontSize)}, {"bold", t.bold}, {"italic", t.italic},
                     {"red", number(t.red)}, {"green", number(t.green)}, {"blue", number(t.blue)}, {"alignment", t.alignment},
                     {"lineSpacing", number(t.lineSpacing)}, {"letterSpacing", number(t.letterSpacing)}};
        if (t.boxWidth > 0 && t.boxHeight > 0) { j["text"]["boxWidth"] = number(t.boxWidth); j["text"]["boxHeight"] = number(t.boxHeight); }
        if (t.warp.active())
            j["text"]["warp"] = {{"style", t.warp.style}, {"bend", number(t.warp.bend)}, {"horizontal", number(t.warp.horizontal)},
                                 {"vertical", number(t.warp.vertical)}, {"verticalOrientation", t.warp.verticalOrientation}};
        if (!t.runs.empty()) {
            json runs = json::array();
            for (const TextRun& r : t.runs) {
                json rj = {{"length", r.length}, {"fontFamily", r.fontFamily}, {"fontSize", number(r.fontSize)}, {"bold", r.bold}, {"italic", r.italic},
                           {"red", number(r.red)}, {"green", number(r.green)}, {"blue", number(r.blue)}, {"letterSpacing", number(r.letterSpacing)}};
                if (r.baselineShift != 0) rj["baselineShift"] = number(r.baselineShift);
                if (r.leading != 0) rj["leading"] = number(r.leading);
                if (r.weight != 0) rj["weight"] = r.weight;
                if (r.caps != TextRun::Caps::Normal) rj["caps"] = r.caps == TextRun::Caps::Small ? 1 : 2;
                if (r.underline) rj["underline"] = true;
                if (r.strikethrough) rj["strikethrough"] = true;
                runs.push_back(rj);
            }
            j["text"]["runs"] = runs;
        }
    }
    return j;
}

struct Manifest {
    int version = projectFormatVersion;
    std::optional<double> resolution;
    Uuid documentId;
    int width = 0, height = 0;
    std::optional<Uuid> activeLayerId;
    std::vector<Record> records;
    std::string extraJson;
};

bool parseManifestJson(const json& j, Manifest& m, ProjectError& error) {
    if (!j.is_object()) { error = invalid(); return false; }
    std::string format;
    if (!getString(j, "format", format, true) || format != formatIdentifier) { error = invalid(); return false; }
    auto v = j.find("version");
    if (v == j.end() || !v->is_number_integer()) { error = invalid(); return false; }
    m.version = v->get<int>();
    if (m.version < 1 || m.version > projectFormatVersion) { error = version(m.version); return false; }
    std::string space;
    if (!getString(j, "colorSpace", space, true) || space != "sRGB") { error = invalid(); return false; }
    double resolution = 0;
    if (j.contains("resolution") && !j["resolution"].is_null()) {
        if (!getDouble(j, "resolution", resolution, true) || resolution < 1 || resolution > 9600) { error = invalid(); return false; }
        m.resolution = resolution;
    }
    std::optional<Uuid> docId;
    if (!getUuid(j, "documentID", docId, true)) { error = invalid(); return false; }
    m.documentId = *docId;
    auto w = j.find("width"), h = j.find("height");
    if (w == j.end() || h == j.end() || !w->is_number_integer() || !h->is_number_integer()) { error = invalid(); return false; }
    m.width = w->get<int>();
    m.height = h->get<int>();
    if (!getUuid(j, "activeLayerID", m.activeLayerId, false)) { error = invalid(); return false; }
    auto layers = j.find("layers");
    if (layers == j.end() || !layers->is_array()) { error = invalid(); return false; }
    for (auto& record : *layers) {
        Record r;
        if (!parseRecord(record, r)) { error = invalid(); return false; }
        m.records.push_back(std::move(r));
    }
    json extra = json::object();
    for (auto& [key, value] : j.items()) if (!knownManifestKeys.count(key)) extra[key] = value;
    if (!extra.empty()) m.extraJson = extra.dump();
    return true;
}

bool validateManifest(const Manifest& m, ProjectError& error) {
    if (!Document::validDimension(m.width) || !Document::validDimension(m.height) || m.records.size() > size_t(Document::maxLayers)) { error = tooLarge(); return false; }
    std::vector<Layer> layers;
    std::set<Uuid> ids;
    for (auto& r : m.records) {
        const Layer& l = r.layer;
        if (l.adjustment && (m.version < 7 || l.isGroup || r.imageFile)) { error = invalid(); return false; }
        if (r.maskFile) {
            if (m.version < (l.isGroup ? 6 : 4) || *r.maskFile != l.id + ".mask.png") { error = invalid(); return false; }
        }
        if (r.maskEnabled && !r.maskFile) { error = invalid(); return false; }
        if (r.maskPlacement && (!r.maskPlacement->isValid() || !r.maskFile)) { error = invalid(); return false; }
        if (!(l.opacity >= 0 && l.opacity <= 1)) { error = invalid(); return false; }
        if (m.version < 3 && (l.opacity != 1 || l.blendMode != BlendMode::Normal)) { error = invalid(); return false; }
        // Version 8: folders with their own opacity, blend mode and isolation (Photoshop's folders).
        if (l.isGroup && m.version < 8 && (l.opacity != 1 || l.blendMode != BlendMode::Normal || !l.passThrough)) { error = invalid(); return false; }
        if (!l.isGroup && !l.passThrough) { error = invalid(); return false; }
        if (m.version < 5 && l.maskSourceId) { error = invalid(); return false; }
        if (m.version == 1 && (l.parentId || l.isGroup)) { error = invalid(); return false; }
        if (!ids.insert(l.id).second || !l.transform.isValid()) { error = invalid(); return false; }
        std::string trimmed = l.name;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
        if (trimmed.empty() || l.name.size() > 16384) { error = invalid(); return false; }
        if (r.imageFile && *r.imageFile != l.id + ".png") { error = invalid(); return false; }
        Layer copy = l;
        if (r.imageFile) copy.asset = Asset{}; // stands in for "has an image" during hierarchy validation
        layers.push_back(copy);
    }
    // Hierarchy: groups carry no image, parents exist and are groups, no cycles.
    for (auto& l : layers) if (l.isGroup && l.asset) { error = invalid(); return false; }
    for (auto& l : layers) l.asset.reset();
    if (!validateHierarchy(layers) || !validateClipping(layers)) { error = invalid(); return false; }
    if (m.activeLayerId && !ids.count(*m.activeLayerId)) { error = invalid(); return false; }
    return true;
}

bool checkSize(int width, int height, long long& used) {
    if (!Document::validDimension(width) || !Document::validDimension(height)) return false;
    if ((long long)width * height > Document::pixelBudget || (long long)width * height > Document::projectPixelBudget - used) return false;
    used += (long long)width * height;
    return true;
}

bool writeBytes(const fs::path& file, const std::vector<uint8_t>& bytes) {
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(out);
}

bool checkFile(const fs::path& file, const fs::path& package, uintmax_t maximumBytes) {
    std::error_code ec;
    fs::path root = fs::weakly_canonical(package, ec);
    if (ec) return false;
    fs::path resolved = fs::weakly_canonical(file, ec);
    if (ec) return false;
    std::string rootText = root.string() + "/";
    if (resolved.string().rfind(rootText, 0) != 0) return false;
    if (fs::is_symlink(file, ec) || !fs::is_regular_file(file, ec)) return false;
    uintmax_t size = fs::file_size(file, ec);
    return !ec && size <= maximumBytes;
}

Document documentFrom(const Manifest& m) {
    Document d;
    d.id = m.documentId;
    d.width = m.width;
    d.height = m.height;
    d.resolution = m.resolution.value_or(72);
    d.extraJson = m.extraJson;
    for (auto& r : m.records) d.layers.push_back(r.layer);
    return d;
}

} // namespace

std::optional<Document> parseManifest(const std::string& text, ProjectError& error, std::optional<Uuid>* activeLayer) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || nestsTooDeep(j)) { error = invalid(); return std::nullopt; }
    Manifest m;
    if (!parseManifestJson(j, m, error) || !validateManifest(m, error)) return std::nullopt;
    if (activeLayer) *activeLayer = m.activeLayerId;
    Document d = documentFrom(m);
    for (size_t i = 0; i < m.records.size(); i++) {
        const Record& r = m.records[i];
        if (r.maskFile) {
            LayerMask mask;
            mask.enabled = r.maskEnabled.value_or(true);
            mask.placement = r.maskPlacement;
            mask.linked = r.maskLinked.value_or(true);
            d.layers[i].mask = mask;
        }
    }
    return d;
}

std::optional<Document> loadProject(const std::string& pathText, ProjectError& error) {
    fs::path path(pathText);
    std::error_code ec;
    if (!fs::is_directory(path, ec)) { error = invalid(); return std::nullopt; }
    fs::path manifestPath = path / "manifest.json";
    if (!checkFile(manifestPath, path, manifestLimit)) { error = invalid(); return std::nullopt; }
    std::ifstream in(manifestPath, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    json j = json::parse(buffer.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || nestsTooDeep(j)) { error = invalid(); return std::nullopt; }
    // Header first, so an unsupported version is reported as such rather than as damage.
    std::string format;
    if (!getString(j, "format", format, true) || format != formatIdentifier) { error = invalid(); return std::nullopt; }
    auto v = j.find("version");
    if (v == j.end() || !v->is_number_integer()) { error = invalid(); return std::nullopt; }
    if (v->get<int>() < 1 || v->get<int>() > projectFormatVersion) { error = version(v->get<int>()); return std::nullopt; }
    Manifest m;
    if (!parseManifestJson(j, m, error) || !validateManifest(m, error)) return std::nullopt;

    Document d = documentFrom(m);
    // Every file is checked against the budgets from its header first; then the images decode side by side.
    struct Load { size_t layer; bool isMask; fs::path file; std::shared_ptr<Image> image; std::shared_ptr<GrayImage> gray; };
    std::vector<Load> loads;
    long long pixels = 0, maskPixels = 0;
    for (size_t i = 0; i < m.records.size(); i++) {
        const Record& r = m.records[i];
        for (bool isMask : {false, true}) {
            const std::optional<std::string>& filename = isMask ? r.maskFile : r.imageFile;
            if (!filename) continue;
            fs::path file = path / "images" / *filename;
            if (!checkFile(file, path, assetLimit)) { error = tooLarge(); return std::nullopt; }
            PngInfo info;
            if (!readPngInfo(file.string(), info) || info.bitDepth > 8) { error = missingImage(); return std::nullopt; }
            if (!checkSize(info.width, info.height, isMask ? maskPixels : pixels)) { error = tooLarge(); return std::nullopt; }
            loads.push_back({i, isMask, file, nullptr, nullptr});
        }
    }
    parallelFor(0, int(loads.size()), 1, [&](int a, int b) {
        for (int k = a; k < b; k++) {
            Load& load = loads[size_t(k)];
            if (load.isMask) load.gray = readPngGray(load.file.string());
            else load.image = readPngImage(load.file.string());
        }
    });
    for (Load& load : loads) {
        const Record& r = m.records[load.layer];
        Layer& layer = d.layers[load.layer];
        if (load.isMask) {
            if (!load.gray) { error = invalid(); return std::nullopt; }
            LayerMask mask;
            mask.asset = MaskAsset::make(load.gray);
            mask.enabled = r.maskEnabled.value_or(true);
            mask.placement = r.maskPlacement;
            mask.linked = r.maskLinked.value_or(true);
            layer.mask = mask;
        } else {
            if (!load.image) { error = missingImage(); return std::nullopt; }
            layer.asset = Asset::make(load.image, layer.name);
            if (layer.shape) layer.shapeImage = layer.asset->image;
            if (layer.text) layer.textImage = layer.asset->image;
        }
    }
    // What a PSD held that we do not model (psd_carry.h), beside the images; optional, and dropped if unreadable.
    auto readCarry = [&](const fs::path& file) -> std::optional<std::vector<uint8_t>> {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec) || !checkFile(file, path, assetLimit)) return std::nullopt;
        std::ifstream in(file, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in.good() && !in.eof()) return std::nullopt;
        return bytes;
    };
    for (Layer& layer : d.layers)
        if (auto bytes = readCarry(path / "images" / (layer.id + ".psdcarry"))) layer.psdCarry = parsePsdLayerCarry(*bytes);
    if (auto bytes = readCarry(path / "images" / "document.psdcarry")) d.psdCarry = parsePsdDocumentCarry(*bytes);
    // Smart objects: the sources in smartobjects/ (each with its image as PNG), the instances beside their layers.
    {
        std::error_code ec;
        const fs::path dir = path / "smartobjects";
        if (fs::is_directory(dir, ec))
            for (auto& entry : fs::directory_iterator(dir, ec)) {
                if (entry.path().extension() != ".source" || d.smartObjects.size() >= 4096) continue;
                auto bytes = readCarry(entry.path());
                auto source = bytes ? parseSmartObjectSource(*bytes) : std::nullopt;
                if (!source) continue;
                fs::path png = entry.path();
                png.replace_extension(".png");
                if (fs::is_regular_file(png, ec) && checkFile(png, path, assetLimit)) source->image = readPngImage(png.string());
                const std::string id = source->id;
                d.smartObjects[id] = std::make_shared<const SmartObjectSource>(std::move(*source));
            }
    }
    for (Layer& layer : d.layers) {
        auto bytes = layer.asset && layer.asset->image ? readCarry(path / "images" / (layer.id + ".smartobject")) : std::nullopt;
        if (auto instance = bytes ? parseSmartObjectInstance(*bytes) : std::nullopt) {
            layer.smartObject = std::move(*instance);
            layer.smartImage = layer.asset->image;
        }
    }
    return d;
}

std::optional<Uuid> loadedActiveLayer(const std::string& pathText) {
    std::ifstream in(fs::path(pathText) / "manifest.json", std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream buffer;
    buffer << in.rdbuf();
    json j = json::parse(buffer.str(), nullptr, false);
    std::optional<Uuid> id;
    if (j.is_object() && !nestsTooDeep(j)) getUuid(j, "activeLayerID", id, false);
    return id;
}

std::string manifestJson(const Document& document, const std::optional<Uuid>& activeLayerId) {
    json j;
    if (!document.extraJson.empty()) { auto extra = json::parse(document.extraJson, nullptr, false); if (extra.is_object()) j = extra; }
    j["format"] = formatIdentifier;
    bool folders = false;
    for (auto& l : document.layers) folders |= l.isGroup && (l.opacity != 1 || l.blendMode != BlendMode::Normal || !l.passThrough);
    j["version"] = folders ? projectFormatVersion : projectMacFormatVersion;
    j["colorSpace"] = "sRGB";
    j["resolution"] = number(document.resolution);
    j["documentID"] = document.id;
    j["width"] = document.width;
    j["height"] = document.height;
    if (activeLayerId) j["activeLayerID"] = *activeLayerId; else j["activeLayerID"] = nullptr;
    j["layers"] = json::array();
    for (auto& l : document.layers) j["layers"].push_back(recordJson(l));
    return j.dump(2);
}

bool saveProject(const Document& document, const std::optional<Uuid>& activeLayerId, const std::string& pathText, ProjectError& error) {
    // Validate exactly what the reader would check, before touching the disk.
    std::string manifest = manifestJson(document, activeLayerId);
    if (manifest.size() > manifestLimit) { error = tooLarge(); return false; }
    {
        ProjectError parseError;
        if (!parseManifest(manifest, parseError)) { error = parseError; return false; }
    }
    long long pixels = 0, maskPixels = 0;
    for (auto& l : document.layers) {
        if (l.asset && l.asset->image && !checkSize(l.asset->image->width(), l.asset->image->height(), pixels)) { error = tooLarge(); return false; }
        if (l.mask && l.mask->asset.image && !checkSize(l.mask->asset.image->width(), l.mask->asset.image->height(), maskPixels)) { error = tooLarge(); return false; }
    }
    fs::path path(pathText);
    fs::path parent = path.parent_path().empty() ? fs::path(".") : path.parent_path();
    std::error_code ec;
    fs::create_directories(parent, ec);
    std::mt19937 rng{std::random_device{}()};
    fs::path staging = parent / (path.filename().string() + ".saving-" + std::to_string(rng()));
    fs::remove_all(staging, ec);
    if (!fs::create_directories(staging / "images", ec)) { error = ioError("could not create " + staging.string()); return false; }
    auto abandon = [&]() { std::error_code e; fs::remove_all(staging, e); };
    {
        std::ofstream out(staging / "manifest.json", std::ios::binary);
        out << manifest;
        if (!out) { abandon(); error = ioError("could not write manifest"); return false; }
    }
    for (auto& l : document.layers) {
        std::string err;
        if (l.asset && l.asset->image && !writePngImage((staging / "images" / (l.id + ".png")).string(), *l.asset->image, 0, &err)) { abandon(); error = encodeError(); return false; }
        if (l.mask && l.mask->asset.image && !writePngGray((staging / "images" / (l.id + ".mask.png")).string(), *l.mask->asset.image, &err)) { abandon(); error = encodeError(); return false; }
        if (l.psdCarry && !writeBytes(staging / "images" / (l.id + ".psdcarry"), serializePsdCarry(*l.psdCarry))) { abandon(); error = ioError("could not write the PSD data of " + l.name); return false; }
    }
    if (document.psdCarry && !writeBytes(staging / "images" / "document.psdcarry", serializePsdCarry(*document.psdCarry))) { abandon(); error = ioError("could not write the PSD data"); return false; }
    // Smart objects (see the loader): a live instance's record beside its layer; every source in smartobjects/.
    for (auto& l : document.layers)
        if (l.isLiveSmartObject() && !writeBytes(staging / "images" / (l.id + ".smartobject"), serializeSmartObjectInstance(*l.smartObject))) { abandon(); error = ioError("could not write the smart object of " + l.name); return false; }
    if (!document.smartObjects.empty()) {
        std::error_code ec;
        fs::create_directories(staging / "smartobjects", ec);
        int n = 0;
        for (auto& [id, source] : document.smartObjects) {
            const fs::path base = staging / "smartobjects" / std::to_string(n++);
            std::string err;
            if (!writeBytes(fs::path(base).replace_extension(".source"), serializeSmartObjectSource(*source))
                || (source->image && !writePngImage(fs::path(base).replace_extension(".png").string(), *source->image, 0, &err))) {
                abandon(); error = ioError("could not write a smart object source"); return false;
            }
        }
    }
    // Swap the finished package in: the old one is moved aside and removed only after the new one is in place.
    fs::path backup = parent / (path.filename().string() + ".replaced-" + std::to_string(rng()));
    bool existed = fs::exists(path, ec);
    if (existed) {
        fs::rename(path, backup, ec);
        if (ec) { abandon(); error = ioError("could not replace " + path.string() + ": " + ec.message()); return false; }
    }
    fs::rename(staging, path, ec);
    if (ec) {
        if (existed) { std::error_code e; fs::rename(backup, path, e); }
        abandon();
        error = ioError("could not move the saved project into place: " + ec.message());
        return false;
    }
    if (existed) fs::remove_all(backup, ec);
    return true;
}

} // namespace compositor
