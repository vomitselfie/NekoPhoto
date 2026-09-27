// The shader runtime the Mosh effects run on (compositor/mosh.h): WGSL's vector types and built-ins in float, the
// texture sampler OpenMosh binds (bilinear, clamp to edge), its common.wgsl helpers, and fragment passes run over
// the worker pool. Ported from OpenMosh (MIT, crates/mosh-engine/shaders/common.wgsl; see THIRD-PARTY-NOTICES.md).
//
// Arithmetic is single precision in the shaders' evaluation order, so the hashes that feed on float bit patterns
// (rand2) see the bits the GPU saw. Where a GPU's compiler fuses a multiply and an add feeding a hash, or divides by
// multiplying with the reciprocal, the effect does the same (fma32, a reciprocal); this file's users are built with
// -ffp-contract=off so the C++ compiler fuses nothing of its own. sin, cos and atan2 are computed here in double from fixed polynomials, not taken
// from the C library, so the pixels are the same on every platform.
#pragma once
#include "compositor/image.h"
#include "compositor/imaget.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace compositor::mosh::rt {

// ---- vectors ---------------------------------------------------------------------------------------------------

struct vec2 { float x = 0, y = 0; };
struct vec3 { float x = 0, y = 0, z = 0; };
struct vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    vec3 rgb() const { return {x, y, z}; }
};
inline vec2 v2(float s) { return {s, s}; }
inline vec3 v3(float s) { return {s, s, s}; }
inline vec4 v4(vec3 c, float a) { return {c.x, c.y, c.z, a}; }

