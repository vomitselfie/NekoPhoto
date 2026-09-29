// The blend modes at 32 bits (blend.h, "32 bits"; docs/bit-depth.md): premultiplied linear float, the W3C Compositing
// and Blending formulas (the PDF blend modes) with Photoshop's own for the modes the W3C lacks (Linear Burn and
// Dodge, Vivid, Linear and Pin Light, Hard Mix, Subtract, Divide, Darker and Lighter Color).
//
// With the source scaled by its coverage (as = sa k), straight colours cs = sc / sa and cb = dc / da, per channel
//     co = sc k (1 - da) + dc (1 - as) + as da B(cb, cs)   (the first two terms premultiplied as they are)
//     ao = as + da (1 - as)
// Photoshop's 32-bit modes take cb and cs unbounded; the others clamp them to 0..1 inside B only, so where a layer
// has nothing under it its colour keeps its range. Nothing here rounds: the double-precision reference in
// tests/float_reference.cpp follows the same formulas.
#include "compositor/blend.h"
#include <algorithm>
#include <cmath>

namespace compositor {

namespace {

inline float screen(float cb, float cs) { return cb + cs - cb * cs; }

inline float colorDodge(float cb, float cs) {
    if (cb <= 0) return 0;
    if (cs >= 1) return 1;
    return std::min(1.0f, cb / (1 - cs));
}

inline float colorBurn(float cb, float cs) {
    if (cb >= 1) return 1;
    if (cs <= 0) return 0;
    return 1 - std::min(1.0f, (1 - cb) / cs);
}

inline float hardLight(float cb, float cs) { return cs <= 0.5f ? cb * 2 * cs : screen(cb, 2 * cs - 1); }

inline float softLight(float cb, float cs) {
    if (cs <= 0.5f) return cb - (1 - 2 * cs) * cb * (1 - cb);
    const float d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : std::sqrt(cb);
    return cb + (2 * cs - 1) * (d - cb);
}

/// B for a separable mode; `cb` and `cs` already clamped for the modes outside the 32-bit set.
inline float separable(BlendMode mode, float cb, float cs) {
    switch (mode) {
    case BlendMode::Multiply: return cb * cs;
    case BlendMode::Screen: return screen(cb, cs);
    case BlendMode::Overlay: return hardLight(cs, cb);
    case BlendMode::Darken: return std::min(cb, cs);
    case BlendMode::Lighten: return std::max(cb, cs);
    case BlendMode::ColorDodge: return colorDodge(cb, cs);
    case BlendMode::ColorBurn: return colorBurn(cb, cs);
    case BlendMode::HardLight: return hardLight(cb, cs);
    case BlendMode::SoftLight: return softLight(cb, cs);
    case BlendMode::Difference: return std::fabs(cb - cs);
    case BlendMode::Exclusion: return cb + cs - 2 * cb * cs;
    case BlendMode::LinearBurn: return std::max(0.0f, cb + cs - 1);
    case BlendMode::LinearDodge: return cb + cs;
    case BlendMode::Subtract: return std::max(0.0f, cb - cs);
    case BlendMode::Divide: return cs <= 0 ? (cb <= 0 ? 0.0f : 1.0f) : cb / cs;
    case BlendMode::VividLight: return cs <= 0.5f ? colorBurn(cb, 2 * cs) : colorDodge(cb, 2 * cs - 1);
    case BlendMode::LinearLight: return std::clamp(cb + 2 * cs - 1, 0.0f, 1.0f);
    case BlendMode::PinLight: return cs < 0.5f ? std::min(cb, 2 * cs) : std::max(cb, 2 * cs - 1);
    case BlendMode::HardMix: return cb + cs >= 1 ? 1.0f : 0.0f;
    default: return cs;
    }
}

bool nonSeparable(BlendMode mode) {
    return mode == BlendMode::Hue || mode == BlendMode::Saturation || mode == BlendMode::Color || mode == BlendMode::Luminosity
        || mode == BlendMode::DarkerColor || mode == BlendMode::LighterColor;
}

inline float lum(const float c[3], const float w[3]) { return w[0] * c[0] + w[1] * c[1] + w[2] * c[2]; }

/// W3C ClipColor. In the 32-bit set (every non-separable mode is) only the lower bound: colour above 1 is light.
void clipColor(float c[3], const float w[3]) {
    const float l = lum(c, w);
    const float n = std::min({c[0], c[1], c[2]});
    if (n < 0 && l - n > 0) for (int k = 0; k < 3; k++) c[k] = l + (c[k] - l) * l / (l - n);
}

void setLum(float c[3], float l, const float w[3]) {
    const float d = l - lum(c, w);
    for (int k = 0; k < 3; k++) c[k] += d;
    clipColor(c, w);
}

inline float sat(const float c[3]) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }

void setSat(float c[3], float s) {
    int mx = 0, mn = 0;
    for (int k = 1; k < 3; k++) { if (c[k] > c[mx]) mx = k; if (c[k] < c[mn]) mn = k; }
    if (mx == mn) { c[0] = c[1] = c[2] = 0; return; }
    const int md = 3 - mx - mn;
    const float range = c[mx] - c[mn];
    c[md] = (c[md] - c[mn]) * s / range;
    c[mx] = s;
    c[mn] = 0;
}

void blendNonSeparable(BlendMode mode, const float cb[3], const float cs[3], const float w[3], float out[3]) {
    float r[3];
    switch (mode) {
    case BlendMode::Hue:
        for (int k = 0; k < 3; k++) r[k] = cs[k];
        setSat(r, sat(cb));
        setLum(r, lum(cb, w), w);
        break;
    case BlendMode::Saturation:
        for (int k = 0; k < 3; k++) r[k] = cb[k];
        setSat(r, sat(cs));
        setLum(r, lum(cb, w), w);
        break;
    case BlendMode::Color:
        for (int k = 0; k < 3; k++) r[k] = cs[k];
        setLum(r, lum(cb, w), w);
        break;
    case BlendMode::Luminosity:
        for (int k = 0; k < 3; k++) r[k] = cb[k];
        setLum(r, lum(cs, w), w);
        break;
    case BlendMode::DarkerColor: {
        const bool source = lum(cs, w) < lum(cb, w);
        for (int k = 0; k < 3; k++) r[k] = source ? cs[k] : cb[k];
        break;
    }
    case BlendMode::LighterColor: {
        const bool source = lum(cs, w) > lum(cb, w);
        for (int k = 0; k < 3; k++) r[k] = source ? cs[k] : cb[k];
        break;
    }
    default:
        for (int k = 0; k < 3; k++) r[k] = cs[k];
        break;
    }
    for (int k = 0; k < 3; k++) out[k] = r[k];
}

inline void compositeNormalF(const float* src, float k, float* dst) {
    const float as = src[3] * k;
    if (!(as > 0)) return;
    const float inv = 1 - as;
    for (int c = 0; c < 3; c++) dst[c] = src[c] * k + dst[c] * inv;
    dst[3] = std::min(1.0f, as + dst[3] * inv);
}

void compositeBlendedF(BlendMode mode, const float* src, float k, float* dst, const float w[3]) {
    const float as = src[3] * k;
    if (!(as > 0)) return;
    const float da = dst[3];
    if (!(da > 0)) { compositeNormalF(src, k, dst); return; }
    const bool unbounded = blendModeAt32(mode);
    float cs[3], cb[3], b[3];
    for (int c = 0; c < 3; c++) {
        cs[c] = src[c] / src[3];
        cb[c] = dst[c] / da;
        if (!unbounded) { cs[c] = std::clamp(cs[c], 0.0f, 1.0f); cb[c] = std::clamp(cb[c], 0.0f, 1.0f); }
    }
    if (nonSeparable(mode)) blendNonSeparable(mode, cb, cs, w, b);
    else for (int c = 0; c < 3; c++) b[c] = separable(mode, cb[c], cs[c]);
    for (int c = 0; c < 3; c++) dst[c] = src[c] * k * (1 - da) + dst[c] * (1 - as) + as * da * b[c];
    dst[3] = std::min(1.0f, as + da * (1 - as));
}

} // namespace

