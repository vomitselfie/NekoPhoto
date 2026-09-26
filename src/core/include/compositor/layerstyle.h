// Photoshop layer styles (drop and inner shadows, outer and inner glows, bevel and emboss, satin, colour,
// gradient and pattern overlays, stroke), read from the 'lfx2' / 'lmfx' block a PSD layer carries
// (psd_carry.h) and drawn by the renderer around the layer's pixels. The model and the calibrated render
// rules follow Patchy (MIT, src/third_party/patchy_psd/README.md), which pinned them against Photoshop 2026;
// see docs/layer-styles.md.
#pragma once
#include "image.h"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace compositor {

struct Layer;
struct Document;

/// Every Photoshop blend mode an effect can use (ours are a subset).
enum class EffectBlend {
    Normal, Dissolve, Darken, Multiply, ColorBurn, LinearBurn, DarkerColor, Lighten, Screen, ColorDodge, LinearDodge,
    LighterColor, Overlay, SoftLight, HardLight, VividLight, LinearLight, PinLight, HardMix, Difference, Exclusion,
    Subtract, Divide, Hue, Saturation, Color, Luminosity
};

struct StyleColor { uint8_t r = 0, g = 0, b = 0; };

struct StyleGradient {
    enum class Type { Linear, Radial, Angle, Reflected, Diamond, ShapeBurst };
    enum class Interpolation { Classic, Perceptual, Linear };
    struct ColorStop { float location = 0; StyleColor color; float midpoint = 0.5f; };
    struct AlphaStop { float location = 0; float opacity = 1; float midpoint = 0.5f; };
    std::vector<ColorStop> colors;
    std::vector<AlphaStop> alphas;
    float smoothness = 1;              // Intr / 4096
    Interpolation interpolation = Interpolation::Classic;
    Type type = Type::Linear;
    float angle = 90, scale = 1, offsetX = 0, offsetY = 0;   // offsets in percent
    bool reverse = false, dither = false, alignWithLayer = true;
    /// A fill layer's gradient ('GdFl'): Photoshop spans it over the bounds' centre chord, unsnapped, and eases even
    /// a two-stop ramp (Patchy's calibration); layer styles keep the overlay geometry.
    bool fillLayer = false;
};

/// A curve (0..255 both ways) remapping an effect's values; empty points are the identity.
struct StyleContour {
    struct Point { float x = 0, y = 0; bool corner = false; };
    std::vector<Point> points;
    bool linear() const;
    std::array<uint8_t, 256> lut() const;
};

struct DropShadow {
    EffectBlend mode = EffectBlend::Multiply; StyleColor color; float opacity = 0.75f;
    float angle = 120, distance = 5, spread = 0, size = 5; bool useGlobalLight = false, layerConceals = true;
    bool enabled = true;   // off: kept for the Layer Style dialog, not drawn
};
struct InnerShadow {
    EffectBlend mode = EffectBlend::Multiply; StyleColor color; float opacity = 0.75f;
    float angle = 120, distance = 5, choke = 0, size = 5; bool useGlobalLight = false;
    bool enabled = true;   // off: kept for the Layer Style dialog, not drawn
};
struct OuterGlow {
    EffectBlend mode = EffectBlend::Screen; StyleColor color{255, 255, 190}; float opacity = 0.75f;
    float spread = 0, size = 5, range = 100; bool precise = false;
    bool enabled = true;   // off: kept for the Layer Style dialog, not drawn
};
struct InnerGlow {
    EffectBlend mode = EffectBlend::Screen; StyleColor color{255, 255, 190}; float opacity = 0.75f;
    float choke = 0, size = 5, range = 100; bool precise = false, center = false;
    bool enabled = true;   // off: kept for the Layer Style dialog, not drawn
};
struct ColorOverlay { EffectBlend mode = EffectBlend::Normal; StyleColor color{255, 0, 0}; float opacity = 1; bool enabled = true; };
struct GradientOverlay { EffectBlend mode = EffectBlend::Normal; float opacity = 1; StyleGradient gradient; bool enabled = true; };
struct PatternOverlay {
    EffectBlend mode = EffectBlend::Normal; float opacity = 1, scale = 1, angle = 0; std::string patternId;
    bool linkWithLayer = true; float phaseX = 0, phaseY = 0;
    bool enabled = true;   // off: kept for the Layer Style dialog, not drawn
};
struct Satin {
    EffectBlend mode = EffectBlend::Multiply; StyleColor color; float opacity = 0.5f;
    float angle = 19, distance = 11, size = 14; bool invert = true;
    bool enabled = true;   // off: kept for the Layer Style dialog, not drawn
};
struct Stroke {
    enum class Position { Outside, Inside, Center };
    EffectBlend mode = EffectBlend::Normal; StyleColor color; float opacity = 1, size = 3;
    Position position = Position::Outside; bool gradientFill = false; StyleGradient gradient; bool overprint = false;
    bool enabled = true;   // off: kept for the Layer Style dialog, not drawn
};
struct Bevel {
    enum class Kind { Inner, Outer, Emboss, Pillow, StrokeEmboss };
    enum class Technique { Smooth, ChiselHard, ChiselSoft };
    EffectBlend highlightMode = EffectBlend::Screen; StyleColor highlight{255, 255, 255}; float highlightOpacity = 0.75f;
    EffectBlend shadowMode = EffectBlend::Multiply; StyleColor shadow; float shadowOpacity = 0.75f;
    float angle = 120, altitude = 30, depth = 1, size = 5, soften = 0; bool up = true, useGlobalLight = false;
    Kind kind = Kind::Inner; Technique technique = Technique::Smooth;
    StyleContour gloss; bool glossAntialiased = false;
    bool useContour = false; StyleContour contour; bool contourAntialiased = false; float contourRange = 0.5f;
    bool useTexture = false; std::string texturePattern; float textureScale = 1, textureDepth = 1;
    bool textureInvert = false, textureLinkWithLayer = true; float texturePhaseX = 0, texturePhaseY = 0;
    bool enabled = true;   // off: kept for the Layer Style dialog, not drawn
};

