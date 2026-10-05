// The adjustments at 16 bits (adjustments.h): every kind on the exact straight colour of a 0..32768 buffer.
//
// The table-driven kinds build 32769-entry tables per channel from the same functions the 8-bit tables sample at
// 256 levels (so a smooth 16-bit gradient stays smooth through Levels or Curves); the colour kinds run their
// per-pixel maths in double (adjustments_more.cpp, colorlookup.cpp); Hue/Saturation samples its 33-point cube in
// float, without the 8-bit kernel's 8.8 fixed-point steps.
#include "compositor/adjustments.h"
#include "compositor/depth.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace compositor {

namespace {

constexpr int levels16 = int(one16) + 1;   // 32769 table entries: every 15-bit value

/// Three tables of 32769 entries, one per colour channel.
using Tables16 = std::array<std::vector<uint16_t>, 3>;

/// Tables from `f(channel, x)`, x and the result straight 0..1; false when every table is the identity.
template <class F>
bool buildTables(Tables16& tables, F&& f) {
    bool identity = true;
    for (int c = 0; c < 3; c++) {
        std::vector<uint16_t>& t = tables[size_t(c)];
        t.resize(size_t(levels16));
        for (int i = 0; i < levels16; i++) {
            const double v = f(c, i / double(one16));
            t[size_t(i)] = uint16_t(std::lround(std::clamp(std::isfinite(v) ? v : 0.0, 0.0, 1.0) * one16));
            identity = identity && t[size_t(i)] == i;
        }
    }
    return !identity;
}

/// Applies per-channel tables: opaque pixels by lookup, partial alpha through the straight value with its fraction
/// (interpolated between neighbouring entries), premultiplied again with rounding.
void applyTables(Image16& image, const Tables16& tables) {
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint16_t* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                const uint32_t a = p[3];
                if (!a) continue;
                if (a == one16) {
                    for (int c = 0; c < 3; c++) p[c] = tables[size_t(c)][std::min<uint32_t>(p[c], one16)];
                    continue;
                }
                const double k = double(one16) / a;
                for (int c = 0; c < 3; c++) {
                    const double s = std::min(double(one16), p[c] * k);
                    const int lo = int(s);
                    const int hi = std::min(int(one16), lo + 1);
                    const double f = s - lo;
                    const double v = tables[size_t(c)][size_t(lo)] * (1 - f) + tables[size_t(c)][size_t(hi)] * f;
                    p[c] = uint16_t(std::min<long>(long(a), std::lround(v * a / one16)));
                }
            }
        }
    });
}

template <class F>
void applyFunction(Image16& image, F&& f) {
    Tables16 tables;
    if (buildTables(tables, f)) applyTables(image, tables);
}

double exposureAt(const ExposureSettings& settings, double encoded) {
    // ExposureSettings::table()'s maths at any input.
    const ExposureSettings s = settings.normalized();
    const double scale = std::pow(2.0, s.exposure);
    double linear = encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
    linear = std::pow(std::max(0.0, linear * scale + s.offset), 1 / s.gamma);
    const double output = linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
    return std::min(1.0, std::max(0.0, output));
}

double posterizeAt(const PosterizeSettings& settings, double x) {
    // Photoshop's buckets are slices of the 256 levels (posterizeTransfer): the bucket is chosen on the input rounded
    // to a level, so an 8-bit image converted to 16 bits posterizes exactly as it did; the output steps are exact.
    const int n = std::clamp(settings.levels, 2, 255);
    const long v = std::lround(std::clamp(x, 0.0, 1.0) * 255);
    return double(v * n / 256) / (n - 1);
}

// ---- Hue/Saturation: a float cube, sampled tetrahedrally as the 8-bit kernel does ------------------------------

constexpr int cubeDim = 33;

