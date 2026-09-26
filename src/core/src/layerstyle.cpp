// Layer styles from a PSD's 'lfx2' / 'lmfx' descriptor. Parsing, the contour and gradient maths and the pattern
// decoder follow Patchy (MIT; src/psd/psd_layer_styles.cpp, core/style_contour.cpp, core/blend_math.cpp and
// docs/ps-compat.md there), whose rules were pinned against Photoshop 2026.
#include "compositor/layerstyle.h"
#include "compositor/blend.h"
#include "compositor/document.h"
#include "psd/psd_descriptor.hpp"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace compositor {

namespace psd = patchy::psd;

namespace {


float unit(float v) { return std::clamp(v, 0.0f, 1.0f); }

std::string enumOf(const psd::DescriptorObject& o, const char* key, const char* fallback) {
    auto v = psd::descriptor_value(o, key);
    return v && v->type == psd::DescriptorValue::Type::Enum ? v->enum_value : fallback;
}

std::string stringOf(const psd::DescriptorObject& o, const char* key) {
    auto v = psd::descriptor_value(o, key);
    return v && v->type == psd::DescriptorValue::Type::String ? v->string_value : std::string();
}

float num(const psd::DescriptorObject& o, const char* key, double fallback) {
    const double v = psd::descriptor_number(o, key, fallback);
    return std::isfinite(v) ? float(v) : float(fallback);
}

float percent(const psd::DescriptorObject& o, const char* key, double fallback) { return unit(num(o, key, fallback) / 100.0f); }

bool flag(const psd::DescriptorObject& o, const char* key, bool fallback) { return psd::descriptor_bool(o, key, fallback); }

EffectBlend blendFrom(const std::string& v, EffectBlend fallback) {
    static const std::map<std::string, EffectBlend> modes{
        {"normal", EffectBlend::Normal}, {"Nrml", EffectBlend::Normal}, {"norm", EffectBlend::Normal},
        {"dissolve", EffectBlend::Dissolve}, {"Dslv", EffectBlend::Dissolve}, {"diss", EffectBlend::Dissolve},
        {"darken", EffectBlend::Darken}, {"Drkn", EffectBlend::Darken}, {"dark", EffectBlend::Darken},
        {"multiply", EffectBlend::Multiply}, {"Mltp", EffectBlend::Multiply}, {"mul ", EffectBlend::Multiply},
        {"colorBurn", EffectBlend::ColorBurn}, {"CBrn", EffectBlend::ColorBurn}, {"idiv", EffectBlend::ColorBurn},
        {"linearBurn", EffectBlend::LinearBurn}, {"lbrn", EffectBlend::LinearBurn},
        {"darkerColor", EffectBlend::DarkerColor}, {"dkCl", EffectBlend::DarkerColor},
        {"lighten", EffectBlend::Lighten}, {"Lghn", EffectBlend::Lighten}, {"lite", EffectBlend::Lighten},
        {"screen", EffectBlend::Screen}, {"Scrn", EffectBlend::Screen}, {"scrn", EffectBlend::Screen},
        {"colorDodge", EffectBlend::ColorDodge}, {"CDdg", EffectBlend::ColorDodge}, {"div ", EffectBlend::ColorDodge},
        {"linearDodge", EffectBlend::LinearDodge}, {"lddg", EffectBlend::LinearDodge},
        {"lighterColor", EffectBlend::LighterColor}, {"lgCl", EffectBlend::LighterColor},
        {"overlay", EffectBlend::Overlay}, {"Ovrl", EffectBlend::Overlay}, {"over", EffectBlend::Overlay},
        {"softLight", EffectBlend::SoftLight}, {"SftL", EffectBlend::SoftLight}, {"sLit", EffectBlend::SoftLight},
        {"hardLight", EffectBlend::HardLight}, {"HrdL", EffectBlend::HardLight}, {"hLit", EffectBlend::HardLight},
        {"vividLight", EffectBlend::VividLight}, {"vLit", EffectBlend::VividLight},
        {"linearLight", EffectBlend::LinearLight}, {"lLit", EffectBlend::LinearLight},
        {"pinLight", EffectBlend::PinLight}, {"pLit", EffectBlend::PinLight},
        {"hardMix", EffectBlend::HardMix}, {"hMix", EffectBlend::HardMix},
        {"difference", EffectBlend::Difference}, {"Dfrn", EffectBlend::Difference}, {"diff", EffectBlend::Difference},
        {"exclusion", EffectBlend::Exclusion}, {"Xclu", EffectBlend::Exclusion}, {"smud", EffectBlend::Exclusion},
        {"blendSubtraction", EffectBlend::Subtract}, {"fsub", EffectBlend::Subtract},
        {"blendDivide", EffectBlend::Divide}, {"fdiv", EffectBlend::Divide},
        {"hue", EffectBlend::Hue}, {"H   ", EffectBlend::Hue}, {"hue ", EffectBlend::Hue},
        {"saturation", EffectBlend::Saturation}, {"Strt", EffectBlend::Saturation}, {"sat ", EffectBlend::Saturation},
        {"color", EffectBlend::Color}, {"Clr ", EffectBlend::Color}, {"colr", EffectBlend::Color},
        {"luminosity", EffectBlend::Luminosity}, {"Lmns", EffectBlend::Luminosity}, {"lum ", EffectBlend::Luminosity}};
    auto it = modes.find(v);
    return it == modes.end() ? fallback : it->second;
}

EffectBlend modeOf(const psd::DescriptorObject& o, const char* key, EffectBlend fallback) {
    auto v = psd::descriptor_value(o, key);
    return v && v->type == psd::DescriptorValue::Type::Enum ? blendFrom(v->enum_value, fallback) : fallback;
}

/// RGB from a colour object ('RGBC'; 'CMYC' converted plainly, as the importer does for CMYK files).
StyleColor colorOf(const psd::DescriptorObject& o, const char* key, StyleColor fallback) {
    auto c = psd::descriptor_object(o, key);
    if (!c) return fallback;
    auto byte = [](double v) { return uint8_t(std::clamp(std::lround(v), 0L, 255L)); };
    if (c->class_id == "CMYC") {
        auto ink = [&](const char* k) { return psd::descriptor_number(*c, k) / 100.0; };
        const double k = ink("Blck");
        return {byte(255 * (1 - ink("Cyn ")) * (1 - k)), byte(255 * (1 - ink("Mgnt")) * (1 - k)), byte(255 * (1 - ink("Ylw ")) * (1 - k))};
    }
    if (c->class_id == "Grsc") { const uint8_t g = byte(255 * (1 - psd::descriptor_number(*c, "Gry ") / 100.0)); return {g, g, g}; }
    return {byte(psd::descriptor_number(*c, "Rd  ")), byte(psd::descriptor_number(*c, "Grn ")), byte(psd::descriptor_number(*c, "Bl  "))};
}

StyleContour contourOf(const psd::DescriptorObject& shape) {
    StyleContour contour;
    auto curve = psd::descriptor_value(shape, "Crv ");
    if (!curve || curve->type != psd::DescriptorValue::Type::List) return contour;
    for (auto& item : curve->list_value) {
        if (item.type != psd::DescriptorValue::Type::Object || !item.object_value) continue;
        contour.points.push_back({num(*item.object_value, "Hrzn", 0), num(*item.object_value, "Vrtc", 0), !flag(*item.object_value, "Cnty", true)});
    }
    return contour;
}

StyleGradient gradientOf(const psd::DescriptorObject& effect) {
    StyleGradient g;
    if (auto grad = psd::descriptor_object(effect, "Grad")) {
        g.smoothness = std::clamp(num(*grad, "Intr", 4096), 0.0f, 4096.0f) / 4096.0f;
        if (auto colors = psd::descriptor_value(*grad, "Clrs"); colors && colors->type == psd::DescriptorValue::Type::List)
            for (auto& item : colors->list_value) {
                if (item.type != psd::DescriptorValue::Type::Object || !item.object_value) continue;
                const auto& s = *item.object_value;
                const std::string type = enumOf(s, "Type", "UsrS");
                const StyleColor fallback = type == "BckC" ? StyleColor{255, 255, 255} : StyleColor{};
                g.colors.push_back({unit(num(s, "Lctn", 0) / 4096.0f), colorOf(s, "Clr ", fallback), unit(num(s, "Mdpn", 50) / 100.0f)});
            }
        if (auto trns = psd::descriptor_value(*grad, "Trns"); trns && trns->type == psd::DescriptorValue::Type::List)
            for (auto& item : trns->list_value) {
                if (item.type != psd::DescriptorValue::Type::Object || !item.object_value) continue;
                const auto& s = *item.object_value;
                g.alphas.push_back({unit(num(s, "Lctn", 0) / 4096.0f), percent(s, "Opct", 100), unit(num(s, "Mdpn", 50) / 100.0f)});
            }
    }
    g.angle = num(effect, "Angl", 90);
    g.scale = std::max(0.01f, num(effect, "Scl ", 100) / 100.0f);
    g.reverse = flag(effect, "Rvrs", false);
    g.dither = flag(effect, "Dthr", false);
    const std::string interp = enumOf(effect, "gs99", enumOf(effect, "gradientsInterpolationMethod", "Gcls").c_str());
    g.interpolation = interp == "Perc" || interp == "perceptual" ? StyleGradient::Interpolation::Perceptual
                    : interp == "Lnr " || interp == "linear" ? StyleGradient::Interpolation::Linear : StyleGradient::Interpolation::Classic;
    g.alignWithLayer = flag(effect, "Algn", true);
    if (auto ofst = psd::descriptor_object(effect, "Ofst")) { g.offsetX = num(*ofst, "Hrzn", 0); g.offsetY = num(*ofst, "Vrtc", 0); }
    const std::string type = enumOf(effect, "Type", "Lnr ");
    g.type = type == "Rdl " ? StyleGradient::Type::Radial : type == "Angl" ? StyleGradient::Type::Angle : type == "Rflc" ? StyleGradient::Type::Reflected
           : type == "Dmnd" ? StyleGradient::Type::Diamond : type == "shapeburst" ? StyleGradient::Type::ShapeBurst : StyleGradient::Type::Linear;
    std::stable_sort(g.colors.begin(), g.colors.end(), [](auto& a, auto& b) { return a.location < b.location; });
    std::stable_sort(g.alphas.begin(), g.alphas.end(), [](auto& a, auto& b) { return a.location < b.location; });
    return g;
}

/// Each enabled instance of an effect (each instance with `all`): the '...Multi' list when there is one, else the
/// single object. `parse` returns the effect it read, whose `enabled` is set from the descriptor.
template <typename F>
void each(const psd::DescriptorObject& root, const char* single, const char* multi, bool all, F parse) {
    auto one = [&](const psd::DescriptorObject& o) {
        const bool on = flag(o, "enab", false);
        if (on || all) parse(o).enabled = on;
    };
    if (auto list = psd::descriptor_value(root, multi); list && list->type == psd::DescriptorValue::Type::List) {
        for (auto& item : list->list_value)
            if (item.type == psd::DescriptorValue::Type::Object && item.object_value) one(*item.object_value);
        return;
    }
    if (auto o = psd::descriptor_object(root, single)) one(*o);
}

std::shared_ptr<LayerStyle> parseEffects(const std::vector<uint8_t>& block, bool all = false) {
    psd::BigEndianReader r(block);
    (void)r.read_u32();                // object effects version
    if (r.read_u32() != 16) return nullptr;
    const psd::DescriptorObject root = psd::read_descriptor(r);
    auto style = std::make_shared<LayerStyle>();
    style->visible = flag(root, "masterFXSwitch", true);
    const float scale = std::max(0.01f, num(root, "Scl ", 100) / 100.0f);   // Scale Effects
    each(root, "DrSh", "dropShadowMulti", all, [&](const psd::DescriptorObject& e) -> auto& {
        DropShadow s;
        s.mode = modeOf(e, "Md  ", EffectBlend::Multiply); s.color = colorOf(e, "Clr ", {}); s.opacity = percent(e, "Opct", 75);
        s.angle = num(e, "lagl", 120); s.useGlobalLight = flag(e, "uglg", false);
        s.distance = std::max(0.0f, num(e, "Dstn", 5)) * scale; s.spread = std::clamp(num(e, "Ckmt", 0), 0.0f, 100.0f);
        s.size = std::max(0.0f, num(e, "blur", 5)) * scale; s.layerConceals = flag(e, "layerConceals", true);
        style->dropShadows.push_back(s);
        return style->dropShadows.back();
    });
    each(root, "IrSh", "innerShadowMulti", all, [&](const psd::DescriptorObject& e) -> auto& {
        InnerShadow s;
        s.mode = modeOf(e, "Md  ", EffectBlend::Multiply); s.color = colorOf(e, "Clr ", {}); s.opacity = percent(e, "Opct", 75);
        s.angle = num(e, "lagl", 120); s.useGlobalLight = flag(e, "uglg", false);
        s.distance = std::max(0.0f, num(e, "Dstn", 5)) * scale; s.choke = std::clamp(num(e, "Ckmt", 0), 0.0f, 100.0f);
        s.size = std::max(0.0f, num(e, "blur", 5)) * scale;
        style->innerShadows.push_back(s);
        return style->innerShadows.back();
    });
    each(root, "OrGl", "outerGlowMulti", all, [&](const psd::DescriptorObject& e) -> auto& {
        OuterGlow g;
        g.mode = modeOf(e, "Md  ", EffectBlend::Screen); g.color = colorOf(e, "Clr ", {255, 255, 190}); g.opacity = percent(e, "Opct", 75);
        g.spread = std::clamp(num(e, "Ckmt", 0), 0.0f, 100.0f); g.size = std::max(0.0f, num(e, "blur", 5)) * scale;
        g.precise = enumOf(e, "GlwT", "SfBL") == "PrBL"; g.range = std::clamp(num(e, "Inpr", 100), 1.0f, 100.0f);
        style->outerGlows.push_back(g);
        return style->outerGlows.back();
    });
    each(root, "IrGl", "innerGlowMulti", all, [&](const psd::DescriptorObject& e) -> auto& {
        InnerGlow g;
        g.mode = modeOf(e, "Md  ", EffectBlend::Screen); g.color = colorOf(e, "Clr ", {255, 255, 190}); g.opacity = percent(e, "Opct", 75);
        g.choke = std::clamp(num(e, "Ckmt", 0), 0.0f, 100.0f); g.size = std::max(0.0f, num(e, "blur", 5)) * scale;
        g.precise = enumOf(e, "GlwT", "SfBL") == "PrBL"; g.range = std::clamp(num(e, "Inpr", 100), 1.0f, 100.0f);
        g.center = enumOf(e, "glwS", "SrcE") == "SrcC";
        style->innerGlows.push_back(g);
        return style->innerGlows.back();
    });
    each(root, "SoFi", "solidFillMulti", all, [&](const psd::DescriptorObject& e) -> auto& {
        style->colorOverlays.push_back({modeOf(e, "Md  ", EffectBlend::Normal), colorOf(e, "Clr ", {255, 0, 0}), percent(e, "Opct", 100)});
        return style->colorOverlays.back();
    });
    each(root, "GrFl", "gradientFillMulti", all, [&](const psd::DescriptorObject& e) -> auto& {
        GradientOverlay g;
        g.mode = modeOf(e, "Md  ", EffectBlend::Normal); g.opacity = percent(e, "Opct", 100); g.gradient = gradientOf(e);
        style->gradientOverlays.push_back(g);
        return style->gradientOverlays.back();
    });
    each(root, "patternFill", "patternFillMulti", all, [&](const psd::DescriptorObject& e) -> auto& {
        PatternOverlay p;
        p.mode = modeOf(e, "Md  ", EffectBlend::Normal); p.opacity = percent(e, "Opct", 100);
        p.scale = std::max(0.01f, num(e, "Scl ", 100) / 100.0f); p.angle = num(e, "Angl", 0);
        if (auto ptrn = psd::descriptor_object(e, "Ptrn")) p.patternId = stringOf(*ptrn, "Idnt");
        p.linkWithLayer = flag(e, "Algn", true);
        if (auto phase = psd::descriptor_object(e, "phase")) { p.phaseX = num(*phase, "Hrzn", 0); p.phaseY = num(*phase, "Vrtc", 0); }
        style->patternOverlays.push_back(p);
        return style->patternOverlays.back();
    });
    each(root, "ChFX", "chromeFXMulti", all, [&](const psd::DescriptorObject& e) -> auto& {
        Satin s;
        s.mode = modeOf(e, "Md  ", EffectBlend::Multiply); s.color = colorOf(e, "Clr ", {}); s.opacity = percent(e, "Opct", 50);
        s.angle = num(e, "lagl", 19); s.distance = std::max(0.0f, num(e, "Dstn", 11)) * scale;
        s.size = std::max(0.0f, num(e, "blur", 14)) * scale; s.invert = flag(e, "Invr", true);
        style->satins.push_back(s);
        return style->satins.back();
    });
    each(root, "FrFX", "frameFXMulti", all, [&](const psd::DescriptorObject& e) -> auto& {
        Stroke s;
        s.mode = modeOf(e, "Md  ", EffectBlend::Normal); s.opacity = percent(e, "Opct", 100);
        s.size = std::max(1.0f, num(e, "Sz  ", 3)) * scale;
        const std::string position = enumOf(e, "Styl", "OutF");
        s.position = position == "InsF" ? Stroke::Position::Inside : position == "CtrF" ? Stroke::Position::Center : Stroke::Position::Outside;
        s.color = colorOf(e, "Clr ", {});
        s.gradientFill = enumOf(e, "PntT", "SClr") == "GrFl";
        if (s.gradientFill) s.gradient = gradientOf(e);
        s.overprint = flag(e, "overprint", false);
        style->strokes.push_back(s);
        return style->strokes.back();
    });
    each(root, "ebbl", "bevelEmbossMulti", all, [&](const psd::DescriptorObject& e) -> auto& {
        Bevel b;
        b.highlightMode = modeOf(e, "hglM", EffectBlend::Screen); b.highlight = colorOf(e, "hglC", {255, 255, 255}); b.highlightOpacity = percent(e, "hglO", 75);
        b.shadowMode = modeOf(e, "sdwM", EffectBlend::Multiply); b.shadow = colorOf(e, "sdwC", {}); b.shadowOpacity = percent(e, "sdwO", 75);
        b.angle = num(e, "lagl", 120); b.useGlobalLight = flag(e, "uglg", false); b.altitude = num(e, "Lald", 30);
        b.depth = std::max(0.01f, num(e, "srgR", 100) / 100.0f); b.size = std::max(1.0f, num(e, "blur", 5)) * scale;
        b.up = enumOf(e, "bvlD", "In  ") != "Out ";
        const std::string kind = enumOf(e, "bvlS", "InrB");
        b.kind = kind == "OtrB" ? Bevel::Kind::Outer : kind == "Embs" ? Bevel::Kind::Emboss : kind == "PlEb" ? Bevel::Kind::Pillow
               : kind == "strokeEmboss" ? Bevel::Kind::StrokeEmboss : Bevel::Kind::Inner;
        const std::string technique = enumOf(e, "bvlT", "SfBL");
        b.technique = technique == "PrBL" ? Bevel::Technique::ChiselHard : technique == "Slmt" ? Bevel::Technique::ChiselSoft : Bevel::Technique::Smooth;
        b.soften = std::max(0.0f, num(e, "Sftn", 0)) * scale;
        if (auto gloss = psd::descriptor_object(e, "TrnS")) b.gloss = contourOf(*gloss);
        b.glossAntialiased = flag(e, "antialiasGloss", false);
        b.useContour = flag(e, "useShape", false);
        if (auto shape = psd::descriptor_object(e, "MpgS")) b.contour = contourOf(*shape);
        b.contourAntialiased = flag(e, "AntA", false);
        b.contourRange = unit(num(e, "Inpr", 50) / 100.0f);
        b.useTexture = flag(e, "useTexture", false);
        b.textureInvert = flag(e, "InvT", false); b.textureLinkWithLayer = flag(e, "Algn", true);
        b.textureScale = std::max(0.01f, num(e, "Scl ", 100) / 100.0f);
        b.textureDepth = std::clamp(num(e, "textureDepth", 100) / 100.0f, -10.0f, 10.0f);
        if (auto ptrn = psd::descriptor_object(e, "Ptrn")) b.texturePattern = stringOf(*ptrn, "Idnt");
        if (auto phase = psd::descriptor_object(e, "phase")) { b.texturePhaseX = num(*phase, "Hrzn", 0); b.texturePhaseY = num(*phase, "Vrtc", 0); }
        style->bevels.push_back(b);
        return style->bevels.back();
    });
    return style;
}

const std::vector<uint8_t>* carriedBlock(const Layer& layer, const char* key) {
    if (!layer.psdCarry) return nullptr;
    for (auto& b : layer.psdCarry->blocks) if (b.key == key) return &b.data;
    return nullptr;
}

std::mutex cacheMutex;

} // namespace

