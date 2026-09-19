// The Shape tool's rasters and the Gradient tool's fills.
#pragma once
#include "document.h"

namespace compositor {

/// A `width` x `height` raster of `kind` filling its box in `color` (straight sRGB 0..1), antialiased where it
/// curves. A rectangle's corners round by `cornerRadius`, at most half its shorter side.
std::shared_ptr<Image> shapeImage(ShapeKind kind, int width, int height, double red, double green, double blue, double cornerRadius);

enum class GradientShape { Linear, Radial };

/// Colours (straight RGBA, 0..1) at the start and end of a gradient.
struct GradientStops { float start[4]; float end[4]; };

/// A gradient over `out` (premultiplied), whose pixels are placed on the document by `pixelToDocument`, drawn over
/// `base` at `opacity` through the optional per-pixel `selection` (grid coverage). Linear runs from `from` to `to`
/// (colours held beyond the ends); radial is centred on `from` with `to` on its rim.
void fillGradient(const Image& base, Image& out, const Affine& pixelToDocument, GradientShape shape, Point from, Point to, const GradientStops& stops, double opacity, const GrayImage* selection);
/// The same on a mask (gray): the stops' red channel is the gray value.
void fillGradient(const GrayImage& base, GrayImage& out, const Affine& pixelToDocument, GradientShape shape, Point from, Point to, const GradientStops& stops, double opacity, const GrayImage* selection);

} // namespace compositor
