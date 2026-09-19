#include "compositor/resample.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace compositor {

namespace {

// ---- Kernels -------------------------------------------------------------------------------------

struct CatmullRomTable {
    int16_t weights[257][4];
    CatmullRomTable() {
        for (int i = 0; i <= 256; i++) {
            double t = i / 256.0, t2 = t * t, t3 = t2 * t;
            double k[4] = {(-t3 + 2 * t2 - t) / 2, (3 * t3 - 5 * t2 + 2) / 2, (-3 * t3 + 4 * t2 + t) / 2, (t3 - t2) / 2};
            int sum = 0;
            for (int j = 0; j < 4; j++) { weights[i][j] = int16_t(std::lround(k[j] * 256)); sum += weights[i][j]; }
            weights[i][t < 0.5 ? 1 : 2] += int16_t(256 - sum);   // the rounding remainder goes to the largest weight
        }
    }
};

const CatmullRomTable& catmullRom() { static const CatmullRomTable table; return table; }

double kernelRadius(ResampleFilter filter) {
    switch (filter) {
    case ResampleFilter::Triangle: return 1;
    case ResampleFilter::CatmullRom: return 2;
    case ResampleFilter::Lanczos3: return 3;
    }
    return 1;
}

double kernelValue(ResampleFilter filter, double x) {
    x = std::fabs(x);
    switch (filter) {
    case ResampleFilter::Triangle: return std::max(0.0, 1 - x);
    case ResampleFilter::CatmullRom: {
        double x2 = x * x, x3 = x2 * x;
        if (x < 1) return (3 * x3 - 5 * x2 + 2) / 2;
        if (x < 2) return (-x3 + 5 * x2 - 8 * x + 4) / 2;
        return 0;
    }
    case ResampleFilter::Lanczos3: {
        if (x < 1e-9) return 1;
        if (x >= 3) return 0;
        double px = M_PI * x;
        return 3 * std::sin(px) * std::sin(px / 3) / (px * px);
    }
    }
    return 0;
}

// ---- Point samplers ------------------------------------------------------------------------------

typedef uint8_t u8x4 __attribute__((vector_size(4)));
typedef int32_t i32x4 __attribute__((vector_size(16)));
typedef uint32_t u32x4 __attribute__((vector_size(16)));

/// The four channels of a pixel as 32-bit lanes.
inline i32x4 lanes(const uint8_t* p) {
    u8x4 v;
    std::memcpy(&v, p, 4);
    return __builtin_convertvector(v, i32x4);
}


/// The two taps and the fraction in 1/1024ths for one axis, clamped to the edge. Ten fractional bits keep
/// the quantisation under a quarter of a level, so the result rounds within a level of the float formula.
struct Taps2 { int i0, i1; unsigned f; };
constexpr unsigned fractionOne = 1024;
inline Taps2 bilinearTaps(double v, int n) {
    v -= 0.5;
    if (v < 0) v = 0; else if (v > n - 1) v = n - 1;
    int i0 = int(v);
    return {i0, std::min(i0 + 1, n - 1), unsigned((v - i0) * fractionOne + 0.5)};
}

/// The four taps and weights for one axis, clamped to the edge.
struct Taps4 { int i[4]; const int16_t* w; };
inline Taps4 bicubicTaps(double v, int n) {
    v -= 0.5;
    if (v < 0) v = 0; else if (v > n - 1) v = n - 1;
    int i1 = int(v);
    Taps4 t;
    t.i[0] = std::max(i1 - 1, 0); t.i[1] = i1; t.i[2] = std::min(i1 + 1, n - 1); t.i[3] = std::min(i1 + 2, n - 1);
    t.w = catmullRom().weights[int((v - i1) * 256)];
    return t;
}

// ---- Separable resampling ------------------------------------------------------------------------

/// Per output pixel along one axis: the first source index, the tap count, packed 8.8 weights summing to
/// 256 (taps beyond the image fold onto the edge pixel, as the point samplers clamp), and the coverage of
/// the layer's edge in 1/256ths: a ramp one output pixel wide, as the projective path antialiases it.
struct AxisTaps {
    std::vector<int> first, count, edge;
    std::vector<size_t> offset;
    std::vector<int16_t> weights;
};

AxisTaps axisTaps(ResampleFilter filter, int outputCount, double origin, double step, int sourceCount) {
    AxisTaps taps;
    taps.first.resize(size_t(outputCount)); taps.count.resize(size_t(outputCount));
    taps.edge.resize(size_t(outputCount)); taps.offset.resize(size_t(outputCount));
    const double widen = std::max(1.0, std::fabs(step)), radius = kernelRadius(filter) * widen;
    std::vector<double> raw;
    std::vector<int16_t> fixed;
    for (int x = 0; x < outputCount; x++) {
        // Pixel j is centred at j + 0.5; the filter is centred on the output pixel's centre in index space.
        const double position = origin + x * step, centre = position - 0.5;
        const double distance = std::min(position, sourceCount - position) / std::fabs(step);
        taps.edge[size_t(x)] = int(std::clamp(distance + 0.5, 0.0, 1.0) * 256 + 0.5);
        int lo = int(std::ceil(centre - radius)), hi = int(std::floor(centre + radius));
        raw.clear();
        double sum = 0;
        for (int j = lo; j <= hi; j++) { raw.push_back(kernelValue(filter, (j - centre) / widen)); sum += raw.back(); }
        if (raw.empty() || sum <= 0) { raw.assign(1, 1.0); lo = hi = int(std::floor(centre + 0.5)); sum = 1; }
        // Fixed point summing to exactly 256, the rounding remainder on the largest weight.
        fixed.assign(raw.size(), 0);
        int total = 0; size_t largest = 0;
        for (size_t k = 0; k < raw.size(); k++) { fixed[k] = int16_t(std::lround(raw[k] / sum * 256)); total += fixed[k]; if (raw[k] > raw[largest]) largest = k; }
        fixed[largest] += int16_t(256 - total);
        // Clamp to the edge: taps before the first pixel fold onto it, taps after the last onto that.
        const int from = std::clamp(lo, 0, sourceCount - 1), to = std::clamp(hi, 0, sourceCount - 1);
        taps.first[size_t(x)] = from;
        taps.count[size_t(x)] = to - from + 1;
        taps.offset[size_t(x)] = taps.weights.size();
        for (int j = from; j <= to; j++) taps.weights.push_back(0);
        for (int j = lo; j <= hi; j++) taps.weights[taps.offset[size_t(x)] + size_t(std::clamp(j, from, to) - from)] += fixed[size_t(j - lo)];
    }
    return taps;
}

template <int C>
void resampleImpl(const uint8_t* src, int sw, int sh, size_t srcStride, uint8_t* dst, int dw, int dh, size_t dstStride,
                  ResampleFilter filter, double originX, double stepX, double originY, double stepY, const uint8_t outside[C]) {
    const AxisTaps tx = axisTaps(filter, dw, originX, stepX, sw), ty = axisTaps(filter, dh, originY, stepY, sh);
    // Horizontal pass into an 8.8 intermediate of dw x sh, rows in parallel.
    std::vector<uint16_t> mid(size_t(dw) * sh * C);
    parallelRows(0, sh, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint8_t* row = src + size_t(y) * srcStride;
            uint16_t* o = &mid[size_t(y) * dw * C];
            for (int x = 0; x < dw; x++, o += C) {
                const int16_t* w = &tx.weights[tx.offset[size_t(x)]];
                const uint8_t* p = row + size_t(tx.first[size_t(x)]) * C;
                const int n = tx.count[size_t(x)];
                if constexpr (C == 4) {
                    i32x4 acc = {0, 0, 0, 0};
                    for (int k = 0; k < n; k++, p += 4) acc += lanes(p) * w[k];
                    const i32x4 zero = {0, 0, 0, 0}, top = {65280, 65280, 65280, 65280};
                    acc = acc < zero ? zero : acc;
                    acc = acc > top ? top : acc;
                    for (int c = 0; c < 4; c++) o[c] = uint16_t(acc[c]);
                } else {
                    int acc = 0;
                    for (int k = 0; k < n; k++) acc += p[k] * w[k];
                    o[0] = uint16_t(std::clamp(acc, 0, 65280));
                }
            }
        }
    });
    // Vertical pass: each output row accumulates its intermediate rows in 16.16, one flat loop per row,
    // then the edge ramp: transparent beyond a layer, `outside` beyond a mask.
    const size_t rowValues = size_t(dw) * C;
    parallelRows(0, dh, [&](int y0, int y1) {
        std::vector<int> acc(rowValues);
        for (int y = y0; y < y1; y++) {
            std::fill(acc.begin(), acc.end(), 0);
            const int16_t* w = &ty.weights[ty.offset[size_t(y)]];
            for (int k = 0, n = ty.count[size_t(y)]; k < n; k++) {
                const uint16_t* m = &mid[size_t(ty.first[size_t(y)] + k) * rowValues];
                const int wk = w[k];
                for (size_t i = 0; i < rowValues; i++) acc[i] += int(m[i]) * wk;
            }
            uint8_t* o = dst + size_t(y) * dstStride;
            const int edgeY = ty.edge[size_t(y)];
            for (int x = 0; x < dw; x++, o += C) {
                const int edge = (tx.edge[size_t(x)] * edgeY + 128) >> 8;   // 0..256
                if constexpr (C == 4) {
                    int a = std::clamp((acc[size_t(x) * 4 + 3] + 32768) >> 16, 0, 255);
                    a = (a * edge + 128) >> 8;
                    for (int c = 0; c < 3; c++) {
                        int v = std::clamp((acc[size_t(x) * 4 + size_t(c)] + 32768) >> 16, 0, 255);
                        o[c] = uint8_t(std::min(a, (v * edge + 128) >> 8));
                    }
                    o[3] = uint8_t(a);
                } else {
                    int v = std::clamp((acc[size_t(x)] + 32768) >> 16, 0, 255);
                    o[0] = uint8_t((v * edge + outside[0] * (256 - edge) + 128) >> 8);
                }
            }
        }
    });
}

} // namespace