bool StyleContour::linear() const {
    if (points.empty()) return true;
    if (points.size() != 2) return false;
    auto at = [](const Point& p, float x, float y) { return std::abs(p.x - x) < 1e-4f && std::abs(p.y - y) < 1e-4f; };
    return at(points.front(), 0, 0) && at(points.back(), 255, 255);
}

std::array<uint8_t, 256> StyleContour::lut() const {
    // Natural cubic through the points, corners splitting it into runs, clamped outside the ends (Patchy's
    // build_style_contour_lut).
    std::array<uint8_t, 256> table{};
    struct Node { double x, y; bool corner; };
    std::vector<Node> nodes;
    for (auto& p : points) nodes.push_back({std::clamp(double(p.x), 0.0, 255.0), std::clamp(double(p.y), 0.0, 255.0), p.corner});
    std::stable_sort(nodes.begin(), nodes.end(), [](auto& a, auto& b) { return a.x < b.x; });
    std::vector<Node> unique;
    for (auto& n : nodes) { if (!unique.empty() && std::abs(unique.back().x - n.x) < 1e-9) unique.back() = n; else unique.push_back(n); }
    if (unique.size() < 2) { for (int i = 0; i < 256; i++) table[size_t(i)] = uint8_t(i); return table; }
    const uint8_t front = uint8_t(std::clamp(std::lround(unique.front().y), 0L, 255L)), back = uint8_t(std::clamp(std::lround(unique.back().y), 0L, 255L));
    for (int i = 0; i < 256; i++) { if (i <= unique.front().x) table[size_t(i)] = front; else if (i >= unique.back().x) table[size_t(i)] = back; }
    auto run = [&](size_t first, size_t last) {
        const size_t count = last - first + 1;
        std::vector<double> d2(count, 0), work(count, 0);
        for (size_t i = 1; i + 1 < count; i++) {
            const Node &p = unique[first + i - 1], &c = unique[first + i], &n = unique[first + i + 1];
            const double ps = c.x - p.x, ns = n.x - c.x, cs = ps + ns, sigma = ps / cs, pivot = sigma * d2[i - 1] + 2;
            d2[i] = (sigma - 1) / pivot;
            work[i] = (6 * ((n.y - c.y) / ns - (c.y - p.y) / ps) / cs - sigma * work[i - 1]) / pivot;
        }
        for (size_t u = count - 1; u > 0; u--) d2[u - 1] = d2[u - 1] * d2[u] + work[u - 1];
        const int begin = int(std::ceil(unique[first].x - 1e-9)), end = int(std::floor(unique[last].x + 1e-9));
        size_t upper = 1;
        for (int input = std::max(0, begin); input <= std::min(255, end); input++) {
            while (upper + 1 < count && input > unique[first + upper].x) upper++;
            const Node &l = unique[first + upper - 1], &r = unique[first + upper];
            const double span = r.x - l.x;
            double out = l.y;
            if (span > 1e-9) {
                const double a = (r.x - input) / span, b = (input - l.x) / span;
                out = a * l.y + b * r.y + ((a * a * a - a) * d2[upper - 1] + (b * b * b - b) * d2[upper]) * span * span / 6;
            }
            table[size_t(input)] = uint8_t(std::clamp(std::lround(out), 0L, 255L));
        }
    };
    size_t start = 0;
    for (size_t i = 1; i < unique.size(); i++) if (unique[i].corner || i + 1 == unique.size()) { run(start, i); start = i; }
    return table;
}

