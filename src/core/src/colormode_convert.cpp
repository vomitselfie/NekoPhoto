// Image > Mode > RGB Color / CMYK Color / Lab Color (docs/color-modes.md, docs/high-bit-depth-plan.md "P7 plan"):
// converting a whole document between colour modes, the colours it stores as values, the adjustment layers a mode
// has no counterpart for, and the Layers panel's thumbnails of CMYK and Lab pixels.
#include "compositor/adjustments.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/layerstyle.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

namespace compositor {

namespace {

using json = nlohmann::json;

/// The key an adjustment layer's settings carry while it is dormant in a mode that has no counterpart for it.
constexpr const char* dormantKey = "dormantInMode";

/// The profile a mode's stored colours are in: the document's own for RGB, sRGB for CMYK and Lab (colours such as
/// text, shape and style colours stay RGB values in every mode; the renderer converts them to the document's mode).
ColorProfile storedColourProfile(ColorMode mode, const ColorProfile& profile) { return mode == ColorMode::RGB ? profile : ColorProfile(); }

/// A stored colour's conversion between modes: into the target's RGB space, through the target's gamut on the way
/// when the target is CMYK (so a colour out of the press gamut comes back as the press can print it).
struct StoredColourMap {
    ColorTransformPtr toTarget;     // RGB floats, stored space to stored space
    ColorTransformPtr toCmyk, back; // RGB to the CMYK profile and back to sRGB (CMYK targets)