const int16_t* catmullRomWeights(int fraction256) { return catmullRom().weights[std::clamp(fraction256, 0, 256)]; }

ResampleFilter filterFor(Sampling sampling) { return sampling == Sampling::High ? ResampleFilter::Lanczos3 : ResampleFilter::Triangle; }


void sampleBilinear(const Image& image, double x, double y, uint8_t out[4]) {
    const Taps2 tx = bilinearTaps(x, image.width()), ty = bilinearTaps(y, image.height());
    const int fx = int(tx.f), gx = int(fractionOne) - fx, fy = int(ty.f), gy = int(fractionOne) - fy;
    // Four pixels weighted in 32-bit lanes, one lane per channel: 0..255 * 1024 per row, then * 1024.
    const i32x4 top = lanes(image.pixel(tx.i0, ty.i0)) * gx + lanes(image.pixel(tx.i1, ty.i0)) * fx;
    const i32x4 bottom = lanes(image.pixel(tx.i0, ty.i1)) * gx + lanes(image.pixel(tx.i1, ty.i1)) * fx;
    const u32x4 value = (u32x4(top * gy + bottom * fy) + (fractionOne * fractionOne / 2)) / (fractionOne * fractionOne);
    const u8x4 bytes = __builtin_convertvector(value, u8x4);
    std::memcpy(out, &bytes, 4);
}

