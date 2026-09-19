// Free distortion: a layer's four corners moved independently. Layer transforms
// are affine, so a distortion previews live and, on Apply, the pixels (and mask)
// are resampled through a homography into the new shape, leaving an ordinary
// axis-aligned layer over the shape's bounds. A port of Document/Distort.swift.
#pragma once
#include "document.h"
#include <array>
#include <optional>

namespace compositor {

using Corners = std::array<Point, 4>; // handle order: top-left, top-right, bottom-right, bottom-left

/// The transform's corners in handle order.
Corners cornersOf(const LayerTransform& transform);
/// Four finite corners making a convex, non-degenerate shape.
bool cornersUsable(const Corners& corners);

/// The perspective mapping of the unit square onto `corners`, and its inverse.
struct Homography {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // row-major 3x3, maps (u, v, 1)
    Point map(Point unit) const;
    Homography inverted() const;
    static Homography unitTo(const Corners& corners);
};

struct WarpedImage { std::shared_ptr<Image> image; LayerTransform transform; };
struct WarpedMask { std::shared_ptr<GrayImage> image; LayerTransform transform; };

/// `image`, shown through `transform`, resampled so its corners land on `corners`: the warped
/// pixels over the shape's whole-pixel bounds and the axis-aligned transform placing them.
/// `limit` (> 0) caps the longest side, for previews. Fails for unusable corners or sizes.
/// Pass the shared image when there is one: its reductions are then cached across calls (previews, repeated warps).
std::optional<WarpedImage> warpImage(const ImagePtr& image, const LayerTransform& transform, const Corners& corners, int limit = 0);
std::optional<WarpedImage> warpImage(const Image& image, const LayerTransform& transform, const Corners& corners, int limit = 0);
/// The same, cropped to the pixels that are actually there. `crop` reports the crop in the warp's pixels.
std::optional<WarpedImage> warpImageTrimmed(const ImagePtr& image, const LayerTransform& transform, const Corners& corners, Rect* crop = nullptr);
std::optional<WarpedImage> warpImageTrimmed(const Image& image, const LayerTransform& transform, const Corners& corners, Rect* crop = nullptr);
/// A mask warped the same way, `background` (its tone past its pixels) outside the shape.
std::optional<WarpedMask> warpMask(const GrayPtr& mask, const LayerTransform& transform, const Corners& corners, uint8_t background, int limit = 0);
std::optional<WarpedMask> warpMask(const GrayImage& mask, const LayerTransform& transform, const Corners& corners, uint8_t background, int limit = 0);
/// Where `placement`'s corners land when the perspective taking `by`'s corners to `corners` is applied to it too.
Corners carriedCorners(const LayerTransform& placement, const LayerTransform& by, const Corners& corners);
/// Document coverage carried by the same perspective: `coverage(doc) -> coverage(warped doc)`. The result has the
/// document's size; the mapping is from the pixel grid placed by `original` to `corners`.
std::shared_ptr<GrayImage> warpCoverage(const GrayImage& coverage, const LayerTransform& original, int pixelWidth, int pixelHeight, const Corners& corners);

} // namespace compositor
