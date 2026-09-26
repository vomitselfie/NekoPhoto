#include "compositor/blend.h"
#include <algorithm>
#include <cmath>

namespace compositor {

namespace {

// ---- non-separable modes (Hue, Saturation, Color, Luminosity): the PDF / W3C definitions in float ----

inline float lum(Rgb c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }

inline Rgb clipColor(Rgb c) {
    float l = lum(c);
    float n = std::min({c.r, c.g, c.b}), x = std::max({c.r, c.g, c.b});
    if (n < 0) { c.r = l + (c.r - l) * l / (l - n); c.g = l + (c.g - l) * l / (l - n); c.b = l + (c.b - l) * l / (l - n); }
    if (x > 1) { c.r = l + (c.r - l) * (1 - l) / (x - l); c.g = l + (c.g - l) * (1 - l) / (x - l); c.b = l + (c.b - l) * (1 - l) / (x - l); }
    return c;
}

inline Rgb setLum(Rgb c, float l) {
    float d = l - lum(c);
    return clipColor({c.r + d, c.g + d, c.b + d});
}

inline float sat(Rgb c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); }

inline Rgb setSat(Rgb c, float s) {
    float* v[3] = {&c.r, &c.g, &c.b};
    int maxI = 0, minI = 0;
    for (int i = 1; i < 3; i++) { if (*v[i] > *v[maxI]) maxI = i; if (*v[i] < *v[minI]) minI = i; }
    if (maxI == minI) return {0, 0, 0};
    int midI = 3 - maxI - minI;
    float cmax = *v[maxI], cmin = *v[minI], cmid = *v[midI];
    Rgb out{0, 0, 0};
    float* o[3] = {&out.r, &out.g, &out.b};
    if (cmax > cmin) {
        *o[midI] = (cmid - cmin) * s / (cmax - cmin);
        *o[maxI] = s;
    }
    *o[minI] = 0;
    return out;
}

// ---- Photoshop's other modes: Patchy's byte kernels, pinned bit-exact against full 256 x 256 Photoshop 2026
// captures (src/core/blend_math.cpp there, MIT; docs/blend-modes.md) ----

int nearestHalfUp(int a, int b) { return (2 * a + b) / (2 * b); }
int nearestHalfDown(int a, int b) { return (2 * a + b - 1) / (2 * b); }
uint8_t byteClamp(float v) { return uint8_t(std::clamp(int(std::lround(v)), 0, 255)); }

uint8_t softLight(int s, int d) {
    const float source = s / 255.0f, base = d / 255.0f;
    float out = base;
    if (source <= 0.5f) out = base - (1 - 2 * source) * base * (1 - base);
    else {
        const float dd = base <= 0.25f ? ((16 * base - 12) * base + 4) * base : std::sqrt(base);
        out = base + (2 * source - 1) * (dd - base);
    }
    return byteClamp(out * 255);
}

// Not the textbook burn(2s) / dodge(2s - 255): the burn half doubles as round(s * 255 / 128) and rounds half down;
// the dodge half doubles as round((s - 128) * 255 / 127) and rounds half up.
uint8_t vividLight(int s, int d) {
    if (s == 0) return 0;
    if (s == 255) return 255;
    if (s < 128) {
        const int doubled = nearestHalfUp(s * 255, 128), numerator = d + doubled - 255;
        return numerator <= 0 ? 0 : uint8_t(std::min(255, nearestHalfDown(numerator * 255, doubled)));
    }
    const int divisor = 255 - nearestHalfUp((s - 128) * 255, 127);
    return divisor <= 0 ? 255 : uint8_t(std::min(255, nearestHalfUp(d * 255, divisor)));
}

// Hard Mix thresholds the textbook floor-rounded vivid light at > 127, not Photoshop's own vivid light.
uint8_t hardMix(int s, int d) {
    int vivid;
    if (s < 128) {
        const int doubled = 2 * s;
        vivid = doubled == 0 ? (d < 255 ? 0 : 255) : std::max(0, 255 - std::min(255, ((255 - d) * 255) / doubled));
    } else vivid = std::min(255, (d * 255) / (255 - 2 * (s - 128)));
    return vivid > 127 ? 255 : 0;
}

uint8_t photoshopChannel(BlendMode mode, uint8_t src, uint8_t dst) {
    const int s = src, d = dst;
    switch (mode) {
    case BlendMode::LinearBurn: return uint8_t(std::clamp(s + d - 255, 0, 255));
    case BlendMode::LinearDodge: return uint8_t(std::min(255, s + d));
    case BlendMode::SoftLight: return softLight(s, d);
    case BlendMode::HardLight: return s < 128 ? uint8_t((2 * s * d) / 255) : uint8_t(255 - (2 * (255 - s) * (255 - d)) / 255);
    case BlendMode::VividLight: return vividLight(s, d);
    case BlendMode::LinearLight: return uint8_t(std::clamp(d + 2 * s - 256, 0, 255));   // Photoshop's -256, not -255
    case BlendMode::PinLight: return s < 128 ? uint8_t(std::min(d, std::clamp(2 * s, 0, 255))) : uint8_t(std::max(d, std::clamp(2 * (s - 128), 0, 255)));
    case BlendMode::HardMix: return hardMix(s, d);
    case BlendMode::Exclusion: return uint8_t(s + d - 2 * ((s * d + 127) / 255));   // the product rounded before doubling
    case BlendMode::Subtract: return uint8_t(std::max(0, d - s));
    case BlendMode::Divide: return s == 0 ? 255 : uint8_t(std::min(255, (d * 255 + s / 2) / s));
    default: return src;
    }
}

bool photoshopByteMode(BlendMode m) {
    switch (m) {
    case BlendMode::LinearBurn: case BlendMode::LinearDodge: case BlendMode::SoftLight: case BlendMode::HardLight: case BlendMode::VividLight:
    case BlendMode::LinearLight: case BlendMode::PinLight: case BlendMode::HardMix: case BlendMode::Exclusion: case BlendMode::Subtract:
    case BlendMode::Divide: return true;
    default: return false;
    }
}

inline float separable(BlendMode mode, float cb, float cs) {
    switch (mode) {
    case BlendMode::Normal: return cs;
    case BlendMode::Multiply: return cb * cs;
    case BlendMode::Screen: return cb + cs - cb * cs;
    case BlendMode::Overlay: return cb <= 0.5f ? cs * 2 * cb : (cs + (2 * cb - 1) - cs * (2 * cb - 1)); // HardLight(cs, cb)
    case BlendMode::Darken: return std::min(cb, cs);
    case BlendMode::Lighten: return std::max(cb, cs);
    case BlendMode::Difference: return std::fabs(cb - cs);
    case BlendMode::ColorDodge:
        if (cb <= 0) return 0;
        if (cs >= 1) return 1;
        return std::min(1.0f, cb / (1 - cs));
    case BlendMode::ColorBurn:
        if (cb >= 1) return 1;
        if (cs <= 0) return 0;
        return 1 - std::min(1.0f, (1 - cb) / cs);
    // Photoshop's others go through photoshopChannel (bytes) in blendColor.
    default: return cs;
    }
}

// ---- separable modes in premultiplied 8-bit integers -------------------------------------------------
//
// With the source already scaled by coverage, s = (sr, sg, sb, sa) and the backdrop d = (dr, dg, db, da),
// the PDF general formula co = cs(1 - ab) + cb(1 - as) + as ab B(cb, cs) becomes, per channel and times 255,
//     255 co = sc (255 - da) + dc (255 - sa) + T
// where T is the blend term in premultiplied form (pixman's combine_*_u):
//     Multiply  sc dc                       Screen     sc da + dc sa - sc dc
//     Darken    min(sc da, dc sa)           Lighten    max(sc da, dc sa)
//     Difference |sc da - dc sa|            Overlay    2 sc dc, or sa da - 2 (da - dc)(sa - sc) when 2 dc > da
//     ColorDodge 0 if dc = 0; sa da if cs >= 1; else min(sa da, sa dc / (1 - cs))
//     ColorBurn  sa da if dc >= da; 0 if cs = 0; else sa da - min(sa da, sa (da - dc) / cs)
// Nine modes need no division at all; dodge and burn need one, and take the source's straight colour
// cs = c / a from the unscaled source (rc, ra), since 1 - cs from two separately rounded values would be
// amplified by the division.

template <BlendMode Mode>
inline unsigned blendTerm(unsigned sc, unsigned sa, unsigned dc, unsigned da, unsigned rc, unsigned ra) {
    if constexpr (Mode == BlendMode::Multiply) return sc * dc;
    else if constexpr (Mode == BlendMode::Screen) return sc * da + dc * sa - sc * dc;
    else if constexpr (Mode == BlendMode::Darken) return std::min(sc * da, dc * sa);
    else if constexpr (Mode == BlendMode::Lighten) return std::max(sc * da, dc * sa);
    else if constexpr (Mode == BlendMode::Difference) { unsigned a = sc * da, b = dc * sa; return a > b ? a - b : b - a; }
    else if constexpr (Mode == BlendMode::Overlay) return 2 * dc <= da ? 2 * sc * dc : sa * da - 2 * (da - dc) * (sa - sc);
    else if constexpr (Mode == BlendMode::ColorDodge) {
        if (dc == 0) return 0;
        if (rc >= ra) return sa * da;
        return std::min(sa * da, (sa * dc * ra + (ra - rc) / 2) / (ra - rc));
    } else if constexpr (Mode == BlendMode::ColorBurn) {
        if (dc >= da) return sa * da;
        if (rc == 0) return 0;
        return sa * da - std::min(sa * da, (sa * (da - dc) * ra + rc / 2) / rc);
    } else return sc * da;   // Normal: as B = cs
}

/// round(t / 255) for t <= 65535 (Blinn): an add, a multiply and a shift.
inline unsigned div255(unsigned t) { return ((t + 128) * 257) >> 16; }

template <BlendMode Mode>
inline void compositeSeparable(const uint8_t* src, unsigned k, uint8_t* dst) {
    // The source scaled by coverage k/256, rounded, as the fast Normal path does.
    unsigned sa = (src[3] * k + 128) >> 8;
    if (sa == 0) return;
    unsigned da = dst[3];
    unsigned ra = sa + da - div255(sa * da);
    for (int c = 0; c < 3; c++) {
        unsigned sc = (src[c] * k + 128) >> 8, dc = dst[c];
        unsigned t = sc * (255 - da) + dc * (255 - sa) + blendTerm<Mode>(sc, sa, dc, da, src[c], src[3]);
        dst[c] = uint8_t(std::min(ra, (t + 127) / 255));
    }
    dst[3] = uint8_t(ra);
}

inline void compositeNormal(const uint8_t* src, unsigned k, uint8_t* dst) {
    // Source-over in fixed point: out = src * coverage + dst * (1 - srcAlpha * coverage).
    unsigned sa = (src[3] * k + 128) >> 8;
    if (sa == 0) return;
    unsigned inv = 255 - sa;
    for (int c = 0; c < 3; c++) dst[c] = uint8_t(((src[c] * k + 128) >> 8) + div255(dst[c] * inv));
    dst[3] = uint8_t(sa + div255(dst[3] * inv));
}

/// The non-separable modes: straight colours in float, then back.
void compositeNonSeparable(BlendMode mode, const uint8_t* src, unsigned k, uint8_t* dst) {
    float coverage = k / 256.0f;
    float as = src[3] / 255.0f * coverage;
    if (as <= 0) return;
    float scale = coverage / 255.0f;
    float sr = src[0] * scale, sg = src[1] * scale, sb = src[2] * scale; // premultiplied, 0..1, scaled by coverage
    float ab = dst[3] / 255.0f;
    float br = dst[0] / 255.0f, bg = dst[1] / 255.0f, bb = dst[2] / 255.0f;
    float ao = as + ab * (1 - as);
    float r, g, b;
    if (ab <= 0) {
        r = sr + br * (1 - as);
        g = sg + bg * (1 - as);
        b = sb + bb * (1 - as);
    } else {
        Rgb cs{sr / as, sg / as, sb / as};
        Rgb cb{br / ab, bg / ab, bb / ab};
        Rgb m = blendColor(mode, cb, cs);
        r = sr * (1 - ab) + br * (1 - as) + as * ab * m.r;
        g = sg * (1 - ab) + bg * (1 - as) + as * ab * m.g;
        b = sb * (1 - ab) + bb * (1 - as) + as * ab * m.b;
    }
    auto q = [](float v) { return uint8_t(std::min(255.0f, std::max(0.0f, v * 255.0f + 0.5f))); };
    uint8_t a8 = q(ao);
    dst[0] = std::min(q(r), a8);
    dst[1] = std::min(q(g), a8);
    dst[2] = std::min(q(b), a8);
    dst[3] = a8;
}

} // namespace

