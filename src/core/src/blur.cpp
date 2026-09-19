#include "compositor/blur.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace compositor {

namespace {

constexpr int bandColumns = 64;   // columns per vertical-pass band: 64 x C values keep each row segment a few cache lines

// ---- pixel access templated on channel count ----------------------------------------------------

template <int C> struct Raster;
template <> struct Raster<4> {
    using Type = Image;
    static const uint8_t* row(const Image& i, int y) { return i.row(y); }
    static uint8_t* row(Image& i, int y) { return i.row(y); }
};
template <> struct Raster<1> {
    using Type = GrayImage;
    static const uint8_t* row(const GrayImage& i, int y) { return i.row(y); }
    static uint8_t* row(GrayImage& i, int y) { return i.row(y); }
};

/// Stores a float or 16.8 fixed-point value back into a byte, clamping colour to alpha for RGBA.
template <int C>
inline void storeRow(uint8_t* out, const float* in, int w) {
    for (int x = 0; x < w; x++, out += C, in += C) {
        if constexpr (C == 4) {
            uint8_t a = uint8_t(std::min(255.0f, std::max(0.0f, in[3] + 0.5f)));
            for (int c = 0; c < 3; c++) out[c] = uint8_t(std::min(float(a), std::max(0.0f, in[c] + 0.5f)));
            out[3] = a;
        } else {
            out[0] = uint8_t(std::min(255.0f, std::max(0.0f, in[0] + 0.5f)));
        }
    }
}

// ---- small sigma: separable FIR --------------------------------------------------------------------

std::vector<float> gaussianKernel(double sigma, int& radius) {
    radius = std::max(1, int(std::ceil(sigma * 3)));
    std::vector<float> kernel(size_t(radius) * 2 + 1);
    float total = 0;
    for (int i = -radius; i <= radius; i++) { kernel[size_t(i + radius)] = float(std::exp(-(i * i) / (2 * sigma * sigma))); total += kernel[size_t(i + radius)]; }
    for (auto& k : kernel) k /= total;
    return kernel;
}

/// Horizontal FIR from bytes into floats, zero outside.
template <int C>
void firRows(const typename Raster<C>::Type& image, std::vector<float>& out, const std::vector<float>& kernel, int radius) {
    const int w = image.width(), h = image.height();
    out.resize(size_t(w) * h * C);
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint8_t* src = Raster<C>::row(image, y);
            float* dst = &out[size_t(y) * w * C];
            for (int x = 0; x < w; x++, dst += C) {
                float acc[C] = {};
                const int i0 = std::max(-radius, -x), i1 = std::min(radius, w - 1 - x);
                for (int i = i0; i <= i1; i++) {
                    const float k = kernel[size_t(i + radius)];
                    const uint8_t* p = src + size_t(x + i) * C;
                    for (int c = 0; c < C; c++) acc[c] += p[c] * k;
                }
                for (int c = 0; c < C; c++) dst[c] = acc[c];
            }
        }
    });
}

/// Vertical FIR over column bands: each output row of a band is a weighted sum of the band's input rows,
/// so the inner loop runs over `bandColumns * C` contiguous floats.
template <int C>
void firColumns(const std::vector<float>& in, typename Raster<C>::Type& image, const std::vector<float>& kernel, int radius) {
    const int w = image.width(), h = image.height();
    const int bands = (w + bandColumns - 1) / bandColumns;
    parallelRows(0, bands, [&](int b0, int b1) {
        std::vector<float> acc(size_t(bandColumns) * C);
        for (int b = b0; b < b1; b++) {
            const int x0 = b * bandColumns, n = std::min(bandColumns, w - x0) * C;
            for (int y = 0; y < h; y++) {
                std::fill(acc.begin(), acc.begin() + n, 0.0f);
                const int i0 = std::max(-radius, -y), i1 = std::min(radius, h - 1 - y);
                for (int i = i0; i <= i1; i++) {
                    const float k = kernel[size_t(i + radius)];
                    const float* p = &in[(size_t(y + i) * w + x0) * C];
                    for (int j = 0; j < n; j++) acc[size_t(j)] += p[j] * k;
                }
                storeRow<C>(Raster<C>::row(image, y) + size_t(x0) * C, acc.data(), n / C);
            }
        }
    }, 1);
}

// ---- large sigma: three integer box passes ---------------------------------------------------------

