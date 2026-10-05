#include "compositor/project.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/parallel.h"
#include "compositor/png.h"
#include <nlohmann/json.hpp>
#include <zlib.h>
#ifdef COMPOSITOR_HAVE_ZSTD
#include <zstd.h>
#endif
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
ProjectError version(int v) { return {ProjectError::Version, "This project uses format version " + std::to_string(v) + ". This app supports versions 1-" + std::to_string(projectFormatVersion) + ".", v}; }
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

/// A CMYK file's text colour as its inks, [c, m, y, k] in 0..1 (optional; false when malformed).
bool readInk(const json& j, std::optional<std::array<float, 4>>& out) {
    auto it = j.find("ink");
    if (it == j.end() || it->is_null()) return true;
    if (!it->is_array() || it->size() != 4) return false;
    std::array<float, 4> ink{};
    for (size_t i = 0; i < 4; i++) {
        if (!(*it)[i].is_number()) return false;
        const double v = (*it)[i].get<double>();
        if (!std::isfinite(v)) return false;
        ink[i] = float(std::clamp(v, 0.0, 1.0));
    }
    out = ink;
    return true;
}

json inkJson(const std::array<float, 4>& ink) { return json::array({number(ink[0]), number(ink[1]), number(ink[2]), number(ink[3])}); }

bool getInt(const json& j, const char* key, int& out, bool required) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return !required;
    if (!it->is_number_integer()) return false;
    const long long v = it->get<long long>();
    if (v < -maxImageSide * 16LL || v > maxImageSide * 16LL) return false;
    out = int(v);
    return true;
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
    "maskFile", "maskEnabled", "maskSourceID", "adjustment", "maskPlacement", "maskLinked", "shape", "text", "passThrough", "artboard"};
const std::set<std::string> knownManifestKeys = {"format", "version", "colorSpace", "resolution", "documentID", "width", "height", "activeLayerID", "layers", "slices", "guides", "animation", "sampleType", "profile", "channels", "colorMode", "encodedProfile"};

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
        if (!readInk(*tx, t.ink)) return false;
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
                if (!readInk(rj, r.ink)) return false;
                t.runs.push_back(r);
            }
        }
        l.text = t;
    }
    if (auto a = j.find("artboard"); a != j.end() && !a->is_null()) {
        if (!a->is_object() || !isGroup) return false;
        Artboard ab;
        if (!getInt(*a, "x", ab.x, true) || !getInt(*a, "y", ab.y, true) || !getInt(*a, "width", ab.width, true) || !getInt(*a, "height", ab.height, true)) return false;
        if (!getInt(*a, "background", ab.background, false) || !getString(*a, "preset", ab.presetName, false)) return false;
        if (!getDouble(*a, "red", ab.red, false) || !getDouble(*a, "green", ab.green, false) || !getDouble(*a, "blue", ab.blue, false)) return false;
        if (ab.width < 1 || ab.height < 1 || ab.width > maxImageSide || ab.height > maxImageSide || ab.background < 1 || ab.background > 4) return false;
        l.artboard = ab;
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
    if (l.isGroup && l.artboard) {
        const Artboard& a = *l.artboard;
        j["artboard"] = {{"x", a.x}, {"y", a.y}, {"width", a.width}, {"height", a.height}, {"background", a.background},
                         {"red", number(a.red)}, {"green", number(a.green)}, {"blue", number(a.blue)}, {"preset", a.presetName}};
    }
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
        if (textInkMatches(t.red, t.green, t.blue, t.ink)) j["text"]["ink"] = inkJson(*t.ink);
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
                if (textInkMatches(r.red, r.green, r.blue, r.ink)) rj["ink"] = inkJson(*r.ink);
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
    std::vector<Slice> slices;
    std::vector<Guide> guides;
    std::string extraJson;
    Animation animation;
    SampleType sampleType = SampleType::U8;
    /// Version 9: a CMYK or Lab document ("colorMode"); RGB when absent.
    ColorMode colorMode = ColorMode::RGB;
    /// Version 8: the document's colour profile is the package's profile.icc ("colorSpace": "icc").
    bool tagged = false;
    /// Version 8: alpha and spot channels, their grays in channels/<id>.png (left empty here).
    std::vector<Channel> channels;
    /// A 32-bit document's encoding profile (Document::encodedProfile): none, untagged, or the package's encoded.icc.
    enum class Encoded { None, Untagged, File } encoded = Encoded::None;
};

/// A channel's manifest entry; its gray is the package's channels/<id>.png at the document's depth.
json channelJson(const Channel& c) {
    return {{"id", c.id}, {"name", c.name}, {"kind", c.kind == ChannelKind::Spot ? "spot" : "alpha"},
            {"color", {number(c.color[0]), number(c.color[1]), number(c.color[2])}}, {"opacity", number(c.opacity)},
            {"colorIndicates", c.selectedAreas ? "selected" : "masked"}, {"file", "channels/" + c.id + ".png"}};
}