Rgb blendColor(BlendMode mode, Rgb cb, Rgb cs) {
    switch (mode) {
    // The whole colour with the lower (Darker) or higher (Lighter) rounded 0.3 / 0.59 / 0.11 luma; ties keep the
    // backdrop (Patchy's calibration).
    case BlendMode::DarkerColor: case BlendMode::LighterColor: {
        auto luma = [](Rgb c) {
            const int w = 30 * byteClamp(c.r * 255) + 59 * byteClamp(c.g * 255) + 11 * byteClamp(c.b * 255);
            return (2 * w + 100) / 200;
        };
        const int ls = luma(cs), lb = luma(cb);
        return (mode == BlendMode::DarkerColor ? ls < lb : ls > lb) ? cs : cb;
    }
    case BlendMode::Hue: return setLum(setSat(cs, sat(cb)), lum(cb));
    case BlendMode::Saturation: return setLum(setSat(cb, sat(cs)), lum(cb));
    case BlendMode::Color: return setLum(cs, lum(cb));
    case BlendMode::Luminosity: return setLum(cb, lum(cs));
    default:
        if (photoshopByteMode(mode)) {
            auto ch = [&](float b, float s2) { return photoshopChannel(mode, byteClamp(s2 * 255), byteClamp(b * 255)) / 255.0f; };
            return {ch(cb.r, cs.r), ch(cb.g, cs.g), ch(cb.b, cs.b)};
        }
        return {separable(mode, cb.r, cs.r), separable(mode, cb.g, cs.g), separable(mode, cb.b, cs.b)};
    }
}