bool blendModeAt32(BlendMode mode) {
    switch (mode) {
    case BlendMode::Normal: case BlendMode::Dissolve: case BlendMode::Darken: case BlendMode::Multiply: case BlendMode::Lighten:
    case BlendMode::LinearDodge: case BlendMode::Difference: case BlendMode::Subtract: case BlendMode::Divide: case BlendMode::Hue:
    case BlendMode::Saturation: case BlendMode::Color: case BlendMode::Luminosity: case BlendMode::DarkerColor: case BlendMode::LighterColor:
        return true;
    default: return false;
    }
}

void compositeSpanF(BlendMode mode, const float* src, const float* coverage, float* dst, int count, const float luma[3]) {
    if (mode == BlendMode::Normal || mode == BlendMode::Dissolve) {
        for (int i = 0; i < count; i++, src += 4, dst += 4) if (coverage[i] > 0) compositeNormalF(src, coverage[i], dst);
        return;
    }
    for (int i = 0; i < count; i++, src += 4, dst += 4) if (coverage[i] > 0) compositeBlendedF(mode, src, coverage[i], dst, luma);
}

void compositePixelF(BlendMode mode, const float* src, float coverage, float* dst, const float luma[3]) {
    if (!(coverage > 0)) return;
    const float k = std::min(coverage, 1.0f);
    if (mode == BlendMode::Normal || mode == BlendMode::Dissolve) compositeNormalF(src, k, dst);
    else compositeBlendedF(mode, src, k, dst, luma);
}

void compositePixelAtF(BlendMode mode, const float* src, float coverage, float* dst, int x, int y, const float luma[3]) {
    if (mode != BlendMode::Dissolve) { compositePixelF(mode, src, coverage, dst, luma); return; }
    // The 8-bit pattern (blend_u8.cpp): a pixel whole or not at all, with the chance its alpha and coverage give it.
    uint32_t h = uint32_t(x) * 0x9E3779B1u ^ uint32_t(y) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    const float chance = src[3] * std::clamp(coverage, 0.0f, 1.0f);
    if (!(src[3] > 0) || float(h >> 8) / float(1u << 24) >= chance) return;
    const float opaque[4] = {src[0] / src[3], src[1] / src[3], src[2] / src[3], 1.0f};
    compositeNormalF(opaque, 1.0f, dst);
}

float blendChannelF(BlendMode mode, float cb, float cs) {
    if (!blendModeAt32(mode)) { cb = std::clamp(cb, 0.0f, 1.0f); cs = std::clamp(cs, 0.0f, 1.0f); }
    return separable(mode, cb, cs);
}

} // namespace compositor