void applyHueSaturation16(Image16& image, const HueSaturationSettings& settings) {
    if (settings.isIdentity()) return;
    if (settings.colorize) {
        // Colorize keeps only the lightness (max + min) / 2: one entry per sum of two 15-bit values.
        const int keys = 2 * int(one16) + 1;
        std::vector<float> table(size_t(keys) * 3);
        parallelRows(0, keys, [&](int k0, int k1) {
            for (int key = k0; key < k1; key++) {
                double r = key / double(keys - 1), g = r, b = r;
                settings.adjust(r, g, b);
                table[size_t(key) * 3] = float(r); table[size_t(key) * 3 + 1] = float(g); table[size_t(key) * 3 + 2] = float(b);
            }
        }, 1024);
        parallelRows(0, image.height(), [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                uint16_t* p = image.row(y);
                for (int x = 0; x < image.width(); x++, p += 4) {
                    const uint32_t a = p[3];
                    if (!a) continue;
                    uint32_t s[3];
                    for (int c = 0; c < 3; c++) s[c] = std::min<uint32_t>(one16, (p[c] * one16 + a / 2) / a);
                    const float* t = &table[size_t(std::max({s[0], s[1], s[2]}) + std::min({s[0], s[1], s[2]})) * 3];
                    for (int c = 0; c < 3; c++) p[c] = uint16_t(std::min<long>(long(a), std::lround(std::clamp(t[c], 0.0f, 1.0f) * float(a))));
                }
            }
        });
        return;
    }
    std::vector<float> cube(size_t(cubeDim) * cubeDim * cubeDim * 3);
    parallelRows(0, cubeDim, [&](int b0, int b1) {
        for (int bi = b0; bi < b1; bi++)
            for (int gi = 0; gi < cubeDim; gi++)
                for (int ri = 0; ri < cubeDim; ri++) {
                    double r = ri / double(cubeDim - 1), g = gi / double(cubeDim - 1), b = bi / double(cubeDim - 1);
                    settings.adjust(r, g, b);
                    const size_t index = (size_t(bi) * cubeDim * cubeDim + size_t(gi) * cubeDim + size_t(ri)) * 3;
                    cube[index] = float(r); cube[index + 1] = float(g); cube[index + 2] = float(b);
                }
    }, 1);
    constexpr size_t stepR = 3, stepG = size_t(cubeDim) * 3, stepB = size_t(cubeDim) * cubeDim * 3;
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint16_t* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                const uint32_t a = p[3];
                if (!a) continue;
                int index[3];
                float frac[3];
                for (int c = 0; c < 3; c++) {
                    const float s = std::min(1.0f, float(p[c]) / float(a)) * float(cubeDim - 1);
                    index[c] = std::min(cubeDim - 2, int(s));
                    frac[c] = s - float(index[c]);
                }
                const size_t step[3] = {stepR, stepG, stepB};
                int order[3] = {0, 1, 2};
                if (frac[order[0]] < frac[order[1]]) std::swap(order[0], order[1]);
                if (frac[order[1]] < frac[order[2]]) std::swap(order[1], order[2]);
                if (frac[order[0]] < frac[order[1]]) std::swap(order[0], order[1]);
                const float* c0 = &cube[size_t(index[2]) * stepB + size_t(index[1]) * stepG + size_t(index[0]) * stepR];
                const float* cA = c0 + step[order[0]];
                const float* cB = cA + step[order[1]];
                const float* c1 = cB + step[order[2]];
                const float tA = frac[order[0]], tB = frac[order[1]], tC = frac[order[2]];
                for (int c = 0; c < 3; c++) {
                    const float v = c0[c] + (cA[c] - c0[c]) * tA + (cB[c] - cA[c]) * tB + (c1[c] - cB[c]) * tC;
                    p[c] = uint16_t(std::min<long>(long(a), std::lround(std::clamp(v, 0.0f, 1.0f) * float(a))));
                }
            }
        }
    });
}

// ---- Gradient Map: the colour at the exact luma, not a 256-step table -------------------------------------------

