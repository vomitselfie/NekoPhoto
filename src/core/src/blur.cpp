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

// ---- large sigma: Deriche's recursive Gaussian ----------------------------------------------------
//
// A fourth-order IIR fit to the Gaussian (Deriche 1993; Getreuer, IPOL 2013): a causal and an anticausal
// pass whose sum is within 0.05 % of the true kernel at every sigma, at a cost independent of sigma.
// y+[n] = sum b_k x[n-k] - sum a_k y+[n-k]; y-[n] = sum b'_k x[n+k] - sum a_k y-[n+k]; y = y+ + y-.

struct DericheFilter {
    double b[4], a[4], anti[4];   // b0..b3, a1..a4, b'1..b'4
};

DericheFilter dericheFor(double sigma) {
    // Deriche's fit: h(n) = sum_k e^{-l_k n/s} (alpha_k cos(w_k n/s) + beta_k sin(w_k n/s)), n >= 0.
    static const double alpha[2] = {1.6800, -0.6803}, beta[2] = {3.7350, -0.2598}, omega[2] = {0.6318, 1.9970}, lambda[2] = {1.7830, 1.7230};
    // Each term is (n0 + n1 z^-1) / (1 + d1 z^-1 + d2 z^-2); the sum has a cubic numerator over a quartic.
    double n[2][2], d[2][3];
    for (int k = 0; k < 2; k++) {
        const double e = std::exp(-lambda[k] / sigma), c = std::cos(omega[k] / sigma), sn = std::sin(omega[k] / sigma);
        n[k][0] = alpha[k];
        n[k][1] = -alpha[k] * e * c + beta[k] * e * sn;
        d[k][0] = 1; d[k][1] = -2 * e * c; d[k][2] = e * e;
    }
    double num[4] = {0, 0, 0, 0}, den[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < 2; i++) for (int j = 0; j < 3; j++) { num[i + j] += n[0][i] * d[1][j]; num[i + j] += n[1][i] * d[0][j]; }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) den[i + j] += d[0][i] * d[1][j];
    DericheFilter f;
    for (int k = 0; k < 4; k++) { f.b[k] = num[k]; f.a[k] = den[k + 1]; }
    // The anticausal numerator from the causal one, then both scaled so the whole response sums to one.
    for (int k = 0; k < 4; k++) f.anti[k] = (k < 3 ? f.b[k + 1] : 0) - f.a[k] * f.b[0];
    double sumB = 0, sumA = 1, sumAnti = 0;
    for (int k = 0; k < 4; k++) { sumB += f.b[k]; sumA += f.a[k]; sumAnti += f.anti[k]; }
    const double gain = (sumB + sumAnti) / sumA;
    for (int k = 0; k < 4; k++) { f.b[k] /= gain; f.anti[k] /= gain; }
    return f;
}

/// Both passes along one line of `n` samples with `lanes` interleaved channels (a row, or a band of columns
/// when `stride` steps between rows); beyond the ends the signal is zero (transparent), as for the FIR.
template <int C>
void dericheLine(const float* in, float* out, int n, int lanes, size_t stride, const DericheFilter& f, double* scratch) {
    if (n <= 0 || lanes <= 0) return;
    // The feedback cancels large terms (the poles sit near 1 for a wide kernel), so the state and the sums
    // are double; the samples stay float.
    const double b0 = f.b[0], b1 = f.b[1], b2 = f.b[2], b3 = f.b[3];
    const double a1 = f.a[0], a2 = f.a[1], a3 = f.a[2], a4 = f.a[3];
    const double c1 = f.anti[0], c2 = f.anti[1], c3 = f.anti[2], c4 = f.anti[3];
    const size_t count = size_t(lanes);
    double *x1 = scratch, *x2 = x1 + count, *x3 = x2 + count, *x4 = x3 + count, *y1 = x4 + count, *y2 = y1 + count, *y3 = y2 + count, *y4 = y3 + count;
    std::fill(scratch, scratch + count * 8, 0.0);
    for (int i = 0; i < n; i++) {
        const float* x = in + size_t(i) * stride;
        float* y = out + size_t(i) * stride;
        for (size_t l = 0; l < count; l++) {
            const double v = b0 * x[l] + b1 * x1[l] + b2 * x2[l] + b3 * x3[l] - a1 * y1[l] - a2 * y2[l] - a3 * y3[l] - a4 * y4[l];
            x3[l] = x2[l]; x2[l] = x1[l]; x1[l] = x[l];
            y4[l] = y3[l]; y3[l] = y2[l]; y2[l] = y1[l]; y1[l] = v;
            y[l] = float(v);
        }
    }
    std::fill(scratch, scratch + count * 8, 0.0);
    for (int i = n - 1; i >= 0; i--) {
        const float* x = in + size_t(i) * stride;
        float* y = out + size_t(i) * stride;
        for (size_t l = 0; l < count; l++) {
            const double v = c1 * x1[l] + c2 * x2[l] + c3 * x3[l] + c4 * x4[l] - a1 * y1[l] - a2 * y2[l] - a3 * y3[l] - a4 * y4[l];
            x4[l] = x3[l]; x3[l] = x2[l]; x2[l] = x1[l]; x1[l] = x[l];
            y4[l] = y3[l]; y3[l] = y2[l]; y2[l] = y1[l]; y1[l] = v;
            y[l] += float(v);
        }
    }
}

template <int C>
void dericheGaussian(typename Raster<C>::Type& image, double sigma) {
    const int w = image.width(), h = image.height();
    const DericheFilter f = dericheFor(sigma);
    // Rows: bytes in, floats out.
    std::vector<float> rows(size_t(w) * h * C);
    parallelRows(0, h, [&](int y0, int y1) {
        std::vector<float> in(size_t(w) * C);
        std::vector<double> scratch(8 * size_t(C));
        for (int y = y0; y < y1; y++) {
            const uint8_t* p = Raster<C>::row(image, y);
            for (int i = 0; i < w * C; i++) in[size_t(i)] = p[i];
            dericheLine<C>(in.data(), &rows[size_t(y) * w * C], w, C, C, f, scratch.data());
        }
    });
    // Columns in bands, row-major: the recursion steps down the rows while the inner loop runs across the band.
    const int bands = (w + bandColumns - 1) / bandColumns;
    parallelRows(0, bands, [&](int b0, int b1) {
        std::vector<float> band(size_t(bandColumns) * C * h), in(size_t(bandColumns) * C * h);
        std::vector<double> scratch(8 * size_t(bandColumns) * C);
        for (int b = b0; b < b1; b++) {
            const int x0 = b * bandColumns, lanes = std::min(bandColumns, w - x0) * C;
            for (int y = 0; y < h; y++) std::copy_n(&rows[(size_t(y) * w + size_t(x0)) * C], lanes, &in[size_t(y) * lanes]);
            dericheLine<C>(in.data(), band.data(), h, lanes, size_t(lanes), f, scratch.data());
            for (int y = 0; y < h; y++) storeRow<C>(Raster<C>::row(image, y) + size_t(x0) * C, &band[size_t(y) * lanes], lanes / C);
        }
    }, 1);
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
        dericheGaussian<C>(image, sigma);
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