// Three box blurs approximating a Gaussian of standard deviation sigma (Kovesi's sizes).
void boxSizes(double sigma, int sizes[3]) {
    double wIdeal = std::sqrt(12 * sigma * sigma / 3 + 1);
    int wl = int(std::floor(wIdeal));
    if (wl % 2 == 0) wl--;
    int wu = wl + 2;
    double mIdeal = (12 * sigma * sigma - 3 * wl * wl - 12 * wl - 9) / (-4 * wl - 4);
    int m = int(std::round(mIdeal));
    for (int i = 0; i < 3; i++) sizes[i] = i < m ? wl : wu;
}

/// Values travel between passes as 8.8 fixed point in uint16.
using Fixed = uint16_t;

/// Vertical box of radius r over column bands: a running column sum per value, divided as it goes.
template <int C>
void boxColumns(const std::vector<Fixed>& in, std::vector<Fixed>& out, int w, int h, int r) {
    const int n = 2 * r + 1;
    const float inv = 1.0f / n;   // a multiply instead of an integer division per value
    const int bands = (w + bandColumns - 1) / bandColumns;
    out.resize(in.size());
    parallelRows(0, bands, [&](int b0, int b1) {
        std::vector<int32_t> sum(size_t(bandColumns) * C);
        for (int b = b0; b < b1; b++) {
            const int x0 = b * bandColumns, len = std::min(bandColumns, w - x0) * C;
            std::fill(sum.begin(), sum.begin() + len, 0);
            for (int y = 0; y < std::min(r, h); y++) {
                const Fixed* p = &in[(size_t(y) * w + x0) * C];
                for (int j = 0; j < len; j++) sum[size_t(j)] += p[j];
            }
            for (int y = 0; y < h; y++) {
                if (y + r < h) { const Fixed* p = &in[(size_t(y + r) * w + x0) * C]; for (int j = 0; j < len; j++) sum[size_t(j)] += p[j]; }
                if (y - r - 1 >= 0) { const Fixed* p = &in[(size_t(y - r - 1) * w + x0) * C]; for (int j = 0; j < len; j++) sum[size_t(j)] -= p[j]; }
                Fixed* o = &out[(size_t(y) * w + x0) * C];
                for (int j = 0; j < len; j++) o[j] = Fixed(int32_t(float(sum[size_t(j)]) * inv + 0.5f));
            }
        }
    }, 1);
}

/// Horizontal box of radius r per row.
template <int C>
void boxRows(const std::vector<Fixed>& in, std::vector<Fixed>& out, int w, int h, int r) {
    const int n = 2 * r + 1;
    const float inv = 1.0f / n;
    out.resize(in.size());
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const Fixed* src = &in[size_t(y) * w * C];
            Fixed* dst = &out[size_t(y) * w * C];
            int32_t sum[C] = {};
            for (int x = 0; x < std::min(r, w); x++) for (int c = 0; c < C; c++) sum[c] += src[size_t(x) * C + c];
            for (int x = 0; x < w; x++) {
                if (x + r < w) for (int c = 0; c < C; c++) sum[c] += src[size_t(x + r) * C + c];
                if (x - r - 1 >= 0) for (int c = 0; c < C; c++) sum[c] -= src[size_t(x - r - 1) * C + c];
                for (int c = 0; c < C; c++) dst[size_t(x) * C + c] = Fixed(int32_t(float(sum[c]) * inv + 0.5f));
            }
        }
    });
}

template <int C>
void boxGaussian(typename Raster<C>::Type& image, double sigma) {
    const int w = image.width(), h = image.height();
    int sizes[3];
    boxSizes(sigma, sizes);
    std::vector<Fixed> a(size_t(w) * h * C), b;
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint8_t* p = Raster<C>::row(image, y);
            Fixed* f = &a[size_t(y) * w * C];
            for (int i = 0; i < w * C; i++) f[i] = Fixed(p[i] << 8);
        }
    });
    for (int s : sizes) {
        const int r = (s - 1) / 2;
        if (r <= 0) continue;
        boxRows<C>(a, b, w, h, r);
        boxColumns<C>(b, a, w, h, r);
    }
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* p = Raster<C>::row(image, y);
            const Fixed* f = &a[size_t(y) * w * C];
            for (int x = 0; x < w; x++, p += C, f += C) {
                if constexpr (C == 4) {
                    uint8_t alpha = uint8_t(std::min(255u, (unsigned(f[3]) + 128u) >> 8));
                    for (int c = 0; c < 3; c++) p[c] = uint8_t(std::min(unsigned(alpha), (unsigned(f[c]) + 128u) >> 8));
                    p[3] = alpha;
                } else {
                    p[0] = uint8_t(std::min(255u, (unsigned(f[0]) + 128u) >> 8));
                }
            }
        }
    });
}

