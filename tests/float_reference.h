// A double-precision compositing reference for 32-bit documents (docs/bit-depth.md, "32 bits"): the W3C / PDF blend
// formulas written out plainly, with nothing shared with the renderer, so the float executor can be checked against it
// within 1e-5 (absolute and relative) rather than against an 8-bit render. What it draws: a flat stack of pixel layers
// placed at whole pixels at 1:1, each with its opacity, blend mode and a pixel mask on the layer's own grid, over
// transparency; folders that isolate their children; and adjustment layers in Normal mode without a mask.
//
// The editing kernels of 32-bit documents (P5b) are written out here too, in double: the adjustments of Photoshop's
// 32-bit set through the document's encoding, the blurs, Add Noise, Lens Correction and the separable resampler. They
// share nothing with the kernels but the settings' own definitions (LevelsRange::normalized, the Curves points,
// HueSaturationSettings::adjust, FilterSettings::normalized).
#pragma once
#include "compositor/adjustments.h"
#include "compositor/document.h"
#include "compositor/filters.h"
#include "compositor/resample.h"
#include <array>
#include <vector>

namespace float_reference {

/// Premultiplied RGBA per pixel, rows top-down.
struct Canvas {
    int width = 0, height = 0;
    std::vector<double> pixels;   // width * height * 4
    double* at(int x, int y) { return &pixels[(size_t(y) * size_t(width) + size_t(x)) * 4]; }
    const double* at(int x, int y) const { return &pixels[(size_t(y) * size_t(width) + size_t(x)) * 4]; }
};

/// A document's encoding as the reference models it: sRGB's formula, or a pure power law.
struct Encoding { bool srgb = true; double gamma = 2.2; };
/// Linear to encoded and back, continued above 1 along the same formula; negative is 0.
double encode(const Encoding& encoding, double linear);
double decode(const Encoding& encoding, double encoded);

/// B(cb, cs) for a separable mode, in double (inputs clamped to 0..1 for the modes outside Photoshop's 32-bit set).
double blendChannel(compositor::BlendMode mode, double cb, double cs);
/// One source pixel (premultiplied) over one backdrop pixel with coverage `k`, in `mode`; `luma` are the Y weights.
void composite(compositor::BlendMode mode, const double src[4], double k, double dst[4], const std::array<double, 3>& luma);
/// The document (32-bit) drawn as described above. Layers the reference does not model are an error (empty result).
Canvas render(const compositor::Document& document, const std::array<double, 3>& luma, const Encoding& encoding = {});

/// A float image as a canvas.
Canvas canvasOf(const compositor::ImageF& image);
/// A 32-bit adjustment on premultiplied linear colour (adjustments_f32.cpp's rules); false for a kind not modelled.
bool adjust(Canvas& canvas, const compositor::AdjustmentSettings& settings, const Encoding& encoding);
/// Gaussian blur with the kernel's taps (radius ceil(3 sigma), weights normalised), zero outside.
void gaussianBlur(Canvas& canvas, double sigma);
/// The recursive Gaussian the kernel uses above sigma 6: Deriche's fourth-order fit, causal and anticausal, zero outside.
void gaussianBlurRecursive(Canvas& canvas, double sigma);
/// Motion blur: shear, box, shear back, with the kernel's geometry and linear interpolation.
void motionBlur(Canvas& canvas, double distance, double angleDegrees);
/// Add Noise with the kernel's pattern for `seed`, on the encoded colour.
void addNoise(Canvas& canvas, double amount, bool gaussian, bool monochromatic, uint32_t seed, const Encoding& encoding);
/// Lens Correction's remove-distortion resampling, zero outside.
void lensCorrection(Canvas& canvas, double distortion, bool bicubic);
/// The separable resampler (resample.h) with exact weights.
Canvas resample(const Canvas& canvas, int width, int height, double originX, double stepX, double originY, double stepY, compositor::ResampleFilter filter);

/// One round brush dab on a layer at 1:1 at the origin (P5c, the 32-bit brush), in double with the profile written out:
/// a hard tip's rim antialiased over one pixel, a soft tip's falloff between the hardness radius and the rim (the
/// exp(-2.5 u^2) curve brush.cpp names), the coverage times `opacity` and the selection (`selection` per pixel, 0..1,
/// or none), and each premultiplied sample moved towards `colour` (linear, alpha 1) by it, or scaled down when
/// erasing. Pixel centres outside the canvas (the canvas is the layer) are not touched.
void brushDab(Canvas& canvas, double cx, double cy, double diameter, double hardness, double opacity, const double colour[3], bool erase,
              const std::vector<double>* selection);

/// The largest difference between the renderer's output and the reference, scaled as |a - b| / (1e-5 + 1e-5 |b|): at
/// most 1 means within 1e-5 absolute plus 1e-5 relative everywhere.
double worstError(const compositor::ImageF& image, const Canvas& reference);

} // namespace float_reference
