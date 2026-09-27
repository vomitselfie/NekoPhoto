// The blend modes at 16 bits (docs/high-bit-depth-plan.md, section 4): premultiplied samples in Photoshop's
// 0..32768, coverage in 32768 steps.
//
// With the source scaled by coverage, s = (sc, sa) and the backdrop d = (dc, da), the PDF general formula becomes,
// per channel and times 32768,
//     32768 co = sc (32768 - da) + dc (32768 - sa) + T
// where T = sa da B(cb, cs) is the blend term in premultiplied form (as blend_u8.cpp derives it). Every mode whose B
// is a product, a sum, a minimum or a maximum of the colours has a T without division, and runs as exact integer
// maths here; the rest (dodge, burn, Vivid Light, Hard Mix, Soft Light, Divide, and the non-separable modes)
// unpremultiply and blend in float, one pixel at a time.
//
// Calibration: an 8-bit document converted to 16 bits renders within one 8-bit level of its 8-bit render in every
// mode (render_hash_tests). Photoshop's 8-bit quirks that are a rounding choice are not copied; those that change
// the result by more than a level are: Linear Light's offset (-256/255, not -1), Vivid Light's halves (split at
// 128/255, the source doubled as x 255/128 and x 255/127), and the choices Hard Mix, Darker Color and Lighter Color
// make, which are taken on the pixels rounded to 8 bits.
#include "compositor/blend.h"
#include "compositor/depth.h"
#include <algorithm>
#include <cmath>