bool LayerStyle::empty() const {
    return !visible || (dropShadows.empty() && innerShadows.empty() && outerGlows.empty() && innerGlows.empty() && colorOverlays.empty()
        && gradientOverlays.empty() && patternOverlays.empty() && satins.empty() && strokes.empty() && bevels.empty());
}

double LayerStyle::reach() const {
    double r = 0;
    for (auto& s : dropShadows) r = std::max(r, double(s.distance + s.size) + 2);
    for (auto& g : outerGlows) r = std::max(r, double(g.size) + 2);
    for (auto& s : strokes) if (s.position != Stroke::Position::Inside) r = std::max(r, double(s.size) + 2);
    for (auto& b : bevels) r = std::max(r, double(b.size + b.soften) + 3);
    // Interior effects blur the matte's outside too: they need room past the shape as well.
    for (auto& s : innerShadows) r = std::max(r, double(s.distance + s.size) + 2);
    for (auto& g : innerGlows) r = std::max(r, double(g.size) + 2);
    for (auto& s : satins) r = std::max(r, double(s.distance + s.size) + 2);
    return std::ceil(r);
}

std::optional<StyleGradient> parseFillGradient(const std::vector<uint8_t>& block) {
    try {
        psd::BigEndianReader r(block);
        if (r.read_u32() != 16) return std::nullopt;
        const psd::DescriptorObject d = psd::read_descriptor(r);
        if (!psd::descriptor_object(d, "Grad")) return std::nullopt;
        StyleGradient g = gradientOf(d);
        g.fillLayer = true;
        return g;
    } catch (std::exception&) { return std::nullopt; }
}