template <int C>
void gaussianBlurImpl(typename Raster<C>::Type& image, double sigma) {
    if (image.isEmpty() || !(sigma > 0)) return;
    if (sigma <= 6) {
        int radius;
        std::vector<float> kernel = gaussianKernel(sigma, radius);
        std::vector<float> rows;
        firRows<C>(image, rows, kernel, radius);
        firColumns<C>(rows, image, kernel, radius);
    } else {
        boxGaussian<C>(image, sigma);
    }
}

// ---- motion blur: shear, box, shear back -----------------------------------------------------------

/// A float RGBA buffer with its own dimensions.
struct FloatImage {
    int width = 0, height = 0;
    std::vector<float> data;
    FloatImage(int w, int h) : width(w), height(h), data(size_t(w) * h * 4, 0.0f) {}
    float* row(int y) { return &data[size_t(y) * width * 4]; }
    const float* row(int y) const { return &data[size_t(y) * width * 4]; }
};

/// Linear interpolation between two rows of a byte image at fractional row position `yf`; zero outside.
inline void sampleRows(const Image& image, int x, double yf, float* out) {
    const int y0 = int(std::floor(yf));
    const float f = float(yf - y0);
    const float w0 = 1 - f, w1 = f;
    const uint8_t* p0 = y0 >= 0 && y0 < image.height() ? image.pixel(x, y0) : nullptr;
    const uint8_t* p1 = y0 + 1 >= 0 && y0 + 1 < image.height() ? image.pixel(x, y0 + 1) : nullptr;
    for (int c = 0; c < 4; c++) out[c] = (p0 ? p0[c] * w0 : 0.0f) + (p1 ? p1[c] * w1 : 0.0f);
}

/// The same along a row of a float image at fractional column `xf`.
inline void sampleColumns(const FloatImage& image, int y, double xf, float* out) {
    const int x0 = int(std::floor(xf));
    const float f = float(xf - x0);
    const float* row = image.row(y);
    const float* p0 = x0 >= 0 && x0 < image.width ? row + size_t(x0) * 4 : nullptr;
    const float* p1 = x0 + 1 >= 0 && x0 + 1 < image.width ? row + size_t(x0 + 1) * 4 : nullptr;
    for (int c = 0; c < 4; c++) out[c] = (p0 ? p0[c] * (1 - f) : 0.0f) + (p1 ? p1[c] * f : 0.0f);
}

inline void sampleRowsFloat(const FloatImage& image, int x, double yf, float* out) {
    const int y0 = int(std::floor(yf));
    const float f = float(yf - y0);
    const float* p0 = y0 >= 0 && y0 < image.height ? image.row(y0) + size_t(x) * 4 : nullptr;
    const float* p1 = y0 + 1 >= 0 && y0 + 1 < image.height ? image.row(y0 + 1) + size_t(x) * 4 : nullptr;
    for (int c = 0; c < 4; c++) out[c] = (p0 ? p0[c] * (1 - f) : 0.0f) + (p1 ? p1[c] * f : 0.0f);
}

/// Box of `length` samples along the rows of a float image, in place; ends outside count as zero.
void boxAlongRows(FloatImage& image, int length) {
    const int r = length / 2, n = 2 * r + 1, w = image.width;
    parallelRows(0, image.height, [&](int y0, int y1) {
        std::vector<float> line(size_t(w) * 4);
        for (int y = y0; y < y1; y++) {
            float* row = image.row(y);
            std::memcpy(line.data(), row, size_t(w) * 4 * sizeof(float));
            float sum[4] = {};
            const float inv = 1.0f / n;
            for (int x = 0; x < std::min(r, w); x++) for (int c = 0; c < 4; c++) sum[c] += line[size_t(x) * 4 + c];
            for (int x = 0; x < w; x++) {
                if (x + r < w) for (int c = 0; c < 4; c++) sum[c] += line[size_t(x + r) * 4 + c];
                if (x - r - 1 >= 0) for (int c = 0; c < 4; c++) sum[c] -= line[size_t(x - r - 1) * 4 + c];
                for (int c = 0; c < 4; c++) row[size_t(x) * 4 + c] = sum[c] * inv;
            }
        }
    });
}

