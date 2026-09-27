// The compositor: renders a document (or any region of it, at any scale) into a
// premultiplied RGBA buffer. Semantics follow ImageExporter.render and
// LiveMaskRenderer on the Mac: folder masks, clipping stacks, own masks,
// opacity, blend modes and adjustment layers, bottom to top.
#pragma once
#include "document.h"
#include <functional>
#include <map>

namespace compositor {

class ColorTransform;

struct RenderOptions {
    /// The document rectangle to render; empty means the whole canvas.
    Rect region;
    /// Output pixels per document pixel.
    double scale = 1;
    /// Blank the output first (false lets a caller draw over an existing backdrop).
    bool clear = true;
    /// The caller's document version, for a RenderCache: bump it whenever the document changes.
    uint64_t version = 0;
    /// The canvas's colour transform to the screen (colormgmt.h): RGBA8 to RGBA8 for an 8-bit document, RGBA16 to
    /// RGBA8 for a 16-bit one, fused with its reduction to 8 bits. Null: none, the pixels as they are.
    const ColorTransform* display = nullptr;
};

/// What a caller keeps between frames while one layer is being edited (the one layer with an override):
/// the layers below it composited, and, when every layer above is a plain Normal pixel layer, those
/// flattened, so a frame is backdrop + the edited layer + one blend. Rebuilt when the version, region,
/// scale or edited layer changes.
struct RenderCache {
    uint64_t version = 0;
    Rect region;
    double scale = 0;
    int width = 0, height = 0;
    Uuid layer;
    std::shared_ptr<Image> backdrop, above;
    bool aboveFlat = false;
    /// The same for a 16-bit document.
    std::shared_ptr<Image16> backdrop16, above16;
};

/// Per-layer overrides while an edit is in progress (a transform being dragged, a brush stroke).
struct LayerOverride {
    std::optional<LayerTransform> transform;
    std::optional<ImagePtr> image;              // replaces the layer's pixels
    std::optional<std::optional<LayerTransform>> maskPlacement;
    std::optional<GrayPtr> maskImage;           // replaces the mask's pixels
    std::optional<BlendMode> blendMode;
    /// The same replacements in a 16-bit document.
    std::optional<Image16Ptr> image16;
    std::optional<Gray16Ptr> maskImage16;
};
using Overrides = std::map<Uuid, LayerOverride>;

/// Renders `document` into `out`, which is sized to fit `options.region * options.scale`.
void render(const Document& document, const RenderOptions& options, Image& out, const Overrides* overrides = nullptr, RenderCache* cache = nullptr);
/// Convenience: the whole document at 1:1.
std::shared_ptr<Image> renderFlattened(const Document& document);
/// A 16-bit document at its own depth (an 8-bit one is rendered at 8 bits and widened): what export and the
/// 16-bit file formats take. render() hands the canvas the same pixels reduced to 8 bits.
void render16(const Document& document, const RenderOptions& options, Image16& out, const Overrides* overrides = nullptr, RenderCache* cache = nullptr);
std::shared_ptr<Image16> renderFlattened16(const Document& document);

/// Draws one raster through a transform into `out`, which represents `region` at `scale`:
/// resampled (mips + bilinear, or nearest), edges antialiased, then multiplied by
/// `coverage` (0..1 per output pixel, optional) and `opacity`, composited in `mode`.
struct DrawParams {
    ImagePtr image;
    LayerTransform transform;
    double opacity = 1;
    BlendMode mode = BlendMode::Normal;
    /// The layer's own mask sampled over its pixel grid or its placement.
    const LayerMask* mask = nullptr;
    std::optional<LayerTransform> maskPlacement; // overrides mask->placement when set
    GrayPtr maskImage;                           // overrides mask->asset.image when set
    LayerTransform layerTransformForMask;        // the transform the mask covers when it has no placement
};
void drawLayer(const DrawParams& params, const Rect& region, double scale, const GrayImage* coverage, Image& out);
/// The same at 16 bits: the layer, its mask and the coverage at the document's depth.
struct DrawParams16 {
    Image16Ptr image;
    LayerTransform transform;
    double opacity = 1;
    BlendMode mode = BlendMode::Normal;
    const LayerMask* mask = nullptr;
    std::optional<LayerTransform> maskPlacement;
    Gray16Ptr maskImage;
    LayerTransform layerTransformForMask;
};
void drawLayer(const DrawParams16& params, const Rect& region, double scale, const Gray16* coverage, Image16& out);

/// Coverage (0..255 per output pixel) of a gray mask placed by `transform` over `region` at `scale`;
/// `outside` is the value beyond the mask's rectangle.
/// With the shared mask, its reductions are cached across frames.
void sampleMaskCoverage(const GrayPtr& mask, const LayerTransform& transform, const Rect& region, double scale, uint8_t outside, GrayImage& out, bool multiply);
void sampleMaskCoverage(const GrayImage& mask, const LayerTransform& transform, const Rect& region, double scale, uint8_t outside, GrayImage& out, bool multiply);
/// The same at 16 bits (coverage 0..32768).
void sampleMaskCoverage(const Gray16Ptr& mask, const LayerTransform& transform, const Rect& region, double scale, uint16_t outside, Gray16& out, bool multiply);
void sampleMaskCoverage(const Gray16& mask, const LayerTransform& transform, const Rect& region, double scale, uint16_t outside, Gray16& out, bool multiply);

/// Image Size: every layer's pixels and mask resampled for a `width` x `height` canvas (the Mac rasterises each
/// transformed layer into an axis-aligned box); placed masks and uniform masks keep their pixels. Adjustment and
/// group records scale their transforms. Returns false when a layer would exceed the size limits.
bool resizeDocument(Document& document, int width, int height, double resolution, Sampling sampling);

/// Resamples `image` through `transform` into a `width` x `height` grid placed by `target` (both in
/// document space): what the Mac does when Image Size or a distort bakes pixels.
std::shared_ptr<Image> resampleLayer(const Image& image, const LayerTransform& transform, const LayerTransform& target, int width, int height);
/// The same with the shared image, whose reductions are then cached across calls.
std::shared_ptr<Image> resampleLayer(const ImagePtr& image, const LayerTransform& transform, const LayerTransform& target, int width, int height);
std::shared_ptr<GrayImage> resampleMask(const GrayImage& mask, const LayerTransform& transform, const LayerTransform& target, int width, int height, uint8_t outside);
/// The same at 16 bits (mips and the point samplers; no separable pass).
std::shared_ptr<Image16> resampleLayer(const Image16Ptr& image, const LayerTransform& transform, const LayerTransform& target, int width, int height);
std::shared_ptr<Gray16> resampleMask(const Gray16& mask, const LayerTransform& transform, const LayerTransform& target, int width, int height, uint16_t outside);

} // namespace compositor