unsigned coverageSteps(float coverage) {
    if (!(coverage > 0)) return 0;
    unsigned k = unsigned(coverage * 256.0f + 0.5f);
    return k > 256 ? 256 : k;
}

void compositePixelSteps(BlendMode mode, const uint8_t* src, unsigned k, uint8_t* dst) {
    if (k == 0) return;
    switch (mode) {
    case BlendMode::Normal: compositeNormal(src, k, dst); break;
    case BlendMode::Multiply: compositeSeparable<BlendMode::Multiply>(src, k, dst); break;
    case BlendMode::Screen: compositeSeparable<BlendMode::Screen>(src, k, dst); break;
    case BlendMode::Overlay: compositeSeparable<BlendMode::Overlay>(src, k, dst); break;
    case BlendMode::Darken: compositeSeparable<BlendMode::Darken>(src, k, dst); break;
    case BlendMode::Lighten: compositeSeparable<BlendMode::Lighten>(src, k, dst); break;
    case BlendMode::Difference: compositeSeparable<BlendMode::Difference>(src, k, dst); break;
    case BlendMode::ColorDodge: compositeSeparable<BlendMode::ColorDodge>(src, k, dst); break;
    case BlendMode::ColorBurn: compositeSeparable<BlendMode::ColorBurn>(src, k, dst); break;
    default: compositeNonSeparable(mode, src, k, dst); break;
    }
}