void sampleBicubic(const Image& image, double x, double y, uint8_t out[4]) {
    const Taps4 tx = bicubicTaps(x, image.width()), ty = bicubicTaps(y, image.height());
    i32x4 acc = {0, 0, 0, 0};
    for (int j = 0; j < 4; j++) {
        const uint8_t* row = image.row(ty.i[j]);
        i32x4 h = lanes(row + size_t(tx.i[0]) * 4) * tx.w[0] + lanes(row + size_t(tx.i[1]) * 4) * tx.w[1]
                + lanes(row + size_t(tx.i[2]) * 4) * tx.w[2] + lanes(row + size_t(tx.i[3]) * 4) * tx.w[3];
        acc += h * ty.w[j];
    }
    // 16.16 with the negative lobes' overshoot clamped; colour stays within alpha (premultiplied).
    const i32x4 zero = {0, 0, 0, 0}, full = {255, 255, 255, 255};
    i32x4 v = (acc + 32768) >> 16;
    v = v < zero ? zero : v;
    v = v > full ? full : v;
    const int a = v[3];
    for (int c = 0; c < 3; c++) out[c] = uint8_t(std::min(v[c], a));
    out[3] = uint8_t(a);
}

int sampleGrayBilinear(const GrayImage& image, double x, double y) {
    const Taps2 tx = bilinearTaps(x, image.width()), ty = bilinearTaps(y, image.height());
    const unsigned fx = tx.f, gx = fractionOne - fx, fy = ty.f, gy = fractionOne - fy;
    unsigned top = image.at(tx.i0, ty.i0) * gx + image.at(tx.i1, ty.i0) * fx, bottom = image.at(tx.i0, ty.i1) * gx + image.at(tx.i1, ty.i1) * fx;
    return int((top * gy + bottom * fy + (fractionOne * fractionOne / 2)) / (fractionOne * fractionOne));
}

int sampleGrayBicubic(const GrayImage& image, double x, double y) {
    const Taps4 tx = bicubicTaps(x, image.width()), ty = bicubicTaps(y, image.height());
    int acc = 0;
    for (int j = 0; j < 4; j++) {
        const uint8_t* row = image.row(ty.i[j]);
        int h = 0;
        for (int i = 0; i < 4; i++) h += row[tx.i[i]] * tx.w[i];
        acc += h * ty.w[j];
    }
    return std::clamp((acc + 32768) >> 16, 0, 255);
}

std::shared_ptr<Image> resampleAxisAligned(const Image& image, int width, int height, double originX, double stepX, double originY, double stepY, ResampleFilter filter) {
    auto out = std::make_shared<Image>(std::max(1, width), std::max(1, height));
    if (image.isEmpty() || width <= 0 || height <= 0) return out;
    const uint8_t transparent[4] = {0, 0, 0, 0};
    resampleImpl<4>(image.data(), image.width(), image.height(), size_t(image.stride()), out->data(), width, height, size_t(out->stride()), filter, originX, stepX, originY, stepY, transparent);
    return out;
}

std::shared_ptr<GrayImage> resampleAxisAligned(const GrayImage& mask, int width, int height, double originX, double stepX, double originY, double stepY, ResampleFilter filter, uint8_t outside) {
    auto out = std::make_shared<GrayImage>(std::max(1, width), std::max(1, height), outside);
    if (mask.isEmpty() || width <= 0 || height <= 0) return out;
    const uint8_t fill[1] = {outside};
    resampleImpl<1>(mask.data(), mask.width(), mask.height(), size_t(mask.stride()), out->data(), width, height, size_t(out->stride()), filter, originX, stepX, originY, stepY, fill);
    return out;
}

} // namespace compositor
