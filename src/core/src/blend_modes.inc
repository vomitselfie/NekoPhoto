// The blend modes of CMYK and Lab documents, written once for 8 and 16 bits (blend_c8.cpp, blend_modes16.cpp).
//
// The RGB kernels (blend_u8.cpp, blend_u16.cpp) are reused unchanged, which keeps every mode's rounding and
// Photoshop calibration: a separable mode treats each channel alone with the shared alpha, so a CMYK pixel is blended
// as two RGBA pixels, (C, M, Y, alpha) and (K, K, K, alpha), whose alphas come out the same. Lab's non-separable
// modes are their own (labNonSeparable). A policy `K` supplies the depth: `Sample`, `Steps`, `one`, `pixel(mode, src,
// k, dst)` (an RGBA kernel with coverage steps), `normal(src, k, dst, channels)`, `stepsOf(coverage)`, `full`.
#include "compositor/blend.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace compositor {

namespace modes {

/// The modes Photoshop offers in RGB but whose meaning in CMYK is not a per-channel formula.
inline bool nonSeparable(BlendMode mode) {
    switch (mode) {
    case BlendMode::Hue: case BlendMode::Saturation: case BlendMode::Color: case BlendMode::Luminosity:
    case BlendMode::DarkerColor: case BlendMode::LighterColor: return true;
    default: return false;
    }
}

/// Lab's excluded modes (Adobe: "For Lab images, the Color Dodge, Color Burn, Darken, Lighten, Difference, Exclusion,
/// Subtract, and Divide modes are unavailable").
inline bool unavailableInLab(BlendMode mode) {
    switch (mode) {
    case BlendMode::ColorDodge: case BlendMode::ColorBurn: case BlendMode::Darken: case BlendMode::Lighten:
    case BlendMode::Difference: case BlendMode::Exclusion: case BlendMode::Subtract: case BlendMode::Divide: return true;
    default: return false;
    }
}

/// CMYK's non-separable modes, as Photoshop computes them (checked against its own renders: tests/colormodes_tests.cpp).
/// No trip through RGB: the PDF Lum/Sat helpers run on the stored complements of C, M and Y (1 is no ink), and K
/// is carried apart, the backdrop's for Hue, Saturation and Color and the source's for Luminosity. Darker and
/// Lighter Color pick a whole pixel, K included, by K' x Lum(C', M', Y'); ties keep the backdrop. `cb` and `cs` are
/// straight complements in 0..1; `m` receives the blended colour.
inline void cmykNonSeparableColor(BlendMode mode, const float* cb, const float* cs, float* m) {
    auto lum = [](const float* c) { return 0.30f * c[0] + 0.59f * c[1] + 0.11f * c[2]; };
    auto sat = [](const float* c) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); };
    auto setLum = [&](float* c, float l) {
        const float d = l - lum(c);
        for (int i = 0; i < 3; i++) c[i] += d;
        const float L = lum(c), lo = std::min({c[0], c[1], c[2]}), hi = std::max({c[0], c[1], c[2]});
        if (lo < 0 && L - lo != 0) for (int i = 0; i < 3; i++) c[i] = L + (c[i] - L) * L / (L - lo);
        const float hi2 = std::max({c[0], c[1], c[2]});
        if (hi2 > 1 && hi2 - L != 0) for (int i = 0; i < 3; i++) c[i] = L + (c[i] - L) * (1 - L) / (hi2 - L);
        (void)hi;
        for (int i = 0; i < 3; i++) c[i] = std::clamp(c[i], 0.0f, 1.0f);
    };
    auto setSat = [](float* c, float s) {
        int lo = 0, mid = 1, hi = 2;
        if (c[lo] > c[mid]) std::swap(lo, mid);
        if (c[mid] > c[hi]) std::swap(mid, hi);
        if (c[lo] > c[mid]) std::swap(lo, mid);
        const float span = c[hi] - c[lo];
        float out[3] = {0, 0, 0};
        if (span > 0) { out[mid] = (c[mid] - c[lo]) * s / span; out[hi] = s; }
        for (int i = 0; i < 3; i++) c[i] = out[i];
    };
    auto lightness = [&](const float* c) { return c[3] * lum(c); };
    const float* whole = cb;
    switch (mode) {
    case BlendMode::Hue: std::copy(cs, cs + 3, m); setSat(m, sat(cb)); setLum(m, lum(cb)); m[3] = cb[3]; return;
    case BlendMode::Saturation: std::copy(cb, cb + 3, m); setSat(m, sat(cs)); setLum(m, lum(cb)); m[3] = cb[3]; return;
    case BlendMode::Color: std::copy(cs, cs + 3, m); setLum(m, lum(cb)); m[3] = cb[3]; return;
    case BlendMode::Luminosity: std::copy(cb, cb + 3, m); setLum(m, lum(cs)); m[3] = cs[3]; return;
    case BlendMode::DarkerColor: if (lightness(cs) < lightness(cb)) whole = cs; break;
    case BlendMode::LighterColor: if (lightness(cs) > lightness(cb)) whole = cs; break;
    default: whole = cs; break;
    }
    std::copy(whole, whole + 4, m);
}

