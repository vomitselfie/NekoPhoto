// Resampling: the point samplers layers are drawn and warped with, and a separable filter for pure
// scaling (Image Size, axis-aligned distorts) that replaces the box-mip-plus-bilinear chain with the
// kernel the Mac's vImage path uses. Everything is 8.8 fixed point over premultiplied bytes.
#pragma once
#include "image.h"
#include "transform.h"
#include <cstdint>
#include <memory>

namespace compositor {

/// Bilinear sample at continuous pixel coordinates (pixel i spans [i, i + 1)), clamped to the edge;
/// within a level of the float formula it replaces.
void sampleBilinear(const Image& image, double x, double y, uint8_t out[4]);
/// Catmull-Rom bicubic: sharper magnification than bilinear and exact on linear ramps; the overshoot of the
/// negative lobes is clamped and colour is kept within alpha.
void sampleBicubic(const Image& image, double x, double y, uint8_t out[4]);
int sampleGrayBilinear(const GrayImage& image, double x, double y);
int sampleGrayBicubic(const GrayImage& image, double x, double y);

/// The Catmull-Rom weights for a fraction in 1/256ths: four taps at offsets -1, 0, 1, 2 summing to 256.
const int16_t* catmullRomWeights(int fraction256);

enum class ResampleFilter { Triangle, CatmullRom, Lanczos3 };
/// The filter a sampling mode resizes with: Lanczos-3 for High, a triangle (area-weighted bilinear) for Smooth.
ResampleFilter filterFor(Sampling sampling);

/// Separable resample for a scale and translation: output pixel x's centre lies at source coordinate
/// originX + x * stepX (likewise y; a negative step mirrors). The kernel is widened by the reduction so
/// minification is antialiased without mips; the image's edge gets a one-output-pixel ramp to transparent (or to `outside` for masks).
std::shared_ptr<Image> resampleAxisAligned(const Image& image, int width, int height, double originX, double stepX, double originY, double stepY, ResampleFilter filter);
std::shared_ptr<GrayImage> resampleAxisAligned(const GrayImage& mask, int width, int height, double originX, double stepX, double originY, double stepY, ResampleFilter filter, uint8_t outside);

} // namespace compositor