std::optional<FillPattern> parseFillPattern(const std::vector<uint8_t>& block) {
    try {
        psd::BigEndianReader r(block);
        if (r.read_u32() != 16) return std::nullopt;
        const psd::DescriptorObject d = psd::read_descriptor(r);
        auto ptrn = psd::descriptor_object(d, "Ptrn");
        if (!ptrn) return std::nullopt;
        FillPattern p;
        p.id = stringOf(*ptrn, "Idnt");
        p.scale = std::max(0.01f, num(d, "Scl ", 100) / 100.0f);
        p.angle = num(d, "Angl", 0);
        p.linked = flag(d, "Algn", true);
        if (auto phase = psd::descriptor_object(d, "phase")) { p.phaseX = num(*phase, "Hrzn", 0); p.phaseY = num(*phase, "Vrtc", 0); }
        return p;
    } catch (std::exception&) { return std::nullopt; }
}

/// The effects block `block` of `layer`, with the global light and the layer's style options resolved.
static std::shared_ptr<LayerStyle> readStyle(const Layer& layer, const std::vector<uint8_t>& block, float angle, float altitude, bool all) {
    std::shared_ptr<LayerStyle> style;
    try { style = parseEffects(block, all); } catch (std::exception&) { return nullptr; }
    if (!style) return nullptr;
    for (auto& s : style->dropShadows) if (s.useGlobalLight) s.angle = angle;
    for (auto& s : style->innerShadows) if (s.useGlobalLight) s.angle = angle;
    for (auto& b : style->bevels) if (b.useGlobalLight) { b.angle = angle; b.altitude = altitude; }
    if (auto lmgm = carriedBlock(layer, "lmgm"); lmgm && !lmgm->empty()) style->maskHidesEffects = (*lmgm)[0] != 0;
    if (auto infx = carriedBlock(layer, "infx"); infx && !infx->empty()) style->blendInteriorAsGroup = (*infx)[0] != 0;
    if (auto fxrp = carriedBlock(layer, "fxrp"); fxrp && fxrp->size() >= 16) {
        psd::BigEndianReader r(*fxrp);
        style->referenceX = psd::read_f64(r); style->referenceY = psd::read_f64(r);
    }
    return style;
}

