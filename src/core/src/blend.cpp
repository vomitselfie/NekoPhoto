#include "compositor/blend.h"
#include <algorithm>
#include <cmath>

namespace compositor {

namespace {

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
    default: return cs;
    }
}

} // namespace

Rgb blendColor(BlendMode mode, Rgb cb, Rgb cs) {
    switch (mode) {
    case BlendMode::Hue: return setLum(setSat(cs, sat(cb)), lum(cb));
    case BlendMode::Saturation: return setLum(setSat(cb, sat(cs)), lum(cb));
    case BlendMode::Color: return setLum(cs, lum(cb));
    case BlendMode::Luminosity: return setLum(cb, lum(cs));
    default: return {separable(mode, cb.r, cs.r), separable(mode, cb.g, cs.g), separable(mode, cb.b, cs.b)};
    }
}

void compositePixel(BlendMode mode, const uint8_t* src, float coverage, uint8_t* dst) {
    if (mode == BlendMode::Normal) {
        // Source-over in fixed point: out = src * coverage + dst * (1 - srcAlpha * coverage).
        unsigned k = unsigned(coverage * 256.0f + 0.5f);
        if (k == 0) return;
        if (k > 256) k = 256;
        unsigned sa = (src[3] * k + 128) >> 8;
        if (sa == 0) return;
        unsigned inv = 255 - sa;
        for (int c = 0; c < 3; c++) dst[c] = uint8_t(((src[c] * k + 128) >> 8) + ((dst[c] * inv + 127) / 255));
        dst[3] = uint8_t(sa + (dst[3] * inv + 127) / 255);
        return;
    }
    float as = src[3] / 255.0f * coverage;
    if (as <= 0) return;
    float k = coverage / 255.0f;
    float sr = src[0] * k, sg = src[1] * k, sb = src[2] * k; // premultiplied, 0..1, scaled by coverage
    float ab = dst[3] / 255.0f;
    float br = dst[0] / 255.0f, bg = dst[1] / 255.0f, bb = dst[2] / 255.0f;
    float ao = as + ab * (1 - as);
    float r, g, b;
    if (mode == BlendMode::Normal || ab <= 0) {
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

void compositeImage(BlendMode mode, const Image& source, double opacity, Image& destination) {
    int w = std::min(source.width(), destination.width()), h = std::min(source.height(), destination.height());
    float coverage = float(std::max(0.0, std::min(1.0, opacity)));
    for (int y = 0; y < h; y++) {
        const uint8_t* s = source.row(y);
        uint8_t* d = destination.row(y);
        for (int x = 0; x < w; x++, s += 4, d += 4) compositePixel(mode, s, coverage, d);
    }
}

} // namespace compositor