    StoredColourMap(ColorMode from, const ColorProfile& fromProfile, ColorMode to, const ColorProfile& toProfile, const ConvertOptions& options) {
        const ColorProfile a = storedColourProfile(from, fromProfile), b = storedColourProfile(to, toProfile);
        if (to == ColorMode::CMYK) {
            toCmyk = transformBetween(a, toProfile, options, PixelFormat::RGBFloat, PixelFormat::CMYKFloat);
            back = transformBetween(toProfile, b, options, PixelFormat::CMYKFloat, PixelFormat::RGBFloat);
        } else {
            toTarget = transformBetween(a, b, options, PixelFormat::RGBFloat, PixelFormat::RGBFloat);
        }
    }
    void operator()(double& r, double& g, double& bl) const {
        float v[3] = {float(r), float(g), float(bl)}, out[4] = {};
        if (toCmyk && back) {
            float ink[4] = {};
            toCmyk->apply(v, ink, 1);
            back->apply(ink, out, 1);
        } else if (toTarget) {
            toTarget->apply(v, out, 1);
        } else return;
        r = std::clamp(double(out[0]), 0.0, 1.0); g = std::clamp(double(out[1]), 0.0, 1.0); bl = std::clamp(double(out[2]), 0.0, 1.0);
    }
    void operator()(uint8_t& r, uint8_t& g, uint8_t& b) const {
        double v[3] = {r / 255.0, g / 255.0, b / 255.0};
        (*this)(v[0], v[1], v[2]);
        r = uint8_t(std::lround(v[0] * 255)); g = uint8_t(std::lround(v[1] * 255)); b = uint8_t(std::lround(v[2] * 255));
    }
    void operator()(StyleColor& c) const { (*this)(c.r, c.g, c.b); }
    void operator()(AdjustmentColor& c) const { (*this)(c.red, c.green, c.blue); }
    void operator()(StyleGradient& gradient) const { for (auto& stop : gradient.colors) (*this)(stop.color); }
};

bool convertStyleColours(LayerStyle& s, const StoredColourMap& convert) {
    if (!hasAnyEffect(s)) return false;
    for (auto& e : s.dropShadows) convert(e.color);
    for (auto& e : s.innerShadows) convert(e.color);
    for (auto& e : s.outerGlows) convert(e.color);
    for (auto& e : s.innerGlows) convert(e.color);
    for (auto& e : s.colorOverlays) convert(e.color);
    for (auto& e : s.gradientOverlays) convert(e.gradient);
    for (auto& e : s.satins) convert(e.color);
    for (auto& e : s.strokes) { convert(e.color); convert(e.gradient); }
    for (auto& e : s.bevels) { convert(e.highlight); convert(e.shadow); }
    return true;
}

/// The profile a converted document carries: the target given, else the mode's default (the bundled Working CMYK
/// for CMYK; Lab and RGB stay untagged, which is Lab D50 and sRGB).
ColorProfile targetProfile(ColorMode to, const ColorProfile& target) {
    if (!target.icc.empty()) return target;
    if (to == ColorMode::CMYK) return defaultCmykProfile();
    return ColorProfile();
}

} // namespace

bool adjustmentOfferedInMode(AdjustmentKind kind, ColorMode mode) {
    if (mode == ColorMode::RGB) return true;
    switch (kind) {
    // Photoshop's Image > Adjustments in CMYK and Lab (docs/color-modes.md lists the sources and what is unverified).
    case AdjustmentKind::Levels: case AdjustmentKind::Curves: case AdjustmentKind::BrightnessContrast:
    case AdjustmentKind::Invert: case AdjustmentKind::Posterize: case AdjustmentKind::Threshold:
    case AdjustmentKind::GradientMap: case AdjustmentKind::PhotoFilter: case AdjustmentKind::ColorLookup:
        return true;
    // Channel Mixer mixes RGB or CMYK channels; Photoshop greys it in Lab.
    case AdjustmentKind::HueSaturation: case AdjustmentKind::ColorBalance: case AdjustmentKind::SelectiveColor:
    case AdjustmentKind::ChannelMixer:
        return mode == ColorMode::CMYK;
    case AdjustmentKind::Exposure:
        return mode == ColorMode::Lab;
    case AdjustmentKind::Vibrance: case AdjustmentKind::BlackWhite: case AdjustmentKind::Grain:
        return false;
    }
    return false;
}

bool isDormantAdjustment(const Layer& layer) {
    if (!layer.adjustment) return false;
    const json j = json::parse(layer.adjustment->json, nullptr, false);
    return j.is_object() && j.contains(dormantKey);
}

void convertModeColor(ColorMode from, const ColorProfile& fromProfile, ColorMode to, const ColorProfile& toProfile, const ConvertOptions& options,
                      double rgb[3]) {
    const StoredColourMap map(from, fromProfile, to, targetProfile(to, toProfile), options);
    map(rgb[0], rgb[1], rgb[2]);
}

ImagePtr modeThumbnail(const AnyImage& image, ColorMode mode, const ColorProfile& profile, int maxSide) {
    if (!image) return std::make_shared<Image>(1, 1);
    const int w0 = image.width(), h0 = image.height();
    const double factor = std::min(1.0, double(maxSide) / std::max(1, std::max(w0, h0)));
    const int w = std::max(1, int(w0 * factor)), h = std::max(1, int(h0 * factor));
    AnyImage reduced;
    if (auto c8 = image.c8()) reduced = (w == w0 && h == h0) ? AnyImage(c8) : AnyImage(ImageC8Ptr(boxResizeImage(*c8, w, h)));
    else if (auto p16 = image.u16()) reduced = (w == w0 && h == h0) ? AnyImage(p16) : AnyImage(Image16Ptr(boxResizeImage(*p16, w, h)));
    else if (auto p8 = image.u8()) reduced = ImagePtr(makeThumbnail(*p8, maxSide));
    else return std::make_shared<Image>(1, 1);
    auto out = std::make_shared<Image>(reduced.width(), reduced.height());
    if (mode == ColorMode::RGB) {
        if (reduced.u8()) *out = *reduced.u8();
        else if (reduced.u16()) out = narrowImage(*reduced.u16());
        return out;
    }
    const ColorTransformPtr t = transformBetween(profile, srgbProfile(), ConvertOptions(), pixelFormatFor(reduced.sampleType(), mode), PixelFormat::RGBA8);
    if (t) convertImageTo8(reduced, *out, *t);
    return out;
}

void refreshModeThumbnails(Document& document) {
    if (document.colorMode == ColorMode::RGB) return;
    for (Layer& layer : document.layers)
        if (layer.asset && layer.asset->image) layer.asset->thumbnail = modeThumbnail(layer.asset->image, document.colorMode, document.profile);
}

bool convertDocumentMode(Document& document, ColorMode to, const ColorProfile& target, const ConvertOptions& options, std::string* why) {
    const ColorMode from = document.colorMode;
    if (from == to) return true;
    if (!colorModeSupportsDepth(to, document.sampleType)) {
        if (why) *why = std::string(colorModeName(to)) + " documents are 8 or 16 bits per channel.";
        return false;
    }
    if (const std::string problem = formatBudgetProblem(document, document.sampleType, to); !problem.empty()) {
        if (why) *why = problem;
        return false;
    }
    const ColorProfile fromProfile = document.profile;
    const ColorProfile toProfile = targetProfile(to, target);
    if (effectiveProfile(toProfile, colorModelOf(to)).model != colorModelOf(to)) {
        if (why) *why = std::string("The profile is not a ") + colorModeName(to) + " profile.";
        return false;
    }
    Document out = document;
    const StoredColourMap colours(from, fromProfile, to, toProfile, options);
    for (Layer& layer : out.layers) {
        // Pixels: every raster (the placed pixels of smart objects, shapes and text too; a smart object keeps its source).
        if (layer.asset && layer.asset->image) {
            const AnyImage before = layer.asset->image;
            const bool liveShape = layer.isLiveShape(), liveText = layer.isLiveText(), liveSmart = layer.isLiveSmartObject();
            const AnyImage after = convertImage(before, from, fromProfile, to, toProfile, options);
            if (!after) {
                if (why) *why = "The colours could not be converted (a profile could not be used).";
                return false;
            }
            layer.asset = Asset::makeAny(after, layer.asset->name);
            if (liveShape) layer.shapeImage = after;
            if (liveText) layer.textImage = after;
            if (liveSmart) layer.smartImage = after;
        }
        // Stored colours.
        if (layer.shape) colours(layer.shape->red, layer.shape->green, layer.shape->blue);
        if (layer.text) {
            colours(layer.text->red, layer.text->green, layer.text->blue);
            for (TextRun& run : layer.text->runs) colours(run.red, run.green, run.blue);
        }
        if (layer.artboard && layer.artboard->background == Artboard::Other) colours(layer.artboard->red, layer.artboard->green, layer.artboard->blue);
        if (layerStyleOf(layer, out) || layer.psdCarry) {
            LayerStyle style = editableLayerStyle(layer, out);
            if (convertStyleColours(style, colours)) setLayerStyle(layer, style);
        }
        // Adjustment layers: colours converted, per-channel curves and levels reset (the channels mean something else
        // now), and a kind the new mode does not offer kept but made dormant: hidden and marked, so converting back
        // wakes it as it was.
        if (layer.adjustment) {
            AdjustmentSettings settings;
            if (AdjustmentSettings::parse(layer.adjustment->json, settings)) {
                if (settings.kind == AdjustmentKind::PhotoFilter) colours(settings.photoFilter.color);
                if (settings.kind == AdjustmentKind::GradientMap) { colours(settings.gradientMap.shadows); colours(settings.gradientMap.highlights); }
                for (size_t c = 1; c < settings.levels.ranges.size(); c++) settings.levels.ranges[c] = LevelsRange();
                for (size_t c = 1; c < settings.curves.channels.size(); c++) settings.curves.channels[c] = {{0, 0}, {255, 255}};
                settings.levels.channel = 0;
                settings.curves.channel = 0;
                json j = json::parse(settings.toJson(), nullptr, false);
                if (!j.is_object()) j = json::object();
                const json old = json::parse(layer.adjustment->json, nullptr, false);
                const bool wasDormant = old.is_object() && old.contains(dormantKey);
                const bool offered = adjustmentOfferedInMode(settings.kind, to);
                if (!offered) {
                    j[dormantKey] = wasDormant ? old[dormantKey] : json{{"visible", layer.visible}};
                    layer.visible = false;
                } else if (wasDormant) {
                    j.erase(dormantKey);
                    const json& mark = old[dormantKey];
                    layer.visible = mark.is_object() && mark.contains("visible") && mark["visible"].is_boolean() ? mark["visible"].get<bool>() : true;
                }
                layer.adjustment = LayerAdjustment{settings.kind, j.dump()};
            }
        }
    }
    out.colorMode = to;
    out.profile = to == ColorMode::RGB ? toProfile : (to == ColorMode::CMYK ? toProfile : ColorProfile());
    conformToFormat(out);
    refreshModeThumbnails(out);
    document = std::move(out);
    return true;
}

} // namespace compositor