static const std::vector<uint8_t>* effectsBlock(const Layer& layer) {
    // 'lmfx' (multiple instances) wins over 'lfx2' ('lfxs' on folders); 'lrFX' is Photoshop 5's mirror and is ignored while either exists.
    const std::vector<uint8_t>* block = carriedBlock(layer, "lmfx");
    if (!block) block = carriedBlock(layer, "lfx2");
    if (!block) block = carriedBlock(layer, "lfxs");   // a folder's
    return block;
}

void layerOpacities(const Layer& layer, float& master, float& fill) {
    master = float(std::clamp(layer.opacity, 0.0, 1.0));
    fill = 1;
    if (layer.psdCarry) {
        const auto& c = *layer.psdCarry;
        if (std::abs(layer.opacity - c.opacity / 255.0 * (c.fill / 255.0)) < 0.5 / 255) { master = c.opacity / 255.0f; fill = c.fill / 255.0f; }
    }
}

std::shared_ptr<const LayerStyle> layerStyleOf(const Layer& layer, const Document& document) {
    const std::vector<uint8_t>* block = effectsBlock(layer);
    if (!block) return nullptr;
    static std::map<const void*, std::pair<std::weak_ptr<const PsdLayerCarry>, std::shared_ptr<const LayerStyle>>> cache;
    float angle = 120, altitude = 30;
    documentGlobalLight(document, angle, altitude);
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto it = cache.find(block);
    if (it != cache.end() && !it->second.first.expired() && it->second.first.lock() == layer.psdCarry) {
        const auto& s = it->second.second;
        bool sameLight = true;   // the global light can change when the document's resources do; cheap to check
        for (auto& d : s ? s->dropShadows : std::vector<DropShadow>{}) if (d.useGlobalLight && d.angle != angle) sameLight = false;
        if (sameLight) return s;
    }
    std::shared_ptr<LayerStyle> style = readStyle(layer, *block, angle, altitude, false);
    if (style && style->empty()) style = nullptr;
    if (cache.size() > 4096) cache.clear();
    cache[block] = {layer.psdCarry, style};
    return style;
}

