#include "compositor/kernels.h"
#include "compositor/resample.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace compositor::kernels {

namespace {

// ---- shared helpers ------------------------------------------------------------------------------

/// round(v * a / 255): premultiply a straight value.
inline uint8_t premultiplied(unsigned v, unsigned a) { return uint8_t((v * a + 127u) / 255u); }

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

} // namespace

// ---- Levels / Curves / Exposure -------------------------------------------------------------------

void applyChannelTables(Image& image, const ChannelTables& tables) {
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                unsigned a = p[3];
                if (a == 0) continue;
                if (a == 255) {
                    p[0] = tables.lut[0][p[0]];
                    p[1] = tables.lut[1][p[1]];
                    p[2] = tables.lut[2][p[2]];
                    continue;
                }
                // Partial alpha: the straight value has a fraction; interpolate the table over it as the reference does.
                for (int c = 0; c < 3; c++) {
                    unsigned x8 = std::min(255u * 256u, (p[c] * 255u * 256u + a / 2) / a);   // straight value, 8 fractional bits
                    unsigned lo = x8 >> 8, hi = std::min(255u, lo + 1), frac = x8 & 255u;
                    int v = int(tables.lut[c][lo]) + ((int(tables.lut[c][hi]) - int(tables.lut[c][lo])) * int(frac) + 128) / 256;
                    p[c] = premultiplied(unsigned(std::clamp(v, 0, 255)), a);
                }
            }
        }
    });
}

// ---- Hue/Saturation ------------------------------------------------------------------------------

namespace {

/// The straight channels of a premultiplied pixel with alpha `a` (nonzero), rounded to bytes.
inline void straightChannels(const uint8_t* p, unsigned a, unsigned out[3]) {
    if (a == 255) { out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; return; }
    const unsigned inverse = (255u << 16) / a + 1;   // 255 / a in 16.16, rounded up so p == a gives 255
    for (int c = 0; c < 3; c++) out[c] = std::min(255u, (p[c] * inverse + 32768u) >> 16);
}

/// An 8.8 fixed-point straight value (0..65280) premultiplied by `a`, rounded.
inline uint8_t premultiplied88(unsigned value, unsigned a) { return uint8_t((value * a + 32640u) / 65280u); }

} // namespace

void applyColorCube(Image& image, const uint16_t* cube) {
    constexpr unsigned dim = unsigned(cubeDim);
    constexpr size_t stepR = 3, stepG = size_t(dim) * 3, stepB = size_t(dim) * dim * 3;
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                const unsigned a = p[3];
                if (!a) continue;
                unsigned s[3];
                straightChannels(p, a, s);
                // Cell index and fraction (0..255 of a cell) along each axis: 32 cells span 0..255, 255 per cell.
                unsigned index[3], frac[3];
                for (int c = 0; c < 3; c++) {
                    unsigned position = s[c] * (dim - 1);
                    index[c] = position / 255; frac[c] = position % 255;
                    if (index[c] == dim - 1) { index[c]--; frac[c] = 255; }
                }
                // Tetrahedral: from the cell's origin corner, step along the axes in decreasing fraction order.
                const size_t step[3] = {stepR, stepG, stepB};
                int order[3] = {0, 1, 2};
                if (frac[order[0]] < frac[order[1]]) std::swap(order[0], order[1]);
                if (frac[order[1]] < frac[order[2]]) std::swap(order[1], order[2]);
                if (frac[order[0]] < frac[order[1]]) std::swap(order[0], order[1]);
                const uint16_t* c0 = cube + index[2] * stepB + index[1] * stepG + index[0] * stepR;
                const uint16_t* cA = c0 + step[order[0]];
                const uint16_t* cB = cA + step[order[1]];
                const uint16_t* c1 = cB + step[order[2]];
                const int tA = int(frac[order[0]]), tB = int(frac[order[1]]), tC = int(frac[order[2]]);
                for (int c = 0; c < 3; c++) {
                    // The weights (255 - tA), (tA - tB), (tB - tC), tC are all non-negative, so the sum is too.
                    int v = int(c0[c]) * 255 + (int(cA[c]) - int(c0[c])) * tA + (int(cB[c]) - int(cA[c])) * tB + (int(c1[c]) - int(cB[c])) * tC;
                    p[c] = premultiplied88(unsigned(v + 127) / 255u, a);
                }
            }
        }
    });
}

void applyLightnessTable(Image& image, const uint16_t* table) {
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                const unsigned a = p[3];
                if (!a) continue;
                unsigned s[3];
                straightChannels(p, a, s);
                const uint16_t* t = table + (std::max({s[0], s[1], s[2]}) + std::min({s[0], s[1], s[2]})) * 3;
                for (int c = 0; c < 3; c++) p[c] = premultiplied88(t[c], a);
            }
        }
    });
}

// ---- Add Noise -----------------------------------------------------------------------------------

void addNoise(Image& image, float amount, bool gaussian, bool monochromatic, uint32_t seed) {
    const float spread = amount / 100.0f * 127.5f;
    const uint32_t width = uint32_t(image.width());
    auto unit = [](uint32_t key) { return float(hash32(key) >> 8) * (1.0f / 16777216.0f); };
    auto sample = [&](uint32_t key) {
        if (!gaussian) return (unit(key) * 2.0f - 1.0f) * spread;
        // Box-Muller: two uniform values make one normally distributed one.
        float u1 = unit(key), u2 = unit(key ^ 0x68e31da4U);
        return std::sqrt(-2.0f * std::log(1.0f - u1)) * std::cos(6.2831853f * u2) * spread * (2.0f / 3.0f);
    };
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* p = image.row(y);
            for (uint32_t x = 0; x < width; x++, p += 4) {
                unsigned alpha = p[3];
                if (!alpha) continue;
                uint32_t base = hash32(seed ^ hash32(uint32_t(y) * width + x));
                float mono = monochromatic ? sample(base) : 0;
                for (int c = 0; c < 3; c++) {
                    float n = monochromatic ? mono : sample(base + uint32_t(c) * 0x9e3779b9U);
                    float value = float(p[c]) * 255.0f / float(alpha) + n;
                    value = value < 0 ? 0 : value > 255 ? 255 : value;
                    p[c] = uint8_t(std::lround(value * float(alpha) / 255.0f));
                }
            }
        }
    });
}

