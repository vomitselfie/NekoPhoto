// The compositor: renders a document (or any region of it, at any scale) into a
// premultiplied RGBA buffer. Semantics follow ImageExporter.render and
// LiveMaskRenderer on the Mac: folder masks, clipping stacks, own masks,
// opacity, blend modes and adjustment layers, bottom to top.
#pragma once
#include "document.h"
#include <functional>
#include <map>

namespace compositor {

struct RenderOptions {
    /// The document rectangle to render; empty means the whole canvas.
    Rect region;
    /// Output pixels per document pixel.
    double scale = 1;
    /// Blank the output first (false lets a caller draw over an existing backdrop).
    bool clear = true;
};

/// Per-layer overrides while an edit is in progress (a transform being dragged, a brush stroke).
struct LayerOverride {
    std::optional<LayerTransform> transform;
    std::optional<ImagePtr> image;              // replaces the layer's pixels
    std::optional<std::optional<LayerTransform>> maskPlacement;
    std::optional<GrayPtr> maskImage;           // replaces the mask's pixels
    std::optional<BlendMode> blendMode;
};
using Overrides = std::map<Uuid, LayerOverride>;

/// Renders `document` into `out`, which is sized to fit `options.region * options.scale`.
void render(const Document& document, const RenderOptions& options, Image& out, const Overrides* overrides = nullptr);
/// Convenience: the whole document at 1:1.
std::shared_ptr<Image> renderFlattened(const Document& document);

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

/// Coverage (0..255 per output pixel) of a gray mask placed by `transform` over `region` at `scale`;
/// `outside` is the value beyond the mask's rectangle.
void sampleMaskCoverage(const GrayImage& mask, const LayerTransform& transform, const Rect& region, double scale, uint8_t outside, GrayImage& out, bool multiply);

/// Image Size: every layer's pixels and mask resampled for a `width` x `height` canvas (the Mac rasterises each
/// transformed layer into an axis-aligned box); placed masks and uniform masks keep their pixels. Adjustment and
/// group records scale their transforms. Returns false when a layer would exceed the size limits.
bool resizeDocument(Document& document, int width, int height, double resolution, Sampling sampling);

/// Resamples `image` through `transform` into a `width` x `height` grid placed by `target` (both in
/// document space): what the Mac does when Image Size or a distort bakes pixels.
std::shared_ptr<Image> resampleLayer(const Image& image, const LayerTransform& transform, const LayerTransform& target, int width, int height);
std::shared_ptr<GrayImage> resampleMask(const GrayImage& mask, const LayerTransform& transform, const LayerTransform& target, int width, int height, uint8_t outside);

} // namespace compositor
