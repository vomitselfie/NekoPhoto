// The Shape tool's rasters and the Gradient tool's fills.
#pragma once
#include "document.h"

namespace compositor {

/// A `width` x `height` raster of `kind` filling its box in `color` (straight sRGB 0..1), antialiased where it
/// curves. A rectangle's corners round by `cornerRadius`, at most half its shorter side.
std::shared_ptr<Image> shapeImage(ShapeKind kind, int width, int height, double red, double green, double blue, double cornerRadius);

enum class GradientShape { Linear, Radial };

/// A colour stop of a multi-stop gradient: `location` 0..1 along it; `midpoint` 0..1 of the way to the next stop is
/// where their blend is half done (Photoshop's diamond).
struct GradientColorStop { float location = 0; float rgb[3] = {0, 0, 0}; float midpoint = 0.5f; };
struct GradientAlphaStop { float location = 0; float opacity = 1; float midpoint = 0.5f; };

/// A gradient's colours (straight RGBA, 0..1). With no `colors`, a two-stop ramp from `start` to `end`; with them,
/// the colour stops (sorted by location) and the opacity stops (none: opaque), each run blended linearly after its
/// midpoint remap, the ends held beyond the outer stops.
struct GradientStops {
    float start[4] = {0, 0, 0, 1};
    float end[4] = {0, 0, 0, 0};
    std::vector<GradientColorStop> colors;
    std::vector<GradientAlphaStop> alphas;
    /// The colour at `t` (0..1).
    void sample(float t, float out[4]) const;
    /// The stops mirrored end for end (the Reverse option).
    void reverse();
};

/// A gradient over `out` (premultiplied), whose pixels are placed on the document by `pixelToDocument`, drawn over
/// `base` at `opacity` through the optional per-pixel `selection` (grid coverage). Linear runs from `from` to `to`
/// (colours held beyond the ends); radial is centred on `from` with `to` on its rim.
void fillGradient(const Image& base, Image& out, const Affine& pixelToDocument, GradientShape shape, Point from, Point to, const GradientStops& stops, double opacity, const GrayImage* selection);
/// The same on a mask (gray): the stops' red channel is the gray value.
void fillGradient(const GrayImage& base, GrayImage& out, const Affine& pixelToDocument, GradientShape shape, Point from, Point to, const GradientStops& stops, double opacity, const GrayImage* selection);

} // namespace compositor
