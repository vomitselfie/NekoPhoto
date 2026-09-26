// SVG import (svg.h): an SVG's shapes as vector shape layers, its groups as folders, and everything the shape model
// cannot hold handed to the app as small standalone SVGs to draw as pixels. The CSS cascade, colours, transforms,
// path grammar, arcs, the viewport mapping and the non-zero fill rule decomposition are ported from Patchy (MIT,
// src/third_party/patchy_psd/README.md): formats/svg_document_read.cpp and formats/vector_fill_rule.cpp.
//
// Ordering: SVG paints first to last, bottom to top, as Document::layers runs; a folder comes before its contents.
#include "compositor/svg.h"
#include "svg_xml.h"
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace compositor {

namespace {

using svgxml::XmlNode;

constexpr size_t kMaximumDrawables = 4000;
constexpr int kMaximumUseDepth = 32;
constexpr size_t kMaximumUseExpansions = 20000;
constexpr size_t kMaximumInflatedBytes = 256u << 20;
constexpr double kEpsilon = 1e-9;

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

std::string_view trimmed(std::string_view v) {
    while (!v.empty() && isSpace(v.front())) v.remove_prefix(1);
    while (!v.empty() && isSpace(v.back())) v.remove_suffix(1);
    return v;
}

std::string lower(std::string_view v) {
    std::string r(v);
    for (char& c : r) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return r;
}

/// A double from the front of `text` (trailing units stay unconsumed; "5em" does not eat the e). Locale-free.
bool parseNumberPrefix(std::string_view text, size_t& consumed, double& value) {
    size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) { negative = text[i] == '-'; i++; }
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    bool any = false;
    std::string digits;
    while (i < text.size() && digit(text[i])) { any = true; digits += text[i++]; }
    if (i < text.size() && text[i] == '.') {
        size_t j = i + 1;
        std::string fraction;
        while (j < text.size() && digit(text[j])) { any = true; fraction += text[j++]; }
        if (any) { i = j; digits += "." + fraction; }
    }
    if (!any) { consumed = 0; return false; }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        size_t j = i + 1;
        std::string exponent = "e";
        if (j < text.size() && (text[j] == '+' || text[j] == '-')) exponent += text[j++];
        bool expDigits = false;
        while (j < text.size() && digit(text[j])) { expDigits = true; exponent += text[j++]; }
        if (expDigits) { digits += exponent; i = j; }
    }
    std::istringstream stream(digits);
    stream.imbue(std::locale::classic());
    double v = 0;
    stream >> v;
    if (!std::isfinite(v)) { consumed = 0; return false; }
    value = negative ? -v : v;
    consumed = i;
    return true;
}

double numberOr(std::string_view text, double fallback) {
    size_t consumed = 0;
    double v = fallback;
    return parseNumberPrefix(trimmed(text), consumed, v) ? v : fallback;
}

std::vector<double> numberList(std::string_view text) {
    std::vector<double> out;
    size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && (isSpace(text[at]) || text[at] == ',')) at++;
        if (at >= text.size()) break;
        double v = 0;
        size_t consumed = 0;
        if (!parseNumberPrefix(text.substr(at), consumed, v)) break;
        out.push_back(v);
        at += consumed;
    }
    return out;
}

std::string formatNumber(double v, bool pathPrecision = false) {
    if (!std::isfinite(v)) v = 0;
    if (pathPrecision) v = std::round(v * 1e4) / 1e4;   // below a ten-thousandth of a pixel is the path storage's rounding
    if (std::abs(v) < 5e-13) v = 0;
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s.precision(10);
    s << v;
    return s.str();
}

// ---- CSS-lite --------------------------------------------------------------------------------------------------

using Declarations = std::map<std::string, std::string, std::less<>>;

Declarations parseDeclarations(std::string_view text) {
    Declarations out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(';', start);
        if (end == std::string_view::npos) end = text.size();
        const auto declaration = text.substr(start, end - start);
        if (auto colon = declaration.find(':'); colon != std::string_view::npos) {
            auto name = lower(trimmed(declaration.substr(0, colon)));
            if (!name.empty()) out[name] = std::string(trimmed(declaration.substr(colon + 1)));
        }
        start = end + 1;
    }
    return out;
}

/// The selectors real exporters write: flat type, .class and #id (Illustrator's ".st0{fill:#FF0000;}").
struct CssRule {
    enum class Kind { Type, Class, Id } kind = Kind::Type;
    std::string selector;
    Declarations values;
    int order = 0;
};

void collectCssRules(const XmlNode& node, std::vector<CssRule>& rules, int& order) {
    if (node.name == "style") {
        const std::string css = node.all_text();
        size_t at = 0;
        while (at < css.size()) {
            const size_t open = css.find('{', at);
            if (open == std::string::npos) break;
            const size_t close = css.find('}', open + 1);
            if (close == std::string::npos) break;
            const Declarations body = parseDeclarations(std::string_view(css).substr(open + 1, close - open - 1));
            const std::string_view selectors = std::string_view(css).substr(at, open - at);
            size_t s = 0;
            while (s <= selectors.size()) {
                size_t comma = selectors.find(',', s);
                if (comma == std::string_view::npos) comma = selectors.size();
                const auto selector = trimmed(selectors.substr(s, comma - s));
                if (!selector.empty() && selector.find_first_of(" \t\n>+~[:") == std::string_view::npos) {
                    CssRule rule;
                    rule.kind = selector.front() == '#' ? CssRule::Kind::Id : selector.front() == '.' ? CssRule::Kind::Class : CssRule::Kind::Type;
                    rule.selector = std::string(rule.kind == CssRule::Kind::Type ? selector : selector.substr(1));
                    rule.values = body;
                    rule.order = order++;
                    rules.push_back(std::move(rule));
                }
                s = comma + 1;
            }
            at = close + 1;
        }
    }
    for (const auto& child : node.children) if (!child.is_text()) collectCssRules(child, rules, order);
}

bool hasClass(const XmlNode& node, std::string_view wanted) {
    const std::string* classes = node.attribute("class");
    if (!classes) return false;
    std::istringstream words(*classes);
    std::string word;
    while (words >> word) if (word == wanted) return true;
    return false;
}

bool ruleMatches(const CssRule& rule, const XmlNode& node) {
    switch (rule.kind) {
    case CssRule::Kind::Type: return node.name == rule.selector;
    case CssRule::Kind::Class: return hasClass(node, rule.selector);
    case CssRule::Kind::Id: { const std::string* id = node.attribute("id"); return id && *id == rule.selector; }
    }
    return false;
}

// ---- Style --------------------------------------------------------------------------------------------------------

/// The inherited properties a standalone SVG for the raster fallback must restate on its wrapper.
const std::set<std::string, std::less<>>& inheritedProperties() {
    static const std::set<std::string, std::less<>> names = {
        "fill", "stroke", "color", "fill-opacity", "stroke-opacity", "stroke-width", "stroke-miterlimit", "stroke-dasharray",
        "stroke-dashoffset", "stroke-linecap", "stroke-linejoin", "fill-rule", "clip-rule", "visibility", "font-family",
        "font-size", "font-weight", "font-style", "font-variant", "font-stretch", "font", "text-anchor", "letter-spacing",
        "word-spacing", "marker-start", "marker-mid", "marker-end", "paint-order", "image-rendering", "shape-rendering",
        "text-rendering", "writing-mode", "direction", "color-interpolation-filters"};
    return names;
}

struct Style {
    std::string fill = "black", stroke = "none", color = "black";
    double fillOpacity = 1, strokeOpacity = 1, opacity = 1;   // opacity is not inherited
    double strokeWidth = 1, miterLimit = 4, dashOffset = 0;
    std::vector<double> dashes;
    VectorStroke::Cap cap = VectorStroke::Cap::Butt;
    VectorStroke::Join join = VectorStroke::Join::Miter;
    bool evenOdd = false;
    bool displayNone = false;                                   // not inherited
    bool visible = true;
    std::string blend = "normal";                              // mix-blend-mode, not inherited
    bool markers = false;
    std::map<std::string, std::string> inherited;              // raw values of inheritedProperties()
};

