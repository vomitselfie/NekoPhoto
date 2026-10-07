// Filters ported from PhotoCraft (https://github.com/storytold/photocraft, crates/algo/src/distort.rs, distort2.rs,
// other.rs, stylize.rs and crates/engine/src/extra_cmds.rs at commit b07a2e5).
// Copyright (c) 2026 ArtCraft Team and the PhotoCraft contributors. MIT License (LICENSES/PhotoCraft-MIT.txt).
//
// They work on a float plane of straight colour with alpha last, as PhotoCraft's do; filters_grid.cpp moves a
// layer's pixels in and out of it at any depth and layout. Internal to the core.
#pragma once
#include <cstdint>
#include <vector>

namespace compositor::photocraft {

/// Where a sample past the reference rectangle comes from.
enum class Edge { Transparent, Repeat, Wrap };

struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    bool contains(int x, int y) const { return x >= x0 && y >= y0 && x < x1 && y < y1; }
};

/// Straight colour, `channels` samples a pixel with alpha last, 0..1 (a 32-bit layer's may pass 1).
struct Plane {
    int width = 0, height = 0, channels = 4;
    std::vector<float> data;
    Plane() = default;
    Plane(int w, int h, int n) : width(w), height(h), channels(n), data(size_t(w) * size_t(h) * size_t(n), 0.0f) {}
    float* at(int x, int y) { return data.data() + (size_t(y) * size_t(width) + size_t(x)) * size_t(channels); }
    const float* at(int x, int y) const { return data.data() + (size_t(y) * size_t(width) + size_t(x)) * size_t(channels); }
    /// Sample `c` at (x, y); 0 past the plane.
    float get(int x, int y, int c) const { return x >= 0 && y >= 0 && x < width && y < height ? at(x, y)[c] : 0.0f; }
    /// The same with `edge` applied past `area`.
    float getEdge(int x, int y, int c, Edge edge, const Rect& area) const;
    /// Bilinear sample at (x, y) (pixel centres at .5), weighted by alpha: straight colour and alpha into `out`.
    void sample(float x, float y, Edge edge, const Rect& area, float* out) const;
};

enum class SpherizeMode { Normal, HorizontalOnly, VerticalOnly };
enum class WaveType { Sine, Triangle, Square };
enum class RippleSize { Small, Medium, Large };
enum class PolarMode { RectangularToPolar, PolarToRectangular };
enum class ZigZagStyle { AroundCenter, OutFromCenter, PondRipples };

// The distortions write `out` inside `bounds` only (the rest is left as it is); `out` starts as a copy of `src`.
void twirl(const Plane& src, Plane& out, const Rect& bounds, float angle);
void pinch(const Plane& src, Plane& out, const Rect& bounds, float amount);
void spherize(const Plane& src, Plane& out, const Rect& bounds, float amount, SpherizeMode mode);
struct WaveSpec {
    uint32_t generators = 5;
    float wavelengthMin = 10, wavelengthMax = 120, amplitudeMin = 5, amplitudeMax = 35;
    WaveType type = WaveType::Sine;
    Edge undefined = Edge::Wrap;
    uint32_t seed = 0;
};
void wave(const Plane& src, Plane& out, const Rect& bounds, const WaveSpec& spec);
void ripple(const Plane& src, Plane& out, const Rect& bounds, float amount, RippleSize size);
void polar(const Plane& src, Plane& out, const Rect& bounds, PolarMode mode);
void zigzag(const Plane& src, Plane& out, const Rect& bounds, float amount, float ridges, ZigZagStyle style);
/// Shear through the curve [0, 0], [0.5, amount / 100], [1, 0] (offset in half-widths, top to bottom).
void shear(const Plane& src, Plane& out, const Rect& bounds, float amount, Edge undefined);
/// Shifts the bounds' content by whole pixels.
void offset(const Plane& src, Plane& out, const Rect& bounds, int dx, int dy, Edge undefined);
/// Minimum or Maximum on every sample over a square (`round` false) or a disc of `radius`.
void minMax(const Plane& src, Plane& out, float radius, bool max, bool round);
/// Find Edges on the first `colours` samples: Sobel magnitude, dark edges on white; alpha kept.
void findEdges(const Plane& src, Plane& out, int colours);
/// Clouds' value at (x, y): smooth value noise over eight octaves, 0..1.
float cloudsValue(float x, float y, float base, uint32_t seed);
/// Clouds' cell for a document whose longest side is `side`.
float cloudsBase(int side);

} // namespace compositor::photocraft
