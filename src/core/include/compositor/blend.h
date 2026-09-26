// Blend modes with the PDF / Photoshop definitions, on straight (unpremultiplied)
// colour in 0..1. Used by every compositing path: canvas, export, thumbnails.
#pragma once
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

} // namespace compositor
