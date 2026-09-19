// The document model: layers, groups, masks, clipping links, adjustments and
// the canvas. A port of the value types in Document/EditorSession.swift and
// friends. Everything here is a plain value; images are shared immutably so
// copying a Document (for undo) costs no pixels.
#pragma once
#include "geometry.h"
#include "image.h"
#include "transform.h"
#include "uuid.h"
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

struct Layer {
    Uuid id;
    std::optional<Asset> asset;
    LayerTransform transform;
    std::string name;
    bool visible = true;
    std::optional<Uuid> parentId;
    bool isGroup = false;
    double opacity = 1;
    BlendMode blendMode = BlendMode::Normal;
    std::optional<Uuid> maskSourceId;
    std::optional<LayerMask> mask;
    std::optional<LayerAdjustment> adjustment;
    /// A shape layer's style (its pixels are an ordinary raster). `shapeImage` is the raster the shape drew;
    /// once the pixels change the layer is plain pixels again.
    std::optional<LayerShapeStyle> shape;
    ImagePtr shapeImage;
    /// Manifest fields this build does not understand, kept for the round trip.
    std::string extraJson;

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
};

/// A selection: document-sized coverage (white = selected) with a flag for antialiased edges.
/// Session-only, never saved. A selection with no coverage at all is an explicit empty selection.
struct Selection {
    GrayPtr coverage;
    bool antialiased = true;
    bool operator==(const Selection& o) const { return coverage == o.coverage && antialiased == o.antialiased; }
    bool isEmpty() const;
    Rect bounds() const;
};

struct Document {
    Uuid id;
    int width = 0;
    int height = 0;
    double resolution = 72;
    std::vector<Layer> layers; // bottom to top
    std::optional<Selection> selection;
    std::string extraJson;

    Document() = default;
    Document(int width, int height);
    bool operator==(const Document& o) const;
    Size size() const { return {double(width), double(height)}; }
    Rect rect() const { return {0, 0, double(width), double(height)}; }

    const Layer* find(const Uuid& id) const;
    Layer* find(const Uuid& id);
    int indexOf(const Uuid& id) const;

    static bool validDimension(int n) { return n >= 1 && n <= 30000; }
    static constexpr int maxLayers = 10000;
    static constexpr long long pixelBudget = 100000000;
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

/// The name "Layer N" / "Folder N" not yet used.
std::string nextLayerName(const std::vector<Layer>& layers, const std::string& prefix);

} // namespace compositor