bool parseChannel(const json& j, Channel& c) {
    if (!j.is_object()) return false;
    std::optional<Uuid> id;
    if (!getUuid(j, "id", id, true) || !getString(j, "name", c.name, false) || c.name.size() > 1024) return false;
    c.id = *id;
    std::string kind = "alpha", indicates = "masked", file = "channels/" + c.id + ".png";
    // A 32-bit document's channels are float sidecars (channels/<id>.f32z); the loader checks the depth matches.
    if (!getString(j, "kind", kind, false) || (kind != "alpha" && kind != "spot")) return false;
    if (!getString(j, "colorIndicates", indicates, false) || (indicates != "masked" && indicates != "selected")) return false;
    if (!getString(j, "file", file, false) || (file != "channels/" + c.id + ".png" && file != "channels/" + c.id + ".f32z")) return false;
    c.kind = kind == "spot" ? ChannelKind::Spot : ChannelKind::Alpha;
    c.selectedAreas = indicates == "selected";
    if (!getDouble(j, "opacity", c.opacity, false) || c.opacity < 0 || c.opacity > 1) return false;
    if (auto color = j.find("color"); color != j.end() && !color->is_null()) {
        if (!color->is_array() || color->size() != 3) return false;
        for (size_t i = 0; i < 3; i++) {
            if (!(*color)[i].is_number()) return false;
            c.color[i] = (*color)[i].get<double>();
            if (!(c.color[i] >= 0 && c.color[i] <= 1)) return false;
        }
    }
    return true;
}

bool parseManifestJson(const json& j, Manifest& m, ProjectError& error) {
    if (!j.is_object()) { error = invalid(); return false; }
    std::string format;
    if (!getString(j, "format", format, true) || format != formatIdentifier) { error = invalid(); return false; }
    auto v = j.find("version");
    if (v == j.end() || !v->is_number_integer()) { error = invalid(); return false; }
    m.version = v->get<int>();
    if (m.version < 1 || m.version > projectFormatVersion) { error = version(m.version); return false; }
    std::string space;
    if (!getString(j, "colorSpace", space, true) || (space != "sRGB" && space != "icc") || (space == "icc" && m.version < 8)) { error = invalid(); return false; }
    m.tagged = space == "icc";
    if (auto p = j.find("profile"); p != j.end() && (!p->is_string() || p->get<std::string>() != "profile.icc" || !m.tagged)) { error = invalid(); return false; }
    // Version 8: the depth of every layer and mask ("u8" or "u16"; 8-bit when absent, as every older project is).
    if (auto t = j.find("sampleType"); t != j.end()) {
        if (!t->is_string() || m.version < 8) { error = invalid(); return false; }
        const std::string type = t->get<std::string>();
        if (type == "u16") m.sampleType = SampleType::U16;
        else if (type == "f32") m.sampleType = SampleType::F32;   // 32 bits: float sidecars (images/<id>.f32z)
        else if (type != "u8") { error = invalid(); return false; }
    }
    if (auto e = j.find("encodedProfile"); e != j.end()) {
        if (!e->is_string() || m.sampleType != SampleType::F32) { error = invalid(); return false; }
        const std::string value = e->get<std::string>();
        if (value == "untagged") m.encoded = Manifest::Encoded::Untagged;
        else if (value == "encoded.icc") m.encoded = Manifest::Encoded::File;
        else { error = invalid(); return false; }
    }
    // Version 9: the colour mode ("cmyk" or "lab"; RGB when absent, as every older project is).
    if (auto c = j.find("colorMode"); c != j.end()) {
        if (!c->is_string() || m.version < 9) { error = invalid(); return false; }
        const std::string mode = c->get<std::string>();
        if (mode == "cmyk") m.colorMode = ColorMode::CMYK;
        else if (mode == "lab") m.colorMode = ColorMode::Lab;
        else if (mode != "rgb") { error = invalid(); return false; }
    }
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
    if (auto sl = j.find("slices"); sl != j.end() && !sl->is_null()) {
        if (!sl->is_array() || sl->size() > 100000) { error = invalid(); return false; }
        for (auto& e : *sl) {
            Slice s;
            int id = 1;
            if (!e.is_object() || !getInt(e, "id", id, true) || id < 1 || !getString(e, "name", s.name, false) || !getInt(e, "x", s.x, true) || !getInt(e, "y", s.y, true)
                || !getInt(e, "width", s.width, true) || !getInt(e, "height", s.height, true) || !getString(e, "url", s.url, false) || !getString(e, "target", s.target, false)
                || !getString(e, "message", s.message, false) || !getString(e, "altTag", s.altTag, false) || s.width < 1 || s.height < 1) { error = invalid(); return false; }
            s.id = uint32_t(id);
            m.slices.push_back(std::move(s));
        }
    }
    if (auto gl = j.find("guides"); gl != j.end() && !gl->is_null()) {
        // [{"orientation": "vertical" | "horizontal", "position": pixels}]
        if (!gl->is_array() || gl->size() > maxGuides) { error = invalid(); return false; }
        for (auto& e : *gl) {
            auto o = e.is_object() ? e.find("orientation") : e.end();
            auto at = e.is_object() ? e.find("position") : e.end();
            if (o == e.end() || at == e.end() || !o->is_string() || !at->is_number()) { error = invalid(); return false; }
            const std::string orientation = o->get<std::string>();
            if (orientation != "vertical" && orientation != "horizontal") { error = invalid(); return false; }
            const double position = at->get<double>();
            if (!std::isfinite(position) || std::abs(position) > guideReach) { error = invalid(); return false; }
            m.guides.push_back({orientation == "vertical" ? Guide::Orientation::Vertical : Guide::Orientation::Horizontal, guidePosition(position)});
        }
    }
    if (auto ch = j.find("channels"); ch != j.end() && !ch->is_null()) {
        if (!ch->is_array() || m.version < 8 || ch->size() > size_t(Document::maxChannels)) { error = invalid(); return false; }
        std::set<Uuid> ids;
        for (auto& e : *ch) {
            Channel c;
            if (!parseChannel(e, c) || !ids.insert(c.id).second) { error = invalid(); return false; }
            m.channels.push_back(std::move(c));
        }
    }
    // Frame animation (NekoPhoto's own key, which other readers skip): a damaged one is dropped, not fatal.
    if (auto a = j.find("animation"); a != j.end() && a->is_object())
        if (auto parsed = parseAnimationJson(a->dump())) m.animation = std::move(*parsed);
    json extra = json::object();
    for (auto& [key, value] : j.items()) if (!knownManifestKeys.count(key)) extra[key] = value;
    if (!extra.empty()) m.extraJson = extra.dump();
    return true;
}

