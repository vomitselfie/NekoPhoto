// Layer styles as JSON, for automation (layers.style_get / layers.style_set). One field list per effect serves both
// directions. Colours are "#rrggbb"; opacities, scales and depths are fractions (1 = 100%); spread, choke and range
// are percent as Photoshop shows them; sizes are pixels and angles degrees.
#include "compositor/layerstyle.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace compositor {

namespace {

using nlohmann::json;

const char* const kBlendNames[] = {"normal", "dissolve", "darken", "multiply", "colorBurn", "linearBurn", "darkerColor", "lighten", "screen",
                                   "colorDodge", "linearDodge", "lighterColor", "overlay", "softLight", "hardLight", "vividLight", "linearLight",
                                   "pinLight", "hardMix", "difference", "exclusion", "subtract", "divide", "hue", "saturation", "color", "luminosity"};

struct Bad : std::runtime_error { using std::runtime_error::runtime_error; };

std::string hex(StyleColor c) { char b[8]; std::snprintf(b, sizeof b, "#%02x%02x%02x", c.r, c.g, c.b); return b; }

StyleColor parseHex(const json& j, const std::string& key) {
    if (!j.is_string()) throw Bad(key + " must be a colour like \"#ff8800\"");
    const std::string s = j.get<std::string>();
    unsigned r, g, b;
    if (s.size() != 7 || s[0] != '#' || std::sscanf(s.c_str() + 1, "%2x%2x%2x", &r, &g, &b) != 3) throw Bad(key + " must be a colour like \"#ff8800\"");
    return {uint8_t(r), uint8_t(g), uint8_t(b)};
}

/// Writes the fields into a JSON object.
struct Out {
    json j = json::object();
    void operator()(const char* k, float& v) { j[k] = v; }
    void operator()(const char* k, bool& v) { j[k] = v; }
    void operator()(const char* k, std::string& v) { j[k] = v; }
    void operator()(const char* k, StyleColor& v) { j[k] = hex(v); }
    void operator()(const char* k, EffectBlend& v) { j[k] = kBlendNames[int(v)]; }
    template <typename E> void choice(const char* k, E& v, std::initializer_list<const char*> names) { j[k] = *(names.begin() + int(v)); }
    void operator()(const char* k, StyleGradient& g);
    void operator()(const char* k, StyleContour& c) {
        json points = json::array();
        for (auto& p : c.points) points.push_back({{"x", p.x}, {"y", p.y}, {"corner", p.corner}});
        j[k] = points;
    }
};

/// Reads the fields present in a JSON object; any other key is an error.
struct In {
    const json& j;
    std::string where;
    std::set<std::string> seen;
    const json* at(const char* k) {
        auto it = j.find(k);
        if (it == j.end()) return nullptr;
        seen.insert(k);
        return &*it;
    }
    std::string name(const char* k) const { return where + "." + k; }
    void operator()(const char* k, float& v) { if (auto x = at(k)) { if (!x->is_number()) throw Bad(name(k) + " must be a number"); v = x->get<float>(); } }
    void operator()(const char* k, bool& v) { if (auto x = at(k)) { if (!x->is_boolean()) throw Bad(name(k) + " must be true or false"); v = x->get<bool>(); } }
    void operator()(const char* k, std::string& v) { if (auto x = at(k)) { if (!x->is_string()) throw Bad(name(k) + " must be a string"); v = x->get<std::string>(); } }
    void operator()(const char* k, StyleColor& v) { if (auto x = at(k)) v = parseHex(*x, name(k)); }
    void operator()(const char* k, EffectBlend& v) {
        if (auto x = at(k)) {
            for (int i = 0; i < int(std::size(kBlendNames)); i++) if (x->is_string() && x->get<std::string>() == kBlendNames[i]) { v = EffectBlend(i); return; }
            throw Bad(name(k) + " must be a blend mode (normal, multiply, screen, ...)");
        }
    }
    template <typename E> void choice(const char* k, E& v, std::initializer_list<const char*> names) {
        if (auto x = at(k)) {
            int i = 0;
            for (const char* n : names) { if (x->is_string() && x->get<std::string>() == n) { v = E(i); return; } i++; }
            std::string list;
            for (const char* n : names) list += (list.empty() ? "" : ", ") + std::string(n);
            throw Bad(name(k) + " must be one of " + list);
        }
    }
    void operator()(const char* k, StyleGradient& g);
    void operator()(const char* k, StyleContour& c) {
        if (auto x = at(k)) {
            if (!x->is_array()) throw Bad(name(k) + " must be a list of points");
            c.points.clear();
            for (auto& p : *x) c.points.push_back({p.value("x", 0.0f), p.value("y", 0.0f), p.value("corner", false)});
        }
    }
    void finish() const {
        for (auto& [key, value] : j.items()) if (!seen.count(key)) throw Bad(where + " has no setting \"" + key + "\"");
    }
};

template <typename V> void fields(V& v, StyleGradient& g) {
    v.choice("type", g.type, {"linear", "radial", "angle", "reflected", "diamond", "shapeBurst"});
    v("angle", g.angle); v("scale", g.scale); v("reverse", g.reverse); v("dither", g.dither); v("alignWithLayer", g.alignWithLayer);
    v("offsetX", g.offsetX); v("offsetY", g.offsetY); v("smoothness", g.smoothness);
    v.choice("interpolation", g.interpolation, {"classic", "perceptual", "linear"});
}

void Out::operator()(const char* k, StyleGradient& g) {
    Out o;
    fields(o, g);
    json colors = json::array(), alphas = json::array();
    for (auto& s : g.colors) colors.push_back({{"location", s.location}, {"color", hex(s.color)}, {"midpoint", s.midpoint}});
    for (auto& s : g.alphas) alphas.push_back({{"location", s.location}, {"opacity", s.opacity}, {"midpoint", s.midpoint}});
    o.j["colors"] = colors;
    o.j["alphas"] = alphas;
    j[k] = o.j;
}

void In::operator()(const char* k, StyleGradient& g) {
    auto x = at(k);
    if (!x) return;
    if (!x->is_object()) throw Bad(name(k) + " must be an object");
    In in{*x, name(k), {}};
    fields(in, g);
    if (auto c = in.at("colors")) {
        g.colors.clear();
        for (auto& s : *c) g.colors.push_back({s.value("location", 0.0f), parseHex(s.value("color", json("#000000")), name(k) + ".colors"), s.value("midpoint", 0.5f)});
    }
    if (auto a = in.at("alphas")) {
        g.alphas.clear();
        for (auto& s : *a) g.alphas.push_back({s.value("location", 0.0f), s.value("opacity", 1.0f), s.value("midpoint", 0.5f)});
    }
    in.finish();
}

template <typename V> void fields(V& v, DropShadow& e) {
    v("enabled", e.enabled); v("mode", e.mode); v("color", e.color); v("opacity", e.opacity); v("angle", e.angle); v("useGlobalLight", e.useGlobalLight);
    v("distance", e.distance); v("spread", e.spread); v("size", e.size); v("layerConceals", e.layerConceals);
}
template <typename V> void fields(V& v, InnerShadow& e) {
    v("enabled", e.enabled); v("mode", e.mode); v("color", e.color); v("opacity", e.opacity); v("angle", e.angle); v("useGlobalLight", e.useGlobalLight);
    v("distance", e.distance); v("choke", e.choke); v("size", e.size);
}
template <typename V> void fields(V& v, OuterGlow& e) {
    v("enabled", e.enabled); v("mode", e.mode); v("color", e.color); v("opacity", e.opacity); v("spread", e.spread); v("size", e.size);
    v("range", e.range); v("precise", e.precise);
}
template <typename V> void fields(V& v, InnerGlow& e) {
    v("enabled", e.enabled); v("mode", e.mode); v("color", e.color); v("opacity", e.opacity); v("choke", e.choke); v("size", e.size);
    v("range", e.range); v("precise", e.precise); v("center", e.center);
}
template <typename V> void fields(V& v, ColorOverlay& e) { v("enabled", e.enabled); v("mode", e.mode); v("color", e.color); v("opacity", e.opacity); }
template <typename V> void fields(V& v, GradientOverlay& e) { v("enabled", e.enabled); v("mode", e.mode); v("opacity", e.opacity); v("gradient", e.gradient); }
template <typename V> void fields(V& v, PatternOverlay& e) {
    v("enabled", e.enabled); v("mode", e.mode); v("opacity", e.opacity); v("scale", e.scale); v("angle", e.angle); v("pattern", e.patternId);
    v("linkWithLayer", e.linkWithLayer); v("phaseX", e.phaseX); v("phaseY", e.phaseY);
}
template <typename V> void fields(V& v, Satin& e) {
    v("enabled", e.enabled); v("mode", e.mode); v("color", e.color); v("opacity", e.opacity); v("angle", e.angle); v("distance", e.distance);
    v("size", e.size); v("invert", e.invert);
}
template <typename V> void fields(V& v, Stroke& e) {
    v("enabled", e.enabled); v("mode", e.mode); v("color", e.color); v("opacity", e.opacity); v("size", e.size);
    v.choice("position", e.position, {"outside", "inside", "center"}); v("gradientFill", e.gradientFill); v("gradient", e.gradient); v("overprint", e.overprint);
}
template <typename V> void fields(V& v, Bevel& e) {
    v("enabled", e.enabled);
    v.choice("style", e.kind, {"innerBevel", "outerBevel", "emboss", "pillowEmboss", "strokeEmboss"});
    v.choice("technique", e.technique, {"smooth", "chiselHard", "chiselSoft"});
    v("depth", e.depth); v("up", e.up); v("size", e.size); v("soften", e.soften); v("angle", e.angle); v("altitude", e.altitude); v("useGlobalLight", e.useGlobalLight);
    v("highlightMode", e.highlightMode); v("highlightColor", e.highlight); v("highlightOpacity", e.highlightOpacity);
    v("shadowMode", e.shadowMode); v("shadowColor", e.shadow); v("shadowOpacity", e.shadowOpacity);
    v("gloss", e.gloss); v("glossAntialiased", e.glossAntialiased);
    v("useContour", e.useContour); v("contour", e.contour); v("contourAntialiased", e.contourAntialiased); v("contourRange", e.contourRange);
    v("useTexture", e.useTexture); v("texturePattern", e.texturePattern); v("textureScale", e.textureScale); v("textureDepth", e.textureDepth);
    v("textureInvert", e.textureInvert); v("textureLinkWithLayer", e.textureLinkWithLayer);
}

/// Each effect list with its JSON name.
template <typename F> void lists(LayerStyle& s, F f) {
    f("dropShadows", s.dropShadows); f("innerShadows", s.innerShadows); f("outerGlows", s.outerGlows); f("innerGlows", s.innerGlows);
    f("bevels", s.bevels); f("satins", s.satins); f("colorOverlays", s.colorOverlays); f("gradientOverlays", s.gradientOverlays);
    f("patternOverlays", s.patternOverlays); f("strokes", s.strokes);
}

} // namespace

