// Blurs. Gaussian: a separable FIR for small sigma and Deriche's fourth-order recursive fit
// above it (within a level of the true kernel at any sigma, at a cost independent of it), both
// without transposes: the vertical passes run over column bands so every inner loop is a
// contiguous vector. Motion: shear, one running-sum box, shear back, so the cost no longer
// depends on the distance. Outside the image is transparent (zero).
#pragma once
#include "image.h"

namespace compositor {

/// Gaussian blur of premultiplied RGBA in place; sigma in pixels.
void gaussianBlur(Image& image, double sigma);
/// The same for an 8-bit mask or coverage raster.
void gaussianBlur(GrayImage& image, double sigma);

/// Photoshop's Motion Blur: an even smear along `distance` pixels at `angleDegrees` (counterclockwise from
/// horizontal, y down), in place.
void motionBlur(Image& image, double distance, double angleDegrees);

} // namespace compositor