/// A layer's pixels in the package: images/<id>.png, or images/<id>.cmyk for a CMYK document's planes.
std::string layerImageFile(const std::string& id, ColorMode mode) { return id + (mode == ColorMode::CMYK ? ".cmyk" : ".png"); }
/// The same at a depth: a 32-bit document's pixels are images/<id>.f32z, its masks images/<id>.mask.f32z.
std::string layerImageFile(const std::string& id, ColorMode mode, SampleType type) { return type == SampleType::F32 ? id + ".f32z" : layerImageFile(id, mode); }
std::string layerMaskFile(const std::string& id, SampleType type) { return id + (type == SampleType::F32 ? ".mask.f32z" : ".mask.png"); }

bool validateManifest(const Manifest& m, ProjectError& error) {
    if (!Document::validDimension(m.width) || !Document::validDimension(m.height) || m.records.size() > size_t(Document::maxLayers)) { error = tooLarge(); return false; }
    std::vector<Layer> layers;
    std::set<Uuid> ids;
    for (auto& r : m.records) {
        const Layer& l = r.layer;
        if (l.adjustment && (m.version < 7 || l.isGroup || r.imageFile)) { error = invalid(); return false; }
        if (r.maskFile) {
            if (m.version < (l.isGroup ? 6 : 4) || *r.maskFile != layerMaskFile(l.id, m.sampleType)) { error = invalid(); return false; }
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
        if (r.imageFile && *r.imageFile != layerImageFile(l.id, m.colorMode, m.sampleType)) { error = invalid(); return false; }
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

bool checkSize(int width, int height, long long& used, SampleType type = SampleType::U8, ColorMode mode = ColorMode::RGB) {
    // The document's budget rules (Document::canCreate), in bytes: a 16-bit document holds half the pixels of an 8-bit
    // one, a CMYK document four fifths. Masks and channels are checked as RGB is: their budget follows the depth alone.
    if (!Document::canCreate(width, height, type, mode) || (long long)width * height > Document::projectPixelBudgetAt(type, mode) - used) return false;
    used += (long long)width * height;
    return true;
}

bool writeBytes(const fs::path& file, const std::vector<uint8_t>& bytes) {
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(out);
}

// ---- CMYK planes (images/<id>.cmyk) ---------------------------------------------------------------------------------
//
// A CMYK layer's pixels have no PNG form, so they are kept raw: a 24-byte header, then the planes compressed.
//
//   0  "NPCMYK" 0 1    magic and version 1
//   8  u32 width, u32 height (little-endian)
//  16  u8 channels (5), u8 bits (8 or 16), u8 compression (1 zlib, 2 zstd), u8 0
//  20  u32 0
//  24  the compressed stream of the planes C, M, Y, K, alpha, one after the other, rows top-down; 16-bit samples
//      little-endian in 0..32768. Samples are as held: premultiplied, the inks inverted (colormodes.h).
//
// Saves write zlib; zstd is read when the build has it.
constexpr uint8_t planeMagic[8] = {'N', 'P', 'C', 'M', 'Y', 'K', 0, 1};
constexpr size_t planeHeader = 24;

void putU32(std::vector<uint8_t>& out, size_t at, uint32_t v) { for (int i = 0; i < 4; i++) out[at + size_t(i)] = uint8_t(v >> (8 * i)); }
uint32_t getU32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

template <class Buffer>
std::vector<uint8_t> planarBytes(const Buffer& image) {
    using Sample = typename Buffer::Sample;
    const size_t w = size_t(image.width()), h = size_t(image.height()), plane = w * h * sizeof(Sample);
    std::vector<uint8_t> raw(plane * 5);
    parallelFor(0, image.height(), 16, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const Sample* row = image.row(y);
            for (size_t c = 0; c < 5; c++) {
                uint8_t* out = raw.data() + c * plane + size_t(y) * w * sizeof(Sample);
                for (size_t x = 0; x < w; x++) {
                    const Sample v = row[x * 5 + c];
                    if constexpr (sizeof(Sample) == 1) out[x] = v;
                    else { out[x * 2] = uint8_t(v); out[x * 2 + 1] = uint8_t(v >> 8); }
                }
            }
        }
    });
    return raw;
}

bool writeCmykPlanes(const fs::path& file, const AnyImage& image) {
    const bool deep = bool(image.u16());
    if (!deep && !image.c8()) return false;
    std::vector<uint8_t> raw = deep ? planarBytes(*image.u16()) : planarBytes(*image.c8());
    uLongf size = compressBound(uLong(raw.size()));
    std::vector<uint8_t> out(planeHeader + size_t(size));
    if (compress2(out.data() + planeHeader, &size, raw.data(), uLong(raw.size()), 3) != Z_OK) return false;
    out.resize(planeHeader + size_t(size));
    std::copy(std::begin(planeMagic), std::end(planeMagic), out.begin());
    putU32(out, 8, uint32_t(image.width()));
    putU32(out, 12, uint32_t(image.height()));
    out[16] = 5; out[17] = deep ? 16 : 8; out[18] = 1; out[19] = 0;
    putU32(out, 20, 0);
    return writeBytes(file, out);
}

struct PlaneInfo { int width = 0, height = 0, bits = 8, compression = 1; };

/// Reads the header only, for the budget checks before anything is decoded.
bool readCmykPlaneInfo(const fs::path& file, PlaneInfo& info) {
    std::ifstream in(file, std::ios::binary);
    uint8_t h[planeHeader] = {};
    if (!in.read(reinterpret_cast<char*>(h), planeHeader)) return false;
    if (!std::equal(std::begin(planeMagic), std::end(planeMagic), h) || h[16] != 5 || (h[17] != 8 && h[17] != 16) || (h[18] != 1 && h[18] != 2)) return false;
    const uint32_t w = getU32(h + 8), height = getU32(h + 12);
    if (w < 1 || height < 1 || w > uint32_t(maxImageSide) || height > uint32_t(maxImageSide)) return false;
    info = {int(w), int(height), h[17], h[18]};
    return true;
}

/// The planes as the document's buffer (ImageC8, or a 5-channel Image16); null when damaged. Every colour sample is
/// kept within its alpha, as premultiplied samples must be.
AnyImage readCmykPlanes(const fs::path& file, SampleType type) {
    PlaneInfo info;
    if (!readCmykPlaneInfo(file, info) || (info.bits == 16) != (type == SampleType::U16)) return nullptr;
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < planeHeader) return nullptr;
    const size_t sample = info.bits == 16 ? 2 : 1, plane = size_t(info.width) * size_t(info.height) * sample;
    std::vector<uint8_t> raw(plane * 5);
    if (info.compression == 1) {
        uLongf size = uLongf(raw.size());
        if (uncompress(raw.data(), &size, bytes.data() + planeHeader, uLong(bytes.size() - planeHeader)) != Z_OK || size != raw.size()) return nullptr;
    } else {
#ifdef COMPOSITOR_HAVE_ZSTD
        const size_t size = ZSTD_decompress(raw.data(), raw.size(), bytes.data() + planeHeader, bytes.size() - planeHeader);
        if (ZSTD_isError(size) || size != raw.size()) return nullptr;
#else
        return nullptr;
#endif
    }
    auto fill = [&](auto& out) {
        using Sample = typename std::remove_reference_t<decltype(out)>::Sample;
        constexpr uint32_t one = sizeof(Sample) == 1 ? 255u : 32768u;
        const size_t w = size_t(info.width);
        parallelFor(0, info.height, 16, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                Sample* row = out.row(y);
                for (size_t x = 0; x < w; x++) {
                    auto at = [&](size_t c) -> uint32_t {
                        const uint8_t* p = raw.data() + c * plane + (size_t(y) * w + x) * sizeof(Sample);
                        if constexpr (sizeof(Sample) == 1) return p[0];
                        else return uint32_t(p[0]) | uint32_t(p[1]) << 8;
                    };
                    const uint32_t alpha = std::min(at(4), one);
                    for (size_t c = 0; c < 4; c++) row[x * 5 + c] = Sample(std::min(at(c), alpha));
                    row[x * 5 + 4] = Sample(alpha);
                }
            }
        });
    };
    if (type == SampleType::U16) {
        auto out = std::make_shared<Image16>(info.width, info.height, 5);
        fill(*out);
        return Image16Ptr(out);
    }
    auto out = std::make_shared<ImageC8>(info.width, info.height, 5);
    fill(*out);
    return ImageC8Ptr(out);
}