namespace compositor {

namespace {

using u64 = uint64_t;
using i64 = int64_t;
constexpr u64 ONE = one16;

template <BlendMode Mode>
inline u64 term16(u64 sc, u64 sa, u64 dc, u64 da) {
    if constexpr (Mode == BlendMode::Multiply) return sc * dc;
    else if constexpr (Mode == BlendMode::Screen) return sc * da + dc * sa - sc * dc;
    else if constexpr (Mode == BlendMode::Darken) return std::min(sc * da, dc * sa);
    else if constexpr (Mode == BlendMode::Lighten) return std::max(sc * da, dc * sa);
    else if constexpr (Mode == BlendMode::Difference) { const u64 a = sc * da, b = dc * sa; return a > b ? a - b : b - a; }
    else if constexpr (Mode == BlendMode::Overlay) return 2 * dc <= da ? 2 * sc * dc : sa * da - 2 * (da - dc) * (sa - sc);
    else if constexpr (Mode == BlendMode::HardLight) return 2 * sc <= sa ? 2 * sc * dc : sa * da - 2 * (da - dc) * (sa - sc);
    else if constexpr (Mode == BlendMode::Exclusion) return sc * da + dc * sa - 2 * sc * dc;
    else if constexpr (Mode == BlendMode::LinearBurn) { const u64 a = sc * da + dc * sa, b = sa * da; return a > b ? a - b : 0; }
    else if constexpr (Mode == BlendMode::LinearDodge) return std::min(sa * da, sc * da + dc * sa);
    else if constexpr (Mode == BlendMode::Subtract) { const u64 a = dc * sa, b = sc * da; return a > b ? a - b : 0; }
    else if constexpr (Mode == BlendMode::LinearLight) {
        // cb + 2 cs - 256/255, Photoshop's offset: sa da B = dc sa + 2 sc da - sa da 256/255.
        const i64 v = i64(dc * sa + 2 * sc * da) - i64((sa * da * 256 + 127) / 255);
        return u64(std::clamp<i64>(v, 0, i64(sa * da)));
    } else if constexpr (Mode == BlendMode::PinLight) {
        if (2 * sc < sa) return std::min(dc * sa, 2 * sc * da);
        const i64 v = i64(2 * sc * da) - i64(sa * da);
        return std::max<u64>(dc * sa, u64(std::max<i64>(v, 0)));
    } else return sc * da;   // Normal: B = cs
}

inline void compositeNormal16(const uint16_t* src, u64 k, uint16_t* dst) {
    const u64 sa = (src[3] * k + ONE / 2) >> 15;
    if (sa == 0) return;
    const u64 inv = ONE - sa;
    for (int c = 0; c < 4; c++) dst[c] = uint16_t(std::min<u64>(ONE, ((src[c] * k + ONE / 2) >> 15) + ((dst[c] * inv + ONE / 2) >> 15)));
}

template <BlendMode Mode>
inline void compositeSeparable16(const uint16_t* src, u64 k, uint16_t* dst) {
    const u64 sa = (src[3] * k + ONE / 2) >> 15;
    if (sa == 0) return;
    const u64 da = dst[3];
    const u64 ra = sa + da - ((sa * da + ONE / 2) >> 15);
    for (int c = 0; c < 3; c++) {
        const u64 sc = std::min<u64>((src[c] * k + ONE / 2) >> 15, sa), dc = std::min<u64>(dst[c], da);
        const u64 t = sc * (ONE - da) + dc * (ONE - sa) + term16<Mode>(sc, sa, dc, da);
        dst[c] = uint16_t(std::min(ra, (t + ONE / 2) >> 15));
    }
    dst[3] = uint16_t(ra);
}

float softLight16(float cb, float cs) {
    if (cs <= 0.5f) return cb - (1 - 2 * cs) * cb * (1 - cb);
    const float d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : std::sqrt(cb);
    return cb + (2 * cs - 1) * (d - cb);
}

float floatChannel(BlendMode mode, float cb, float cs) {
    switch (mode) {
    case BlendMode::ColorDodge: if (cb <= 0) return 0; if (cs >= 1) return 1; return std::min(1.0f, cb / (1 - cs));
    case BlendMode::ColorBurn: if (cb >= 1) return 1; if (cs <= 0) return 0; return 1 - std::min(1.0f, (1 - cb) / cs);
    case BlendMode::SoftLight: return softLight16(cb, cs);
    case BlendMode::VividLight: {
        // Photoshop's halves split at 128/255 and double the source as 255/128 and 255/127 do (blend_u8.cpp).
        const float s = cs * 255;
        if (s < 128) { const float doubled = s / 128; return doubled <= 0 ? 0 : std::max(0.0f, 1 - (1 - cb) / doubled); }
        const float divisor = 1 - (s - 128) / 127;
        return divisor <= 0 ? 1 : std::min(1.0f, cb / divisor);
    }
    case BlendMode::Divide: return cs <= 0 ? 1 : std::min(1.0f, cb / cs);
    default: return cs;
    }
}

bool nonSeparable(BlendMode mode) {
    switch (mode) {
    case BlendMode::Hue: case BlendMode::Saturation: case BlendMode::Color: case BlendMode::Luminosity: return true;
    default: return false;
    }
}

/// The modes that choose rather than mix (Hard Mix per channel, Darker and Lighter Color per pixel) decide on the
/// pixels rounded to 8 bits, with the 8-bit kernels' own arithmetic: a 16-bit document made from an 8-bit one then
/// chooses as the 8-bit render does. What is chosen keeps its 16 bits.
bool choosesAtEightBits(BlendMode mode) { return mode == BlendMode::HardMix || mode == BlendMode::DarkerColor || mode == BlendMode::LighterColor; }

/// The straight colour the 8-bit kernels see for a premultiplied 16-bit pixel.
void straightAtEightBits(const uint16_t* p, float out[3]) {
    const uint8_t a = narrow16(p[3]);
    for (int c = 0; c < 3; c++) out[c] = a ? std::min(1.0f, float(narrow16(p[c])) / float(a)) : 0.0f;
}

/// Straight colours in float, then back: the float family and the non-separable modes.
void compositeFloat16(BlendMode mode, const uint16_t* src, u64 k, uint16_t* dst) {
    if (src[3] == 0) return;
    const float coverage = float(k) / float(ONE);
    const float as = src[3] / float(ONE) * coverage;
    if (as <= 0) return;
    const float ab = dst[3] / float(ONE);
    // The source's straight colour from the unscaled pixel: scaling first would amplify rounding in the divisions.
    float cs[3], sp[3], bp[3];
    for (int c = 0; c < 3; c++) {
        cs[c] = std::min(1.0f, src[c] / float(src[3]));
        sp[c] = cs[c] * as;
        bp[c] = dst[c] / float(ONE);
    }
    float out[3];
    if (ab <= 0) {
        for (int c = 0; c < 3; c++) out[c] = sp[c] + bp[c] * (1 - as);
    } else {
        float cb[3];
        for (int c = 0; c < 3; c++) cb[c] = std::min(1.0f, bp[c] / ab);
        float m[3];
        if (choosesAtEightBits(mode)) {
            float s8[3], b8[3];
            straightAtEightBits(src, s8);
            straightAtEightBits(dst, b8);
            if (mode == BlendMode::HardMix) {
                for (int c = 0; c < 3; c++) m[c] = photoshopBlendByteTabled(mode, blendByteOf(s8[c] * 255), blendByteOf(b8[c] * 255)) > 127 ? 1.0f : 0.0f;
            } else {
                const Rgb chosen = blendColor(mode, {b8[0], b8[1], b8[2]}, {s8[0], s8[1], s8[2]});
                const bool source = chosen.r == s8[0] && chosen.g == s8[1] && chosen.b == s8[2] && !(s8[0] == b8[0] && s8[1] == b8[1] && s8[2] == b8[2]);
                for (int c = 0; c < 3; c++) m[c] = source ? cs[c] : cb[c];
            }
        } else if (nonSeparable(mode)) {
            const Rgb r = blendColor(mode, {cb[0], cb[1], cb[2]}, {cs[0], cs[1], cs[2]});
            m[0] = r.r; m[1] = r.g; m[2] = r.b;
        } else for (int c = 0; c < 3; c++) m[c] = floatChannel(mode, cb[c], cs[c]);
        for (int c = 0; c < 3; c++) out[c] = sp[c] * (1 - ab) + bp[c] * (1 - as) + as * ab * m[c];
    }
    auto q = [](float v) { return uint16_t(std::clamp(v * float(ONE) + 0.5f, 0.0f, float(ONE))); };
    const uint16_t a = q(as + ab * (1 - as));
    for (int c = 0; c < 3; c++) dst[c] = std::min(q(out[c]), a);
    dst[3] = a;
}

template <BlendMode Mode>
void spanOf(const uint16_t* src, const uint32_t* steps, uint16_t* dst, int count) {
    for (int i = 0; i < count; i++, src += 4, dst += 4) {
        if (!steps[i]) continue;
        if constexpr (Mode == BlendMode::Normal) compositeNormal16(src, steps[i], dst);
        else compositeSeparable16<Mode>(src, steps[i], dst);
    }
}

void floatSpan(BlendMode mode, const uint16_t* src, const uint32_t* steps, uint16_t* dst, int count) {
    for (int i = 0; i < count; i++, src += 4, dst += 4) if (steps[i]) compositeFloat16(mode, src, steps[i], dst);
}

} // namespace

unsigned coverageSteps16(float coverage) {
    if (!(coverage > 0)) return 0;
    const unsigned k = unsigned(coverage * float(ONE) + 0.5f);
    return k > ONE ? unsigned(ONE) : k;
}

void compositeSpan16(BlendMode mode, const uint16_t* src, const uint32_t* steps, uint16_t* dst, int count) {
    switch (mode) {
    case BlendMode::Normal: case BlendMode::Dissolve: spanOf<BlendMode::Normal>(src, steps, dst, count); break;
    case BlendMode::Multiply: spanOf<BlendMode::Multiply>(src, steps, dst, count); break;
    case BlendMode::Screen: spanOf<BlendMode::Screen>(src, steps, dst, count); break;
    case BlendMode::Overlay: spanOf<BlendMode::Overlay>(src, steps, dst, count); break;
    case BlendMode::Darken: spanOf<BlendMode::Darken>(src, steps, dst, count); break;
    case BlendMode::Lighten: spanOf<BlendMode::Lighten>(src, steps, dst, count); break;
    case BlendMode::Difference: spanOf<BlendMode::Difference>(src, steps, dst, count); break;
    case BlendMode::HardLight: spanOf<BlendMode::HardLight>(src, steps, dst, count); break;
    case BlendMode::Exclusion: spanOf<BlendMode::Exclusion>(src, steps, dst, count); break;
    case BlendMode::LinearBurn: spanOf<BlendMode::LinearBurn>(src, steps, dst, count); break;
    case BlendMode::LinearDodge: spanOf<BlendMode::LinearDodge>(src, steps, dst, count); break;
    case BlendMode::Subtract: spanOf<BlendMode::Subtract>(src, steps, dst, count); break;
    case BlendMode::LinearLight: spanOf<BlendMode::LinearLight>(src, steps, dst, count); break;
    case BlendMode::PinLight: spanOf<BlendMode::PinLight>(src, steps, dst, count); break;
    default: floatSpan(mode, src, steps, dst, count); break;
    }
}

void compositePixelSteps16(BlendMode mode, const uint16_t* src, unsigned k, uint16_t* dst) {
    const uint32_t steps = k;
    compositeSpan16(mode, src, &steps, dst, 1);
}

void compositePixelAt16(BlendMode mode, const uint16_t* src, float coverage, uint16_t* dst, int x, int y) {
    if (mode != BlendMode::Dissolve) { compositePixelSteps16(mode, src, coverageSteps16(coverage), dst); return; }
    // The 8-bit pattern (blend_u8.cpp): a pixel whole or not at all, with the chance its alpha and coverage give it.
    uint32_t h = uint32_t(x) * 0x9E3779B1u ^ uint32_t(y) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    const float chance = src[3] / float(ONE) * coverage;
    if (float(h >> 8) / float(1u << 24) >= chance || src[3] == 0) return;
    uint16_t opaque[4];
    for (int c = 0; c < 3; c++) opaque[c] = uint16_t(std::min<u64>(ONE, (u64(src[c]) * ONE + src[3] / 2) / src[3]));
    opaque[3] = uint16_t(ONE);
    compositeNormal16(opaque, ONE, dst);
}

float blendChannel16(BlendMode mode, float cb, float cs) {
    switch (mode) {
    case BlendMode::Multiply: return cb * cs;
    case BlendMode::Screen: return cb + cs - cb * cs;
    case BlendMode::Overlay: return cb <= 0.5f ? 2 * cs * cb : 1 - 2 * (1 - cb) * (1 - cs);
    case BlendMode::HardLight: return cs <= 0.5f ? 2 * cs * cb : 1 - 2 * (1 - cb) * (1 - cs);
    case BlendMode::Darken: return std::min(cb, cs);
    case BlendMode::Lighten: return std::max(cb, cs);
    case BlendMode::Difference: return std::fabs(cb - cs);
    case BlendMode::Exclusion: return cb + cs - 2 * cb * cs;
    case BlendMode::LinearBurn: return std::max(0.0f, cb + cs - 1);
    case BlendMode::LinearDodge: return std::min(1.0f, cb + cs);
    case BlendMode::Subtract: return std::max(0.0f, cb - cs);
    case BlendMode::LinearLight: return std::clamp(cb + 2 * cs - 256.0f / 255.0f, 0.0f, 1.0f);
    case BlendMode::PinLight: return cs < 0.5f ? std::min(cb, 2 * cs) : std::max(cb, 2 * cs - 1);
    default: return floatChannel(mode, cb, cs);
    }
}

} // namespace compositor
