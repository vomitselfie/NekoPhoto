// Vector masks: Photoshop's 'vmsk' / 'vsms' paths (the shape of a shape layer, or a vector mask on any layer),
// read from what a PSD layer carries (psd_carry.h) and drawn as coverage. The record layout and the combine rules
// follow Patchy (MIT, src/third_party/patchy_psd/README.md): 26-byte records, knots as (in, anchor, out) pairs of
// y, x in 8.24 fixed point of the canvas; subpaths of one shape group fill even-odd together, and groups combine in
// order by their operation (add, subtract, intersect, exclude). The path follows its layer when the layer moves.
#pragma once
#include "image.h"
#include "transform.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace compositor {

struct Layer;
struct Document;

struct VectorPath {
    enum class Op : uint8_t { Xor = 0, Add = 1, Subtract = 2, Intersect = 3 };
    struct Knot { double inX, inY, x, y, outX, outY; };   // document pixels
    struct Subpath { std::vector<Knot> knots; bool closed = true; Op op = Op::Add; int32_t group = 0; };
    std::vector<Subpath> subpaths;
    bool inverted = false, disabled = false;
};

/// A 'vmsk' / 'vsms' payload on a `width` x `height` canvas; none when it cannot be read.
std::optional<VectorPath> parseVectorMask(const std::vector<uint8_t>& payload, int width, int height);
/// The payload with every knot moved by `map` (document pixels to document pixels), the rest byte for byte.
std::optional<std::vector<uint8_t>> mapVectorMask(const std::vector<uint8_t>& payload, int width, int height,
                                                  const std::function<Point(Point)>& map);

/// Coverage (0..255) of `path` over `region` (document pixels) at `scale`, `w` x `h` output pixels, antialiased.
std::shared_ptr<GrayImage> rasterizeVectorMask(const VectorPath& path, const Rect& region, double scale, int w, int h);

/// The layer's vector mask as it now stands: its carried path, moved along with the layer since it was read.
/// None when it has none (or it is switched off).
std::optional<VectorPath> layerVectorMask(const Layer& layer, const Document& document);

/// Photoshop's mask parameters (a mask section with flag bit 4): density (0..255 raw) and feather (pixels) of
/// the pixel mask and of the vector mask.
struct MaskParameters { std::optional<int> userDensity, vectorDensity; std::optional<double> userFeather, vectorFeather; };
std::optional<MaskParameters> parseMaskParameters(const std::vector<uint8_t>& section);
/// Density and feather applied to coverage drawn at `scale` (feather is a gaussian of sigma = feather pixels).
void applyMaskParameters(GrayImage& coverage, std::optional<int> density, std::optional<double> feather, double scale, bool clampEdges = false);

/// A shape's stroke ('vstk'): drawn along its path in its own colour and opacity over the fill.
struct VectorStroke {
    bool enabled = false, fillEnabled = true;
    double width = 3;
    enum class Align { Inside, Center, Outside } align = Align::Center;
    float opacity = 1;
    uint8_t r = 0, g = 0, b = 0;
    enum class Cap { Butt, Round, Square } cap = Cap::Butt;
    enum class Join { Miter, Round, Bevel } join = Join::Miter;
    double miterLimit = 100;
    std::vector<double> dashes;            // on, off, ... in stroke widths; empty: solid
    double dashOffset = 0;                 // in stroke widths
};
std::optional<VectorStroke> layerVectorStroke(const Layer& layer);
/// Coverage (0..255) of the stroke band over `region` at `scale`.
std::shared_ptr<GrayImage> rasterizeVectorStroke(const VectorPath& path, const VectorStroke& stroke, const Rect& region, double scale, int w, int h);

/// A fill layer's contents ('GdFl' gradient or 'PtFl' pattern) over the canvas, as Photoshop draws them; none when
/// the layer has neither (or they cannot be read).
ImagePtr renderFillLayer(const Layer& layer, const Document& document);

/// How a point moves when a layer goes from `before` (a `w0` x `h0` raster) to `after` (`w1` x `h1`).
Point mapLayerPoint(Point p, const LayerTransform& before, int w0, int h0, const LayerTransform& after, int w1, int h1);

} // namespace compositor