// ---- Float planes (images/<id>.f32z, images/<id>.mask.f32z, channels/<id>.f32z) --------------------------------------
//
// A 32-bit document's pixels, with no PNG form: a 24-byte header, then one zlib stream.
//
//   0  "NPF32Z" 0 1    magic and version 1
//   8  u32 width, u32 height (little-endian)
//  16  u8 channels (4: R, G, B, alpha; 1: a gray), u8 32, u8 1 (zlib), u8 0
//  20  u32 0
//  24  the planes one after the other, rows top-down, each row PSD's predictor for 32-bit channels
//      (predictFloatRow: the floats as four byte planes, big-endian, then each byte the difference from the one before).
//      Samples are as held: premultiplied linear light, colour unbounded, alpha and gray 0..1.
constexpr uint8_t floatMagic[8] = {'N', 'P', 'F', '3', '2', 'Z', 0, 1};

bool writeFloatPlanes(const fs::path& file, const float* samples, int width, int height, int channels) {
    const size_t w = size_t(width), h = size_t(height), rowBytes = w * 4;
    std::vector<uint8_t> raw(rowBytes * h * size_t(channels));
    parallelFor(0, height, 16, [&](int y0, int y1) {
        std::vector<float> row(w);
        for (int y = y0; y < y1; y++)
            for (int c = 0; c < channels; c++) {
                for (size_t x = 0; x < w; x++) row[x] = samples[(size_t(y) * w + x) * size_t(channels) + size_t(c)];
                predictFloatRow(row.data(), width, raw.data() + (size_t(c) * h + size_t(y)) * rowBytes);
            }
    });
    uLongf size = compressBound(uLong(raw.size()));
    std::vector<uint8_t> out(planeHeader + size_t(size));
    if (compress2(out.data() + planeHeader, &size, raw.data(), uLong(raw.size()), 3) != Z_OK) return false;
    out.resize(planeHeader + size_t(size));
    std::copy(std::begin(floatMagic), std::end(floatMagic), out.begin());
    putU32(out, 8, uint32_t(width));
    putU32(out, 12, uint32_t(height));
    out[16] = uint8_t(channels); out[17] = 32; out[18] = 1; out[19] = 0;
    putU32(out, 20, 0);
    return writeBytes(file, out);
}