/// One premultiplied CMYK pixel (C', M', Y', K', alpha) through a non-separable mode; coverage scales the source's alpha.
template <class K>
inline void cmykNonSeparable(BlendMode mode, const typename K::Sample* src, float coverage, typename K::Sample* dst) {
    using T = typename K::Sample;
    const float one = float(K::one);
    const float as = src[4] / one * coverage;
    if (as <= 0 || src[4] == 0) return;
    const float ab = dst[4] / one;
    float cs[4], sp[4], bp[4], out[4];
    for (int c = 0; c < 4; c++) {
        cs[c] = std::min(1.0f, src[c] / float(src[4]));
        sp[c] = cs[c] * as;
        bp[c] = dst[c] / one;
    }
    if (ab <= 0) {
        for (int c = 0; c < 4; c++) out[c] = sp[c] + bp[c] * (1 - as);
    } else {
        float cb[4], m[4];
        for (int c = 0; c < 4; c++) cb[c] = std::min(1.0f, bp[c] / ab);
        cmykNonSeparableColor(mode, cb, cs, m);
        for (int c = 0; c < 4; c++) out[c] = sp[c] * (1 - ab) + bp[c] * (1 - as) + as * ab * std::clamp(m[c], 0.0f, 1.0f);
    }
    auto q = [&](float v) { return T(std::clamp(v * one + 0.5f, 0.0f, one)); };
    const T a = q(as + ab * (1 - as));
    for (int c = 0; c < 4; c++) dst[c] = std::min(q(out[c]), a);
    dst[4] = a;
}

/// One CMYK pixel through an RGBA kernel: (C, M, Y, alpha), then (K, K, K, alpha).
template <class K>
inline void cmykPixel(BlendMode mode, const typename K::Sample* src, typename K::Steps k, typename K::Sample* dst) {
    using T = typename K::Sample;
    T s1[4] = {src[0], src[1], src[2], src[4]}, d1[4] = {dst[0], dst[1], dst[2], dst[4]};
    T s2[4] = {src[3], src[3], src[3], src[4]}, d2[4] = {dst[3], dst[3], dst[3], dst[4]};
    K::pixel(mode, s1, k, d1);
    K::pixel(mode, s2, k, d2);
    dst[0] = d1[0]; dst[1] = d1[1]; dst[2] = d1[2]; dst[3] = d2[0]; dst[4] = d1[3];
}