struct LayerStyle {
    bool visible = true;               // masterFXSwitch
    bool maskHidesEffects = false;     // lmgm
    bool blendInteriorAsGroup = false; // infx
    std::vector<DropShadow> dropShadows;
    std::vector<InnerShadow> innerShadows;
    std::vector<OuterGlow> outerGlows;
    std::vector<InnerGlow> innerGlows;
    std::vector<ColorOverlay> colorOverlays;
    std::vector<GradientOverlay> gradientOverlays;
    std::vector<PatternOverlay> patternOverlays;
    std::vector<Satin> satins;
    std::vector<Stroke> strokes;
    std::vector<Bevel> bevels;
    /// The layers's effects reference point ('fxrp'), for patterns linked with the layer.
    double referenceX = 0, referenceY = 0;
    bool empty() const;
    /// How far, in document pixels, the effects reach past the layer's pixels (or read past them).
    double reach() const;
};

/// A layer's style, parsed once per carried block and cached; null when the layer has none (or all are off).
std::shared_ptr<const LayerStyle> layerStyleOf(const Layer& layer, const Document& document);

// ---- Editing (the Layer Style dialog) ------------------------------------------------------------------------

/// The layer's style for editing: every effect, the ones switched off too (`enabled` false), with global-light
/// angles resolved; an empty style when the layer has none.
LayerStyle editableLayerStyle(const Layer& layer, const Document& document);
/// The 'lfx2' block for `style` (Photoshop 2026's descriptor shapes, Patchy's authoring): each kind of effect as one
/// object, or its '...Multi' list when there are several.
std::vector<uint8_t> authorLayerStyleBlock(const LayerStyle& style);
/// Gives the layer `style`: its carried effects blocks ('lfx2', 'lmfx', 'lfxs', 'lrFX') are replaced by one authored
/// 'lfx2', or removed when the style has no effects at all; 'lmgm' and 'infx' follow the style's two options.
void setLayerStyle(Layer& layer, const LayerStyle& style);
/// Whether the style has any effect, on or off.
bool hasAnyEffect(const LayerStyle& style);
/// The style as JSON (automation's layers.style_get), and back: keys left out keep their defaults, unknown ones are
/// refused with `error`.
std::string layerStyleToJson(const LayerStyle& style);
bool layerStyleFromJson(const std::string& json, LayerStyle& out, std::string* error);

/// The document's patterns (Photoshop 'Patt' blocks), by id, straight RGBA; parsed once per document carry.
struct PatternTile { int width = 0, height = 0; std::vector<uint8_t> rgba; };
std::shared_ptr<const std::map<std::string, PatternTile>> documentPatterns(const Document& document);

/// The document's global light (image resources 1037 and 1049), in degrees.
void documentGlobalLight(const Document& document, float& angle, float& altitude);

/// A fill layer's or a shape stroke's gradient from its descriptor block (u32 16 + descriptor, or the descriptor
/// object itself); none when it has no gradient.
std::optional<StyleGradient> parseFillGradient(const std::vector<uint8_t>& block);
/// A fill layer's pattern ('PtFl'): the pattern id, scale, angle and phase.
struct FillPattern { std::string id; float scale = 1, angle = 0, phaseX = 0, phaseY = 0; bool linked = true; };
std::optional<FillPattern> parseFillPattern(const std::vector<uint8_t>& block);

/// Master opacity and Fill as Photoshop splits them (NekoPhoto keeps one opacity: their product).
void layerOpacities(const Layer& layer, float& master, float& fill);

// ---- Pieces the renderer uses (layerstyle_render.cpp) --------------------------------------------------------

/// `backdrop` (straight 0..1) blended with `source` in `mode`.
void effectBlend(EffectBlend mode, const float backdrop[3], const float source[3], float out[3]);
/// A gradient's position (0..1) at document pixel (x, y) over `bounds` (document pixels).
float gradientPosition(const StyleGradient& g, double boundsX, double boundsY, double boundsW, double boundsH, double x, double y);
StyleColor gradientColor(const StyleGradient& g, float position);
float gradientOpacity(const StyleGradient& g, float position);

} // namespace compositor