void applyStyleValue(Style& style, std::string_view rawName, std::string_view rawValue) {
    const std::string name = lower(rawName);
    const std::string value(trimmed(rawValue));
    const std::string keyword = lower(value);
    if (keyword == "inherit") return;
    if (inheritedProperties().count(name)) style.inherited[name] = value;
    if (name == "fill") style.fill = value;
    else if (name == "stroke") style.stroke = value;
    else if (name == "color") style.color = value;
    else if (name == "fill-opacity") style.fillOpacity = std::clamp(value.ends_with('%') ? numberOr(value, 100) / 100 : numberOr(value, style.fillOpacity), 0.0, 1.0);
    else if (name == "stroke-opacity") style.strokeOpacity = std::clamp(value.ends_with('%') ? numberOr(value, 100) / 100 : numberOr(value, style.strokeOpacity), 0.0, 1.0);
    else if (name == "opacity") style.opacity = std::clamp(value.ends_with('%') ? numberOr(value, 100) / 100 : numberOr(value, style.opacity), 0.0, 1.0);
    else if (name == "stroke-width") style.strokeWidth = std::max(0.0, numberOr(value, style.strokeWidth));
    else if (name == "stroke-miterlimit") style.miterLimit = std::max(1.0, numberOr(value, style.miterLimit));
    else if (name == "stroke-dashoffset") style.dashOffset = numberOr(value, style.dashOffset);
    else if (name == "stroke-dasharray") style.dashes = keyword == "none" ? std::vector<double>{} : numberList(value);
    else if (name == "stroke-linecap") style.cap = keyword == "round" ? VectorStroke::Cap::Round : keyword == "square" ? VectorStroke::Cap::Square : VectorStroke::Cap::Butt;
    else if (name == "stroke-linejoin") style.join = keyword == "round" ? VectorStroke::Join::Round : keyword == "bevel" ? VectorStroke::Join::Bevel : VectorStroke::Join::Miter;
    else if (name == "fill-rule") style.evenOdd = keyword == "evenodd";
    else if (name == "display") style.displayNone = keyword == "none";
    else if (name == "visibility") style.visible = keyword != "hidden" && keyword != "collapse";
    else if (name == "mix-blend-mode") style.blend = keyword;
    else if (name == "marker-start" || name == "marker-mid" || name == "marker-end" || name == "marker") style.markers = keyword != "none" && !keyword.empty();
}

/// The cascade: presentation attributes, then stylesheet rules (type < class < id, later wins), then style="".
Style resolveStyle(const XmlNode& node, const Style& parent, const std::vector<CssRule>& rules) {
    Style style = parent;
    style.opacity = 1;
    style.displayNone = false;
    style.blend = "normal";
    for (const auto& [name, value] : node.attributes) if (name != "style") applyStyleValue(style, name, value);
    std::vector<const CssRule*> applicable;
    for (const auto& rule : rules) if (ruleMatches(rule, node)) applicable.push_back(&rule);
    std::stable_sort(applicable.begin(), applicable.end(), [](const CssRule* a, const CssRule* b) {
        auto rank = [](CssRule::Kind k) { return k == CssRule::Kind::Id ? 2 : k == CssRule::Kind::Class ? 1 : 0; };
        return rank(a->kind) != rank(b->kind) ? rank(a->kind) < rank(b->kind) : a->order < b->order;
    });
    for (const CssRule* rule : applicable) for (const auto& [name, value] : rule->values) applyStyleValue(style, name, value);
    if (const std::string* inlineStyle = node.attribute("style"))
        for (const auto& [name, value] : parseDeclarations(*inlineStyle)) applyStyleValue(style, name, value);
    return style;
}

// ---- Colours ----------------------------------------------------------------------------------------------------

struct NamedColour { const char* name; uint32_t rgb; };
constexpr NamedColour kNamedColours[] = {
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4}, {"azure", 0xF0FFFF},
    {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"black", 0x000000}, {"blanchedalmond", 0xFFEBCD}, {"blue", 0x0000FF},
    {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A}, {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00},
    {"chocolate", 0xD2691E}, {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C},
    {"cyan", 0x00FFFF}, {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B}, {"darkgray", 0xA9A9A9},
    {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9}, {"darkkhaki", 0xBDB76B}, {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F},
    {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC}, {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F},
    {"darkslateblue", 0x483D8B}, {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1},
    {"darkviolet", 0x9400D3}, {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22}, {"fuchsia", 0xFF00FF},
    {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700}, {"goldenrod", 0xDAA520}, {"gray", 0x808080},
    {"green", 0x008000}, {"greenyellow", 0xADFF2F}, {"grey", 0x808080}, {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4},
    {"indianred", 0xCD5C5C}, {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA},
    {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6}, {"lightcoral", 0xF08080},
    {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3}, {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3},
    {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A}, {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA},
    {"lightslategray", 0x778899}, {"lightslategrey", 0x778899}, {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0},
    {"lime", 0x00FF00}, {"limegreen", 0x32CD32}, {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000},
    {"mediumaquamarine", 0x66CDAA}, {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB},
    {"mediumseagreen", 0x3CB371}, {"mediumslateblue", 0x7B68EE}, {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC},
    {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1}, {"moccasin", 0xFFE4B5},
    {"navajowhite", 0xFFDEAD}, {"navy", 0x000080}, {"oldlace", 0xFDF5E6}, {"olive", 0x808000}, {"olivedrab", 0x6B8E23},
    {"orange", 0xFFA500}, {"orangered", 0xFF4500}, {"orchid", 0xDA70D6}, {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98},
    {"paleturquoise", 0xAFEEEE}, {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F},
    {"pink", 0xFFC0CB}, {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399},
    {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1}, {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072},
    {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57}, {"seashell", 0xFFF5EE}, {"sienna", 0xA0522D}, {"silver", 0xC0C0C0},
    {"skyblue", 0x87CEEB}, {"slateblue", 0x6A5ACD}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xFFFAFA},
    {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4}, {"tan", 0xD2B48C}, {"teal", 0x008080}, {"thistle", 0xD8BFD8},
    {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE}, {"wheat", 0xF5DEB3}, {"white", 0xFFFFFF},
    {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00}, {"yellowgreen", 0x9ACD32},
};

/// RGBA in 0..1; none for "none" and what cannot be read.
std::optional<std::array<double, 4>> parseColour(std::string_view raw) {
    const std::string value = lower(trimmed(raw));
    if (value.empty() || value == "none") return std::nullopt;
    if (value == "transparent") return std::array<double, 4>{0, 0, 0, 0};
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    if (value.front() == '#') {
        const std::string_view hex = std::string_view(value).substr(1);
        std::array<double, 4> out{0, 0, 0, 1};
        if (hex.size() == 3 || hex.size() == 4) {
            for (size_t i = 0; i < hex.size(); i++) { const int n = nibble(hex[i]); if (n < 0) return std::nullopt; out[i] = n * 17 / 255.0; }
            return out;
        }
        if (hex.size() == 6 || hex.size() == 8) {
            for (size_t i = 0; i < hex.size() / 2; i++) {
                const int hi = nibble(hex[i * 2]), lo = nibble(hex[i * 2 + 1]);
                if (hi < 0 || lo < 0) return std::nullopt;
                out[i] = (hi * 16 + lo) / 255.0;
            }
            return out;
        }
        return std::nullopt;
    }
    const size_t open = value.find('('), close = value.rfind(')');
    if (open != std::string::npos && close != std::string::npos && close > open) {
        const std::string function = value.substr(0, open);
        std::string body = value.substr(open + 1, close - open - 1);
        std::replace(body.begin(), body.end(), ',', ' ');
        std::replace(body.begin(), body.end(), '/', ' ');
        std::istringstream words(body);
        std::vector<std::string> parts;
        for (std::string w; words >> w;) parts.push_back(w);
        auto channel = [](std::string part, double full) {
            const bool percent = part.ends_with('%');
            if (percent) part.pop_back();
            return std::clamp(numberOr(part, 0) / (percent ? 100.0 : full), 0.0, 1.0);
        };
        if ((function == "rgb" || function == "rgba") && parts.size() >= 3) {
            std::array<double, 4> out{channel(parts[0], 255), channel(parts[1], 255), channel(parts[2], 255), 1};
            if (parts.size() > 3) out[3] = channel(parts[3], 1);
            return out;
        }
        if ((function == "hsl" || function == "hsla") && parts.size() >= 3) {
            double hue = std::fmod(std::fmod(numberOr(parts[0], 0), 360.0) + 360.0, 360.0) / 360.0;
            const double sat = channel(parts[1], 100), light = channel(parts[2], 100);
            auto component = [](double p, double q, double t) {
                if (t < 0) t += 1;
                if (t > 1) t -= 1;
                if (t < 1.0 / 6) return p + (q - p) * 6 * t;
                if (t < 0.5) return q;
                if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
                return p;
            };
            std::array<double, 4> out{light, light, light, 1};
            if (sat > 0) {
                const double q = light < 0.5 ? light * (1 + sat) : light + sat - light * sat, p = 2 * light - q;
                out[0] = component(p, q, hue + 1.0 / 3); out[1] = component(p, q, hue); out[2] = component(p, q, hue - 1.0 / 3);
            }
            if (parts.size() > 3) out[3] = channel(parts[3], 1);
            return out;
        }
        return std::nullopt;
    }
    for (const auto& entry : kNamedColours)
        if (value == entry.name) return std::array<double, 4>{((entry.rgb >> 16) & 255) / 255.0, ((entry.rgb >> 8) & 255) / 255.0, (entry.rgb & 255) / 255.0, 1};
    return std::nullopt;
}