/// Reads the header only, for the budget checks before anything is decoded.
bool readFloatPlaneInfo(const fs::path& file, PlaneInfo& info, int channels) {
    std::ifstream in(file, std::ios::binary);
    uint8_t h[planeHeader] = {};
    if (!in.read(reinterpret_cast<char*>(h), planeHeader)) return false;
    if (!std::equal(std::begin(floatMagic), std::end(floatMagic), h) || h[16] != channels || h[17] != 32 || h[18] != 1) return false;
    const uint32_t w = getU32(h + 8), height = getU32(h + 12);
    if (w < 1 || height < 1 || w > uint32_t(maxImageSide) || height > uint32_t(maxImageSide)) return false;
    info = {int(w), int(height), 32, 1};
    return true;
}

/// The planes into `out` (width * height * channels floats, interleaved); false when damaged. NaN and infinities are
/// cleaned as they come in (depth.h).
bool readFloatPlanes(const fs::path& file, int channels, PlaneInfo& info, std::vector<float>& out) {
    if (!readFloatPlaneInfo(file, info, channels)) return false;
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < planeHeader) return false;
    const size_t w = size_t(info.width), h = size_t(info.height), rowBytes = w * 4;
    std::vector<uint8_t> raw(rowBytes * h * size_t(channels));
    uLongf size = uLongf(raw.size());
    if (uncompress(raw.data(), &size, bytes.data() + planeHeader, uLong(bytes.size() - planeHeader)) != Z_OK || size != raw.size()) return false;
    out.assign(w * h * size_t(channels), 0.0f);
    parallelFor(0, info.height, 16, [&](int y0, int y1) {
        std::vector<float> row(w);
        for (int y = y0; y < y1; y++)
            for (int c = 0; c < channels; c++) {
                const bool alpha = channels == 1 || c == channels - 1;
                unpredictFloatRow(raw.data() + (size_t(c) * h + size_t(y)) * rowBytes, info.width, row.data());
                for (size_t x = 0; x < w; x++) out[(size_t(y) * w + x) * size_t(channels) + size_t(c)] = alpha ? cleanCoverage(row[x]) : cleanColour(row[x]);
            }
    });
    return true;
}

ImageFPtr readFloatImage(const fs::path& file) {
    PlaneInfo info;
    std::vector<float> samples;
    if (!readFloatPlanes(file, 4, info, samples)) return nullptr;
    auto image = std::make_shared<ImageF>(info.width, info.height);
    std::copy(samples.begin(), samples.end(), image->data());
    return image;
}

GrayFPtr readFloatGray(const fs::path& file) {
    PlaneInfo info;
    std::vector<float> samples;
    if (!readFloatPlanes(file, 1, info, samples)) return nullptr;
    auto gray = std::make_shared<GrayF>(info.width, info.height);
    std::copy(samples.begin(), samples.end(), gray->data());
    return gray;
}