LayerStyle editableLayerStyle(const Layer& layer, const Document& document) {
    const std::vector<uint8_t>* block = effectsBlock(layer);
    if (!block) return {};
    float angle = 120, altitude = 30;
    documentGlobalLight(document, angle, altitude);
    auto style = readStyle(layer, *block, angle, altitude, true);
    return style ? *style : LayerStyle{};
}

void documentGlobalLight(const Document& document, float& angle, float& altitude) {
    angle = 120; altitude = 30;
    if (!document.psdCarry) return;
    for (auto& r : document.psdCarry->resources) {
        if (r.data.size() < 4) continue;
        const int32_t v = int32_t(uint32_t(r.data[0]) << 24 | uint32_t(r.data[1]) << 16 | uint32_t(r.data[2]) << 8 | r.data[3]);
        if (r.id == 1037) angle = float(v);
        if (r.id == 1049) altitude = float(v);
    }
}

// ---- Patterns ----------------------------------------------------------------------------------------------

namespace {

void unpack(const uint8_t* in, size_t n, uint8_t* out, size_t size) {
    size_t i = 0, o = 0;
    while (i < n && o < size) {
        const int8_t h = int8_t(in[i++]);
        if (h >= 0) { for (int k = 0; k <= h && i < n && o < size; k++) out[o++] = in[i++]; }
        else if (h != -128) { if (i >= n) break; const uint8_t v = in[i++]; for (int k = 0; k < 1 - h && o < size; k++) out[o++] = v; }
    }
}

/// One 'Patt' block's patterns (layout in Patchy's docs/ps-compat.md).
void readPatterns(const std::vector<uint8_t>& block, std::map<std::string, PatternTile>& out) {
    psd::BigEndianReader r(block);
    while (r.remaining() >= 4) {
        const uint32_t length = r.read_u32();
        if (length == 0 || length > r.remaining()) break;
        const size_t start = r.position(), end = start + length;
        try {
            if (r.read_u32() != 1) { r.skip(end - r.position()); goto next; }
            {
                const uint32_t mode = r.read_u32();
                const int h = r.read_u16(), w = r.read_u16();
                const uint32_t nameLength = r.read_u32();
                r.skip(size_t(nameLength) * 2);
                const size_t idLength = r.read_u8();
                auto idBytes = r.read_span(idLength);
                const std::string id(idBytes.begin(), idBytes.end());
                std::vector<uint8_t> palette;
                if (mode == 2) { auto p = r.read_span(768); palette.assign(p.begin(), p.end()); }
                if (r.read_u32() != 3) { r.skip(end - r.position()); goto next; }
                (void)r.read_u32();   // VMA list length
                const int top = int(r.read_u32()), left = int(r.read_u32()), bottom = int(r.read_u32()), right = int(r.read_u32());
                const int maxChannels = int(r.read_u32());
                const int pw = right - left, ph = bottom - top;
                if (pw <= 0 || ph <= 0 || pw > 4096 || ph > 4096 || w <= 0 || h <= 0) { r.skip(end - r.position()); goto next; }
                std::vector<std::vector<uint8_t>> planes;
                std::vector<uint8_t> alpha;
                for (int slot = 0; slot < maxChannels + 2 && r.position() + 4 <= end; slot++) {
                    if (r.read_u32() == 0) continue;
                    const uint32_t planeLength = r.read_u32();
                    const size_t planeEnd = r.position() + planeLength;
                    const uint32_t depth = r.read_u32();
                    const int t = int(r.read_u32()), l = int(r.read_u32()), b = int(r.read_u32()), rr = int(r.read_u32());
                    (void)r.read_u16();
                    const int compression = r.read_u8();
                    const int cw = rr - l, ch = b - t;
                    std::vector<uint8_t> plane(size_t(pw) * ph, 255);
                    if (depth == 8 && cw == pw && ch == ph) {
                        if (compression == 0) { auto d = r.read_span(size_t(cw) * ch); std::copy(d.begin(), d.end(), plane.begin()); }
                        else if (compression == 1) {
                            std::vector<size_t> counts(static_cast<size_t>(ch));
                            for (auto& c : counts) c = r.read_u16();
                            for (int y = 0; y < ch; y++) { auto row = r.read_span(counts[size_t(y)]); unpack(row.data(), row.size(), plane.data() + size_t(y) * cw, size_t(cw)); }
                        }
                    }
                    r.skip(planeEnd > r.position() ? planeEnd - r.position() : 0);
                    if (slot == maxChannels + 1) alpha = std::move(plane); else planes.push_back(std::move(plane));
                }
                PatternTile tile;
                tile.width = pw; tile.height = ph;
                tile.rgba.resize(size_t(pw) * ph * 4);
                for (size_t i = 0; i < size_t(pw) * ph; i++) {
                    uint8_t c[3];
                    if (mode == 2 && !planes.empty() && palette.size() == 768) { const uint8_t k = planes[0][i]; c[0] = palette[k]; c[1] = palette[256 + k]; c[2] = palette[512 + k]; }
                    else if (mode == 4 && planes.size() >= 4) { const int k = planes[3][i]; for (int j = 0; j < 3; j++) c[j] = uint8_t(planes[size_t(j)][i] * k / 255); }
                    else if (planes.size() >= 3) for (int j = 0; j < 3; j++) c[j] = planes[size_t(j)][i];
                    else { const uint8_t g = planes.empty() ? 0 : planes[0][i]; c[0] = c[1] = c[2] = g; }
                    for (int j = 0; j < 3; j++) tile.rgba[i * 4 + size_t(j)] = c[j];
                    tile.rgba[i * 4 + 3] = alpha.empty() ? 255 : alpha[i];
                }
                out[id] = std::move(tile);
            }
        } catch (std::exception&) { return; }
    next:
        r.skip(std::min(r.remaining(), end > r.position() ? end - r.position() : 0));
        while (r.position() % 4 && r.remaining()) r.skip(1);
    }
}

} // namespace

