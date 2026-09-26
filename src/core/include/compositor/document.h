// The document model: layers, groups, masks, clipping links, adjustments and
// the canvas. A port of the value types in Document/EditorSession.swift and
// friends. Everything here is a plain value; images are shared immutably so
// copying a Document (for undo) costs no pixels.
#pragma once
#include "geometry.h"
#include "psd_carry.h"
#include "smartobject.h"
#include "image.h"
#include "transform.h"
#include "uuid.h"
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace compositor {

enum class BlendMode { Normal, Multiply, Screen, Overlay, Darken, Lighten, Difference, ColorDodge, ColorBurn, Hue, Saturation, Color, Luminosity };
constexpr int blendModeCount = 13;
const char* blendModeName(BlendMode mode);          // "Color Dodge" etc, as in the manifest
bool parseBlendMode(const std::string& name, BlendMode& out);

/// An imported or painted raster with its 96 px thumbnail (ImportedImage).
struct Asset {
    ImagePtr image;
    ImagePtr thumbnail;
    std::string name;
    static Asset make(ImagePtr image, std::string name);
};

struct MaskAsset {
    GrayPtr image;
    GrayPtr thumbnail;
    static MaskAsset make(GrayPtr image);
    static MaskAsset solid(bool revealing);
};

/// A layer mask: gray coverage over the layer's pixel grid, or placed on its own (LayerMask).
struct LayerMask {
    MaskAsset asset;
    bool enabled = true;
    std::optional<LayerTransform> placement;
    bool linked = true;
    bool operator==(const LayerMask& o) const {
        return asset.image == o.asset.image && enabled == o.enabled && placement == o.placement && linked == o.linked;
    }
    /// White or black, whichever most of the edge is: what shows beyond a placed mask.
    static uint8_t background(const GrayImage& thumbnail);
    /// Where the mask sits once its layer moves from `from` to `to`.
    std::optional<LayerTransform> placementMovingLayer(const LayerTransform& from, const LayerTransform& to) const;
};

enum class AdjustmentKind { HueSaturation, Levels, Curves, Exposure, GradientMap, Grain };
const char* adjustmentKindName(AdjustmentKind kind);
bool parseAdjustmentKind(const std::string& name, AdjustmentKind& out);

/// An adjustment layer's settings. The full settings are kept as the manifest's
/// JSON so unknown or newer fields round-trip; the fields the renderer uses are
/// decoded on demand (see adjustments.h).
struct LayerAdjustment {
    AdjustmentKind kind = AdjustmentKind::Levels;
    std::string json; // the manifest's "adjustment" object, serialized
    bool operator==(const LayerAdjustment&) const = default;
};

enum class ShapeKind { Rectangle, Ellipse };

struct LayerShapeStyle {
    ShapeKind kind = ShapeKind::Rectangle;
    double red = 0, green = 0, blue = 0;
    double cornerRadius = 0;
    bool operator==(const LayerShapeStyle&) const = default;
};

/// One stretch of a text layer in its own style (Photoshop's style runs).
struct TextRun {
    int length = 0;                    // UTF-16 code units of the layer's text (Qt's and Photoshop's unit)
    std::string fontFamily;
    double fontSize = 48;
    bool bold = false, italic = false;
    int weight = 0;                    // 100 (thin) .. 900 (black), from the face's name; 0: from `bold`
    double red = 0, green = 0, blue = 0;
    double letterSpacing = 0;
    double baselineShift = 0;          // pixels, up
    double leading = 0;                // baseline to baseline for a line holding it (the largest on a line wins); 0: 1.2 x size
    enum class Caps { Normal, Small, All } caps = Caps::Normal;
    bool underline = false, strikethrough = false;
    bool operator==(const TextRun&) const = default;
};

/// Photoshop's Warp Text: a preset style bent over the text's layout box.
struct TextWarp {
    std::string style;                 // "warpArc", "warpFlag", ... (Photoshop's names); empty: none
    double bend = 0;                   // percent, -100..100
    double horizontal = 0, vertical = 0;   // distortion, percent
    bool verticalOrientation = false;  // Photoshop's warpRotate Vrtc
    bool active() const { return !style.empty() && style != "warpNone"; }
    bool operator==(const TextWarp&) const = default;
};

/// A text layer's content and style. Its pixels are an ordinary raster the app renders from these (the
/// core has no font engine), so every consumer of the document, the Mac app included, sees pixels.
struct LayerText {
    std::string text;
    std::string fontFamily;            // empty: the system's default sans-serif
    double fontSize = 48;              // document pixels
    bool bold = false, italic = false;
    double red = 0, green = 0, blue = 0;
    int alignment = 0;                 // 0 left, 1 centre, 2 right (multi-line text)
    double lineSpacing = 1;            // multiple of the font's line height
    double letterSpacing = 0;          // extra pixels between glyphs
    /// Paragraph (box) text: lines wrap at the box's width and what does not fit its height is hidden; the first
    /// baseline sits the first line's cap height below the box's top, as Photoshop sets it. 0: point text.
    double boxWidth = 0, boxHeight = 0;
    TextWarp warp;
    /// Text in more than one style: the runs, in order, covering the text (the fields above then mirror the first
    /// run). Empty: all of it in the style above.
    std::vector<TextRun> runs;
    bool operator==(const LayerText&) const = default;
};

/// The text's length in UTF-16 code units.
int utf16Length(const std::string& utf8);
/// The runs to draw `text` with: its own, fitted to its length (the last one stretched or cut), or one run of its
/// style over all of it.
std::vector<TextRun> textRuns(const LayerText& text);
/// The layer's style as one run (its fields).
TextRun baseTextRun(const LayerText& text);
/// Runs kept in step with an edit that turned `before` into `after` (the changed middle takes the style of the run
/// it starts in).
std::vector<TextRun> adjustTextRuns(const std::vector<TextRun>& runs, const std::string& before, const std::string& after);
/// Adjacent equal runs merged, the plain fields set from the first run, and a single run they say in full dropped.
void settleTextRuns(LayerText& text);
/// `after` (an edit of `before` through its plain fields, its runs untouched) with the edit carried into the runs:
/// the text's change moves the run boundaries, a new size scales every run by the same factor, and any other
/// field changed is given to every run.
LayerText carryTextEdit(const LayerText& before, LayerText after);

struct Layer {
    Uuid id;
    std::optional<Asset> asset;
    LayerTransform transform;
    std::string name;
    bool visible = true;
    std::optional<Uuid> parentId;
    bool isGroup = false;
    /// A folder's children blend straight into what is below it (Photoshop's Pass Through); false isolates them
    /// and composites the folder's result in its own blend mode, as Photoshop does for every other mode.
    bool passThrough = true;
    double opacity = 1;
    BlendMode blendMode = BlendMode::Normal;
    std::optional<Uuid> maskSourceId;
    std::optional<LayerMask> mask;
    std::optional<LayerAdjustment> adjustment;
    /// A shape layer's style (its pixels are an ordinary raster). `shapeImage` is the raster the shape drew;
    /// once the pixels change the layer is plain pixels again.
    std::optional<LayerShapeStyle> shape;
    ImagePtr shapeImage;
    /// A text layer's content and style, with the raster it rendered; once the pixels change the layer is
    /// plain pixels again, as with shapes.
    std::optional<LayerText> text;
    ImagePtr textImage;
    /// Manifest fields this build does not understand, kept for the round trip.
    std::string extraJson;
    /// What the PSD this layer came from held that NekoPhoto does not model (psd_carry.h).
    std::shared_ptr<const PsdLayerCarry> psdCarry;
    /// A smart object instance (smartobject.h), with the raster it placed; once the pixels change some other way
    /// the layer is plain pixels again, as with text.
    std::optional<SmartObjectInstance> smartObject;
    ImagePtr smartImage;

    Layer() = default;
    /// A new layer holding `asset`, its top-left at `origin`.
    Layer(Asset asset, Point origin);
    /// A new blank layer (pixels are allocated when painting begins).
    Layer(std::string name, Size blankSize);

    bool operator==(const Layer& o) const;
    Point origin() const { return transform.origin; }
    Size size() const { return transform.size; }
    /// Pixel grid of the layer's raster, or the rounded transform size for a blank layer.
    int pixelWidth() const;
    int pixelHeight() const;
    /// Where the mask's pixels sit: its own placement, else the layer's transform.
    LayerTransform maskTransform() const { return mask && mask->placement ? *mask->placement : transform; }
    /// The shape this layer still is: none once its pixels were edited some other way.
    bool isLiveShape() const { return shape && shapeImage && asset && asset->image == shapeImage; }
    /// The text this layer still is, likewise.
    bool isLiveText() const { return text && textImage && asset && asset->image == textImage; }
    /// The smart object this layer still places.
    bool isLiveSmartObject() const { return smartObject && smartImage && asset && asset->image == smartImage; }
};

/// A selection: document-sized coverage (white = selected) with a flag for antialiased edges.
/// Session-only, never saved. A selection with no coverage at all is an explicit empty selection.
struct Selection {
    GrayPtr coverage;
    bool antialiased = true;
    bool operator==(const Selection& o) const { return coverage == o.coverage && antialiased == o.antialiased; }
    bool isEmpty() const;
    Rect bounds() const;
    /// The nonzero pixel bounds, scanned once per coverage raster and remembered.
    const PixelBounds& pixelBounds() const;

private:
    mutable const GrayImage* boundsFor_ = nullptr;
    mutable PixelBounds bounds_;
};

struct Document {
    Uuid id;
    int width = 0;
    int height = 0;
    double resolution = 72;
    std::vector<Layer> layers; // bottom to top
    std::optional<Selection> selection;
    std::string extraJson;
    /// The PSD's image resources and global blocks, when the document was opened from one (psd_carry.h).
    std::shared_ptr<const PsdDocumentCarry> psdCarry;
    /// Smart object sources, by id, shared by every layer that places them (and by undo snapshots).
    std::map<std::string, std::shared_ptr<const SmartObjectSource>> smartObjects;

    Document() = default;
    Document(int width, int height);
    bool operator==(const Document& o) const;
    Size size() const { return {double(width), double(height)}; }
    Rect rect() const { return {0, 0, double(width), double(height)}; }

    const Layer* find(const Uuid& id) const;
    Layer* find(const Uuid& id);
    int indexOf(const Uuid& id) const;

    static bool validDimension(int n) { return n >= 1 && n <= maxImageSide; }
    static constexpr int maxLayers = 10000;
    /// One image, layer, mask or canvas: 100 megapixels, as in Compositor for macOS.
    static constexpr long long pixelBudget = 100000000;
    /// All the layers' pixels together, and separately all the masks': a gigapixel, 4 GB of RGBA, so a stack
    /// of full-size game textures fits. Compositor for macOS stops at pixelBudget in total and cannot open a
    /// larger project (see fitsMacBudget).
    static constexpr long long projectPixelBudget = 1000000000;
    /// The pixels held by every layer's image, and by every mask.
    long long layerPixels() const;
    long long maskPixels() const;
    /// Whether Compositor for macOS can open this project: its loader allows pixelBudget in total.
    bool fitsMacBudget() const {
        // The Mac app reads projects up to version 7: folders with their own opacity, mode or isolation need 8.
        for (const Layer& l : layers) if (l.isGroup && (l.opacity != 1 || l.blendMode != BlendMode::Normal || !l.passThrough)) return false;
        return layerPixels() <= pixelBudget && maskPixels() <= pixelBudget;
    }
};

/// Layer hierarchy helpers (Document/LayerGroups.swift `LayerHierarchy`).
struct HierarchyEntry { const Layer* layer; int depth; bool visible; };
std::vector<HierarchyEntry> hierarchyEntries(const std::vector<Layer>& layers, bool topFirst = false, const std::set<Uuid>* collapsed = nullptr);
/// Visible, non-group layers in render order (bottom to top).
std::vector<const Layer*> renderLayers(const std::vector<Layer>& layers);
std::set<Uuid> effectiveVisibleIds(const std::vector<Layer>& layers);
std::set<Uuid> descendantIds(const std::vector<Layer>& layers, const Uuid& id);
/// Parents must exist and be groups, no cycles, nesting at most 64 deep, groups carry no image.
bool validateHierarchy(const std::vector<Layer>& layers, std::string* error = nullptr);
/// Clipping links: sources exist, are not groups or adjustments, no self links or cycles, chains under 256.
bool validateClipping(const std::vector<Layer>& layers, std::string* error = nullptr);

/// The part of the canvas that may look different between two versions of a document: the bounds of every
/// layer added, removed or changed. The whole canvas when something reaches further (a folder, an adjustment
/// layer, the stacking order, the canvas size); an empty rectangle when nothing visible changed.
Rect changedArea(const Document& before, const Document& after);

/// The name "Layer N" / "Folder N" not yet used.
std::string nextLayerName(const std::vector<Layer>& layers, const std::string& prefix);

} // namespace compositor