/// Box of `length` samples down the columns, over column bands.
void boxAlongColumns(FloatImage& image, int length) {
    const int r = length / 2, n = 2 * r + 1, w = image.width, h = image.height;
    const int bands = (w + bandColumns - 1) / bandColumns;
    FloatImage out(w, h);
    parallelRows(0, bands, [&](int b0, int b1) {
        std::vector<float> sum(size_t(bandColumns) * 4);
        const float inv = 1.0f / n;
        for (int b = b0; b < b1; b++) {
            const int x0 = b * bandColumns, len = std::min(bandColumns, w - x0) * 4;
            std::fill(sum.begin(), sum.begin() + len, 0.0f);
            for (int y = 0; y < std::min(r, h); y++) { const float* p = image.row(y) + size_t(x0) * 4; for (int j = 0; j < len; j++) sum[size_t(j)] += p[j]; }
            for (int y = 0; y < h; y++) {
                if (y + r < h) { const float* p = image.row(y + r) + size_t(x0) * 4; for (int j = 0; j < len; j++) sum[size_t(j)] += p[j]; }
                if (y - r - 1 >= 0) { const float* p = image.row(y - r - 1) + size_t(x0) * 4; for (int j = 0; j < len; j++) sum[size_t(j)] -= p[j]; }
                float* o = out.row(y) + size_t(x0) * 4;
                for (int j = 0; j < len; j++) o[j] = sum[size_t(j)] * inv;
            }
        }
    }, 1);
    image.data.swap(out.data);
}

} // namespace

void gaussianBlur(Image& image, double sigma) { gaussianBlurImpl<4>(image, sigma); }
void gaussianBlur(GrayImage& image, double sigma) { gaussianBlurImpl<1>(image, sigma); }

void motionBlur(Image& image, double distance, double angleDegrees) {
    if (image.isEmpty() || distance < 1) return;
    const int w = image.width(), h = image.height();
    const double radians = angleDegrees * M_PI / 180;
    const double dx = std::cos(radians), dy = -std::sin(radians);   // y down
    if (std::fabs(dx) >= std::fabs(dy)) {
        // Columns shift vertically by x * slope so the smear becomes horizontal.
        const double slope = dy / dx;
        const double shiftMin = std::min(0.0, (w - 1) * slope), shiftMax = std::max(0.0, (w - 1) * slope);
        const int vOffset = int(std::floor(-shiftMax)) - 1;        // buffer row 0 is image row vOffset for column 0
        const int vRows = int(std::ceil(h - shiftMin)) + 2 - vOffset;
        FloatImage sheared(w, vRows);
        parallelRows(0, vRows, [&](int v0, int v1) {
            for (int v = v0; v < v1; v++) {
                float* row = sheared.row(v);
                for (int x = 0; x < w; x++, row += 4) sampleRows(image, x, (v + vOffset) + x * slope, row);
            }
        });
        boxAlongRows(sheared, std::max(1, int(std::round(distance * std::fabs(dx)))));
        parallelRows(0, h, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                uint8_t* out = image.row(y);
                float s[4];
                for (int x = 0; x < w; x++, out += 4) {
                    sampleRowsFloat(sheared, x, (y - x * slope) - vOffset, s);
                    storeRow<4>(out, s, 1);
                }
            }
        });
    } else {
        // Rows shift horizontally by y * slope so the smear becomes vertical.
        const double slope = dx / dy;
        const double shiftMin = std::min(0.0, (h - 1) * slope), shiftMax = std::max(0.0, (h - 1) * slope);
        const int uOffset = int(std::floor(-shiftMax)) - 1;
        const int uCols = int(std::ceil(w - shiftMin)) + 2 - uOffset;
        FloatImage sheared(uCols, h);
        parallelRows(0, h, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                float* row = sheared.row(y);
                for (int u = 0; u < uCols; u++, row += 4) {
                    // S(u, y) = I(u + y * slope, y), interpolated along the image row.
                    const double xf = (u + uOffset) + y * slope;
                    const int x0 = int(std::floor(xf));
                    const float f = float(xf - x0);
                    const uint8_t* p0 = x0 >= 0 && x0 < w ? image.pixel(x0, y) : nullptr;
                    const uint8_t* p1 = x0 + 1 >= 0 && x0 + 1 < w ? image.pixel(x0 + 1, y) : nullptr;
                    for (int c = 0; c < 4; c++) row[c] = (p0 ? p0[c] * (1 - f) : 0.0f) + (p1 ? p1[c] * f : 0.0f);
                }
            }
        });
        boxAlongColumns(sheared, std::max(1, int(std::round(distance * std::fabs(dy)))));
        parallelRows(0, h, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                uint8_t* out = image.row(y);
                float s[4];
                for (int x = 0; x < w; x++, out += 4) {
                    sampleColumns(sheared, y, (x - y * slope) - uOffset, s);
                    storeRow<4>(out, s, 1);
                }
            }
        });
    }
}

} // namespace compositor