std::map<std::string, PatternTile> parsePatternBlock(const std::vector<uint8_t>& payload) {
    std::map<std::string, PatternTile> out;
    readPatterns(payload, out);
    return out;
}

std::optional<LayerStyle> parseLayerStyleBlock(const std::vector<uint8_t>& block) {
    try {
        if (auto style = parseEffects(block, true)) return *style;
    } catch (std::exception&) {}
    return std::nullopt;
}

std::shared_ptr<const std::map<std::string, PatternTile>> documentPatterns(const Document& document) {
    static std::map<const void*, std::pair<std::weak_ptr<const PsdDocumentCarry>, std::shared_ptr<const std::map<std::string, PatternTile>>>> cache;
    if (!document.psdCarry) return nullptr;
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto it = cache.find(document.psdCarry.get());
    if (it != cache.end() && it->second.first.lock() == document.psdCarry) return it->second.second;
    auto patterns = std::make_shared<std::map<std::string, PatternTile>>();
    for (auto& g : document.psdCarry->globals) if (g.key == "Patt" || g.key == "Pat2" || g.key == "Pat3") readPatterns(g.data, *patterns);
    if (cache.size() > 64) cache.clear();
    cache[document.psdCarry.get()] = {document.psdCarry, patterns};
    return patterns;
}

// ---- Blending and gradients ----------------------------------------------------------------------------------

void effectBlend(EffectBlend mode, const float b[3], const float s[3], float out[3]) {
    auto lum = [](const float c[3]) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; };
    auto nonSeparable = [&](BlendMode m) { Rgb r = blendColor(m, {b[0], b[1], b[2]}, {s[0], s[1], s[2]}); out[0] = r.r; out[1] = r.g; out[2] = r.b; };
    switch (mode) {
    case EffectBlend::Hue: nonSeparable(BlendMode::Hue); return;
    case EffectBlend::Saturation: nonSeparable(BlendMode::Saturation); return;
    case EffectBlend::Color: nonSeparable(BlendMode::Color); return;
    case EffectBlend::Luminosity: nonSeparable(BlendMode::Luminosity); return;
    case EffectBlend::DarkerColor: for (int k = 0; k < 3; k++) out[k] = lum(s) < lum(b) ? s[k] : b[k]; return;
    case EffectBlend::LighterColor: for (int k = 0; k < 3; k++) out[k] = lum(s) > lum(b) ? s[k] : b[k]; return;
    default: break;
    }
    for (int k = 0; k < 3; k++) {
        const float cb = b[k], cs = s[k];
        float r = cs;
        switch (mode) {
        case EffectBlend::Darken: r = std::min(cb, cs); break;
        case EffectBlend::Multiply: r = cb * cs; break;
        case EffectBlend::ColorBurn: r = cb >= 1 ? 1 : cs <= 0 ? 0 : 1 - std::min(1.0f, (1 - cb) / cs); break;
        case EffectBlend::LinearBurn: r = std::max(0.0f, cb + cs - 1); break;
        case EffectBlend::Lighten: r = std::max(cb, cs); break;
        case EffectBlend::Screen: r = cb + cs - cb * cs; break;
        case EffectBlend::ColorDodge: r = cb <= 0 ? 0 : cs >= 1 ? 1 : std::min(1.0f, cb / (1 - cs)); break;
        case EffectBlend::LinearDodge: r = std::min(1.0f, cb + cs); break;
        case EffectBlend::Overlay: r = cb <= 0.5f ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs); break;
        case EffectBlend::HardLight: r = cs <= 0.5f ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs); break;
        case EffectBlend::SoftLight: {
            const float d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : std::sqrt(cb);
            r = cs <= 0.5f ? cb - (1 - 2 * cs) * cb * (1 - cb) : cb + (2 * cs - 1) * (d - cb);
            break;
        }
        case EffectBlend::VividLight:
            r = cs <= 0.5f ? (cs <= 0 ? 0 : 1 - std::min(1.0f, (1 - cb) / (2 * cs))) : (cs >= 1 ? 1 : std::min(1.0f, cb / (2 * (1 - cs))));
            break;
        case EffectBlend::LinearLight: r = std::clamp(cb + 2 * cs - 1, 0.0f, 1.0f); break;
        case EffectBlend::PinLight: r = cs <= 0.5f ? std::min(cb, 2 * cs) : std::max(cb, 2 * cs - 1); break;
        case EffectBlend::HardMix: r = cb + cs >= 1 ? 1 : 0; break;
        case EffectBlend::Difference: r = std::abs(cb - cs); break;
        case EffectBlend::Exclusion: r = cb + cs - 2 * cb * cs; break;
        case EffectBlend::Subtract: r = std::max(0.0f, cb - cs); break;
        case EffectBlend::Divide: r = cs <= 0 ? (cb > 0 ? 1 : 0) : std::min(1.0f, cb / cs); break;
        default: break;
        }
        out[k] = r;
    }
}