void applyGradientMap16(Image16& image, const GradientMapSettings& settings) {
    const AdjustmentColor dark = (settings.reversed ? settings.highlights : settings.shadows).clamped();
    const AdjustmentColor light = (settings.reversed ? settings.shadows : settings.highlights).clamped();
    const double from[3] = {dark.red, dark.green, dark.blue}, span[3] = {light.red - dark.red, light.green - dark.green, light.blue - dark.blue};
    // Perceptual and Linear: the method's ramp, sampled at the exact luma.
    const std::optional<GradientStops> ramp = settings.method == GradientMethod::Classic ? std::nullopt : std::optional<GradientStops>(settings.ramp());
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint16_t* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                const uint32_t a = p[3];
                if (!a) continue;
                const double t = std::min(1.0, (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) / a);
                if (ramp) {
                    float col[4];
                    ramp->sample(float(t), col);
                    for (int c = 0; c < 3; c++) p[c] = uint16_t(std::min<long>(long(a), std::lround(std::clamp(double(col[c]), 0.0, 1.0) * a)));
                    continue;
                }
                for (int c = 0; c < 3; c++) p[c] = uint16_t(std::min<long>(long(a), std::lround((from[c] + span[c] * t) * a)));
            }
        }
    });
}

// ---- Grain: adjust_grain's pattern (upstream Compositor's AdjustPixels.c, MIT) at 16 bits --------------------------

inline uint32_t mix32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

inline float lattice(int64_t ix, int64_t iy, uint32_t seed) {
    const uint32_t h = mix32(uint32_t(ix) * 0x9E3779B1U ^ mix32(uint32_t(iy) * 0x85EBCA77U ^ seed));
    return float(h & 0xFFFFU) / 65535.0f + float(h >> 16) / 65535.0f - 1.0f;
}

void applyGrain16(Image16& image, const GrainSettings& settings, Point origin, double unitsPerPixel) {
    const GrainSettings s = settings.normalized();
    if (s.amount <= 0 || !(unitsPerPixel > 0) || !std::isfinite(unitsPerPixel)) return;
    const double size = s.size > 0 ? s.size : 1;
    const float strength = float(std::min(1.0, s.amount / 100.0)) * 0.35f;   // of full scale
    const float rough = float(std::clamp(s.roughness / 100.0, 0.0, 1.0));
    const uint32_t seed = s.seed, fineSeed = mix32(seed ^ 0xA511E9B3U);
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const double v = origin.y + y * unitsPerPixel + 0.5 * unitsPerPixel;
            const double cellY = std::floor(v / size);
            float ty = float(v / size - cellY);
            ty = ty * ty * (3.0f - 2.0f * ty);
            const int64_t iy = int64_t(cellY), fineY = int64_t(std::floor(v));
            uint16_t* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                const uint32_t a = p[3];
                if (!a) continue;
                const double u = origin.x + (x + 0.5) * unitsPerPixel;
                const double cellX = std::floor(u / size);
                float tx = float(u / size - cellX);
                tx = tx * tx * (3.0f - 2.0f * tx);
                const int64_t ix = int64_t(cellX);
                const float n00 = lattice(ix, iy, seed), n10 = lattice(ix + 1, iy, seed);
                const float n01 = lattice(ix, iy + 1, seed), n11 = lattice(ix + 1, iy + 1, seed);
                const float top = n00 + (n10 - n00) * tx, bottom = n01 + (n11 - n01) * tx;
                const float smooth = (top + (bottom - top) * ty) * 1.6f;
                const float fine = lattice(int64_t(std::floor(u)), fineY, fineSeed);
                const float noise = smooth + (fine - smooth) * rough;
                const float k = 1.0f / float(a);
                const float r = p[0] * k, g = p[1] * k, b = p[2] * k;
                const float level = std::min(1.0f, 0.2126f * r + 0.7152f * g + 0.0722f * b);
                const float delta = noise * strength * (0.4f + 2.4f * level * (1.0f - level));
                const float c[3] = {r, g, b};
                for (int i = 0; i < 3; i++) p[i] = uint16_t(std::clamp(c[i] + delta, 0.0f, 1.0f) * float(a) + 0.5f);
            }
        }
    });
}

} // namespace