/// Lab's non-separable modes on one pixel, in float: L from one side and a/b (or chroma, or hue) from the other, as
/// Photoshop's Lab Luminosity and Color are described (the L channel is lightness; a and b are colour). Darker and
/// Lighter Color choose the whole pixel by L (ties keep the backdrop). Coverage scales the source's alpha.
template <class K>
inline void labNonSeparable(BlendMode mode, const typename K::Sample* src, float coverage, typename K::Sample* dst) {
    using T = typename K::Sample;
    const float one = float(K::one);
    const float as = src[3] / one * coverage;
    if (as <= 0 || src[3] == 0) return;
    const float ab = dst[3] / one;
    const float n = float(K::labOffset) / one;   // a = b = 0, in 0..1
    float cs[3], sp[3], bp[3];
    for (int c = 0; c < 3; c++) {
        cs[c] = std::min(1.0f, src[c] / float(src[3]));
        sp[c] = cs[c] * as;
        bp[c] = dst[c] / one;
    }
    float out[3];
    if (ab <= 0) {
        for (int c = 0; c < 3; c++) out[c] = sp[c] + bp[c] * (1 - as);
    } else {
        float cb[3];
        for (int c = 0; c < 3; c++) cb[c] = std::min(1.0f, bp[c] / ab);
        float m[3] = {cb[0], cb[1], cb[2]};
        const float sa = cs[1] - n, sb = cs[2] - n, ba = cb[1] - n, bb = cb[2] - n;
        const float sc = std::hypot(sa, sb), bc = std::hypot(ba, bb);
        switch (mode) {
        case BlendMode::Luminosity: m[0] = cs[0]; break;
        case BlendMode::Color: m[1] = cs[1]; m[2] = cs[2]; break;
        case BlendMode::Hue:
            // The source's hue at the backdrop's chroma and lightness; a neutral source has no hue to give.
            if (sc > 0) { m[1] = n + bc * sa / sc; m[2] = n + bc * sb / sc; }
            break;
        case BlendMode::Saturation:
            // The source's chroma at the backdrop's hue and lightness; a neutral backdrop has no hue to keep.
            if (bc > 0) { m[1] = n + sc * ba / bc; m[2] = n + sc * bb / bc; }
            break;
        case BlendMode::DarkerColor: if (cs[0] < cb[0]) std::memcpy(m, cs, sizeof m); break;
        case BlendMode::LighterColor: if (cs[0] > cb[0]) std::memcpy(m, cs, sizeof m); break;
        default: std::memcpy(m, cs, sizeof m); break;
        }
        for (int c = 0; c < 3; c++) out[c] = sp[c] * (1 - ab) + bp[c] * (1 - as) + as * ab * std::clamp(m[c], 0.0f, 1.0f);
    }
    auto q = [&](float v) { return T(std::clamp(v * one + 0.5f, 0.0f, one)); };
    const T a = q(as + ab * (1 - as));
    for (int c = 0; c < 3; c++) dst[c] = std::min(q(out[c]), a);
    dst[3] = a;
}

template <class K>
void span(BlendMode mode, ColorMode colorMode, const typename K::Sample* src, const typename K::Steps* steps, typename K::Sample* dst, int count) {
    const int n = colorModeChannels(colorMode);
    if (colorMode == ColorMode::RGB) { K::rgbSpan(mode, src, steps, dst, count); return; }
    if (mode == BlendMode::Normal || mode == BlendMode::Dissolve) {
        for (int i = 0; i < count; i++, src += n, dst += n) if (steps[i]) K::normal(src, steps[i], dst, n);
        return;
    }
    if (colorMode == ColorMode::CMYK && nonSeparable(mode)) {
        for (int i = 0; i < count; i++, src += 5, dst += 5) if (steps[i]) cmykNonSeparable<K>(mode, src, float(steps[i]) / float(K::full), dst);
        return;
    }
    if (colorMode == ColorMode::CMYK) {
        for (int i = 0; i < count; i++, src += 5, dst += 5) if (steps[i]) cmykPixel<K>(mode, src, steps[i], dst);
        return;
    }
    if (nonSeparable(mode)) {
        for (int i = 0; i < count; i++, src += 4, dst += 4) if (steps[i]) labNonSeparable<K>(mode, src, float(steps[i]) / float(K::full), dst);
        return;
    }
    for (int i = 0; i < count; i++, src += 4, dst += 4) if (steps[i]) K::pixel(mode, src, steps[i], dst);
}

template <class K>
void pixelAt(BlendMode mode, ColorMode colorMode, const typename K::Sample* src, float coverage, typename K::Sample* dst, int x, int y) {
    using T = typename K::Sample;
    const int n = colorModeChannels(colorMode), c = n - 1;
    if (mode != BlendMode::Dissolve) {
        const typename K::Steps k = K::stepsOf(coverage);
        span<K>(mode, colorMode, src, &k, dst, 1);
        return;
    }
    // Dissolve's pattern and rule (blend_u8.cpp): a pixel whole or not at all, with the chance its alpha and coverage give it.
    uint32_t h = uint32_t(x) * 0x9E3779B1u ^ uint32_t(y) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    const float chance = src[c] / float(K::one) * coverage;
    if (float(h >> 8) / float(1u << 24) >= chance || src[c] == 0) return;
    T opaque[5];
    for (int i = 0; i < c; i++) opaque[i] = T(std::min<uint32_t>(K::one, uint32_t(src[i]) * K::one / src[c]));
    opaque[c] = T(K::one);
    K::normal(opaque, K::full, dst, n);
}

} // namespace modes

} // namespace compositor