bool checkFile(const fs::path& file, const fs::path& package, uintmax_t maximumBytes) {
    std::error_code ec;
    fs::path root = fs::weakly_canonical(package, ec);
    if (ec) return false;
    fs::path resolved = fs::weakly_canonical(file, ec);
    if (ec) return false;
    std::string rootText = (root / "").string();   // with the trailing separator: "/" here, "\\" on Windows
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
    d.sampleType = m.sampleType;
    d.colorMode = m.colorMode;
    if (m.encoded == Manifest::Encoded::Untagged) d.encodedProfile = ColorProfile{};
    d.slices = m.slices;
    d.guides = m.guides;
    for (auto& r : m.records) d.layers.push_back(r.layer);
    d.channels = m.channels;
    d.animation = m.animation;
    pruneAnimation(d);
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

std::optional<Document> loadProject(const std::string& pathText, ProjectError& error, const ProjectLoadLimits& limits) {
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
    if (!colorModeSupportsDepth(d.colorMode, d.sampleType) || !Document::canCreate(d.width, d.height, d.sampleType, d.colorMode)) { error = tooLarge(); return std::nullopt; }
    const bool cmyk = d.colorMode == ColorMode::CMYK;
    long long layerBytes = 0;
    const long long pixelBytes = colorModeChannels(d.colorMode) * (long long)sampleBytes(d.sampleType);
    // Every file is checked against the budgets from its header first; then the images decode side by side.
    struct Load { size_t layer; bool isMask; fs::path file; AnyImage image; AnyGray gray; };
    const bool deep = d.sampleType == SampleType::U16;
    const bool floating = d.sampleType == SampleType::F32;
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
            if (cmyk && !isMask) {
                PlaneInfo planes;
                if (!readCmykPlaneInfo(file, planes) || planes.bits != (deep ? 16 : 8)) { error = missingImage(); return std::nullopt; }
                info.width = planes.width;
                info.height = planes.height;
            } else if (floating) {
                PlaneInfo planes;
                if (!readFloatPlaneInfo(file, planes, isMask ? 1 : 4)) { error = missingImage(); return std::nullopt; }
                info.width = planes.width;
                info.height = planes.height;
            } else if (!readPngInfo(file.string(), info) || info.bitDepth > (deep ? 16 : 8)) { error = missingImage(); return std::nullopt; }
            if (isMask ? !checkSize(info.width, info.height, maskPixels, d.sampleType) : !checkSize(info.width, info.height, pixels, d.sampleType, d.colorMode)) { error = tooLarge(); return std::nullopt; }
            if (!isMask) {
                const long long bytes = (long long)info.width * info.height * pixelBytes;
                if (bytes > limits.layerBytes - layerBytes) { error = tooLarge(); return std::nullopt; }
                layerBytes += bytes;
            }
            loads.push_back({i, isMask, file, nullptr, nullptr});
        }
    }
    parallelFor(0, int(loads.size()), 1, [&](int a, int b) {
        for (int k = a; k < b; k++) {
            Load& load = loads[size_t(k)];
            if (cmyk && !load.isMask) load.image = readCmykPlanes(load.file, d.sampleType);
            else if (floating) {
                if (load.isMask) load.gray = readFloatGray(load.file);
                else load.image = readFloatImage(load.file);
            } else if (deep) {
                // A 16-bit document's layers and masks are 16-bit PNGs.
                if (load.isMask) load.gray = Gray16Ptr(readPngGray16(load.file.string()));
                else load.image = Image16Ptr(readPngImage16(load.file.string()));
            } else if (load.isMask) load.gray = GrayPtr(readPngGray(load.file.string()));
            else load.image = ImagePtr(readPngImage(load.file.string()));
        }
    });
    for (Load& load : loads) {
        const Record& r = m.records[load.layer];
        Layer& layer = d.layers[load.layer];
        if (load.isMask) {
            if (!load.gray) { error = invalid(); return std::nullopt; }
            LayerMask mask;
            mask.asset = MaskAsset::makeAny(load.gray);
            mask.enabled = r.maskEnabled.value_or(true);
            mask.placement = r.maskPlacement;
            mask.linked = r.maskLinked.value_or(true);
            layer.mask = mask;
        } else {
            if (!load.image) { error = missingImage(); return std::nullopt; }
            layer.asset = Asset::makeAny(load.image, layer.name);
            if (layer.shape) layer.shapeImage = layer.asset->image;
            if (layer.text) layer.textImage = layer.asset->image;
        }
    }
    // Alpha and spot channels (version 8): each gray is channels/<id>.png, at the document's depth and size.
    for (Channel& c : d.channels) {
        const fs::path file = path / "channels" / (c.id + (floating ? ".f32z" : ".png"));
        PngInfo info;
        if (floating) {
            PlaneInfo planes;
            if (!checkFile(file, path, assetLimit) || !readFloatPlaneInfo(file, planes, 1)) { error = missingImage(); return std::nullopt; }
            info.width = planes.width;
            info.height = planes.height;
        } else if (!checkFile(file, path, assetLimit) || !readPngInfo(file.string(), info) || info.bitDepth > (deep ? 16 : 8)) { error = missingImage(); return std::nullopt; }
        if (info.width != d.width || info.height != d.height) { error = invalid(); return std::nullopt; }
        if (!checkSize(info.width, info.height, maskPixels, d.sampleType)) { error = tooLarge(); return std::nullopt; }
        if (floating) c.image = readFloatGray(file);
        else if (deep) c.image = Gray16Ptr(readPngGray16(file.string()));
        else c.image = GrayPtr(readPngGray(file.string()));
        if (!c.image || c.image.width() != d.width || c.image.height() != d.height) { error = missingImage(); return std::nullopt; }
    }
    // What a PSD held that we do not model (psd_carry.h), beside the images; optional, and dropped if unreadable.
    // Every sidecar counts toward one total (limits.sidecarBytes); past it the package is refused.
    unsigned long long sidecarBytes = 0;
    bool overLimit = false;
    auto readCarry = [&](const fs::path& file) -> std::optional<std::vector<uint8_t>> {
        std::error_code ec;
        if (overLimit || !fs::is_regular_file(file, ec) || !checkFile(file, path, assetLimit)) return std::nullopt;
        const uintmax_t size = fs::file_size(file, ec);
        if (ec) return std::nullopt;
        if (size > limits.sidecarBytes - sidecarBytes) { overLimit = true; return std::nullopt; }
        sidecarBytes += size;
        std::ifstream in(file, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in.good() && !in.eof()) return std::nullopt;
        return bytes;
    };
    for (Layer& layer : d.layers)
        if (auto bytes = readCarry(path / "images" / (layer.id + ".psdcarry"))) layer.psdCarry = parsePsdLayerCarry(*bytes);
    if (auto bytes = readCarry(path / "images" / "document.psdcarry")) d.psdCarry = parsePsdDocumentCarry(*bytes);
    for (Channel& c : d.channels)
        if (auto bytes = readCarry(path / "channels" / (c.id + ".psdcarry"))) c.psdCarry = parsePsdChannelCarry(*bytes);
    // The colour profile, kept byte for byte (colorprofile.h); an unreadable one leaves the document untagged.
    if (m.tagged)
        if (auto bytes = readCarry(path / "profile.icc"))
            if (auto profile = profileFromIcc(*bytes); profile && profile->model == colorModelOf(d.colorMode)) d.profile = std::move(*profile);
    if (m.encoded == Manifest::Encoded::File)
        if (auto bytes = readCarry(path / "encoded.icc"))
            if (auto profile = profileFromIcc(*bytes); profile && profile->model == ColorModel::RGB) d.encodedProfile = std::move(*profile);
    // Smart objects: the sources in smartobjects/ (each with its image as PNG, 16-bit for a 16-bit source), the
    // instances beside their layers. Their count, their files and their decoded bytes are limited in total, each image
    // by the budget rules at its depth too.
    {
        std::error_code ec;
        const fs::path dir = path / "smartobjects";
        int sources = 0;
        long long sourceBytes = 0;
        if (fs::is_directory(dir, ec))
            for (auto& entry : fs::directory_iterator(dir, ec)) {
                if (entry.path().extension() != ".source") continue;
                if (++sources > limits.smartObjects) { error = tooLarge(); return std::nullopt; }
                auto bytes = readCarry(entry.path());
                auto source = bytes ? parseSmartObjectSource(*bytes) : std::nullopt;
                if (!source) continue;
                fs::path png = entry.path();
                png.replace_extension(".png");
                PngInfo info;
                if (fs::is_regular_file(png, ec) && checkFile(png, path, assetLimit) && readPngInfo(png.string(), info)) {
                    const SampleType depth = info.bitDepth == 16 ? SampleType::U16 : SampleType::U8;
                    // Sources are RGBA PNGs: four samples a pixel at their own depth.
                    const long long bytesNeeded = (long long)info.width * info.height * colorModeChannels(ColorMode::RGB) * (long long)sampleBytes(depth);
                    if (!Document::canCreate(info.width, info.height, depth) || bytesNeeded > limits.smartObjectBytes - sourceBytes) {
                        error = tooLarge();
                        return std::nullopt;
                    }
                    sourceBytes += bytesNeeded;
                    if (depth == SampleType::U16) source->image = Image16Ptr(readPngImage16(png.string()));
                    else source->image = ImagePtr(readPngImage(png.string()));
                }
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
    if (overLimit) { error = tooLarge(); return std::nullopt; }
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
    for (auto& l : document.layers) folders |= l.isGroup && (l.opacity != 1 || l.blendMode != BlendMode::Normal || !l.passThrough || l.artboard);
    folders |= !document.slices.empty();   // artboards and slices are version 8 too: the Mac app has neither
    folders |= document.sampleType != SampleType::U8;   // so is a 16-bit document
    folders |= !document.profile.empty();               // and one with a colour profile
    folders |= !document.channels.empty();              // or with alpha channels
    j["version"] = document.colorMode != ColorMode::RGB ? projectFormatVersion : folders ? projectRgbFormatVersion : projectMacFormatVersion;
    j["colorSpace"] = document.profile.empty() ? "sRGB" : "icc";
    if (!document.profile.empty()) j["profile"] = "profile.icc";
    else j.erase("profile");
    if (document.sampleType == SampleType::U16) j["sampleType"] = "u16";
    else if (document.sampleType == SampleType::F32) j["sampleType"] = "f32";
    else j.erase("sampleType");
    if (document.sampleType == SampleType::F32 && document.encodedProfile) j["encodedProfile"] = document.encodedProfile->empty() ? "untagged" : "encoded.icc";
    else j.erase("encodedProfile");
    if (document.colorMode != ColorMode::RGB) j["colorMode"] = colorModeKey(document.colorMode);
    else j.erase("colorMode");
    j["resolution"] = number(document.resolution);
    j["documentID"] = document.id;
    j["width"] = document.width;
    j["height"] = document.height;
    if (activeLayerId) j["activeLayerID"] = *activeLayerId; else j["activeLayerID"] = nullptr;
    j["layers"] = json::array();
    for (auto& l : document.layers) {
        json record = recordJson(l);
        if (record.contains("imageFile")) record["imageFile"] = layerImageFile(l.id, document.colorMode, document.sampleType);
        if (record.contains("maskFile")) record["maskFile"] = layerMaskFile(l.id, document.sampleType);
        j["layers"].push_back(std::move(record));
    }
    if (!document.slices.empty()) {
        j["slices"] = json::array();
        for (const Slice& s : document.slices)
            j["slices"].push_back({{"id", s.id}, {"name", s.name}, {"x", s.x}, {"y", s.y}, {"width", s.width}, {"height", s.height},
                                   {"url", s.url}, {"target", s.target}, {"message", s.message}, {"altTag", s.altTag}});
    }
    if (!document.guides.empty()) {
        j["guides"] = json::array();
        for (const Guide& g : document.guides) j["guides"].push_back({{"orientation", g.vertical() ? "vertical" : "horizontal"}, {"position", number(g.position)}});
    } else j.erase("guides");
    if (!document.animation.empty()) j["animation"] = json::parse(animationJson(document.animation));
    else j.erase("animation");
    if (!document.channels.empty()) {
        j["channels"] = json::array();
        for (const Channel& c : document.channels) {
            json entry = channelJson(c);
            if (document.sampleType == SampleType::F32) entry["file"] = "channels/" + c.id + ".f32z";
            j["channels"].push_back(std::move(entry));
        }
    } else j.erase("channels");
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
    if (!colorModeSupportsDepth(document.colorMode, document.sampleType)) { error = encodeError(); return false; }
    const int channels = colorModeChannels(document.colorMode);
    long long pixels = 0, maskPixels = 0;
    for (auto& l : document.layers) {
        // Every buffer must be at the document's depth: the package says one depth for all.
        if (l.asset && l.asset->image && (l.asset->image.sampleType() != document.sampleType || l.asset->image.channels() != channels
                                          || !checkSize(l.asset->image.width(), l.asset->image.height(), pixels, document.sampleType, document.colorMode))) { error = tooLarge(); return false; }
        if (l.mask && l.mask->asset.image && (l.mask->asset.image.sampleType() != document.sampleType || !checkSize(l.mask->asset.image.width(), l.mask->asset.image.height(), maskPixels, document.sampleType))) { error = tooLarge(); return false; }
    }
    for (const Channel& c : document.channels)
        if (!c.image || c.image.sampleType() != document.sampleType || c.image.width() != document.width || c.image.height() != document.height
            || !checkSize(c.image.width(), c.image.height(), maskPixels, document.sampleType)) { error = tooLarge(); return false; }
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
        const std::string imageFile = (staging / "images" / (l.id + ".png")).string(), maskFile = (staging / "images" / (l.id + ".mask.png")).string();
        if (document.colorMode == ColorMode::CMYK) {
            if (l.asset && l.asset->image && !writeCmykPlanes(staging / "images" / layerImageFile(l.id, ColorMode::CMYK), l.asset->image)) { abandon(); error = encodeError(); return false; }
        } else if (document.sampleType == SampleType::F32) {
            if (l.asset && l.asset->image.f32() && !writeFloatPlanes(staging / "images" / layerImageFile(l.id, ColorMode::RGB, SampleType::F32), l.asset->image.f32()->data(), l.asset->image.width(), l.asset->image.height(), 4)) { abandon(); error = encodeError(); return false; }
        } else {
            // RGB, and Lab: L, a, b and alpha go in the PNG's four samples as they are (the manifest says they are Lab).
            if (l.asset && l.asset->image.u8() && !writePngImage(imageFile, *l.asset->image.u8(), 0, &err)) { abandon(); error = encodeError(); return false; }
            if (l.asset && l.asset->image.u16() && !writePngImage16(imageFile, *l.asset->image.u16(), 0, &err)) { abandon(); error = encodeError(); return false; }
        }
        if (l.mask && l.mask->asset.image.u8() && !writePngGray(maskFile, *l.mask->asset.image.u8(), &err)) { abandon(); error = encodeError(); return false; }
        if (l.mask && l.mask->asset.image.u16() && !writePngGray16(maskFile, *l.mask->asset.image.u16(), &err)) { abandon(); error = encodeError(); return false; }
        if (l.mask && l.mask->asset.image.f32()
            && !writeFloatPlanes(staging / "images" / layerMaskFile(l.id, SampleType::F32), l.mask->asset.image.f32()->data(), l.mask->asset.image.width(), l.mask->asset.image.height(), 1)) { abandon(); error = encodeError(); return false; }
        if (l.psdCarry && !writeBytes(staging / "images" / (l.id + ".psdcarry"), serializePsdCarry(*l.psdCarry))) { abandon(); error = ioError("could not write the PSD data of " + l.name); return false; }
    }
    if (document.psdCarry && !writeBytes(staging / "images" / "document.psdcarry", serializePsdCarry(*document.psdCarry))) { abandon(); error = ioError("could not write the PSD data"); return false; }
    if (!document.channels.empty()) {
        if (!fs::create_directories(staging / "channels", ec)) { abandon(); error = ioError("could not create the channels folder"); return false; }
        for (const Channel& c : document.channels) {
            std::string err;
            const std::string file = (staging / "channels" / (c.id + ".png")).string();
            if ((c.image.u8() && !writePngGray(file, *c.image.u8(), &err)) || (c.image.u16() && !writePngGray16(file, *c.image.u16(), &err))) { abandon(); error = encodeError(); return false; }
            if (c.image.f32() && !writeFloatPlanes(staging / "channels" / (c.id + ".f32z"), c.image.f32()->data(), c.image.width(), c.image.height(), 1)) { abandon(); error = encodeError(); return false; }
            if (c.psdCarry && !writeBytes(staging / "channels" / (c.id + ".psdcarry"), serializePsdCarry(*c.psdCarry))) { abandon(); error = ioError("could not write the PSD data of " + c.name); return false; }
        }
    }
    if (!document.profile.empty() && !writeBytes(staging / "profile.icc", document.profile.icc)) { abandon(); error = ioError("could not write the colour profile"); return false; }
    if (document.sampleType == SampleType::F32 && document.encodedProfile && !document.encodedProfile->empty()
        && !writeBytes(staging / "encoded.icc", document.encodedProfile->icc)) { abandon(); error = ioError("could not write the colour profile"); return false; }
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
                || (source->image.u8() && !writePngImage(fs::path(base).replace_extension(".png").string(), *source->image.u8(), 0, &err))
                || (source->image.u16() && !writePngImage16(fs::path(base).replace_extension(".png").string(), *source->image.u16(), 0, &err))) {
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
