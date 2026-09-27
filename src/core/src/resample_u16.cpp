// The separable resampler at 16 bits (resample.h). Its taps are resample.cpp's, copied here so the 8-bit
// translation unit is built exactly as it was; a change to the kernels or the taps belongs in both.
#include "compositor/resample.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace compositor {

namespace {

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

/// resampleImpl at 16 bits (0..32768): the same taps, float sums, so no 8-bit step is introduced.
template <int C>
void resampleImpl16(const uint16_t* src, int sw, int sh, size_t srcStride, uint16_t* dst, int dw, int dh, size_t dstStride,
                    ResampleFilter filter, double originX, double stepX, double originY, double stepY, const uint16_t outside[C]) {
    const AxisTaps tx = axisTaps(filter, dw, originX, stepX, sw), ty = axisTaps(filter, dh, originY, stepY, sh);
    constexpr float one = 32768.0f, unit = 1.0f / 256.0f;
    std::vector<float> mid(size_t(dw) * sh * C);
    parallelRows(0, sh, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint16_t* row = src + size_t(y) * srcStride;
            float* o = &mid[size_t(y) * dw * C];
            for (int x = 0; x < dw; x++, o += C) {
                const int16_t* w = &tx.weights[tx.offset[size_t(x)]];
                const uint16_t* p = row + size_t(tx.first[size_t(x)]) * C;
                const int n = tx.count[size_t(x)];
                float acc[C] = {};
                for (int k = 0; k < n; k++, p += C) for (int c = 0; c < C; c++) acc[c] += float(p[c]) * float(w[k]);
                for (int c = 0; c < C; c++) o[c] = std::clamp(acc[c] * unit, 0.0f, one);
            }
        }
    });
    const size_t rowValues = size_t(dw) * C;
    parallelRows(0, dh, [&](int y0, int y1) {
        std::vector<float> acc(rowValues);
        for (int y = y0; y < y1; y++) {
            std::fill(acc.begin(), acc.end(), 0.0f);
            const int16_t* w = &ty.weights[ty.offset[size_t(y)]];
            for (int k = 0, n = ty.count[size_t(y)]; k < n; k++) {
                const float* m = &mid[size_t(ty.first[size_t(y)] + k) * rowValues];
                const float wk = float(w[k]);
                for (size_t i = 0; i < rowValues; i++) acc[i] += m[i] * wk;
            }
            uint16_t* o = dst + size_t(y) * dstStride;
            const int edgeY = ty.edge[size_t(y)];
            for (int x = 0; x < dw; x++, o += C) {
                const float edge = float((tx.edge[size_t(x)] * edgeY + 128) >> 8) * unit;   // 0..1
                if constexpr (C == 4) {
                    const float a = std::clamp(acc[size_t(x) * 4 + 3] * unit, 0.0f, one) * edge;
                    const uint16_t alpha = uint16_t(a + 0.5f);
                    for (int c = 0; c < 3; c++) {
                        const float v = std::clamp(acc[size_t(x) * 4 + size_t(c)] * unit, 0.0f, one) * edge;
                        o[c] = std::min(alpha, uint16_t(v + 0.5f));
                    }
                    o[3] = alpha;
                } else {
                    const float v = std::clamp(acc[size_t(x)] * unit, 0.0f, one);
                    o[0] = uint16_t(v * edge + float(outside[0]) * (1 - edge) + 0.5f);
                }
            }
        }
    });
}

} // namespace

std::shared_ptr<Image16> resampleAxisAligned(const Image16& image, int width, int height, double originX, double stepX, double originY, double stepY, ResampleFilter filter) {
    auto out = std::make_shared<Image16>(std::max(1, width), std::max(1, height));
    if (image.isEmpty() || width <= 0 || height <= 0) return out;
    const uint16_t transparent[4] = {0, 0, 0, 0};
    resampleImpl16<4>(image.data(), image.width(), image.height(), size_t(image.width()) * 4, out->data(), width, height, size_t(out->width()) * 4,
                      filter, originX, stepX, originY, stepY, transparent);
    return out;
}

std::shared_ptr<Gray16> resampleAxisAligned(const Gray16& mask, int width, int height, double originX, double stepX, double originY, double stepY, ResampleFilter filter, uint16_t outside) {
    auto out = std::make_shared<Gray16>(std::max(1, width), std::max(1, height), outside);
    if (mask.isEmpty() || width <= 0 || height <= 0) return out;
    const uint16_t fill[1] = {outside};
    resampleImpl16<1>(mask.data(), mask.width(), mask.height(), size_t(mask.width()), out->data(), width, height, size_t(out->width()),
                      filter, originX, stepX, originY, stepY, fill);
    return out;
}

} // namespace compositor
