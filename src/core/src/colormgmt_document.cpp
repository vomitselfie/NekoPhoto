// Convert to Profile over a whole document (colormgmt.h): pixels, and every colour the document stores as a value.
#include "compositor/adjustments.h"
#include "compositor/colormgmt.h"
#include "compositor/document.h"
#include "compositor/layerstyle.h"
#include "compositor/vectorlayer.h"
#include <cmath>

namespace compositor {

namespace {

struct ColourConverter {
    const ColorProfile& from;
    const ColorProfile& to;
    const ConvertOptions& options;
    ColorTransformPtr rgb;   // straight floats

    void operator()(double& r, double& g, double& b) const {
        if (!rgb) return;
        float in[3] = {float(r), float(g), float(b)}, out[3] = {};
        rgb->apply(in, out, 1);
        r = std::clamp(double(out[0]), 0.0, 1.0); g = std::clamp(double(out[1]), 0.0, 1.0); b = std::clamp(double(out[2]), 0.0, 1.0);
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

bool convertStyle(LayerStyle& s, const ColourConverter& convert) {
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

} // namespace

bool convertDocumentProfile(Document& document, const ColorProfile& target, const ConvertOptions& options, std::string* why) {
    const ColorProfile from = document.profile;
    const ColorModel model = colorModelOf(document.colorMode);
    const ColorProfile& a = effectiveProfile(from, model);
    const ColorProfile& b = effectiveProfile(target, model);
    if (a.model != model || b.model != model) {
        if (why) *why = std::string("Only ") + colorModeName(document.colorMode) + " profiles can be converted between here; Image > Mode changes the colour mode.";
        return false;
    }
    if (equivalentProfiles(a, b)) { document.profile = target; return true; }
    const bool deep = document.sampleType == SampleType::U16;
    if (document.sampleType == SampleType::F32) { if (why) *why = "32-bit documents cannot be converted yet."; return false; }
    if (document.colorMode != ColorMode::RGB) {
        // A CMYK or Lab document between two profiles of its mode: every raster layer's pixels. Its stored colours are
        // still RGB values until Image > Mode converts them (P7 step D), so they are left as they are.
        ColorTransformPtr pixels = transformBetween(from, target, options, pixelFormatFor(document.sampleType, document.colorMode),
                                                    pixelFormatFor(document.sampleType, document.colorMode));
        if (!pixels) { if (why) *why = "The profiles could not be read."; return false; }
        for (Layer& layer : document.layers) {
            if (!layer.asset || !layer.asset->image) continue;
            const AnyImage before = layer.asset->image;
            AnyImage after;
            if (auto p = before.c8()) { auto copy = std::make_shared<ImageC8>(*p); convertImage(*copy, pixels.get()); after = ImageC8Ptr(copy); }
            else if (auto p16 = before.u16()) { auto copy = std::make_shared<Image16>(*p16); convertImage(*copy, pixels.get()); after = Image16Ptr(copy); }
            else if (auto p8 = before.u8()) { auto copy = std::make_shared<Image>(*p8); convertImage(*copy, pixels.get()); after = ImagePtr(copy); }
            if (!after) continue;
            const bool liveShape = layer.isLiveShape(), liveText = layer.isLiveText(), liveSmart = layer.isLiveSmartObject();
            layer.asset = Asset::makeAny(after, layer.asset->name);
            if (liveShape) layer.shapeImage = after;
            if (liveText) layer.textImage = after;
            if (liveSmart) layer.smartImage = after;
        }
        document.profile = target;
        return true;
    }
    ColorTransformPtr pixels = transformBetween(from, target, options, deep ? PixelFormat::RGBA16 : PixelFormat::RGBA8, deep ? PixelFormat::RGBA16 : PixelFormat::RGBA8);
    ColorTransformPtr colours = transformBetween(from, target, options, PixelFormat::RGBFloat, PixelFormat::RGBFloat);
    if (!pixels || !colours) { if (why) *why = "The profiles could not be read."; return false; }
    const ColourConverter convert{from, target, options, colours};

    for (Layer& layer : document.layers) {
        // A vector shape is drawn again from its converted colours (8-bit documents, where vector shapes are edited).
        if (!deep && isVectorShapeLayer(layer)) {
            if (auto shape = vectorShapeOf(layer, document)) {
                convert(shape->r, shape->g, shape->b);
                if (shape->fillPaint.kind == VectorPaint::Kind::Gradient) convert(shape->fillPaint.gradient);
                convert(shape->stroke.r, shape->stroke.g, shape->stroke.b);
                LayerStyle style = editableLayerStyle(layer, document);
                if (convertStyle(style, convert)) setLayerStyle(layer, style);
                setVectorShape(layer, document, *shape);
                continue;
            }
        }
        if (layer.asset && layer.asset->image) {
            const AnyImage before = layer.asset->image;
            const bool liveShape = layer.isLiveShape(), liveText = layer.isLiveText(), liveSmart = layer.isLiveSmartObject();
            AnyImage after;
            if (auto p = before.u8()) { auto copy = std::make_shared<Image>(*p); convertImage(*copy, pixels.get()); after = ImagePtr(copy); }
            else if (auto p16 = before.u16()) { auto copy = std::make_shared<Image16>(*p16); convertImage(*copy, pixels.get()); after = Image16Ptr(copy); }
            if (after) {
                layer.asset = Asset::makeAny(after, layer.asset->name);
                // Still the shape, text or smart object it was: its pixels are those, converted.
                if (liveShape) layer.shapeImage = after;
                if (liveText) layer.textImage = after;
                if (liveSmart) layer.smartImage = after;
            }
        }
        if (layer.shape) convert(layer.shape->red, layer.shape->green, layer.shape->blue);
        if (layer.text) {
            convert(layer.text->red, layer.text->green, layer.text->blue);
            for (TextRun& run : layer.text->runs) convert(run.red, run.green, run.blue);
        }
        if (layer.artboard && layer.artboard->background == Artboard::Other) convert(layer.artboard->red, layer.artboard->green, layer.artboard->blue);
        if (layer.adjustment) {
            AdjustmentSettings settings;
            if (AdjustmentSettings::parse(layer.adjustment->json, settings)) {
                bool changed = false;
                if (settings.kind == AdjustmentKind::PhotoFilter) { convert(settings.photoFilter.color); changed = true; }
                if (settings.kind == AdjustmentKind::GradientMap) { convert(settings.gradientMap.shadows); convert(settings.gradientMap.highlights); changed = true; }
                if (changed) layer.adjustment = settings.toLayerAdjustment();
            }
        }
        if (layerStyleOf(layer, document) || layer.psdCarry) {
            LayerStyle style = editableLayerStyle(layer, document);
            if (convertStyle(style, convert)) setLayerStyle(layer, style);
        }
    }
    document.profile = target;
    return true;
}

} // namespace compositor