void applyInvert(Image16& image) {
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint16_t* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4)
                for (int c = 0; c < 3; c++) p[c] = uint16_t(p[3] - std::min(p[c], p[3]));
        }
    });
}

void applyInvert(Gray16& mask) {
    uint16_t* d = mask.data();
    for (size_t i = 0, n = size_t(mask.width()) * size_t(mask.height()); i < n; i++) d[i] = uint16_t(one16 - std::min<uint32_t>(d[i], one16));
}

bool applyAdjustment(const AdjustmentSettings& settings, Image16& image, const Rect& region, double scale) {
    switch (settings.kind) {
    case AdjustmentKind::HueSaturation: applyHueSaturation16(image, settings.hsv); return true;
    case AdjustmentKind::Levels:
        if (!settings.levels.isIdentity()) applyFunction(image, [&](int c, double x) { return settings.levels.apply(x, c + 1); });
        return true;
    case AdjustmentKind::Curves:
        if (settings.curves.isValid() && !settings.curves.isIdentity())
            applyFunction(image, [&](int c, double x) { return settings.curves.value(settings.curves.value(x * 255, c + 1), 0) / 255; });
        return true;
    case AdjustmentKind::Exposure:
        if (!settings.exposure.normalized().isIdentity()) applyFunction(image, [&](int, double x) { return exposureAt(settings.exposure, x); });
        return true;
    case AdjustmentKind::GradientMap: applyGradientMap16(image, settings.gradientMap); return true;
    case AdjustmentKind::Grain: {
        const Rect r = region.isEmpty() ? Rect(0, 0, image.width(), image.height()) : region;
        applyGrain16(image, settings.grain, r.origin(), scale > 0 ? 1 / scale : 1);
        return true;
    }
    case AdjustmentKind::Invert: applyInvert(image); return true;
    case AdjustmentKind::BrightnessContrast:
        applyFunction(image, [&](int, double x) { return brightnessContrastAt(settings.brightnessContrast, x); });
        return true;
    case AdjustmentKind::Posterize: applyFunction(image, [&](int, double x) { return posterizeAt(settings.posterize, x); }); return true;
    case AdjustmentKind::Threshold: applyThreshold(image, settings.threshold); return true;
    case AdjustmentKind::BlackWhite: applyBlackWhite(image, settings.blackWhite); return true;
    case AdjustmentKind::ColorBalance: applyColorBalance(image, settings.colorBalance); return true;
    case AdjustmentKind::Vibrance: applyVibrance(image, settings.vibrance); return true;
    case AdjustmentKind::PhotoFilter: applyPhotoFilter(image, settings.photoFilter); return true;
    case AdjustmentKind::ChannelMixer: applyChannelMixer(image, settings.channelMixer); return true;
    case AdjustmentKind::SelectiveColor: applySelectiveColor(image, settings.selectiveColor); return true;
    case AdjustmentKind::ColorLookup: applyColorLookup(image, settings.colorLookup); return true;
    }
    return false;
}

bool applyAdjustment(const LayerAdjustment& adjustment, Image16& image, const Rect& region, double scale) {
    AdjustmentSettings settings;
    if (!AdjustmentSettings::parse(adjustment.json, settings)) return false;
    return applyAdjustment(settings, image, region, scale);
}

std::array<std::vector<double>, 4> levelsHistogram(const Image16& image, const Gray16* coverage) {
    // The dialog's histogram has 256 bins whatever the depth: the 8-bit counts of the image rounded to 8 bits.
    auto narrow = narrowImage(image);
    std::shared_ptr<GrayImage> cover = coverage ? narrowGray(*coverage) : nullptr;
    return levelsHistogram(*narrow, cover.get());
}

} // namespace compositor