// Written out rather than generated: the component order of each operation is part of the port.
inline vec2 operator+(vec2 a, vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline vec2 operator-(vec2 a, vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline vec2 operator*(vec2 a, vec2 b) { return {a.x * b.x, a.y * b.y}; }
inline vec2 operator/(vec2 a, vec2 b) { return {a.x / b.x, a.y / b.y}; }
inline vec2 operator+(vec2 a, float s) { return {a.x + s, a.y + s}; }
inline vec2 operator-(vec2 a, float s) { return {a.x - s, a.y - s}; }
inline vec2 operator*(vec2 a, float s) { return {a.x * s, a.y * s}; }
inline vec2 operator/(vec2 a, float s) { return {a.x / s, a.y / s}; }
inline vec2 operator*(float s, vec2 a) { return {s * a.x, s * a.y}; }
inline vec2 operator-(float s, vec2 a) { return {s - a.x, s - a.y}; }
inline vec2 operator/(float s, vec2 a) { return {s / a.x, s / a.y}; }
inline vec2 operator-(vec2 a) { return {-a.x, -a.y}; }
inline vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline vec3 operator*(vec3 a, vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline vec3 operator*(vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline vec3 operator/(vec3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
inline vec3 operator+(vec3 a, float s) { return {a.x + s, a.y + s, a.z + s}; }
inline vec3 operator-(vec3 a, float s) { return {a.x - s, a.y - s, a.z - s}; }
inline vec3 operator-(float s, vec3 a) { return {s - a.x, s - a.y, s - a.z}; }
inline vec3 operator*(float s, vec3 a) { return {s * a.x, s * a.y, s * a.z}; }
inline vec4 operator+(vec4 a, vec4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline vec4 operator*(vec4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
inline vec4 operator/(vec4 a, float s) { return {a.x / s, a.y / s, a.z / s, a.w / s}; }
inline vec4 operator-(vec4 a, vec4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline vec3 operator/(vec3 a, vec3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }

// ---- built-ins ---------------------------------------------------------------------------------------------------

inline float fract(float v) { return v - std::floor(v); }
inline vec2 floor(vec2 v) { return {std::floor(v.x), std::floor(v.y)}; }
inline vec2 fract(vec2 v) { return {fract(v.x), fract(v.y)}; }
inline vec3 floor(vec3 v) { return {std::floor(v.x), std::floor(v.y), std::floor(v.z)}; }
inline float clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
inline vec2 clamp(vec2 v, float lo, float hi) { return {clamp(v.x, lo, hi), clamp(v.y, lo, hi)}; }
inline vec3 clamp(vec3 v, float lo, float hi) { return {clamp(v.x, lo, hi), clamp(v.y, lo, hi), clamp(v.z, lo, hi)}; }
inline vec2 max(vec2 a, vec2 b) { return {std::max(a.x, b.x), std::max(a.y, b.y)}; }
inline vec3 max(vec3 a, vec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline vec3 min(vec3 a, vec3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
/// WGSL's mix: a * (1 - t) + b * t.
inline float mix(float a, float b, float t) { return a * (1.0f - t) + b * t; }
inline vec3 mix(vec3 a, vec3 b, float t) { return {mix(a.x, b.x, t), mix(a.y, b.y, t), mix(a.z, b.z, t)}; }
inline vec3 mix(vec3 a, vec3 b, vec3 t) { return {mix(a.x, b.x, t.x), mix(a.y, b.y, t.y), mix(a.z, b.z, t.z)}; }
inline vec4 mix(vec4 a, vec4 b, float t) { return {mix(a.x, b.x, t), mix(a.y, b.y, t), mix(a.z, b.z, t), mix(a.w, b.w, t)}; }
inline float step(float edge, float v) { return v >= edge ? 1.0f : 0.0f; }
inline float smoothstep(float e0, float e1, float v) {
    float t = clamp((v - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
inline float dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
inline float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(vec2 v) { return std::sqrt(dot(v, v)); }
inline float distance(vec2 a, vec2 b) { return length(a - b); }
inline vec3 abs(vec3 v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }
/// WGSL's i32(f): truncation toward zero, saturating (as WGSL does) for values past the range.
inline int32_t toI32(float v) {
    if (!(v == v)) return 0;
    if (v >= 2147483520.0f) return INT32_MAX;
    if (v <= -2147483648.0f) return INT32_MIN;
    return int32_t(v);
}
/// WGSL's u32(f): truncation, saturating at 0 and the top.
inline uint32_t toU32(float v) {
    if (!(v > 0)) return 0;
    if (v >= 4294967040.0f) return UINT32_MAX;
    return uint32_t(v);
}
/// a * b + c rounded once, as a GPU's fused multiply-add: the product is exact in double, and the sum is rounded to
/// double and then to float (IEEE operations only, so the same on every CPU).
inline float fma32(float a, float b, float c) { return float(double(a) * double(b) + double(c)); }
inline vec2 fma32(vec2 a, vec2 b, vec2 c) { return {fma32(a.x, b.x, c.x), fma32(a.y, b.y, c.y)}; }
inline uint32_t bitsOf(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; }

// sin, cos, atan2, exp and log in double from fixed polynomials: IEEE arithmetic only, so every platform gets the same
// bits.
double sinD(double x);
double cosD(double x);
double atan2D(double y, double x);
double expD(double x);
double logD(double x);
/// WGSL's pow: exp(y log x), for x >= 0 (0 to a positive power is 0; a negative x gives NaN).
double powD(double x, double y);
inline float sin(float v) { return float(sinD(v)); }
inline float cos(float v) { return float(cosD(v)); }
inline float atan2(float y, float x) { return float(atan2D(y, x)); }
inline float exp(float v) { return float(expD(v)); }
inline float pow(float x, float y) { return float(powD(x, y)); }
inline vec3 pow(vec3 x, vec3 y) { return {pow(x.x, y.x), pow(x.y, y.y), pow(x.z, y.z)}; }

// ---- common.wgsl -------------------------------------------------------------------------------------------------

/// PCG-based 2D hash (Jarzynski & Olano).
inline void pcg2d(uint32_t& x, uint32_t& y) {
    x = x * 1664525u + 1013904223u;
    y = y * 1664525u + 1013904223u;
    x += y * 1664525u;
    y += x * 1664525u;
    x ^= x >> 16;
    y ^= y >> 16;
    x += y * 1664525u;
    y += x * 1664525u;
    x ^= x >> 16;
    y ^= y >> 16;
}
/// A uniform number in [0, 1] from a point's float bits.
inline float rand2(vec2 co) {
    uint32_t x = bitsOf(co.x), y = bitsOf(co.y);
    pcg2d(x, y);
    return float(x) * (1.0f / 4294967296.0f);
}
inline float luma(vec3 c) { return dot(c, vec3{0.299f, 0.587f, 0.114f}); }
/// rot2(a) * v with WGSL's column-major mat2x2(vec2(c, s), vec2(-s, c)).
inline vec2 rotate(float a, vec2 v) {
    float c = cos(a), s = sin(a);
    return {c * v.x + -s * v.y, s * v.x + c * v.y};
}
vec3 hsv2rgb(vec3 c);
vec3 rgb2hsv(vec3 c);
/// Smooth value noise in [0, 1].
inline float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 s = f * f * (3.0f - 2.0f * f);
    float a = rand2(i);
    float b = rand2(i + vec2{1.0f, 0.0f});
    float c = rand2(i + vec2{0.0f, 1.0f});
    float d = rand2(i + vec2{1.0f, 1.0f});
    return mix(mix(a, b, s.x), mix(c, d, s.x), s.y);
}
/// 4x4 Bayer ordered-dither threshold in [0, 1).
inline float bayer4(vec2 px) {
    static constexpr float m[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    uint32_t x = toU32(px.x) & 3u, y = toU32(px.y) & 3u;
    return m[y * 4u + x] / 16.0f;
}

// ---- textures ----------------------------------------------------------------------------------------------------

/// Straight RGBA at 16 bits a channel (0..65535): an 8-bit value v is stored as v * 257, exactly v / 255 when read.
class Frame {
public:
    Frame() = default;
    Frame(int width, int height) : width_(width), height_(height), pixels_(size_t(width) * height * 4, 0) {}
    int width() const { return width_; }
    int height() const { return height_; }
    uint16_t* row(int y) { return pixels_.data() + size_t(y) * width_ * 4; }
    const uint16_t* row(int y) const { return pixels_.data() + size_t(y) * width_ * 4; }
    vec4 load(int x, int y) const {
        const uint16_t* p = row(y) + size_t(x) * 4;
        const float* k = unitTable();
        return {k[p[0]], k[p[1]], k[p[2]], k[p[3]]};
    }
    /// v / 65535 for every v, correctly rounded (a multiply by the reciprocal would read 65535 as just under 1).
    static const float* unitTable();
    void store(int x, int y, vec4 c) {
        uint16_t* p = row(y) + size_t(x) * 4;
        p[0] = quantize(c.x); p[1] = quantize(c.y); p[2] = quantize(c.z); p[3] = quantize(c.w);
    }
    static uint16_t quantize(float v) { return uint16_t(clamp(v == v ? v : 0.0f, 0.0f, 1.0f) * 65535.0f + 0.5f); }
private:
    int width_ = 0, height_ = 0;
    std::vector<uint16_t> pixels_;
};

enum class Address { Clamp, Repeat };
inline int wrapIndex(int i, int n, Address mode) {
    if (mode == Address::Clamp) return std::min(std::max(i, 0), n - 1);
    int m = i % n;
    return m < 0 ? m + n : m;
}
/// A coordinate in texels, NaN and far-off values held to a range the integer conversion takes.
inline float texel(float uv, int n) {
    float t = uv * float(n) - 0.5f;
    return t == t ? clamp(t, -1.0e6f, 1.0e6f) : 0.0f;
}
/// textureSample / textureSampleLevel with OpenMosh's sampler: bilinear, clamp to edge (or repeat). The weights are
/// rounded to 1/256 of a texel, as GPUs filter (8 bits of sub-texel precision), so a coordinate a hair off a texel's
/// centre reads that texel alone.
inline vec4 sample(const Frame& tex, vec2 uv, Address mode = Address::Clamp) {
    const int64_t tx = int64_t(std::floor(texel(uv.x, tex.width()) * 256.0f + 0.5f));
    const int64_t ty = int64_t(std::floor(texel(uv.y, tex.height()) * 256.0f + 0.5f));
    const int ix = int(tx >> 8), iy = int(ty >> 8);
    const float fx = float(tx & 255) / 256.0f, fy = float(ty & 255) / 256.0f;
    int x0 = wrapIndex(ix, tex.width(), mode), x1 = wrapIndex(ix + 1, tex.width(), mode);
    int y0 = wrapIndex(iy, tex.height(), mode), y1 = wrapIndex(iy + 1, tex.height(), mode);
    vec4 a = tex.load(x0, y0), b = tex.load(x1, y0), c = tex.load(x0, y1), d = tex.load(x1, y1);
    auto lerp = [](vec4 p, vec4 q, float t) { return vec4{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t, p.w + (q.w - p.w) * t}; };
    return lerp(lerp(a, b, fx), lerp(c, d, fx), fy);
}
/// The texel under `uv` (a nearest-filter sampler).
inline vec4 sampleNearest(const Frame& tex, vec2 uv, Address mode = Address::Clamp) {
    float tx = uv.x * float(tex.width()), ty = uv.y * float(tex.height());
    tx = tx == tx ? clamp(tx, -1.0e6f, 1.0e6f) : 0.0f;
    ty = ty == ty ? clamp(ty, -1.0e6f, 1.0e6f) : 0.0f;
    return tex.load(wrapIndex(int(std::floor(tx)), tex.width(), mode), wrapIndex(int(std::floor(ty)), tex.height(), mode));
}

// ---- passes ------------------------------------------------------------------------------------------------------

/// What a fragment shader sees: its uv at the pixel's centre (top-down, as OpenMosh's) and the pixel itself.
struct Frag {
    vec2 uv;
    int x, y;
};

/// OpenMosh's uniforms: the parameters positional in p[0..7], the seed, time (0 for a still image) and, for
/// multi-pass effects, the pass index. With them, the textures a Composite effect reads besides its input: `aux` (an
/// overlay, a mask or a caption's text; a 1x1 transparent texel when there is none, as OpenMosh binds) with the size
/// the effect takes it to be, and `source`, the original the mask effects reveal (null: transparent, so the masked-out
/// pixels show what is beneath the layer).
struct Uniforms {
    vec2 resolution;
    float time = 0;
    float seed = 0;
    float p[8] = {};
    int pass = 0;
    const Frame* aux = nullptr;
    vec2 auxSize{1.0f, 1.0f};
    const Frame* source = nullptr;
};

/// Runs `shader(Frag) -> vec4` over every pixel of a width x height target, rows on the worker pool, handing each
/// finished row to `sink(y, row)`. Every pixel is computed on its own, so the thread count never changes a result.
template <class Shader, class Sink>
void shade(int width, int height, Shader&& shader, Sink&& sink) {
    parallelRows(0, height, [&](int y0, int y1) {
        std::vector<vec4> row(static_cast<size_t>(width));
        for (int y = y0; y < y1; y++) {
            float v = (float(y) + 0.5f) / float(height);
            for (int x = 0; x < width; x++) row[size_t(x)] = shader(Frag{{(float(x) + 0.5f) / float(width), v}, x, y});
            sink(y, row.data());
        }
    }, 8);
}
/// A pass into a new frame (the next pass's input: ping-pong).
template <class Shader>
Frame pass(int width, int height, Shader&& shader) {
    Frame out(width, height);
    shade(width, height, shader, [&](int y, const vec4* row) { for (int x = 0; x < width; x++) out.store(x, y, row[x]); });
    return out;
}

// ---- the boundary: premultiplied pixels to straight frames and back ------------------------------------------------

Frame toFrame(const Image& image);
Frame toFrame(const Image16& image);
/// Premultiplies straight `c` into 8 or 16-bit samples, as a GPU writes an Rgba8Unorm target (rounded, clamped).
void storePremultiplied(uint8_t* p, vec4 c);
void storePremultiplied(uint16_t* p, vec4 c);

} // namespace compositor::mosh::rt
