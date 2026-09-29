// Resampling pixels of any layout (docs/color-modes.md, "Transforms and resampling"): Image Size, Distort, Warp, the
// warp cage and Free Transform of selected pixels in CMYK and Lab documents, on their own samples.
//
// Lab (4 samples, a and b offset) is resampled by the RGB samplers as it is: premultiplied samples interpolate linearly,
// and a weighted mean of offset values is the offset of the weighted mean, so a and b come out as the mean of the
// neighbours' a and b. CMYK (5 samples) goes through the same samplers twice, as (C, M, Y, alpha) and (K, K, K, alpha):
// both passes see the same alpha at the same positions, so every sample is weighed exactly as the RGB samplers weigh a
// channel, and the RGB paths are the code they were.
#pragma once
#include "imaget.h"
#include "warp.h"
#include "warpmesh.h"
#include "resample.h"
#include <optional>

namespace compositor {

/// A resampled buffer of any layout and the transform placing it.
struct WarpedAny { AnyImage image; LayerTransform transform; };

/// Whether `image` holds 5 samples a pixel (CMYK, at 8 or 16 bits).
bool isFiveSample(const AnyImage& image);

/// A 5-sample buffer split into the two 4-sample buffers the samplers take, (C, M, Y, alpha) and (K, K, K, alpha), and
/// joined back (alpha from the first). Exposed for the tests.
std::pair<std::shared_ptr<Image>, std::shared_ptr<Image>> splitFiveSample(const ImageC8& image);
std::pair<std::shared_ptr<Image16>, std::shared_ptr<Image16>> splitFiveSample(const Image16& image);
std::shared_ptr<ImageC8> joinFiveSample(const Image& cmy, const Image& black);
std::shared_ptr<Image16> joinFiveSample(const Image16& cmy, const Image16& black);

/// warpImage, warpImageTrimmed and resampleLayer (warp.h, render.h) for a buffer of any depth and layout.
std::optional<WarpedAny> warpImageAny(const AnyImage& image, const LayerTransform& transform, const Corners& corners, int limit = 0);
std::optional<WarpedAny> warpImageTrimmedAny(const AnyImage& image, const LayerTransform& transform, const Corners& corners, Rect* crop = nullptr);
AnyImage resampleLayerAny(const AnyImage& image, const LayerTransform& transform, const LayerTransform& target, int width, int height);
/// renderWarpedImage and renderWarpedOverBox (warpmesh.h) for a buffer of any depth and layout.
std::optional<WarpedAny> renderWarpedImageAny(const AnyImage& image, const WarpMesh& mesh, const std::array<double, 8>& quad, const Rect* clip = nullptr);
std::optional<WarpedAny> renderWarpedOverBoxAny(const AnyImage& image, const WarpMesh& mesh, const Rect& box);
/// resampleAxisAligned (resample.h) for an 8- or 16-bit buffer of any layout (32 bits too).
AnyImage resampleAxisAlignedAny(const AnyImage& image, int width, int height, double originX, double stepX, double originY, double stepY, ResampleFilter filter);

/// `onto` with `over` composited Normal over it at full opacity (premultiplied source-over, every sample), both of the
/// same layout and size: merging a transformed selection back into its layer.
AnyImage compositeOverAny(const AnyImage& over, const AnyImage& onto);
/// `image` placed at (`x`, `y`) in a transparent buffer of `width` x `height` at its layout.
AnyImage placeInAny(const AnyImage& image, int width, int height, int x, int y);

} // namespace compositor
