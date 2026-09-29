// Blend modes with the PDF / Photoshop definitions, on straight (unpremultiplied)
// colour in 0..1. Used by every compositing path: canvas, export, thumbnails.
#pragma once
#include "colormodes.h"
#include "document.h"
#include <cstdint>

namespace compositor {

struct Rgb { float r, g, b; };

/// B(cb, cs) for one pixel: the blended colour before compositing with alpha.
Rgb blendColor(BlendMode mode, Rgb backdrop, Rgb source);

/// Composites a premultiplied source pixel over a premultiplied backdrop pixel in place, in `mode`.
/// `src` and `dst` are RGBA8 premultiplied; `coverage` (0..1) scales the source alpha.
void compositePixel(BlendMode mode, const uint8_t* src, float coverage, uint8_t* dst);
/// The same at a pixel position, which Dissolve needs (its random pattern belongs to the pixel grid).
void compositePixelAt(BlendMode mode, const uint8_t* src, float coverage, uint8_t* dst, int x, int y);
/// Coverage quantised to the 0..256 steps every blend uses (256 = full).
unsigned coverageSteps(float coverage);
void compositePixelSteps(BlendMode mode, const uint8_t* src, unsigned steps, uint8_t* dst);
/// Normal-mode source-over of `count` pixels with a coverage step per pixel; identical to the per-pixel path.
void compositeSpanNormal(const uint8_t* src, const uint16_t* steps, uint8_t* dst, int count);

/// For tests: Photoshop's byte kernel for the modes it has one for (Linear Burn ... Divide), computed and through the
/// lookup table the compositor uses; and the float-to-byte rounding the table is read with, and its reference.
uint8_t photoshopBlendByte(BlendMode mode, uint8_t source, uint8_t backdrop);
uint8_t photoshopBlendByteTabled(BlendMode mode, uint8_t source, uint8_t backdrop);
uint8_t blendByteOf(float v);
uint8_t blendByteReference(float v);

/// Composites `source` over `destination` in place (same size), scaling source alpha by `opacity`.
void compositeImage(BlendMode mode, const Image& source, double opacity, Image& destination);

// ---- 16 bits (blend_u16.cpp) ---------------------------------------------------------------------------------
//
// Premultiplied samples in 0..32768 and coverage in 0..32768 steps (32768 = full). Normal and the separable modes
// with a product form are exact 15-bit integer maths; the modes that divide or take a root (the dodge and burn
// family, Soft Light, Hard Mix, Divide) and the non-separable ones blend straight colour in float.

/// Coverage (0..1) as 0..32768 steps.
unsigned coverageSteps16(float coverage);
/// Composites a premultiplied 16-bit source pixel over a backdrop pixel in place, the source scaled by `k` steps.
void compositePixelSteps16(BlendMode mode, const uint16_t* src, unsigned k, uint16_t* dst);
/// The same with coverage 0..1, at a document pixel (Dissolve's pattern is the 8-bit one).
void compositePixelAt16(BlendMode mode, const uint16_t* src, float coverage, uint16_t* dst, int x, int y);
/// `count` pixels in one mode (not Dissolve), a step count per pixel (0 skips it): the mode is chosen once per span.
void compositeSpan16(BlendMode mode, const uint16_t* src, const uint32_t* steps, uint16_t* dst, int count);
/// B(cb, cs) as the 16-bit kernels compute it, for tests: straight colour in 0..1.
float blendChannel16(BlendMode mode, float cb, float cs);

// ---- 32 bits (blend_f32.cpp) ---------------------------------------------------------------------------------
//
// Premultiplied linear float, colour unbounded above, alpha and coverage 0..1: the W3C / PDF compositing formulas,
//     co = cs as (1 - ab) + cb ab (1 - as) + as ab B(cb, cs),   ao = as + ab (1 - as),
// with the source's alpha scaled by the coverage. Photoshop's 32-bit modes (blendModeAt32) blend the values as they
// are, above 1 included; the others clamp their inputs to 0..1 inside B (a file or a converted document still draws;
// the picker greys them). The non-separable modes, Darker Color and Lighter Color take luminance from `luma`, the
// profile's linear Y weights.

/// The modes Photoshop offers in a 32-bit document: Normal, Dissolve, Darken, Multiply, Lighten, Linear Dodge (Add),
/// Difference, Subtract, Divide, Hue, Saturation, Color, Luminosity, Darker Color and Lighter Color.
bool blendModeAt32(BlendMode mode);
/// `count` pixels in one mode (not Dissolve), a coverage per pixel (0 skips it).
void compositeSpanF(BlendMode mode, const float* src, const float* coverage, float* dst, int count, const float luma[3]);
void compositePixelF(BlendMode mode, const float* src, float coverage, float* dst, const float luma[3]);
/// The same at a document pixel: Dissolve's pattern is the 8-bit one.
void compositePixelAtF(BlendMode mode, const float* src, float coverage, float* dst, int x, int y, const float luma[3]);
/// B(cb, cs) for a separable mode as the 32-bit kernels compute it (inputs clamped for the modes outside the 32-bit
/// set), for tests.
float blendChannelF(BlendMode mode, float cb, float cs);
// ---- CMYK and Lab (blend_c8.cpp at 8 bits, blend_modes16.cpp at 16) -----------------------------------------------
//
// A CMYK pixel is 5 samples (inverted ink, then alpha) and a Lab one 4 (L, offset a and b, alpha), premultiplied
// (colormodes.h). The separable modes are the RGB kernels applied per channel on the stored values, which for CMYK
// is Photoshop's own model (inverted ink behaves like light); Normal and Dissolve use the RGB kernels unchanged.
// Lab's Hue, Saturation, Color, Luminosity, Darker Color and Lighter Color work in L and a/b (LCh) directly.
// CMYK's are Photoshop's: the PDF helpers on the C, M and Y complements, K carried apart (blend_modes.inc).

/// Whether Photoshop offers `mode` for layers in documents of `colorMode` (the picker greys the others). RGB: every
/// mode. CMYK: every mode. Lab: all but Color Dodge, Color Burn, Darken, Lighten, Difference, Exclusion, Subtract and
/// Divide (Adobe's "Layer opacity and blending modes" help page).
bool blendModeAvailable(BlendMode mode, ColorMode colorMode);
/// The mode a layer in `mode` is drawn with in a `colorMode` document: itself, or Normal for a mode that is not
/// offered there (Lab's eight).
BlendMode blendModeFor(BlendMode mode, ColorMode colorMode);

/// 8 bits: `count` pixels of `colorMode` (5 samples for CMYK, 4 for Lab), coverage steps 0..256 as the RGB kernels
/// take them (0 skips a pixel). `mode` is what blendModeFor gives (Dissolve takes compositePixelAtMode8).
void compositeSpanMode8(BlendMode mode, ColorMode colorMode, const uint8_t* src, const uint16_t* steps, uint8_t* dst, int count);
/// One pixel with coverage 0..1 at a document position (Dissolve's pattern is the RGB one).
void compositePixelAtMode8(BlendMode mode, ColorMode colorMode, const uint8_t* src, float coverage, uint8_t* dst, int x, int y);
/// 16 bits: the same with samples and steps in 0..32768.
void compositeSpanMode16(BlendMode mode, ColorMode colorMode, const uint16_t* src, const uint32_t* steps, uint16_t* dst, int count);
void compositePixelAtMode16(BlendMode mode, ColorMode colorMode, const uint16_t* src, float coverage, uint16_t* dst, int x, int y);
/// An adjustment layer's blend of straight colour (0..1 per stored channel) in `colorMode`: `cs` becomes B(cb, cs)
/// for the `colorMode` document's layout (C colour channels).
void blendStraightMode(BlendMode mode, ColorMode colorMode, const float* cb, float* cs);

} // namespace compositor
