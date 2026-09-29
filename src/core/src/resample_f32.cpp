// The separable resampler at 32 bits (resample.h): resample.cpp's filters and tap positions with exact weights (no 8.8
// fixed point), on premultiplied linear float. Light above 1 is resampled as it is; the negative lobes of Catmull-Rom
// and Lanczos are held at zero, alpha and coverage at 0..1.
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

/// Per output pixel along one axis: the first source index, the tap count, weights summing to 1 (taps beyond the image
/// fold onto the edge pixel), and the coverage of the layer's edge (a ramp one output pixel wide), as resample.cpp.
struct AxisTaps {
    std::vector<int> first, count;
    std::vector<float> edge;
    std::vector<size_t> offset;
    std::vector<float> weights;
};

AxisTaps axisTaps(ResampleFilter filter, int outputCount, double origin, double step, int sourceCount) {
    AxisTaps taps;
    taps.first.resize(size_t(outputCount)); taps.count.resize(size_t(outputCount));
    taps.edge.resize(size_t(outputCount)); taps.offset.resize(size_t(outputCount));
    const double widen = std::max(1.0, std::fabs(step)), radius = kernelRadius(filter) * widen;
    std::vector<double> raw;
    for (int x = 0; x < outputCount; x++) {
        const double position = origin + x * step, centre = position - 0.5;
        const double distance = std::min(position, sourceCount - position) / std::fabs(step);
        taps.edge[size_t(x)] = float(std::clamp(distance + 0.5, 0.0, 1.0));
        int lo = int(std::ceil(centre - radius)), hi = int(std::floor(centre + radius));
        raw.clear();
        double sum = 0;
        for (int j = lo; j <= hi; j++) { raw.push_back(kernelValue(filter, (j - centre) / widen)); sum += raw.back(); }
        if (raw.empty() || sum <= 0) { raw.assign(1, 1.0); lo = hi = int(std::floor(centre + 0.5)); sum = 1; }
        const int from = std::clamp(lo, 0, sourceCount - 1), to = std::clamp(hi, 0, sourceCount - 1);
        taps.first[size_t(x)] = from;
        taps.count[size_t(x)] = to - from + 1;
        taps.offset[size_t(x)] = taps.weights.size();
        std::vector<double> folded(size_t(to - from + 1), 0.0);
        for (int j = lo; j <= hi; j++) folded[size_t(std::clamp(j, from, to) - from)] += raw[size_t(j - lo)] / sum;
        for (double w : folded) taps.weights.push_back(float(w));
    }
    return taps;
}

template <int C>
void resampleImplF(const float* src, int sw, int sh, size_t srcStride, float* dst, int dw, int dh, size_t dstStride,
                   ResampleFilter filter, double originX, double stepX, double originY, double stepY, float outside) {
    const AxisTaps tx = axisTaps(filter, dw, originX, stepX, sw), ty = axisTaps(filter, dh, originY, stepY, sh);
    std::vector<float> mid(size_t(dw) * sh * C);
    parallelRows(0, sh, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const float* row = src + size_t(y) * srcStride;
            float* o = &mid[size_t(y) * dw * C];
            for (int x = 0; x < dw; x++, o += C) {
                const float* w = &tx.weights[tx.offset[size_t(x)]];
                const float* p = row + size_t(tx.first[size_t(x)]) * C;
                const int n = tx.count[size_t(x)];
                float acc[C] = {};
                for (int k = 0; k < n; k++, p += C) for (int c = 0; c < C; c++) acc[c] += p[c] * w[k];
                for (int c = 0; c < C; c++) o[c] = acc[c];
            }
        }
    });
    const size_t rowValues = size_t(dw) * C;
    parallelRows(0, dh, [&](int y0, int y1) {
        std::vector<float> acc(rowValues);
        for (int y = y0; y < y1; y++) {
            std::fill(acc.begin(), acc.end(), 0.0f);
            const float* w = &ty.weights[ty.offset[size_t(y)]];
            for (int k = 0, n = ty.count[size_t(y)]; k < n; k++) {
                const float* m = &mid[size_t(ty.first[size_t(y)] + k) * rowValues];
                const float wk = w[k];
                for (size_t i = 0; i < rowValues; i++) acc[i] += m[i] * wk;
            }
            float* o = dst + size_t(y) * dstStride;
            const float edgeY = ty.edge[size_t(y)];
            for (int x = 0; x < dw; x++, o += C) {
                const float edge = tx.edge[size_t(x)] * edgeY;
                if constexpr (C == 4) {
                    for (int c = 0; c < 3; c++) o[c] = std::max(0.0f, acc[size_t(x) * 4 + size_t(c)]) * edge;
                    o[3] = std::clamp(acc[size_t(x) * 4 + 3], 0.0f, 1.0f) * edge;
                } else {
                    o[0] = std::clamp(acc[size_t(x)], 0.0f, 1.0f) * edge + outside * (1 - edge);
                }
            }
        }
    });
}

} // namespace

std::shared_ptr<ImageF> resampleAxisAligned(const ImageF& image, int width, int height, double originX, double stepX, double originY, double stepY, ResampleFilter filter) {
    auto out = std::make_shared<ImageF>(std::max(1, width), std::max(1, height));
    if (image.isEmpty() || width <= 0 || height <= 0) return out;
    resampleImplF<4>(image.data(), image.width(), image.height(), size_t(image.width()) * 4, out->data(), width, height, size_t(out->width()) * 4,
                     filter, originX, stepX, originY, stepY, 0.0f);
    return out;
}

std::shared_ptr<GrayF> resampleAxisAligned(const GrayF& mask, int width, int height, double originX, double stepX, double originY, double stepY, ResampleFilter filter, float outside) {
    auto out = std::make_shared<GrayF>(std::max(1, width), std::max(1, height), outside);
    if (mask.isEmpty() || width <= 0 || height <= 0) return out;
    resampleImplF<1>(mask.data(), mask.width(), mask.height(), size_t(mask.width()), out->data(), width, height, size_t(out->width()),
                     filter, originX, stepX, originY, stepY, outside);
    return out;
}

} // namespace compositor