std::string layerStyleToJson(const LayerStyle& style) {
    LayerStyle s = style;
    json j = {{"visible", s.visible}, {"maskHidesEffects", s.maskHidesEffects}, {"blendInteriorAsGroup", s.blendInteriorAsGroup}};
    lists(s, [&](const char* name, auto& list) {
        if (list.empty()) return;
        json items = json::array();
        for (auto& e : list) { Out o; fields(o, e); items.push_back(o.j); }
        j[name] = items;
    });
    return j.dump();
}

bool layerStyleFromJson(const std::string& text, LayerStyle& out, std::string* error) {
    try {
        const json j = json::parse(text);
        if (!j.is_object()) throw Bad("the style must be an object");
        LayerStyle s;
        In top{j, "style", {}};
        top("visible", s.visible); top("maskHidesEffects", s.maskHidesEffects); top("blendInteriorAsGroup", s.blendInteriorAsGroup);
        lists(s, [&](const char* name, auto& list) {
            auto x = top.at(name);
            if (!x) return;
            if (!x->is_array()) throw Bad(std::string(name) + " must be a list");
            for (size_t i = 0; i < x->size(); i++) {
                if (!(*x)[i].is_object()) throw Bad(std::string(name) + " items must be objects");
                typename std::decay_t<decltype(list)>::value_type e{};
                In in{(*x)[i], std::string(name) + "[" + std::to_string(i) + "]", {}};
                fields(in, e);
                in.finish();
                list.push_back(e);
            }
        });
        top.finish();
        out = s;
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

} // namespace compositor
