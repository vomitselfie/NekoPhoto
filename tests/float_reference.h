// A double-precision compositing reference for 32-bit documents (docs/bit-depth.md, "32 bits"): the W3C / PDF blend
// formulas written out plainly, with nothing shared with the renderer, so the float executor can be checked against it
// within 1e-5 (absolute and relative) rather than against an 8-bit render. What it draws: a flat stack of pixel layers
// placed at whole pixels at 1:1, each with its opacity, blend mode and a pixel mask on the layer's own grid, over
// transparency; and folders that isolate their children.
#pragma once
#include "compositor/document.h"
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

/// B(cb, cs) for a separable mode, in double (inputs clamped to 0..1 for the modes outside Photoshop's 32-bit set).
double blendChannel(compositor::BlendMode mode, double cb, double cs);
/// One source pixel (premultiplied) over one backdrop pixel with coverage `k`, in `mode`; `luma` are the Y weights.
void composite(compositor::BlendMode mode, const double src[4], double k, double dst[4], const std::array<double, 3>& luma);
/// The document (32-bit) drawn as described above. Layers the reference does not model are an error (empty result).
Canvas render(const compositor::Document& document, const std::array<double, 3>& luma);

/// The largest difference between the renderer's output and the reference, scaled as |a - b| / (1e-5 + 1e-5 |b|): at
/// most 1 means within 1e-5 absolute plus 1e-5 relative everywhere.
double worstError(const compositor::ImageF& image, const Canvas& reference);

} // namespace float_reference
