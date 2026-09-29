// Selections as document-sized coverage rasters. The Mac keeps a CGPath and
// combines paths with boolean operations; here outlines are rasterised once
// (antialiased scanline fill, nonzero winding) and combined per pixel.
#pragma once
#include "document.h"
#include <vector>

namespace compositor {

enum class SelectionMode { Replace, Add, Subtract, Intersect };

/// Fills a closed polygon (document pixels) into a `width` x `height` coverage image.
std::shared_ptr<GrayImage> rasterizePolygon(const std::vector<Point>& points, int width, int height, bool antialiased);
/// Fills an ellipse inscribed in `rect`.
std::shared_ptr<GrayImage> rasterizeEllipse(const Rect& rect, int width, int height, bool antialiased);
std::shared_ptr<GrayImage> rasterizeRect(const Rect& rect, int width, int height, bool antialiased);

/// `current` combined with `shape` in `mode`; a null `current` means no selection.
std::optional<Selection> combineSelection(const std::optional<Selection>& current, const GrayImage& shape, SelectionMode mode, bool antialiased);
/// The same at a document's depth: `shape` (either depth) and the current coverage brought to `depth` first.
std::optional<Selection> combineSelection(const std::optional<Selection>& current, const AnyGray& shape, SelectionMode mode, bool antialiased, SampleType depth);
/// Inverse and Expand/Contract keep the selection's depth.
Selection invertSelection(const Selection& selection, int width, int height);
/// The selection moved by whole pixels, at its depth; what moves off the canvas is lost.
Selection offsetSelection(const Selection& selection, int dx, int dy);
/// Grows (positive) or shrinks (negative) the selection by `amount` pixels with round corners.
Selection resizeSelection(const Selection& selection, int amount);

/// Closed loops of pixel-edge corner points (document pixels) around the selected pixels, for marching ants.
/// `tooDetailed` reports an outline past the tracer's edge limit (the loops are then empty).
std::vector<std::vector<Point>> selectionOutline(const GrayImage& coverage, bool* tooDetailed = nullptr);

/// Alpha of `image` (the layer's pixels through its transform) as a document-sized coverage: Load Selection.
std::shared_ptr<GrayImage> coverageFromLayer(const Document& document, const Layer& layer);
/// The same for a 16-bit layer, at 16 bits.
std::shared_ptr<Gray16> coverageFromLayer16(const Document& document, const Layer& layer);
/// And for a 32-bit layer, as float coverage.
std::shared_ptr<GrayF> coverageFromLayerF(const Document& document, const Layer& layer);

} // namespace compositor
