// Blurs. Gaussian: a separable FIR for small sigma and Deriche's fourth-order recursive fit
// above it (within a level of the true kernel at any sigma, at a cost independent of it), both
// without transposes: the vertical passes run over column bands so every inner loop is a
// contiguous vector. Motion: shear, one running-sum box, shear back, so the cost no longer
// depends on the distance. Outside the image is transparent (zero).
#pragma once
#include "image.h"
#include "imaget.h"
#include <cstddef>

namespace nekophoto {

/// Gaussian blur of premultiplied RGBA in place; sigma in pixels.
void gaussianBlur(Image& image, double sigma);
/// The same for an 8-bit mask or coverage raster.
void gaussianBlur(GrayImage& image, double sigma);
/// The same at 16 bits (0..32768).
void gaussianBlur(Image16& image, double sigma);
void gaussianBlur(Gray16& image, double sigma);
/// The same at 32 bits (premultiplied linear float, unrounded; blur_f32.cpp).
void gaussianBlur(ImageF& image, double sigma);
void gaussianBlur(GrayF& image, double sigma);
/// The most of the row pass's floats a Gaussian keeps at once (default 64 MB): a larger image is blurred a slice of
/// columns at a time, with the very same result. Settable for tests, which slice small images; 0 restores the default.
size_t gaussianSliceBytes();
void setGaussianSliceBytes(size_t bytes);

/// Photoshop's Motion Blur: an even smear along `distance` pixels at `angleDegrees` (counterclockwise from
/// horizontal, y down), in place.
void motionBlur(Image& image, double distance, double angleDegrees);
void motionBlur(Image16& image, double distance, double angleDegrees);
void motionBlur(ImageF& image, double distance, double angleDegrees);

} // namespace nekophoto
