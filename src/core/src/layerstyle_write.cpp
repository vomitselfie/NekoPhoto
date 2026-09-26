// Layer styles written back as Photoshop's 'lfx2' descriptor (layerstyle.h), for the Layer Style dialog. The shapes,
// key order and id forms are Patchy's authoring (MIT, src/third_party/patchy_psd/README.md), which pinned each against
// what Photoshop 2026 writes: Photoshop resets some effects whose descriptor is not laid out as its own.
#include "compositor/document.h"
#include "compositor/layerstyle.h"
#include "psd/psd_descriptor.hpp"
#include <algorithm>
#include <cmath>
#include <memory>

namespace psd = patchy::psd;

namespace compositor {

namespace {

using psd::DescriptorObject;
using psd::DescriptorValue;

/// A descriptor object built in order: each key is written in the order it was added.
struct Desc {
    DescriptorObject o;
    explicit Desc(const char* classId, const char* name = "") { o.class_id = classId; o.name = name; }
    Desc& put(const std::string& key, DescriptorValue v) {
        o.key_order.push_back({key, key.size() != 4});
        o.values[key] = std::move(v);
        return *this;
    }
    Desc& boolean(const std::string& key, bool b) { DescriptorValue v; v.type = DescriptorValue::Type::Bool; v.bool_value = b; return put(key, v); }
    Desc& integer(const std::string& key, int32_t i) { DescriptorValue v; v.type = DescriptorValue::Type::Integer; v.integer_value = i; return put(key, v); }
    Desc& number(const std::string& key, double d) { DescriptorValue v; v.type = DescriptorValue::Type::Double; v.double_value = d; return put(key, v); }
    Desc& unitFloat(const std::string& key, const char* unit, double d) {
        DescriptorValue v; v.type = DescriptorValue::Type::UnitFloat; v.unit = unit; v.double_value = d; return put(key, v);
    }
    Desc& percent(const std::string& key, double fraction) { return unitFloat(key, "#Prc", fraction * 100.0); }
    Desc& pixels(const std::string& key, double px) { return unitFloat(key, "#Pxl", px); }
    Desc& angle(const std::string& key, double degrees) { return unitFloat(key, "#Ang", degrees); }
    Desc& text(const std::string& key, const std::string& s) { DescriptorValue v; v.type = DescriptorValue::Type::String; v.string_value = s; return put(key, v); }
    Desc& enumeration(const std::string& key, const std::string& type, const std::string& value) {
        DescriptorValue v; v.type = DescriptorValue::Type::Enum; v.enum_type = type; v.enum_value = value;
        v.enum_type_long_form = type.size() != 4; v.enum_value_long_form = value.size() != 4;
        return put(key, v);
    }
    Desc& object(const std::string& key, Desc d) { return put(key, asValue(std::move(d))); }
    Desc& list(const std::string& key, std::vector<Desc> items) {
        DescriptorValue v; v.type = DescriptorValue::Type::List;
        for (Desc& d : items) v.list_value.push_back(asValue(std::move(d)));
        return put(key, v);
    }
    static DescriptorValue asValue(Desc d) {
        DescriptorValue v; v.type = DescriptorValue::Type::Object;
        d.o.class_id_long_form = d.o.class_id.size() != 4;
        v.object_value = std::make_shared<DescriptorObject>(std::move(d.o));
        return v;
    }
};

const char* blendValue(EffectBlend mode) {
    switch (mode) {
    case EffectBlend::Normal: return "normal";
    case EffectBlend::Dissolve: return "dissolve";
    case EffectBlend::Darken: return "darken";
    case EffectBlend::Multiply: return "multiply";
    case EffectBlend::ColorBurn: return "colorBurn";
    case EffectBlend::LinearBurn: return "linearBurn";
    case EffectBlend::DarkerColor: return "darkerColor";
    case EffectBlend::Lighten: return "lighten";
    case EffectBlend::Screen: return "screen";
    case EffectBlend::ColorDodge: return "colorDodge";
    case EffectBlend::LinearDodge: return "linearDodge";
    case EffectBlend::LighterColor: return "lighterColor";
    case EffectBlend::Overlay: return "overlay";
    case EffectBlend::SoftLight: return "softLight";
    case EffectBlend::HardLight: return "hardLight";
    case EffectBlend::VividLight: return "vividLight";
    case EffectBlend::LinearLight: return "linearLight";
    case EffectBlend::PinLight: return "pinLight";
    case EffectBlend::HardMix: return "hardMix";
    case EffectBlend::Difference: return "difference";
    case EffectBlend::Exclusion: return "exclusion";
    case EffectBlend::Subtract: return "blendSubtraction";   // Photoshop's stringIDs for these two
    case EffectBlend::Divide: return "blendDivide";
    case EffectBlend::Hue: return "hue";
    case EffectBlend::Saturation: return "saturation";
    case EffectBlend::Color: return "color";
    case EffectBlend::Luminosity: return "luminosity";
    }
    return "normal";
}

Desc& blend(Desc& d, const char* key, EffectBlend mode) { return d.enumeration(key, "BlnM", blendValue(mode)); }

/// A colour object: channels 0..255 as plain doubles, the form Photoshop writes.
Desc rgb(StyleColor c) { Desc d("RGBC"); d.number("Rd  ", c.r).number("Grn ", c.g).number("Bl  ", c.b); return d; }

int32_t location(float f) { return int32_t(std::lround(std::clamp(f, 0.0f, 1.0f) * 4096.0f)); }
int32_t midpoint(float f) { return int32_t(std::lround(std::clamp(f, 0.0f, 1.0f) * 100.0f)); }

/// The gradient object ('Grdn'). A stroke's puts each stop's colour first and names the object "Gradient" (Photoshop's
/// FrFX shape); an overlay's does neither.
Desc gradient(const StyleGradient& g, bool strokeShape) {
    auto colors = g.colors;
    auto alphas = g.alphas;
    if (colors.empty()) colors = {{0, {0, 0, 0}, 0.5f}, {1, {255, 255, 255}, 0.5f}};
    if (alphas.empty()) alphas = {{0, 1, 0.5f}, {1, 1, 0.5f}};
    std::stable_sort(colors.begin(), colors.end(), [](auto& a, auto& b) { return a.location < b.location; });
    std::stable_sort(alphas.begin(), alphas.end(), [](auto& a, auto& b) { return a.location < b.location; });
    Desc d("Grdn", strokeShape ? "Gradient" : "");
    d.text("Nm  ", "Custom").enumeration("GrdF", "GrdF", "CstS").number("Intr", double(g.smoothness) * 4096.0);
    std::vector<Desc> stops;
    for (auto& s : colors) {
        Desc stop("Clrt");
        if (strokeShape) stop.object("Clr ", rgb(s.color));
        stop.enumeration("Type", "Clry", "UsrS").integer("Lctn", location(s.location)).integer("Mdpn", midpoint(s.midpoint));
        if (!strokeShape) stop.object("Clr ", rgb(s.color));
        stops.push_back(std::move(stop));
    }
    d.list("Clrs", std::move(stops));
    std::vector<Desc> transparency;
    for (auto& s : alphas) {
        Desc stop("TrnS");
        stop.percent("Opct", std::clamp(s.opacity, 0.0f, 1.0f)).integer("Lctn", location(s.location)).integer("Mdpn", midpoint(s.midpoint));
        transparency.push_back(std::move(stop));
    }
    d.list("Trns", std::move(transparency));
    return d;
}

const char* gradientType(StyleGradient::Type t) {
    switch (t) {
    case StyleGradient::Type::Radial: return "Rdl ";
    case StyleGradient::Type::Angle: return "Angl";
    case StyleGradient::Type::Reflected: return "Rflc";
    case StyleGradient::Type::Diamond: return "Dmnd";
    case StyleGradient::Type::ShapeBurst: return "shapeburst";   // Photoshop 2026's own spelling
    case StyleGradient::Type::Linear: break;
    }
    return "Lnr ";
}

const char* interpolation(StyleGradient::Interpolation i) {
    return i == StyleGradient::Interpolation::Perceptual ? "perceptual" : i == StyleGradient::Interpolation::Linear ? "linear" : "Gcls";
}

Desc point(double x, double y, bool percent) {
    Desc d("Pnt ");
    if (percent) d.percent("Hrzn", x / 100.0).percent("Vrtc", y / 100.0);
    else d.number("Hrzn", x).number("Vrtc", y);
    return d;
}

bool linear(const StyleContour& c) { return c.points.empty() || c.linear(); }

Desc contour(const StyleContour& c, const char* linearName = "Linear") {
    Desc d("ShpC");
    std::vector<Desc> points;
    if (linear(c)) {
        d.text("Nm  ", linearName);
        for (double v : {0.0, 255.0}) { Desc p("CrPt"); p.number("Hrzn", v).number("Vrtc", v); points.push_back(std::move(p)); }
    } else {
        d.text("Nm  ", "Custom");
        for (auto& pt : c.points) { Desc p("CrPt"); p.number("Hrzn", pt.x).number("Vrtc", pt.y).boolean("Cnty", !pt.corner); points.push_back(std::move(p)); }
    }
    d.list("Crv ", std::move(points));
    return d;
}

Desc pattern(const std::string& id) { Desc d("Ptrn"); d.text("Nm  ", id).text("Idnt", id); return d; }

Desc dropShadow(const DropShadow& s) {
    Desc d("DrSh");
    d.boolean("enab", s.enabled);
    blend(d, "Md  ", s.mode).object("Clr ", rgb(s.color)).percent("Opct", s.opacity);
    d.boolean("uglg", s.useGlobalLight).angle("lagl", s.angle).pixels("Dstn", s.distance).percent("Ckmt", s.spread / 100.0).pixels("blur", s.size);
    d.percent("Nose", 0).boolean("AntA", false).boolean("layerConceals", s.layerConceals);
    return d;
}

Desc innerShadow(const InnerShadow& s) {
    Desc d("IrSh");
    d.boolean("enab", s.enabled);
    blend(d, "Md  ", s.mode).object("Clr ", rgb(s.color)).percent("Opct", s.opacity);
    d.boolean("uglg", s.useGlobalLight).angle("lagl", s.angle).pixels("Dstn", s.distance).percent("Ckmt", s.choke / 100.0).pixels("blur", s.size);
    d.percent("Nose", 0).boolean("AntA", false);
    return d;
}

Desc outerGlow(const OuterGlow& g) {
    Desc d("OrGl");
    d.boolean("enab", g.enabled);
    blend(d, "Md  ", g.mode).object("Clr ", rgb(g.color)).percent("Opct", g.opacity);
    d.enumeration("GlwT", "BETE", g.precise ? "PrBL" : "SfBL").percent("Ckmt", g.spread / 100.0).pixels("blur", g.size);
    d.percent("Nose", 0).percent("ShdN", 0).boolean("AntA", false).percent("Inpr", g.range / 100.0);
    return d;
}

Desc innerGlow(const InnerGlow& g) {
    Desc d("IrGl");
    d.boolean("enab", g.enabled);
    blend(d, "Md  ", g.mode).object("Clr ", rgb(g.color)).percent("Opct", g.opacity);
    d.enumeration("GlwT", "BETE", g.precise ? "PrBL" : "SfBL").percent("Ckmt", g.choke / 100.0).pixels("blur", g.size);
    d.percent("Nose", 0).enumeration("glwS", "IGSr", g.center ? "SrcC" : "SrcE").percent("ShdN", 0).boolean("AntA", false).percent("Inpr", g.range / 100.0);
    return d;
}

Desc colorOverlay(const ColorOverlay& c) {
    Desc d("SoFi");
    d.boolean("enab", c.enabled);
    blend(d, "Md  ", c.mode).object("Clr ", rgb(c.color)).percent("Opct", c.opacity);
    return d;
}

Desc gradientOverlay(const GradientOverlay& g) {
    Desc d("GrFl");
    d.boolean("enab", g.enabled).boolean("present", true).boolean("showInDialog", true);
    blend(d, "Md  ", g.mode).percent("Opct", g.opacity);
    d.object("Grad", gradient(g.gradient, false)).angle("Angl", g.gradient.angle).enumeration("Type", "GrdT", gradientType(g.gradient.type));
    d.boolean("Rvrs", g.gradient.reverse).boolean("Dthr", g.gradient.dither);
    d.enumeration("gs99", "gradientInterpolationMethodType", interpolation(g.gradient.interpolation));
    d.boolean("Algn", g.gradient.alignWithLayer).percent("Scl ", g.gradient.scale).object("Ofst", point(g.gradient.offsetX, g.gradient.offsetY, true));
    return d;
}

Desc patternOverlay(const PatternOverlay& p) {
    Desc d("patternFill");
    d.boolean("enab", p.enabled).boolean("present", true).boolean("showInDialog", true);
    blend(d, "Md  ", p.mode).percent("Opct", p.opacity).object("Ptrn", pattern(p.patternId));
    d.angle("Angl", p.angle).percent("Scl ", p.scale).boolean("Algn", p.linkWithLayer).object("phase", point(p.phaseX, p.phaseY, false));
    return d;
}

Desc satin(const Satin& s) {
    Desc d("ChFX");
    d.boolean("enab", s.enabled).boolean("present", true).boolean("showInDialog", true);
    blend(d, "Md  ", s.mode).object("Clr ", rgb(s.color)).boolean("AntA", false).boolean("Invr", s.invert).percent("Opct", s.opacity);
    d.angle("lagl", s.angle).pixels("Dstn", s.distance).pixels("blur", s.size).object("MpgS", contour({}, "$$$/Contours/Defaults/Linear=Linear"));
    return d;
}

Desc stroke(const Stroke& s) {
    Desc d("FrFX");
    d.boolean("enab", s.enabled).boolean("present", true).boolean("showInDialog", true);
    d.enumeration("Styl", "FStl", s.position == Stroke::Position::Inside ? "InsF" : s.position == Stroke::Position::Center ? "CtrF" : "OutF");
    d.enumeration("PntT", "FrFl", s.gradientFill ? "GrFl" : "SClr");
    blend(d, "Md  ", s.mode).percent("Opct", s.opacity).pixels("Sz  ", s.size);
    if (s.gradientFill) {
        // Photoshop writes a black colour ahead of the gradient even though PntT selects the gradient.
        d.object("Clr ", rgb({})).object("Grad", gradient(s.gradient, true));
        d.enumeration("gradientsInterpolationMethod", "gradientInterpolationMethodType", interpolation(s.gradient.interpolation));
        d.angle("Angl", s.gradient.angle).enumeration("Type", "GrdT", gradientType(s.gradient.type));
        d.boolean("Rvrs", s.gradient.reverse).boolean("Dthr", s.gradient.dither).percent("Scl ", s.gradient.scale);
        d.boolean("Algn", s.gradient.alignWithLayer).object("Ofst", point(s.gradient.offsetX, s.gradient.offsetY, true));
    } else {
        d.object("Clr ", rgb(s.color));
    }
    d.boolean("overprint", s.overprint);
    return d;
}

Desc bevel(const Bevel& b) {
    Desc d("ebbl");
    d.boolean("enab", b.enabled).boolean("present", true).boolean("showInDialog", true);
    blend(d, "hglM", b.highlightMode).object("hglC", rgb(b.highlight)).percent("hglO", b.highlightOpacity);
    blend(d, "sdwM", b.shadowMode).object("sdwC", rgb(b.shadow)).percent("sdwO", b.shadowOpacity);
    d.enumeration("bvlT", "bvlT", b.technique == Bevel::Technique::ChiselHard ? "PrBL" : b.technique == Bevel::Technique::ChiselSoft ? "Slmt" : "SfBL");
    d.enumeration("bvlS", "BESl", b.kind == Bevel::Kind::Outer ? "OtrB" : b.kind == Bevel::Kind::Emboss ? "Embs" : b.kind == Bevel::Kind::Pillow ? "PlEb"
                                  : b.kind == Bevel::Kind::StrokeEmboss ? "strokeEmboss" : "InrB");
    d.boolean("uglg", b.useGlobalLight).angle("lagl", b.angle).angle("Lald", b.altitude).percent("srgR", b.depth).pixels("blur", b.size);
    d.enumeration("bvlD", "BESs", b.up ? "In  " : "Out ").object("TrnS", contour(b.gloss)).boolean("antialiasGloss", b.glossAntialiased);
    d.pixels("Sftn", b.soften).boolean("useShape", b.useContour);
    if (b.useContour) d.object("MpgS", contour(b.contour)).boolean("AntA", b.contourAntialiased).percent("Inpr", b.contourRange);
    d.boolean("useTexture", b.useTexture);
    if (b.useTexture) {
        d.boolean("InvT", b.textureInvert).boolean("Algn", b.textureLinkWithLayer).percent("Scl ", b.textureScale).percent("textureDepth", b.textureDepth);
        d.object("Ptrn", pattern(b.texturePattern)).object("phase", point(b.texturePhaseX, b.texturePhaseY, false));
    }
    return d;
}

template <typename Effect, typename Write>
void effects(Desc& root, const char* single, const char* multi, const std::vector<Effect>& list, Write write) {
    if (list.empty()) return;
    if (list.size() == 1) { root.object(single, write(list.front())); return; }
    std::vector<Desc> items;
    for (const Effect& e : list) items.push_back(write(e));
    root.list(multi, std::move(items));
}

std::vector<uint8_t> flagBlock(bool on) { return {uint8_t(on), 0, 0, 0}; }

} // namespace

bool hasAnyEffect(const LayerStyle& s) {
    return !s.dropShadows.empty() || !s.innerShadows.empty() || !s.outerGlows.empty() || !s.innerGlows.empty() || !s.colorOverlays.empty()
        || !s.gradientOverlays.empty() || !s.patternOverlays.empty() || !s.satins.empty() || !s.strokes.empty() || !s.bevels.empty();
}

std::vector<uint8_t> authorLayerStyleBlock(const LayerStyle& style) {
    Desc root("null");
    root.percent("Scl ", 1).boolean("masterFXSwitch", style.visible);
    effects(root, "DrSh", "dropShadowMulti", style.dropShadows, dropShadow);
    effects(root, "IrSh", "innerShadowMulti", style.innerShadows, innerShadow);
    effects(root, "OrGl", "outerGlowMulti", style.outerGlows, outerGlow);
    effects(root, "IrGl", "innerGlowMulti", style.innerGlows, innerGlow);
    effects(root, "ChFX", "chromeFXMulti", style.satins, satin);
    effects(root, "ebbl", "bevelEmbossMulti", style.bevels, bevel);
    effects(root, "SoFi", "solidFillMulti", style.colorOverlays, colorOverlay);
    effects(root, "GrFl", "gradientFillMulti", style.gradientOverlays, gradientOverlay);
    effects(root, "patternFill", "patternFillMulti", style.patternOverlays, patternOverlay);
    effects(root, "FrFX", "frameFXMulti", style.strokes, stroke);
    psd::BigEndianWriter w;
    w.write_u32(0);    // object effects version
    w.write_u32(16);   // descriptor version
    psd::write_descriptor(w, root.o);
    return w.bytes();
}

void setLayerStyle(Layer& layer, const LayerStyle& style) {
    auto carry = layer.psdCarry ? std::make_shared<PsdLayerCarry>(*layer.psdCarry) : std::make_shared<PsdLayerCarry>();
    auto& blocks = carry->blocks;
    auto isEffects = [](const PsdBlock& b) { return b.key == "lfx2" || b.key == "lmfx" || b.key == "lfxs" || b.key == "lrFX"; };
    // The new block takes the place of the first one it replaces, so the record keeps its order.
    auto at = std::find_if(blocks.begin(), blocks.end(), isEffects);
    const auto index = at - blocks.begin();
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(), isEffects), blocks.end());
    if (hasAnyEffect(style)) {
        const auto pos = blocks.begin() + std::min<std::ptrdiff_t>(index, std::ptrdiff_t(blocks.size()));
        blocks.insert(pos, PsdBlock{"lfx2", authorLayerStyleBlock(style)});
    }
    auto setFlag = [&](const char* key, bool on) {
        auto it = std::find_if(blocks.begin(), blocks.end(), [&](const PsdBlock& b) { return b.key == key; });
        if (it != blocks.end()) it->data = flagBlock(on);
        else if (on) blocks.push_back({key, flagBlock(on)});
    };
    setFlag("lmgm", style.maskHidesEffects);
    setFlag("infx", style.blendInteriorAsGroup);
    layer.psdCarry = std::move(carry);
}

} // namespace compositor
