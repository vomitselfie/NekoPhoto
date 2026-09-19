// Optimised per-pixel kernels. The portable C routines under Compositor/Rendering are
// the reference implementations (and stay unchanged); these are the versions the editor
// runs: row-parallel, integer where the reference used float, and laid out so the
// compiler can vectorise them. tests/kernel_tests.cpp holds each one to its reference.
#pragma once
#include "image.h"
#include <cstdint>

namespace compositor::kernels {

/// Per-channel byte lookup tables (straight 0..255 in, 0..255 out) applied to premultiplied pixels.
struct ChannelTables {
    uint8_t lut[3][256];
};
/// Levels, Curves and Exposure: unpremultiply once per pixel, look up, re-premultiply. Bit-identical to the
/// float reference for opaque pixels, within 1 LSB elsewhere.
void applyChannelTables(Image& image, const ChannelTables& tables);

/// Hue/Saturation: a 33-point colour cube of straight RGB in 8.8 fixed point (0..65280), laid out
/// [blue][green][red][channel], sampled tetrahedrally (four corners, exact on the greys and on the
/// r = g, g = b and r = b creases where trilinear interpolation bends).
constexpr int cubeDim = 33;
void applyColorCube(Image& image, const uint16_t* cube);
/// Colorize: the output colour depends only on the pixel's lightness (max + min) / 2, so a 511-entry
/// table of straight RGB in 8.8 fixed point keyed by max + min of the straight channels is exact.
void applyLightnessTable(Image& image, const uint16_t* table);

/// Add Noise: the reference's hash-based uniform or Gaussian noise, row-parallel and with the monochromatic
/// value computed once per pixel. Identical output to the reference.
void addNoise(Image& image, float amount, bool gaussian, bool monochromatic, uint32_t seed);

/// Lens Correction: `destination = source` resampled through the radial model `scale = 1 - k r^2`
/// (r normalised to the half diagonal), bilinear, transparent outside. Row-parallel; identical to the reference.
void lensDistort(const Image& source, Image& destination, double k, bool bicubic = false);

/// Gradient Map: Rec. 709 luma of the straight colour indexes a 256 x RGB table. One division per pixel
/// (luma is linear in the premultiplied channels); the index can differ from the reference by one at partial alpha.
void gradientMap(Image& image, const uint8_t* table);

/// Invert the colour of premultiplied pixels (`c' = a - c`), four pixels per vector.
void invertColors(Image& image);

} // namespace compositor::kernels
