// Selection shape operations on 8-bit coverage rasters (white = selected): exact Euclidean
// distance transforms for Expand, Contract and Border, a disc majority for Smooth, and a
// Gaussian for Feather. Photoshop's kernels for these are circular; so are ours.
#pragma once
#include "image.h"
#include <memory>
#include <vector>

namespace compositor {

/// Squared Euclidean distance from every pixel to the nearest pixel where `inside(value)` holds
/// (Felzenszwalb-Huttenlocher, exact, O(N), parallel). Pixels that are themselves inside get 0.
/// `selected` chooses the target set: true = distance to selected pixels (coverage >= 128), false = to unselected.
std::vector<float> squaredDistanceTransform(const GrayImage& coverage, bool selected);

/// Expand (amount > 0) or contract (amount < 0) by a circular kernel of that radius, with a one-pixel
/// antialiased rim. The input is read as selected at coverage >= 128.
std::shared_ptr<GrayImage> growSelection(const GrayImage& coverage, int amount);
/// Photoshop's Smooth: a pixel is selected when more than half the pixels within `radius` are.
std::shared_ptr<GrayImage> smoothSelection(const GrayImage& coverage, int radius);
/// Photoshop's Border: a soft band `width` pixels wide centred on the selection edge.
std::shared_ptr<GrayImage> borderSelection(const GrayImage& coverage, int width);
/// Photoshop's Feather: a Gaussian with sigma = `radius` over the coverage.
std::shared_ptr<GrayImage> featherSelection(const GrayImage& coverage, double radius);

} // namespace compositor