namespace {
float midpointRemap(float t, float m) {
    t = unit(t);
    if (m == 0.5f) return t;
    m = std::clamp(m, 0.0001f, 0.9999f);
    return t <= m ? 0.5f * t / m : 0.5f + 0.5f * (t - m) / (1 - m);
}
double catmullRom(double p0, double p1, double p2, double p3, double t) {
    const double t2 = t * t, t3 = t2 * t;
    return 0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
}
double toLinear(double v) { v = std::clamp(v, 0.0, 1.0); return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double toSrgb(double v) { v = std::clamp(v, 0.0, 1.0); return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055; }
} // namespace

float gradientPosition(const StyleGradient& g, double bx, double by, double bw, double bh, double x, double y) {
    // Photoshop's overlay geometry (Patchy's gradient_position, LayerProjection basis): the centre snaps to a
    // pixel, Linear and Reflected span the bounds' projection on the axis, the half-ramp is whole pixels.
    double cx = bx + bw * (0.5 + g.offsetX / 100.0), cy = by + bh * (0.5 + g.offsetY / 100.0);
    if (!g.fillLayer) { cx = std::floor(cx) + 0.5; cy = std::floor(cy) + 0.5; }
    const double px = x + 0.5, py = y + 0.5, a = g.angle * M_PI / 180;
    const double lx = (px - cx) * std::cos(a) - (py - cy) * std::sin(a), ly = (px - cx) * std::sin(a) + (py - cy) * std::cos(a);
    const double ac = std::abs(std::cos(a)), as = std::abs(std::sin(a));
    // Overlays span the bounds' projection on the axis; fill layers the chord through the centre.
    const double span = g.fillLayer ? std::max(1.0, std::min(ac > 1e-6 ? bw / ac : 1e300, as > 1e-6 ? bh / as : 1e300))
                                    : std::max(1.0, ac * bw + as * bh);
    const double raw = span * std::max(0.01f, g.scale) * 0.5;
    const double half = g.fillLayer ? std::max(0.5, raw) : std::max(1.0, std::floor(raw));
    double position = 0;
    switch (g.type) {
    case StyleGradient::Type::Radial: position = std::sqrt(lx * lx + ly * ly) / half; break;
    case StyleGradient::Type::Angle:
        if (lx == 0 && ly == 0) position = 0.25;
        else { position = std::atan2(ly, lx) / (2 * M_PI); if (position < 0) position += 1; }
        break;
    case StyleGradient::Type::Reflected: position = std::abs(lx) / half; break;
    case StyleGradient::Type::Diamond: position = (std::abs(lx) + std::abs(ly)) / half; break;
    default: position = 0.5 + lx / (2 * half); break;
    }
    if (g.reverse) position = 1 - position;
    return unit(float(position));
}

float gradientOpacity(const StyleGradient& g, float t) {
    const auto& s = g.alphas;
    if (s.empty()) return 1;
    if (t <= s.front().location) return s.front().opacity;
    if (t >= s.back().location) return s.back().opacity;
    for (size_t i = 1; i < s.size(); i++) {
        if (t > s[i].location) continue;
        float u = (t - s[i - 1].location) / std::max(0.0001f, s[i].location - s[i - 1].location);
        u = midpointRemap(u, s[i].midpoint);
        if (g.fillLayer && g.smoothness > 0) {
            // Fill layers ease the opacity ramp as they ease the colours.
            const double p = i > 1 ? s[i - 2].opacity : s[i - 1].opacity, n = i + 1 < s.size() ? s[i + 1].opacity : s[i].opacity;
            const double lin = s[i - 1].opacity + (s[i].opacity - s[i - 1].opacity) * u;
            return unit(float(lin + (catmullRom(p, s[i - 1].opacity, s[i].opacity, n, u) - lin) * g.smoothness));
        }
        return s[i - 1].opacity + (s[i].opacity - s[i - 1].opacity) * u;
    }
    return s.back().opacity;
}

StyleColor gradientColor(const StyleGradient& g, float t) {
    const auto& s = g.colors;
    if (s.empty()) { const uint8_t v = uint8_t(std::lround(unit(t) * 255)); return {v, v, v}; }
    if (t <= s.front().location) return s.front().color;
    if (t >= s.back().location) return s.back().color;
    auto byte = [](double v) { return uint8_t(std::clamp(std::lround(v), 0L, 255L)); };
    for (size_t i = 1; i < s.size(); i++) {
        if (t > s[i].location) continue;
        const auto &l = s[i - 1], &r = s[i];
        double u = midpointRemap((t - l.location) / std::max(0.0001f, r.location - l.location), r.midpoint);
        if (g.interpolation == StyleGradient::Interpolation::Linear) {
            auto c = [&](uint8_t a, uint8_t b) { return byte(toSrgb(toLinear(a / 255.0) + (toLinear(b / 255.0) - toLinear(a / 255.0)) * u) * 255); };
            return {c(l.color.r, r.color.r), c(l.color.g, r.color.g), c(l.color.b, r.color.b)};
        }
        // Classic: linear blended toward a Catmull-Rom through the neighbours by the smoothness (when there are
        // more than two stops). Perceptual renders as Classic here.
        const auto& p = i > 1 ? s[i - 2].color : l.color;
        const auto& n = i + 1 < s.size() ? s[i + 1].color : r.color;
        const double smooth = s.size() > 2 || g.fillLayer ? g.smoothness : 0.0;
        auto c = [&](uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3) { const double lin = p1 + (p2 - p1) * u; return byte(lin + (catmullRom(p0, p1, p2, p3, u) - lin) * smooth); };
        return {c(p.r, l.color.r, r.color.r, n.r), c(p.g, l.color.g, r.color.g, n.g), c(p.b, l.color.b, r.color.b, n.b)};
    }
    return s.back().color;
}

} // namespace compositor