uint8_t byteOf(double v) { return uint8_t(std::clamp(std::lround(v * 255), 0L, 255L)); }

// ---- Transforms ---------------------------------------------------------------------------------------------------

/// `first` applied, then `then` (SVG's "then * first").
Affine compose(const Affine& then, const Affine& first) { return first.concatenating(then); }

Affine parseTransform(std::string_view text) {
    Affine result;
    size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && (isSpace(text[at]) || text[at] == ',')) at++;
        const size_t open = text.find('(', at);
        if (open == std::string_view::npos) break;
        const size_t close = text.find(')', open + 1);
        if (close == std::string_view::npos) break;
        const std::string name = lower(trimmed(text.substr(at, open - at)));
        const auto v = numberList(text.substr(open + 1, close - open - 1));
        Affine part;
        if (name == "matrix" && v.size() == 6) part = {v[0], v[1], v[2], v[3], v[4], v[5]};
        else if (name == "translate" && !v.empty()) part = Affine::translation(v[0], v.size() > 1 ? v[1] : 0);
        else if (name == "scale" && !v.empty()) part = Affine::scaling(v[0], v.size() > 1 ? v[1] : v[0]);
        else if (name == "rotate" && !v.empty()) {
            const Affine rotation = Affine::rotation(v[0] * M_PI / 180);
            part = v.size() >= 3 ? compose(Affine::translation(v[1], v[2]), compose(rotation, Affine::translation(-v[1], -v[2]))) : rotation;
        } else if (name == "skewx" && !v.empty()) part = {1, 0, std::tan(v[0] * M_PI / 180), 1, 0, 0};
        else if (name == "skewy" && !v.empty()) part = {1, std::tan(v[0] * M_PI / 180), 0, 1, 0, 0};
        result = compose(result, part);
        at = close + 1;
    }
    return result;
}

std::string matrixText(const Affine& m) {
    return "matrix(" + formatNumber(m.a) + " " + formatNumber(m.b) + " " + formatNumber(m.c) + " " + formatNumber(m.d) + " " + formatNumber(m.tx) + " " + formatNumber(m.ty) + ")";
}

void transformPath(VectorPath& path, const Affine& m) {
    for (auto& s : path.subpaths)
        for (auto& k : s.knots) {
            const Point i = m.apply({k.inX, k.inY}), a = m.apply({k.x, k.y}), o = m.apply({k.outX, k.outY});
            k = {i.x, i.y, a.x, a.y, o.x, o.y};
        }
}

// ---- Path data ----------------------------------------------------------------------------------------------------

using Subpath = VectorPath::Subpath;
using Knot = VectorPath::Knot;

Knot corner(double x, double y) { return {x, y, x, y, x, y}; }

void lineTo(Subpath& s, double x, double y) { if (!s.knots.empty()) s.knots.push_back(corner(x, y)); }

void cubicTo(Subpath& s, double x1, double y1, double x2, double y2, double x, double y) {
    if (s.knots.empty()) return;
    s.knots.back().outX = x1; s.knots.back().outY = y1;
    Knot next = corner(x, y);
    next.inX = x2; next.inY = y2;
    s.knots.push_back(next);
}

