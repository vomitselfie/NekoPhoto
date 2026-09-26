// Photoshop's warp mesh (a smart object's Warp, Edit > Transform > Warp): a Bezier patch of 2..4 x 2..4 control
// points in the contents' space, the preset styles baked to such a patch, and the resampler that draws an image
// through it onto the placement quad. A port of Patchy's core/warp_mesh (MIT, src/third_party/patchy_psd/README.md);
// the constructions were pinned by Patchy against Photoshop 2026's own bakes (docs/smart-objects.md).
#pragma once
#include "image.h"
#include "transform.h"
#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace compositor {

struct WarpMesh {
    int uOrder = 4, vOrder = 4;            // control points per row, rows (2..4 each)
    std::vector<double> xs, ys;            // row-major, contents space
    bool operator==(const WarpMesh&) const = default;
};

/// A flat mesh over the rectangle.
WarpMesh identityWarpMesh(double left, double top, double right, double bottom, int uOrder, int vOrder);
/// The patch's point at (u, v) in [0, 1]^2 (outside it extrapolates).
Point evaluateWarpMesh(const WarpMesh& mesh, double u, double v);
/// Whether a preset style ('warpArc', 'warpFlag', ...) can be baked.
bool warpStyleBakes(std::string_view style);
/// Photoshop's bake of a preset style at `bend` percent over a `width` x `height` contents rectangle, turned
/// vertical when `vertical` (warpRotate Vrtc); none for other styles or an empty size.
std::optional<WarpMesh> styleWarpMesh(std::string_view style, double bend, bool vertical, double width, double height);
/// Horizontal and vertical distortion (percent): rows scaled first, then columns, each about its edge points' midpoint.
void distortWarpMesh(WarpMesh& mesh, double horizontal, double vertical);

/// `image` drawn through `mesh`, the mesh's control-point hull mapped onto `quad` (document pixels, top-left,
/// top-right, bottom-right, bottom-left, as Photoshop stores a warped placement): the pixels over the result's
/// whole-pixel bounds and the axis-aligned transform placing them. None for an empty image or a degenerate quad.
struct WarpedRaster { std::shared_ptr<Image> image; LayerTransform transform; };
/// `clip` (document pixels), when given, bounds the result: only what falls inside it is drawn.
std::optional<WarpedRaster> renderWarpedImage(const Image& image, const WarpMesh& mesh, const std::array<double, 8>& quad, const Rect* clip = nullptr);

/// `image` bent by `mesh`, a patch over `box` (in the image's pixels; the mesh in the box's own space, 0..width by
/// 0..height): the whole image, the patch extended past the box where the image reaches beyond it (Warp Text over
/// its layout box, ink poking out of it). The result's transform places it in the image's pixel space.
std::optional<WarpedRaster> renderWarpedOverBox(const Image& image, const WarpMesh& mesh, const Rect& box);

} // namespace compositor