void compositePixel(BlendMode mode, const uint8_t* src, float coverage, uint8_t* dst) {
    // Without a position Dissolve has no pattern: it draws as Normal.
    compositePixelSteps(mode == BlendMode::Dissolve ? BlendMode::Normal : mode, src, coverageSteps(coverage), dst);
}

void compositePixelAt(BlendMode mode, const uint8_t* src, float coverage, uint8_t* dst, int x, int y) {
    if (mode != BlendMode::Dissolve) { compositePixelSteps(mode, src, coverageSteps(coverage), dst); return; }
    // Photoshop's Dissolve: each pixel whole or not at all, drawn with the chance its alpha (and coverage) gives it.
    uint32_t h = uint32_t(x) * 0x9E3779B1u ^ uint32_t(y) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    const float chance = src[3] / 255.0f * coverage;
    if (float(h >> 8) / float(1u << 24) >= chance || src[3] == 0) return;
    uint8_t opaque[4];
    for (int c = 0; c < 3; c++) opaque[c] = uint8_t(std::min(255, src[c] * 255 / src[3]));
    opaque[3] = 255;
    compositePixelSteps(BlendMode::Normal, opaque, 256, dst);
}

void compositeSpanNormal(const uint8_t* src, const uint16_t* steps, uint8_t* dst, int count) {
    for (int i = 0; i < count; i++, src += 4, dst += 4) {
        const unsigned k = steps[i];
        if (k == 0) continue;
        const unsigned sa = (src[3] * k + 128) >> 8;
        if (sa == 0) continue;
        const unsigned inv = 255 - sa;
        // The alpha lane uses the same expression as the colour lanes: (src * k) >> 8 is sa.
        for (int c = 0; c < 4; c++) dst[c] = uint8_t(((src[c] * k + 128) >> 8) + div255(dst[c] * inv));
    }
}

void compositeImage(BlendMode mode, const Image& source, double opacity, Image& destination) {
    int w = std::min(source.width(), destination.width()), h = std::min(source.height(), destination.height());
    unsigned k = coverageSteps(float(std::max(0.0, std::min(1.0, opacity))));
    for (int y = 0; y < h; y++) {
        const uint8_t* s = source.row(y);
        uint8_t* d = destination.row(y);
        for (int x = 0; x < w; x++, s += 4, d += 4) compositePixelSteps(mode, s, k, d);
    }
}

} // namespace compositor