/// An endpoint-parametrised elliptical arc as cubics of at most 90 degrees each.
void arcTo(Subpath& s, double rx, double ry, double rotation, bool large, bool sweep, double x2, double y2) {
    if (s.knots.empty()) return;
    const double x1 = s.knots.back().x, y1 = s.knots.back().y;
    rx = std::abs(rx); ry = std::abs(ry);
    if (rx < kEpsilon || ry < kEpsilon || (std::abs(x2 - x1) < kEpsilon && std::abs(y2 - y1) < kEpsilon)) { lineTo(s, x2, y2); return; }
    const double phi = rotation * M_PI / 180, cp = std::cos(phi), sp = std::sin(phi);
    const double dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    const double xp = cp * dx + sp * dy, yp = -sp * dx + cp * dy;
    const double lambda = xp * xp / (rx * rx) + yp * yp / (ry * ry);
    if (lambda > 1) { rx *= std::sqrt(lambda); ry *= std::sqrt(lambda); }
    const double num = std::max(0.0, rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp);
    const double den = rx * rx * yp * yp + ry * ry * xp * xp;
    const double coef = (large == sweep ? -1.0 : 1.0) * std::sqrt(den > 0 ? num / den : 0);
    const double cxp = coef * (rx * yp / ry), cyp = coef * (-ry * xp / rx);
    const double cx = cp * cxp - sp * cyp + (x1 + x2) / 2, cy = sp * cxp + cp * cyp + (y1 + y2) / 2;
    auto angle = [](double ux, double uy, double vx, double vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
    const double theta1 = angle(1, 0, (xp - cxp) / rx, (yp - cyp) / ry);
    double delta = angle((xp - cxp) / rx, (yp - cyp) / ry, (-xp - cxp) / rx, (-yp - cyp) / ry);
    if (!sweep && delta > 0) delta -= 2 * M_PI;
    if (sweep && delta < 0) delta += 2 * M_PI;
    const int segments = std::max(1, int(std::ceil(std::abs(delta) / (M_PI / 2) - 1e-9)));
    const double step = delta / segments;
    auto map = [&](double u, double v) { return Point{cx + cp * rx * u - sp * ry * v, cy + sp * rx * u + cp * ry * v}; };
    for (int i = 0; i < segments; i++) {
        const double t1 = theta1 + i * step, t2 = t1 + step;
        const double alpha = 4.0 / 3 * std::tan((t2 - t1) / 4);
        const double c1 = std::cos(t1), s1 = std::sin(t1), c2 = std::cos(t2), s2 = std::sin(t2);
        const Point a = map(c1 - alpha * s1, s1 + alpha * c1), b = map(c2 + alpha * s2, s2 - alpha * c2), e = map(c2, s2);
        cubicTo(s, a.x, a.y, b.x, b.y, e.x, e.y);
    }
}

struct PathScanner {
    std::string_view text;
    size_t at = 0;
    void skip() { while (at < text.size() && (isSpace(text[at]) || text[at] == ',')) at++; }
    bool hasNumber() {
        skip();
        if (at >= text.size()) return false;
        const char c = text[at];
        return c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9');
    }
    bool number(double& v) {
        skip();
        size_t consumed = 0;
        if (at >= text.size() || !parseNumberPrefix(text.substr(at), consumed, v)) return false;
        at += consumed;
        return true;
    }
    bool flag(bool& v) {   // arc flags may run together ("a1 1 0 011 0")
        skip();
        if (at >= text.size() || (text[at] != '0' && text[at] != '1')) return false;
        v = text[at++] == '1';
        return true;
    }
};

/// The full d= grammar (M L H V C S Q T A Z, relative forms, implicit repeats); quadratics become cubics. Each
/// subpath its own group, in order. Throws on malformed data.
VectorPath parsePathData(std::string_view data) {
    PathScanner scan{data};
    VectorPath result;
    Subpath current;
    current.closed = false;
    double x = 0, y = 0, startX = 0, startY = 0, lastCx = 0, lastCy = 0, lastQx = 0, lastQy = 0;
    char command = 0, previous = 0;
    int32_t group = 0;
    auto finish = [&] {
        if (current.knots.empty()) return;
        // A closing segment back onto the start knot is the close itself: merge the two knots.
        if (current.closed && current.knots.size() > 1) {
            const Knot& last = current.knots.back();
            Knot& first = current.knots.front();
            if (std::abs(last.x - first.x) < 1e-9 && std::abs(last.y - first.y) < 1e-9) {
                first.inX = last.inX; first.inY = last.inY;
                current.knots.pop_back();
            }
        }
        current.group = group++;
        result.subpaths.push_back(std::move(current));
        current = Subpath{};
        current.closed = false;
    };
    while (true) {
        scan.skip();
        if (scan.at >= data.size()) break;
        const char c = data[scan.at];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) { command = c; scan.at++; }
        else if (command == 0) throw std::runtime_error("path data must begin with a command letter");
        if (previous == 0 && command != 'M' && command != 'm') throw std::runtime_error("path data must begin with a move");
        const bool relative = command >= 'a' && command <= 'z';
        const char upper = relative ? char(command - 'a' + 'A') : command;
        if (upper == 'Z') {
            if (!current.knots.empty()) { current.closed = true; finish(); }
            x = startX; y = startY;
            previous = 'Z';
            command = 0;
            // A drawing command right after Z starts a new subpath at the start point.
            scan.skip();
            if (scan.at < data.size()) {
                const char n = data[scan.at];
                if (((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z')) && n != 'M' && n != 'm' && n != 'Z' && n != 'z') current.knots.push_back(corner(x, y));
            }
            continue;
        }
        if (!scan.hasNumber()) throw std::runtime_error(std::string("path command '") + command + "' is missing its coordinates");
        bool first = true;
        while (scan.hasNumber()) {
            double a = 0, b = 0, c2 = 0, d = 0, e = 0, f = 0;
            auto need = [&](bool ok) { if (!ok) throw std::runtime_error(std::string("invalid path command '") + command + "'"); };
            switch (upper) {
            case 'M':
                need(scan.number(a) && scan.number(b));
                if (relative) { a += x; b += y; }
                if (first) { finish(); current.knots.push_back(corner(a, b)); startX = a; startY = b; }
                else lineTo(current, a, b);
                x = a; y = b;
                break;
            case 'L':
                need(scan.number(a) && scan.number(b));
                if (relative) { a += x; b += y; }
                lineTo(current, a, b);
                x = a; y = b;
                break;
            case 'H':
                need(scan.number(a));
                if (relative) a += x;
                lineTo(current, a, y);
                x = a;
                break;
            case 'V':
                need(scan.number(a));
                if (relative) a += y;
                lineTo(current, x, a);
                y = a;
                break;
            case 'C':
                need(scan.number(a) && scan.number(b) && scan.number(c2) && scan.number(d) && scan.number(e) && scan.number(f));
                if (relative) { a += x; b += y; c2 += x; d += y; e += x; f += y; }
                cubicTo(current, a, b, c2, d, e, f);
                lastCx = c2; lastCy = d; x = e; y = f;
                break;
            case 'S':
                need(scan.number(c2) && scan.number(d) && scan.number(e) && scan.number(f));
                if (relative) { c2 += x; d += y; e += x; f += y; }
                if (previous != 'C' && previous != 'S') { lastCx = x; lastCy = y; }
                cubicTo(current, 2 * x - lastCx, 2 * y - lastCy, c2, d, e, f);
                lastCx = c2; lastCy = d; x = e; y = f;
                break;
            case 'Q':
                need(scan.number(a) && scan.number(b) && scan.number(c2) && scan.number(d));
                if (relative) { a += x; b += y; c2 += x; d += y; }
                cubicTo(current, x + 2 * (a - x) / 3, y + 2 * (b - y) / 3, c2 + 2 * (a - c2) / 3, d + 2 * (b - d) / 3, c2, d);
                lastQx = a; lastQy = b; x = c2; y = d;
                break;
            case 'T':
                need(scan.number(c2) && scan.number(d));
                if (relative) { c2 += x; d += y; }
                if (previous != 'Q' && previous != 'T') { lastQx = x; lastQy = y; }
                a = 2 * x - lastQx; b = 2 * y - lastQy;
                cubicTo(current, x + 2 * (a - x) / 3, y + 2 * (b - y) / 3, c2 + 2 * (a - c2) / 3, d + 2 * (b - d) / 3, c2, d);
                lastQx = a; lastQy = b; x = c2; y = d;
                break;
            case 'A': {
                double rotation = 0;
                bool large = false, sweep = false;
                need(scan.number(a) && scan.number(b) && scan.number(rotation) && scan.flag(large) && scan.flag(sweep) && scan.number(c2) && scan.number(d));
                if (relative) { c2 += x; d += y; }
                arcTo(current, a, b, rotation, large, sweep, c2, d);
                x = c2; y = d;
                break;
            }
            default: throw std::runtime_error(std::string("unsupported path command '") + command + "'");
            }
            previous = upper;
            first = false;
            scan.skip();
            if (scan.at < data.size()) {
                const char n = data[scan.at];
                if ((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z')) break;
            }
        }
    }
    finish();
    return result;
}

VectorPath polygonPath(std::string_view points, bool closed) {
    const auto v = numberList(points);
    VectorPath path;
    if (v.size() < 4) return path;
    Subpath s;
    s.closed = closed;
    for (size_t i = 0; i + 1 < v.size(); i += 2) s.knots.push_back(corner(v[i], v[i + 1]));
    path.subpaths.push_back(std::move(s));
    return path;
}

// ---- Fill rules (Patchy's vector_fill_rule, with the nesting rule made exact for islands in holes) ----------------

std::vector<Point> polyline(const Subpath& s) {
    std::vector<Point> out;
    const size_t n = s.knots.size();
    for (size_t i = 0; i < n; i++) {
        const Knot& a = s.knots[i];
        out.push_back({a.x, a.y});
        if (i + 1 == n && !s.closed) break;
        const Knot& b = s.knots[(i + 1) % n];
        for (double t : {0.25, 0.5, 0.75}) {
            const double u = 1 - t;
            out.push_back({u * u * u * a.x + 3 * u * u * t * a.outX + 3 * u * t * t * b.inX + t * t * t * b.x,
                           u * u * u * a.y + 3 * u * u * t * a.outY + 3 * u * t * t * b.inY + t * t * t * b.y});
        }
    }
    return out;
}

double signedArea(const std::vector<Point>& p) {
    if (p.size() < 3) return 0;
    double area = 0;
    for (size_t i = 0; i < p.size(); i++) { const Point& a = p[i]; const Point& b = p[(i + 1) % p.size()]; area += a.x * b.y - b.x * a.y; }
    return area / 2;
}

bool contains(const std::vector<Point>& p, double x, double y) {
    bool inside = false;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
        if ((p[i].y > y) != (p[j].y > y) && x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x) inside = !inside;
    return !p.empty() && inside;
}

} // namespace

void applySvgFillRule(VectorPath& path, bool evenOdd) {
    if (evenOdd || path.subpaths.size() < 2) {
        for (auto& s : path.subpaths) { s.group = 0; s.op = VectorPath::Op::Add; }
        return;
    }
    // Non-zero: each subpath its own group, largest first so outlines come before what they hold. A subpath adds
    // when the winding just inside it (its own direction plus every outline around it) is non-zero, else subtracts.
    // Exact for nested outlines that do not cross one another; crossing outlines union.
    const size_t n = path.subpaths.size();
    std::vector<std::vector<Point>> lines(n);
    std::vector<double> areas(n);
    for (size_t i = 0; i < n; i++) { lines[i] = polyline(path.subpaths[i]); areas[i] = signedArea(lines[i]); }
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return std::abs(areas[a]) > std::abs(areas[b]); });
    VectorPath out;
    out.inverted = path.inverted;
    out.disabled = path.disabled;
    int32_t group = 0;
    for (size_t i : order) {
        Subpath s = path.subpaths[i];
        s.group = group++;
        s.op = VectorPath::Op::Add;
        if (!lines[i].empty() && std::abs(areas[i]) > 0) {
            int winding = areas[i] > 0 ? 1 : -1;
            const Point probe = lines[i].front();
            for (size_t j = 0; j < n; j++)
                if (j != i && std::abs(areas[j]) > std::abs(areas[i]) && contains(lines[j], probe.x, probe.y)) winding += areas[j] > 0 ? 1 : -1;
            if (winding == 0 && group > 1) s.op = VectorPath::Op::Subtract;
        }
        out.subpaths.push_back(std::move(s));
    }
    path = std::move(out);
}

std::optional<VectorPath> parseSvgPathData(const std::string& data, std::string* error) {
    try {
        return parsePathData(data);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

std::string svgPathData(const VectorPath& path) {
    std::string out;
    auto straight = [](const Knot& a, const Knot& b) {
        return std::abs(a.outX - a.x) < 1e-9 && std::abs(a.outY - a.y) < 1e-9 && std::abs(b.inX - b.x) < 1e-9 && std::abs(b.inY - b.y) < 1e-9;
    };
    auto segment = [&](const Knot& a, const Knot& b) {
        if (straight(a, b)) out += "L" + formatNumber(b.x, true) + " " + formatNumber(b.y, true);
        else out += "C" + formatNumber(a.outX, true) + " " + formatNumber(a.outY, true) + " " + formatNumber(b.inX, true) + " " + formatNumber(b.inY, true) + " " + formatNumber(b.x, true) + " " + formatNumber(b.y, true);
    };
    for (const auto& s : path.subpaths) {
        if (s.knots.empty()) continue;
        if (!out.empty()) out += ' ';
        out += "M" + formatNumber(s.knots.front().x, true) + " " + formatNumber(s.knots.front().y, true);
        for (size_t i = 1; i < s.knots.size(); i++) segment(s.knots[i - 1], s.knots[i]);
        if (s.closed && s.knots.size() > 1) {
            if (!straight(s.knots.back(), s.knots.front())) segment(s.knots.back(), s.knots.front());
            out += "Z";
        }
    }
    return out;
}

namespace {

// ---- Writing a node back out, for the raster fallback -----------------------------------------------------------

std::string escapeXml(std::string_view text, bool attribute) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': if (attribute) out += "&quot;"; else out += c; break;
        default: out += c;
        }
    }
    return out;
}

/// The node as XML. Foreign-namespace elements and attributes are dropped (their prefixes are not declared in the
/// fragment); href is written as xlink:href as well, which every renderer reads.
void writeNode(const XmlNode& node, std::string& out) {
    if (node.is_text()) { out += escapeXml(node.text, false); return; }
    if (node.name.find(':') != std::string::npos) return;
    out += "<" + node.name;
    std::set<std::string> written;   // href and xlink:href both read as href: write one
    for (const auto& [name, value] : node.attributes) {
        if (name.find(':') != std::string::npos && name != "xml:space") continue;
        if (!written.insert(name).second) continue;
        out += " " + std::string(name == "href" ? "xlink:href" : name) + "=\"" + escapeXml(value, true) + "\"";
    }
    if (node.children.empty()) { out += "/>"; return; }
    out += ">";
    for (const auto& child : node.children) writeNode(child, out);
    out += "</" + node.name + ">";
}

bool isDefinition(const std::string& name) {
    return name == "defs" || name == "style" || name == "linearGradient" || name == "radialGradient" || name == "pattern" || name == "clipPath"
        || name == "mask" || name == "filter" || name == "symbol" || name == "marker" || name == "font" || name == "font-face";
}

void collectDefinitions(const XmlNode& node, std::string& out) {
    for (const auto& child : node.children) {
        if (child.is_text()) continue;
        if (isDefinition(child.name)) writeNode(child, out);
        else collectDefinitions(child, out);
    }
}

// ---- The importer -----------------------------------------------------------------------------------------------

struct Importer {
    const XmlNode& root;
    SvgImport result;
    std::vector<CssRule> css;
    std::unordered_map<std::string, const XmlNode*> ids;
    std::string definitions;
    size_t drawables = 0, useExpansions = 0;
    std::set<std::string> useStack;
    std::map<std::string, int> counters;
    /// The raster part the last layer of the current sibling run is, to merge neighbours into (index into rasterParts).
    struct Run { std::optional<size_t> part; };

    explicit Importer(const XmlNode& r) : root(r) {}

    void note(const std::string& text) {
        auto& notes = result.notes;
        if (std::find(notes.begin(), notes.end(), text) == notes.end()) notes.push_back(text);
    }

    void indexIds(const XmlNode& node) {
        if (const std::string* id = node.attribute("id"); id && !id->empty()) ids.emplace(*id, &node);
        for (const auto& child : node.children) if (!child.is_text()) indexIds(child);
    }

    std::string nameFor(const XmlNode& node, const std::string& kind) {
        if (const std::string* id = node.attribute("id"); id && !trimmed(*id).empty()) return std::string(trimmed(*id));
        for (const auto& child : node.children)
            if (child.name == "title") if (auto title = trimmed(child.all_text()); !title.empty()) return std::string(title);
        return kind + " " + std::to_string(++counters[kind]);
    }

    void countDrawable() {
        if (++drawables > kMaximumDrawables) throw std::runtime_error("the SVG has more than " + std::to_string(kMaximumDrawables) + " elements to draw");
    }

    static BlendMode blendFromCss(const std::string& v) {
        static const std::map<std::string, BlendMode> modes = {
            {"multiply", BlendMode::Multiply}, {"screen", BlendMode::Screen}, {"overlay", BlendMode::Overlay}, {"darken", BlendMode::Darken},
            {"lighten", BlendMode::Lighten}, {"color-dodge", BlendMode::ColorDodge}, {"color-burn", BlendMode::ColorBurn},
            {"hard-light", BlendMode::HardLight}, {"soft-light", BlendMode::SoftLight}, {"difference", BlendMode::Difference},
            {"exclusion", BlendMode::Exclusion}, {"hue", BlendMode::Hue}, {"saturation", BlendMode::Saturation}, {"color", BlendMode::Color},
            {"luminosity", BlendMode::Luminosity}, {"plus-lighter", BlendMode::LinearDodge}};
        auto it = modes.find(v);
        return it == modes.end() ? BlendMode::Normal : it->second;
    }

    bool references(const XmlNode& node, const char* attribute, const Style&) const {
        const std::string* v = node.attribute(attribute);
        if (v && lower(trimmed(*v)) != "none" && !trimmed(*v).empty()) return true;
        if (const std::string* inlineStyle = node.attribute("style")) {
            auto d = parseDeclarations(*inlineStyle);
            if (auto it = d.find(attribute); it != d.end() && lower(trimmed(it->second)) != "none") return true;
        }
        for (const auto& rule : css)
            if (ruleMatches(rule, node)) if (auto it = rule.values.find(attribute); it != rule.values.end() && lower(trimmed(it->second)) != "none") return true;
        return false;
    }

    /// Why this element (or group) cannot be vectors, or empty when it can.
    std::string rasterReason(const XmlNode& node, const Style& style) const {
        if (references(node, "filter", style)) return "filters";
        // An inside or outside stroke we (or Patchy) wrote is cut by a clip or mask of its own outline: not a real one.
        const bool strokeCut = node.attribute("data-nekophoto-stroke-align") || node.attribute("data-patchy-stroke-align");
        if (!strokeCut && references(node, "clip-path", style)) return "clip paths";
        if (!strokeCut && references(node, "mask", style)) return "masks";
        if (node.name != "g" && node.name != "svg" && style.markers) return "markers";
        return {};
    }

    /// A paint as a solid colour with its alpha; none for no paint; a reason when it is a gradient or pattern.
    std::optional<std::array<double, 4>> solidPaint(const std::string& raw, const Style& style, std::string& unsupported) {
        std::string paint(trimmed(raw));
        const std::string keyword = lower(paint);
        if (keyword.empty() || keyword == "none") return std::nullopt;
        if (keyword == "currentcolor") paint = style.color;
        if (paint.starts_with("url(")) {
            const size_t hash = paint.find('#'), close = paint.find(')');
            const XmlNode* referenced = nullptr;
            if (hash != std::string::npos && close != std::string::npos && close > hash) {
                auto it = ids.find(std::string(trimmed(std::string_view(paint).substr(hash + 1, close - hash - 1))));
                if (it != ids.end()) referenced = it->second;
            }
            if (!referenced) {
                if (close != std::string::npos) if (auto fallback = trimmed(std::string_view(paint).substr(close + 1)); !fallback.empty()) return solidPaint(std::string(fallback), style, unsupported);
                return std::nullopt;   // an unresolvable reference paints nothing
            }
            unsupported = referenced->name == "pattern" ? "patterns" : "gradients";
            return std::nullopt;
        }
        auto colour = parseColour(paint);
        if (!colour) { note("An SVG colour \"" + paint + "\" could not be read; black was used."); return std::array<double, 4>{0, 0, 0, 1}; }
        return colour;
    }

    struct Geometry { VectorPath path; bool fillable = true; };

    std::optional<Geometry> geometry(const XmlNode& node) {
        auto attr = [&](const char* name, double fallback = 0) { const std::string* v = node.attribute(name); return v ? numberOr(*v, fallback) : fallback; };
        Geometry g;
        if (node.name == "path") {
            const std::string* d = node.attribute("d");
            if (!d || trimmed(*d).empty()) return std::nullopt;
            try { g.path = parsePathData(*d); }
            catch (const std::exception& e) {
                note(std::string("An SVG path's data could not be read (") + e.what() + "); it was left out.");
                return std::nullopt;
            }
        } else if (node.name == "polygon" || node.name == "polyline") {
            const std::string* points = node.attribute("points");
            if (!points) return std::nullopt;
            g.path = polygonPath(*points, node.name == "polygon");
        } else if (node.name == "rect") {
            const double x = attr("x"), y = attr("y"), w = attr("width"), h = attr("height");
            if (w <= 0 || h <= 0) return std::nullopt;
            const bool hasRx = node.attribute("rx"), hasRy = node.attribute("ry");
            double rx = std::max(0.0, hasRx ? attr("rx") : hasRy ? attr("ry") : 0), ry = std::max(0.0, hasRy ? attr("ry") : rx);
            rx = std::min(rx, w / 2); ry = std::min(ry, h / 2);
            if (rx > kEpsilon && ry > kEpsilon) {
                // Elliptical corners as arcs, clockwise from the top edge.
                const std::string d = "M" + formatNumber(x + rx) + " " + formatNumber(y) + "H" + formatNumber(x + w - rx) + "A" + formatNumber(rx) + " " + formatNumber(ry) + " 0 0 1 " + formatNumber(x + w) + " " + formatNumber(y + ry)
                    + "V" + formatNumber(y + h - ry) + "A" + formatNumber(rx) + " " + formatNumber(ry) + " 0 0 1 " + formatNumber(x + w - rx) + " " + formatNumber(y + h)
                    + "H" + formatNumber(x + rx) + "A" + formatNumber(rx) + " " + formatNumber(ry) + " 0 0 1 " + formatNumber(x) + " " + formatNumber(y + h - ry)
                    + "V" + formatNumber(y + ry) + "A" + formatNumber(rx) + " " + formatNumber(ry) + " 0 0 1 " + formatNumber(x + rx) + " " + formatNumber(y) + "Z";
                g.path = parsePathData(d);
            } else g.path = rectanglePath(Rect(x, y, w, h));
        } else if (node.name == "circle" || node.name == "ellipse") {
            const double cx = attr("cx"), cy = attr("cy");
            const double rx = node.name == "circle" ? attr("r") : attr("rx", attr("ry")), ry = node.name == "circle" ? attr("r") : attr("ry", attr("rx"));
            if (rx <= 0 || ry <= 0) return std::nullopt;
            g.path = ellipsePath(Rect(cx - rx, cy - ry, 2 * rx, 2 * ry));
        } else if (node.name == "line") {
            Subpath s;
            s.closed = false;
            s.knots = {corner(attr("x1"), attr("y1")), corner(attr("x2"), attr("y2"))};
            g.path.subpaths.push_back(std::move(s));
            g.fillable = false;
        } else return std::nullopt;
        if (g.path.subpaths.empty()) return std::nullopt;
        return g;
    }

    Layer& addLayer(Layer layer, const std::optional<Uuid>& parent) {
        layer.parentId = parent;
        result.document.layers.push_back(std::move(layer));
        return result.document.layers.back();
    }

    /// Adds `node` to the raster part the run is on, or starts a new one (a blank placeholder layer).
    void rasterize(const XmlNode& node, const Style& inherited, const Affine& ctm, const std::optional<Uuid>& parent, Run& run, const std::string& kind) {
        std::string fragment = "<g transform=\"" + matrixText(ctm) + "\"";
        std::string styleText;
        for (const auto& [name, value] : inherited.inherited) styleText += name + ":" + value + ";";
        if (!styleText.empty()) fragment += " style=\"" + escapeXml(styleText, true) + "\"";
        fragment += ">";
        writeNode(node, fragment);
        fragment += "</g>";
        const bool continues = run.part && !result.document.layers.empty() && result.document.layers.back().id == result.rasterParts[*run.part].layer;
        if (continues) { result.rasterParts[*run.part].svg += fragment; return; }
        Layer layer(nameFor(node, kind), Size(1, 1));
        const Uuid id = layer.id;
        addLayer(std::move(layer), parent);
        result.rasterParts.push_back({id, fragment});
        run.part = result.rasterParts.size() - 1;
    }

    void importShape(const XmlNode& node, const Style& style, const Affine& parentCtm, const std::optional<Uuid>& parent, Run& run) {
        auto g = geometry(node);
        if (!g) return;
        countDrawable();
        Affine ctm = parentCtm;
        if (const std::string* t = node.attribute("transform")) ctm = compose(parentCtm, parseTransform(*t));

        std::string fillUnsupported, strokeUnsupported;
        const auto fillPaint = g->fillable ? solidPaint(style.fill, style, fillUnsupported) : std::nullopt;
        const auto strokePaint = style.strokeWidth > 0 ? solidPaint(style.stroke, style, strokeUnsupported) : std::nullopt;
        std::string reason = rasterReason(node, style);
        if (reason.empty() && !fillUnsupported.empty()) reason = fillUnsupported;
        if (reason.empty() && !strokeUnsupported.empty()) reason = strokeUnsupported;
        if (!reason.empty()) {
            note("SVG " + reason + " have no counterpart in shape layers; those elements were drawn as pixels.");
            rasterize(node, style, parentCtm, parent, run, "Shape");
            return;
        }
        run.part.reset();

        applySvgFillRule(g->path, style.evenOdd);
        transformPath(g->path, ctm);
        const double scale = std::sqrt(std::abs(ctm.determinant()));
        if (strokePaint && std::abs(std::hypot(ctm.a, ctm.b) - std::hypot(ctm.c, ctm.d)) > 0.05 * std::max(1.0, scale))
            note("A stroke under an SVG transform that stretches one way more than the other was given one width.");

        VectorShape shape;
        shape.path = std::move(g->path);
        shape.fill = fillPaint.has_value();
        const auto& paint = fillPaint ? *fillPaint : strokePaint ? *strokePaint : std::array<double, 4>{0, 0, 0, 1};
        shape.r = byteOf(paint[0]); shape.g = byteOf(paint[1]); shape.b = byteOf(paint[2]);
        VectorStroke& stroke = shape.stroke;
        stroke.enabled = strokePaint.has_value() && style.strokeWidth * scale > kEpsilon;
        stroke.fillEnabled = shape.fill;
        if (stroke.enabled) {
            stroke.width = std::max(0.01, style.strokeWidth * scale);
            stroke.r = byteOf((*strokePaint)[0]); stroke.g = byteOf((*strokePaint)[1]); stroke.b = byteOf((*strokePaint)[2]);
            stroke.cap = style.cap;
            stroke.join = style.join;
            stroke.miterLimit = style.miterLimit;
            stroke.align = VectorStroke::Align::Center;
            // Our own exports (and Patchy's) write inside and outside strokes at double width with a hint.
            const std::string* align = node.attribute("data-nekophoto-stroke-align");
            if (!align) align = node.attribute("data-patchy-stroke-align");
            if (align && (*align == "inside" || *align == "outside")) {
                stroke.align = *align == "inside" ? VectorStroke::Align::Inside : VectorStroke::Align::Outside;
                const std::string* width = node.attribute("data-nekophoto-stroke-width");
                if (!width) width = node.attribute("data-patchy-stroke-width");
                stroke.width = std::max(0.01, width ? numberOr(*width, stroke.width / 2 / scale) * scale : stroke.width / 2);
            }
            bool anyDash = false;
            for (double dash : style.dashes) anyDash = anyDash || dash > 0;
            if (anyDash) {
                for (double dash : style.dashes) stroke.dashes.push_back(std::max(0.0, dash * scale) / stroke.width);
                if (stroke.dashes.size() % 2) { const auto copy = stroke.dashes; stroke.dashes.insert(stroke.dashes.end(), copy.begin(), copy.end()); }
                stroke.dashOffset = style.dashOffset * scale / stroke.width;
            }
        }
        // Opacity: the layer carries opacity x fill-opacity (x a colour's alpha); the stroke's own opacity divides that
        // back out, so a stroke more opaque than its fill is held to the fill's.
        const double fillFactor = shape.fill ? style.fillOpacity * (*fillPaint)[3] : 1.0;
        const double strokeTarget = strokePaint ? style.strokeOpacity * (*strokePaint)[3] : 1.0;
        double layerOpacity = style.opacity * fillFactor;
        if (stroke.enabled) {
            if (!shape.fill) layerOpacity = style.opacity * strokeTarget, stroke.opacity = 1;
            else {
                stroke.opacity = float(std::clamp(fillFactor > 1e-6 ? strokeTarget / fillFactor : strokeTarget, 0.0, 1.0));
                if (strokeTarget > fillFactor + 1e-6) note("An SVG stroke more opaque than its fill was drawn at the fill's opacity.");
            }
        }
        const std::string kind = node.name == "rect" ? "Rectangle" : node.name == "circle" || node.name == "ellipse" ? "Ellipse" : node.name == "line" ? "Line"
                                 : node.name == "polygon" ? "Polygon" : "Shape";
        Layer layer(nameFor(node, kind), Size(1, 1));
        setVectorShape(layer, result.document, shape);
        layer.opacity = std::clamp(layerOpacity, 0.0, 1.0);
        layer.visible = !style.displayNone && style.visible;
        layer.blendMode = blendFromCss(style.blend);
        addLayer(std::move(layer), parent);
        result.shapeLayers++;
    }

    static bool switchChildSelectable(const XmlNode& child) {
        if (child.attribute("requiredExtensions") || child.attribute("requiredFeatures")) return false;
        if (const std::string* language = child.attribute("systemLanguage")) return language->starts_with("en") || language->find(",en") != std::string::npos;
        return true;
    }

    void importChildren(const XmlNode& node, const Style& inherited, const Affine& ctm, const std::optional<Uuid>& parent, int useDepth) {
        Run run;
        importChildren(node, inherited, ctm, parent, useDepth, run);
    }

    void importChildren(const XmlNode& node, const Style& inherited, const Affine& ctm, const std::optional<Uuid>& parent, int useDepth, Run& run) {
        for (const auto& child : node.children) {
            if (child.is_text() || isDefinition(child.name) || child.name == "title" || child.name == "desc" || child.name == "metadata"
                || child.name == "script" || child.name.find(':') != std::string::npos)
                continue;   // definitions, non-rendered content and foreign namespaces
            const Style style = resolveStyle(child, inherited, css);
            if (child.name == "switch") {
                for (const auto& candidate : child.children) {
                    if (candidate.is_text() || !switchChildSelectable(candidate)) continue;
                    XmlNode wrapper;
                    wrapper.name = "g";
                    wrapper.children.push_back(candidate);
                    importChildren(wrapper, style, ctm, parent, useDepth, run);
                    break;
                }
                continue;
            }
            if (child.name == "use") {
                importUse(child, style, ctm, parent, useDepth, run);
                continue;
            }
            if (child.name == "a") {   // links are transparent containers
                importChildren(child, style, ctm, parent, useDepth, run);
                continue;
            }
            if (child.name == "g" || child.name == "svg") {
                const std::string reason = rasterReason(child, style);
                if (!reason.empty()) {
                    note("SVG " + reason + " have no counterpart in shape layers; the groups using them were drawn as pixels.");
                    countDrawable();
                    rasterize(child, inherited, ctm, parent, run, "Group");
                    continue;
                }
                run.part.reset();
                Affine local;
                if (const std::string* t = child.attribute("transform")) local = parseTransform(*t);
                if (child.name == "svg") {
                    local = compose(local, Affine::translation(numberOr(child.attribute("x") ? *child.attribute("x") : "0", 0), numberOr(child.attribute("y") ? *child.attribute("y") : "0", 0)));
                    if (child.attribute("viewBox")) note("A nested SVG's viewBox was left out (its contents were placed unscaled).");
                }
                Layer folder(nameFor(child, "Group"), result.document.size());
                folder.isGroup = true;
                folder.opacity = style.opacity;
                folder.visible = !style.displayNone && style.visible;
                folder.blendMode = blendFromCss(style.blend);
                // SVG isolates a group that has opacity or a blend mode; a plain one draws straight through.
                folder.passThrough = style.opacity >= 1 && folder.blendMode == BlendMode::Normal;
                const Uuid id = folder.id;
                addLayer(std::move(folder), parent);
                importChildren(child, style, compose(ctm, local), id, useDepth);
                continue;
            }
            static const std::set<std::string> shapes = {"path", "rect", "circle", "ellipse", "line", "polyline", "polygon"};
            if (shapes.count(child.name)) { importShape(child, style, ctm, parent, run); continue; }
            if (style.displayNone || !style.visible) continue;   // hidden text or images draw nothing
            countDrawable();
            const std::string kind = child.name == "text" ? "Text" : child.name == "image" ? "Image" : "Element";
            note(child.name == "text" ? "SVG text was drawn as pixels (it is not editable text here)."
                 : child.name == "image" ? "SVG images were placed as pixel layers."
                 : "SVG <" + child.name + "> elements were drawn as pixels.");
            rasterize(child, inherited, ctm, parent, run, kind);
        }
    }

    void importUse(const XmlNode& child, const Style& style, const Affine& ctm, const std::optional<Uuid>& parent, int useDepth, Run& run) {
        if (useDepth >= kMaximumUseDepth) { note("SVG <use> references nest too deeply; the deepest were left out."); return; }
        const std::string* href = child.attribute("href");
        if (!href || href->empty() || href->front() != '#') { note("An SVG <use> without a reference inside the file was left out."); return; }
        if (!useStack.insert(*href).second) { note("A circular SVG <use> reference was left out."); return; }
        if (++useExpansions > kMaximumUseExpansions) { note("The SVG repeats too many <use> references; the rest were left out."); useStack.erase(*href); return; }
        auto found = ids.find(href->substr(1));
        if (found == ids.end()) note("An SVG <use> refers to an element that is not there.");
        else {
            Affine placement = Affine::translation(numberOr(child.attribute("x") ? *child.attribute("x") : "0", 0), numberOr(child.attribute("y") ? *child.attribute("y") : "0", 0));
            if (const std::string* t = child.attribute("transform")) placement = compose(parseTransform(*t), placement);
            XmlNode wrapper;
            wrapper.name = "g";
            XmlNode instance = *found->second;
            if (instance.name == "symbol" || instance.name == "svg") {
                instance.name = "g";
                if (instance.attribute("viewBox")) note("An SVG symbol's viewBox was left out (its contents were placed unscaled).");
                instance.attributes.erase(std::remove_if(instance.attributes.begin(), instance.attributes.end(), [](const auto& a) {
                    return a.first == "viewBox" || a.first == "x" || a.first == "y" || a.first == "width" || a.first == "height" || a.first == "id"; }), instance.attributes.end());
            }
            wrapper.children.push_back(std::move(instance));
            importChildren(wrapper, style, compose(ctm, placement), parent, useDepth + 1, run);
        }
        useStack.erase(*href);
    }

    void run() {
        if (root.name != "svg") throw std::runtime_error("not an SVG document (its root element is <" + root.name + ">)");
        indexIds(root);
        int order = 0;
        collectCssRules(root, css, order);
        collectDefinitions(root, definitions);

        // The canvas: CSS pixels (96 to the inch for physical units); unitless or percentage sizes take the viewBox.
        bool physical = false;
        auto length = [&physical](const std::string* value) -> std::optional<double> {
            if (!value) return std::nullopt;
            const auto text = trimmed(*value);
            if (text.empty() || text.ends_with('%')) return std::nullopt;
            size_t consumed = 0;
            double n = 0;
            if (!parseNumberPrefix(text, consumed, n)) return std::nullopt;
            const std::string unit = lower(trimmed(text.substr(consumed)));
            if (unit.empty() || unit == "px") return n;
            physical = true;
            if (unit == "in") return n * 96;
            if (unit == "cm") return n * 96 / 2.54;
            if (unit == "mm") return n * 96 / 25.4;
            if (unit == "pt") return n * 96 / 72;
            if (unit == "pc") return n * 96 / 6;
            physical = false;
            return std::nullopt;
        };
        const auto view = numberList(root.attribute("viewBox") ? *root.attribute("viewBox") : "");
        auto width = length(root.attribute("width")), height = length(root.attribute("height"));
        const bool hasView = view.size() == 4 && view[2] > 0 && view[3] > 0;
        if (hasView && width && !height) height = *width * view[3] / view[2];
        if (hasView && height && !width) width = *height * view[2] / view[3];
        if (!width && hasView) width = view[2];
        if (!height && hasView) height = view[3];
        if (!width || !height || *width <= 0 || *height <= 0) {
            width = 300; height = 150;
            note("The SVG gives no usable size; it was opened at 300 x 150.");
        }
        double clamp = 1;
        if (*width > maxImageSide || *height > maxImageSide) {
            clamp = std::min(maxImageSide / *width, maxImageSide / *height);
            *width *= clamp; *height *= clamp;
            note("The SVG was scaled down to fit 30,000 pixels a side.");
        }
        const int w = std::max(1, int(std::lround(*width))), h = std::max(1, int(std::lround(*height)));
        if ((long long)w * h > Document::pixelBudget) {
            const double fit = std::sqrt(double(Document::pixelBudget) / (double(w) * h));
            clamp *= fit;
            *width *= fit; *height *= fit;
            note("The SVG was scaled down to fit the 100-megapixel canvas budget.");
        }
        result.document = Document(std::max(1, int(std::lround(*width))), std::max(1, int(std::lround(*height))));
        result.document.resolution = physical ? 96 : 72;

        // viewBox to viewport, with preserveAspectRatio (default xMidYMid meet).
        Affine viewport = Affine::scaling(clamp, clamp);
        if (hasView) {
            const double sx = *width / view[2], sy = *height / view[3];
            const std::string par = lower(root.attribute("preserveAspectRatio") ? trimmed(*root.attribute("preserveAspectRatio")) : "xmidymid meet");
            if (par.find("none") != std::string::npos) viewport = {sx, 0, 0, sy, -view[0] * sx, -view[1] * sy};
            else {
                const double s = par.find("slice") != std::string::npos ? std::max(sx, sy) : std::min(sx, sy);
                const double ax = par.find("xmin") != std::string::npos ? 0 : par.find("xmax") != std::string::npos ? 1 : 0.5;
                const double ay = par.find("ymin") != std::string::npos ? 0 : par.find("ymax") != std::string::npos ? 1 : 0.5;
                viewport = {s, 0, 0, s, -view[0] * s + (*width - view[2] * s) * ax, -view[1] * s + (*height - view[3] * s) * ay};
            }
        }
        const Style rootStyle = resolveStyle(root, Style{}, css);
        if (rootStyle.opacity < 1) note("The SVG's own opacity was left out.");
        importChildren(root, rootStyle, viewport, std::nullopt, 0);

        // Each raster part as a standalone SVG over the document, in document pixels.
        const std::string head = "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" version=\"1.1\" width=\""
            + std::to_string(result.document.width) + "\" height=\"" + std::to_string(result.document.height) + "\" viewBox=\"0 0 "
            + std::to_string(result.document.width) + " " + std::to_string(result.document.height) + "\">" + (definitions.empty() ? std::string() : "<defs>" + definitions + "</defs>");
        for (auto& part : result.rasterParts) part.svg = head + part.svg + "</svg>";
    }
};

std::optional<std::vector<uint8_t>> inflateGzip(const std::vector<uint8_t>& bytes, std::string* error) {
    z_stream stream{};
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) { if (error) *error = "could not start decompression"; return std::nullopt; }
    stream.next_in = const_cast<Bytef*>(bytes.data());
    stream.avail_in = uInt(bytes.size());
    std::vector<uint8_t> out;
    uint8_t buffer[65536];
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        stream.next_out = buffer;
        stream.avail_out = sizeof buffer;
        rc = inflate(&stream, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) break;
        out.insert(out.end(), buffer, buffer + (sizeof buffer - stream.avail_out));
        if (out.size() > kMaximumInflatedBytes) { rc = Z_MEM_ERROR; break; }
        if (rc == Z_OK && stream.avail_in == 0 && stream.avail_out != 0) break;
    }
    inflateEnd(&stream);
    if (rc != Z_STREAM_END) { if (error) *error = "the compressed SVG (SVGZ) is damaged"; return std::nullopt; }
    return out;
}

} // namespace

std::optional<SvgImport> importSvg(const std::vector<uint8_t>& raw, std::string* error) {
    try {
        std::vector<uint8_t> inflated;
        const std::vector<uint8_t>* bytes = &raw;
        if (raw.size() >= 2 && raw[0] == 0x1F && raw[1] == 0x8B) {
            auto out = inflateGzip(raw, error);
            if (!out) return std::nullopt;
            inflated = std::move(*out);
            bytes = &inflated;
        }
        const XmlNode root = svgxml::parse_xml(std::span<const uint8_t>(bytes->data(), bytes->size()));
        Importer importer(root);
        importer.run();
        if (importer.result.document.layers.size() > size_t(Document::maxLayers)) throw std::runtime_error("the SVG has more elements than a document holds layers");
        return std::move(importer.result);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

std::optional<std::vector<uint8_t>> readSvgBytes(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (error) *error = "could not open the file"; return std::nullopt; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 2 && bytes[0] == 0x1F && bytes[1] == 0x8B) return inflateGzip(bytes, error);
    return bytes;
}

std::optional<SvgImport> importSvgFile(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (error) *error = "could not open the file"; return std::nullopt; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return importSvg(bytes, error);
}

} // namespace compositor