// ---- Lens Correction -----------------------------------------------------------------------------

void lensDistort(const Image& source, Image& destination, double k, bool bicubic) {
    const int width = source.width(), height = source.height();
    const double cx = width * 0.5, cy = height * 0.5;
    const double halfDiagonal2 = cx * cx + cy * cy;
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const double dy = y + 0.5 - cy;
            uint8_t* out = destination.row(y);
            for (int x = 0; x < width; x++, out += 4) {
                const double dx = x + 0.5 - cx;
                const double scale = 1.0 - k * (dx * dx + dy * dy) / halfDiagonal2;
                // Source position in pixel-centre coordinates; samples outside are transparent.
                const double sx = cx + dx * scale - 0.5, sy = cy + dy * scale - 0.5;
                const double fx0 = std::floor(sx), fy0 = std::floor(sy);
                const double fx = sx - fx0, fy = sy - fy0;
                const long x0 = long(fx0), py0 = long(fy0);
                if (bicubic) {
                    // Catmull-Rom over the 4x4 neighbourhood: sharper where the correction magnifies (the corners).
                    const int16_t *wx = catmullRomWeights(int(fx * 256)), *wy = catmullRomWeights(int(fy * 256));
                    int acc[4] = {0, 0, 0, 0};
                    for (int j = 0; j < 4; j++) {
                        const long row = py0 - 1 + j;
                        if (row < 0 || row >= height || wy[j] == 0) continue;
                        const uint8_t* line = source.row(int(row));
                        int h[4] = {0, 0, 0, 0};
                        for (int i = 0; i < 4; i++) {
                            const long column = x0 - 1 + i;
                            if (column < 0 || column >= width) continue;
                            const uint8_t* p = line + size_t(column) * 4;
                            for (int c = 0; c < 4; c++) h[c] += p[c] * wx[i];
                        }
                        for (int c = 0; c < 4; c++) acc[c] += h[c] * wy[j];
                    }
                    int a = std::clamp((acc[3] + 32768) >> 16, 0, 255);
                    for (int c = 0; c < 3; c++) out[c] = uint8_t(std::clamp((acc[c] + 32768) >> 16, 0, a));
                    out[3] = uint8_t(a);
                    continue;
                }
                double sums[4] = {0, 0, 0, 0};
                for (int j = 0; j < 2; j++) {
                    const long row = py0 + j;
                    if (row < 0 || row >= height) continue;
                    const double wy = j ? fy : 1 - fy;
                    if (wy == 0) continue;
                    const uint8_t* line = source.row(int(row));
                    for (int i = 0; i < 2; i++) {
                        const long column = x0 + i;
                        if (column < 0 || column >= width) continue;
                        const double weight = wy * (i ? fx : 1 - fx);
                        if (weight == 0) continue;
                        const uint8_t* p = line + size_t(column) * 4;
                        for (int c = 0; c < 4; c++) sums[c] += weight * p[c];
                    }
                }
                for (int c = 0; c < 4; c++) out[c] = uint8_t(std::lround(sums[c]));
            }
        }
    });
}

// ---- Gradient Map --------------------------------------------------------------------------------

void gradientMap(Image& image, const uint8_t* table) {
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                const unsigned a = p[3];
                if (a == 0) continue;
                // Luma of the straight colour: the premultiplied luma scaled by 255/a, one rounded division.
                const unsigned luma = 2126u * p[0] + 7152u * p[1] + 722u * p[2];
                unsigned level = a == 255 ? (luma + 5000u) / 10000u : (luma * 255u + 5000u * a) / (10000u * a);
                const uint8_t* color = table + std::min(255u, level) * 3;
                p[0] = premultiplied(color[0], a);
                p[1] = premultiplied(color[1], a);
                p[2] = premultiplied(color[2], a);
            }
        }
    });
}

// ---- Invert --------------------------------------------------------------------------------------

void invertColors(Image& image) {
#if defined(__GNUC__) || defined(__clang__)
    typedef uint8_t u8x16 __attribute__((vector_size(16)));
    constexpr bool vectorised = true;
#else
    constexpr bool vectorised = false;
#endif
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* p = image.row(y);
            int x = 0;
#if defined(__GNUC__) || defined(__clang__)
            if (vectorised) {
                for (; x + 4 <= image.width(); x += 4, p += 16) {
                    u8x16 v;
                    std::memcpy(&v, p, 16);
                    // Every lane takes its pixel's alpha; the alpha lanes then get their own value back.
                    u8x16 a = __builtin_shufflevector(v, v, 3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15);
                    u8x16 inverted = a - v;
                    u8x16 result = __builtin_shufflevector(inverted, v, 0, 1, 2, 19, 4, 5, 6, 23, 8, 9, 10, 27, 12, 13, 14, 31);
                    std::memcpy(p, &result, 16);
                }
            }
#endif
            for (; x < image.width(); x++, p += 4) {
                const uint8_t a = p[3];
                p[0] = uint8_t(a - p[0]);
                p[1] = uint8_t(a - p[1]);
                p[2] = uint8_t(a - p[2]);
            }
        }
    });
}

} // namespace compositor::kernels
