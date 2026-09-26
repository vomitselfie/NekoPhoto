// Smart Filter kernels and the stack renderer.
//
// A port of Patchy's src/filters/smart_filter_renderer.cpp (MIT, see src/third_party/patchy_psd/README.md), with
// the helpers it uses from filter_support.hpp, rect_utils.hpp and filter_engine.cpp's noise hash. Patchy's buffers
// are STRAIGHT RGBA8; NekoPhoto's are premultiplied, so every public entry point unpremultiplies on the way in and
// premultiplies on the way out, and the kernels in between run on straight colour exactly as Patchy's do. Progress
// reporting and cancellation are dropped; the arithmetic, the rounding and the edge rules are kept as they were.
//
// Patent design constraint carried over from Patchy (its docs/legal-constraints.md): Median and Dust & Scratches
// keep exactly ONE plain window histogram updated a single pixel value at a time (never merged or slid column
// histograms), and Surface Blur uses no value histogram at all (direct accumulation at small radii, per-level box
// sums at large radii).
#include "compositor/smartfilter.h"
#include "compositor/blend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

namespace compositor {

namespace {

constexpr double kMinimumGaussianRadius = 0.1;
constexpr double kMaximumGaussianRadius = 1000.0;
constexpr double kMinimumMedianRadius = 1.0;
constexpr double kMaximumMedianRadius = 500.0;
constexpr int32_t kMinimumDustAndScratchesRadius = 1;
constexpr int32_t kMaximumDustAndScratchesRadius = 500;
constexpr int32_t kMinimumDustAndScratchesThreshold = 0;
constexpr int32_t kMaximumDustAndScratchesThreshold = 255;
constexpr double kMinimumSurfaceBlurRadius = 1.0;
constexpr double kMaximumSurfaceBlurRadius = 100.0;
constexpr int32_t kMinimumSurfaceBlurThreshold = 2;
constexpr int32_t kMaximumSurfaceBlurThreshold = 255;
constexpr double kMinimumUnsharpMaskAmount = 1.0;
constexpr double kMaximumUnsharpMaskAmount = 500.0;
constexpr int32_t kMinimumUnsharpMaskThreshold = 0;
constexpr int32_t kMaximumUnsharpMaskThreshold = 255;
constexpr int32_t kMinimumMotionBlurAngle = -360;
constexpr int32_t kMaximumMotionBlurAngle = 360;
constexpr int32_t kMinimumMotionBlurDistance = 1;
constexpr int32_t kMaximumMotionBlurDistance = 999;
constexpr int32_t kMinimumPlasticWrapHighlightStrength = 0;
constexpr int32_t kMaximumPlasticWrapHighlightStrength = 20;
constexpr int32_t kMinimumPlasticWrapDetail = 1;
constexpr int32_t kMaximumPlasticWrapDetail = 15;
constexpr int32_t kMinimumPlasticWrapSmoothness = 1;
constexpr int32_t kMaximumPlasticWrapSmoothness = 15;
constexpr int32_t kMinimumMosaicCellSize = 2;
constexpr int32_t kMaximumMosaicCellSize = 200;
constexpr int32_t kMinimumEmbossAngle = -360;
constexpr int32_t kMaximumEmbossAngle = 360;
constexpr int32_t kMinimumEmbossHeight = 1;
constexpr int32_t kMaximumEmbossHeight = 100;
constexpr int32_t kMinimumEmbossAmount = 1;
constexpr int32_t kMaximumEmbossAmount = 500;
constexpr double kMinimumBoxBlurRadius = 1.0;
constexpr double kMaximumBoxBlurRadius = 2000.0;
constexpr int32_t kMinimumRadialBlurAmount = 1;
constexpr int32_t kMaximumRadialBlurAmount = 100;
constexpr double kMinimumAddNoiseAmount = 0.1;
constexpr double kMaximumAddNoiseAmount = 400.0;
constexpr int32_t kMinimumAddNoiseSeed = 0;
constexpr int32_t kMaximumAddNoiseSeed = 999999999;
constexpr int32_t kBoxBlurDirectMaximumRadius = 12;
constexpr double kGaussianMarginScale = 3.0;
constexpr double kDirectGaussianMaximumRadius = 8.0;
constexpr int32_t kSurfaceBlurDirectMaximumRadius = 8;

/// Patchy's FilterRenderResult: straight RGBA8 pixels (stride = width * 4) placed at `bounds`.
struct Result {
    Image pixels;
    PixelRect bounds;
};

bool pixelsEmpty(const Result& r) { return r.pixels.isEmpty(); }

PixelRect intersectRect(PixelRect a, PixelRect b) {
    const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.x + a.width, b.x + b.width), y1 = std::min(a.y + a.height, b.y + b.height);
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

PixelRect unionRect(PixelRect a, PixelRect b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.width, b.x + b.width), y1 = std::max(a.y + a.height, b.y + b.height);
    return {x0, y0, x1 - x0, y1 - y0};
}

const uint8_t* sampleResult(const Result& result, int32_t documentX, int32_t documentY) {
    const auto localX = int64_t(documentX) - result.bounds.x;
    const auto localY = int64_t(documentY) - result.bounds.y;
    if (localX < 0 || localY < 0 || localX >= result.bounds.width || localY >= result.bounds.height) return nullptr;
    return result.pixels.pixel(int32_t(localX), int32_t(localY));
}

Result embedInFilterCanvas(const Result& placed, PixelRect canvasBounds) {
    if (canvasBounds.empty() || placed.bounds == canvasBounds) return placed;
    Image canvas(canvasBounds.width, canvasBounds.height);
    const auto copied = intersectRect(placed.bounds, canvasBounds);
    if (!copied.empty()) {
        const int sx = copied.x - placed.bounds.x, sy = copied.y - placed.bounds.y;
        const int dx = copied.x - canvasBounds.x, dy = copied.y - canvasBounds.y;
        const size_t rowBytes = size_t(copied.width) * 4U;
        for (int32_t y = 0; y < copied.height; ++y) {
            const auto* source = placed.pixels.pixel(sx, sy + y);
            std::copy(source, source + rowBytes, canvas.pixel(dx, dy + y));
        }
    }
    return Result{std::move(canvas), canvasBounds};
}

uint8_t roundedByte(double value) {
    if (!(value > 0.0)) return 0;
    if (value >= 255.0) return 255;
    return uint8_t(std::floor(value + 0.5));
}

uint8_t clampLong(long value) { return uint8_t(std::clamp(value, 0L, 255L)); }

// ---- Gaussian line plans -----------------------------------------------------------------------------------

struct GaussianLinePlan {
    bool direct = false;
    std::vector<double> kernel;
    double gain = 1.0;
    double coefficient1 = 0.0;
    double coefficient2 = 0.0;
    double coefficient3 = 0.0;
};

GaussianLinePlan makeGaussianLinePlan(double radius, int margin) {
    GaussianLinePlan plan;
    if (radius <= kDirectGaussianMaximumRadius) {
        plan.direct = true;
        plan.kernel.resize(size_t(margin) * 2U + 1U);
        struct Calibration {
            double radius;
            std::vector<double> weights;
        };
        // Photoshop 27.8 captures of a one-pixel vertical line. Interpolating the measured kernels keeps every
        // captured radius exact after byte rounding (notably radius 0.5).
        static const std::vector<Calibration> kCalibrations{
            {0.1, {255}},
            {0.2, {9, 237, 9}},
            {0.25, {24, 207, 24}},
            {0.3, {36, 183, 36}},
            {0.35, {44, 167, 44}},
            {0.4, {49, 157, 49}},
            {0.45, {52, 151, 52}},
            {0.49, {54, 147, 54}},
            {0.5, {55, 145, 55}},
            {0.51, {1, 55, 143, 55, 1}},
            {0.6, {3, 58, 133, 58, 3}},
            {0.7, {7, 60, 122, 60, 7}},
            {0.8, {11, 60, 114, 60, 11}},
            {0.9, {1, 14, 60, 106, 60, 14, 1}},
            {1.0, {2, 18, 60, 96, 60, 18, 2}},
            {1.1, {3, 21, 59, 90, 59, 21, 3}},
            {1.5, {2, 10, 28, 52, 72, 52, 28, 10, 2}},
            {2.0, {2, 7, 17, 30, 43, 58, 43, 30, 17, 7, 2}},
            {2.5, {1, 5, 11, 20, 31, 39, 42, 39, 31, 20, 11, 5, 1}},
            {3.0, {1, 2, 5, 9, 14, 21, 27, 32, 34, 32, 27, 21, 14, 9, 5, 2, 1}},
            {4.0, {1, 2, 4, 6, 9, 12, 16, 19, 22, 24, 24, 24, 22, 19, 16, 12, 9, 6, 4, 2, 1}},
            // The sub-byte tails keep radius 4.5's observed one-level outer halo.
            {4.5, {0.08, 0.16, 0.40, 1, 2, 3, 5, 7, 9, 12, 16, 19, 21, 22, 22.3, 22, 21, 19, 16, 12, 9, 7, 5, 3, 2, 1,
                   0.40, 0.16, 0.08}},
            {8.0, {1, 1, 1, 2, 2, 3, 4, 4, 5, 6, 7, 8, 9, 10, 11, 11, 12, 12, 13, 13, 13, 12, 12, 11, 11, 10, 9, 8, 7, 6,
                   5, 4, 4, 3, 2, 2, 1, 1, 1}},
        };
        const auto upper = std::lower_bound(kCalibrations.begin(), kCalibrations.end(), radius,
                                            [](const Calibration& c, double value) { return c.radius < value; });
        const auto& high = upper == kCalibrations.end() ? kCalibrations.back() : *upper;
        const auto& low = upper == kCalibrations.begin() ? *upper : *(upper - 1);
        const auto span = high.radius - low.radius;
        const auto mix = span <= 0.0 ? 0.0 : (radius - low.radius) / span;
        const auto calibratedWeight = [](const Calibration& c, int offset) {
            const auto support = int(c.weights.size() / 2U);
            return offset < -support || offset > support ? 0.0 : c.weights[size_t(offset + support)];
        };
        double sum = 0.0;
        for (int offset = -margin; offset <= margin; ++offset) {
            const auto weight = calibratedWeight(low, offset) * (1.0 - mix) + calibratedWeight(high, offset) * mix;
            plan.kernel[size_t(offset + margin)] = weight;
            sum += weight;
        }
        if (!std::isfinite(sum) || sum <= 0.0) {
            plan.kernel.assign(1, 1.0);
            return plan;
        }
        for (auto& weight : plan.kernel) weight /= sum;
        return plan;
    }

    // Young and van Vliet's third-order recursive approximation; boundaries repeat the canvas edge.
    const auto q = radius >= 2.5 ? 0.98711 * radius - 0.96330 : 3.97156 - 4.14554 * std::sqrt(1.0 - 0.26891 * radius);
    const auto q2 = q * q;
    const auto q3 = q2 * q;
    const auto b0 = 1.57825 + 2.44413 * q + 1.4281 * q2 + 0.422205 * q3;
    plan.coefficient1 = (2.44413 * q + 2.85619 * q2 + 1.26661 * q3) / b0;
    plan.coefficient2 = -(1.4281 * q2 + 1.26661 * q3) / b0;
    plan.coefficient3 = 0.422205 * q3 / b0;
    plan.gain = 1.0 - plan.coefficient1 - plan.coefficient2 - plan.coefficient3;
    return plan;
}

GaussianLinePlan makeHighPassLinePlan(double radius, int margin) {
    if (radius < 8.0 || radius > 12.0) return makeGaussianLinePlan(radius, margin);
    struct Calibration {
        double radius;
        std::span<const double> weights;
    };
    static constexpr std::array<double, 39> kRadius8Weights{1, 1, 1, 2, 2, 3, 4, 4, 5, 6, 7, 8, 9, 10, 11, 11, 12, 12, 13, 13,
                                                            13, 12, 12, 11, 11, 10, 9, 8, 7, 6, 5, 4, 4, 3, 2, 2, 1, 1, 1};
    static constexpr std::array<double, 47> kRadius10Weights{1, 1, 1, 1, 2, 2, 2, 3, 3, 4, 5, 5, 6, 6, 7, 7,
                                                             8, 8, 9, 9, 10, 10, 10, 10, 10, 10, 10, 9, 9, 8, 8, 7,
                                                             7, 6, 6, 5, 5, 4, 3, 3, 2, 2, 2, 1, 1, 1, 1};
    static constexpr std::array<double, 51> kRadius11Weights{1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7,
                                                             7, 7, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 8, 8, 8, 7, 7,
                                                             7, 6, 6, 5, 5, 4, 4, 3, 3, 2, 2, 2, 1, 1, 1, 1, 1};
    static constexpr std::array<double, 57> kRadius12Weights{1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6,
                                                             6, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 7, 7, 7, 7, 6,
                                                             6, 5, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1};
    static constexpr std::array<Calibration, 4> kCalibrations{{
        {8.0, kRadius8Weights},
        {10.0, kRadius10Weights},
        {11.0, kRadius11Weights},
        {12.0, kRadius12Weights},
    }};
    const auto upper = std::lower_bound(kCalibrations.begin(), kCalibrations.end(), radius,
                                        [](const Calibration& c, double value) { return c.radius < value; });
    const auto& high = upper == kCalibrations.end() ? kCalibrations.back() : *upper;
    const auto& low = upper == kCalibrations.begin() ? *upper : *(upper - 1);
    const auto lowSum = std::accumulate(low.weights.begin(), low.weights.end(), 0.0);
    const auto highSum = std::accumulate(high.weights.begin(), high.weights.end(), 0.0);
    const auto normalizedWeight = [](const Calibration& c, double sum, std::ptrdiff_t offset) {
        const auto support = std::ptrdiff_t(c.weights.size() / 2U);
        if (offset < -support || offset > support) return 0.0;
        return c.weights[size_t(offset + support)] / sum;
    };
    const auto span = high.radius - low.radius;
    const auto mix = span <= 0.0 ? 0.0 : (radius - low.radius) / span;
    GaussianLinePlan plan;
    plan.direct = true;
    const auto support = std::ptrdiff_t(std::max(low.weights.size(), high.weights.size()) / 2U);
    plan.kernel.assign(size_t(support) * 2U + 1U, 0.0);
    for (std::ptrdiff_t offset = -support; offset <= support; ++offset)
        plan.kernel[size_t(offset + support)] =
            normalizedWeight(low, lowSum, offset) * (1.0 - mix) + normalizedWeight(high, highSum, offset) * mix;
    return plan;
}

GaussianLinePlan makeUnsharpLinePlan(double radius, int margin) {
    if (radius != 2.5) return makeGaussianLinePlan(radius, margin);
    // Unsharp Mask's own quantised radius-2.5 low-pass kernel (sums to 256).
    static constexpr std::array<double, 13> kRadius2_5Weights{2, 4, 12, 20, 30, 38, 44, 38, 30, 20, 12, 4, 2};
    GaussianLinePlan plan;
    plan.direct = true;
    plan.kernel.assign(size_t(margin) * 2U + 1U, 0.0);
    constexpr auto kSum = 256.0;
    constexpr auto kSupport = std::ptrdiff_t(kRadius2_5Weights.size() / 2U);
    for (std::ptrdiff_t offset = -kSupport; offset <= kSupport; ++offset)
        plan.kernel[size_t(offset + margin)] = kRadius2_5Weights[size_t(offset + kSupport)] / kSum;
    return plan;
}

void filterGaussianLine(std::vector<double>& values, std::vector<double>& scratch, const GaussianLinePlan& plan) {
    const auto count = values.size();
    if (count == 0) return;
    scratch.resize(count);
    if (plan.direct) {
        const auto radius = std::ptrdiff_t(plan.kernel.size() / 2U);
        for (size_t index = 0; index < count; ++index) {
            double value = 0.0;
            for (std::ptrdiff_t offset = -radius; offset <= radius; ++offset) {
                const auto source = std::clamp<std::ptrdiff_t>(std::ptrdiff_t(index) + offset, 0, std::ptrdiff_t(count) - 1);
                value += values[size_t(source)] * plan.kernel[size_t(offset + radius)];
            }
            scratch[index] = value;
        }
        values.swap(scratch);
        return;
    }
    for (size_t index = 0; index < count; ++index) {
        const auto previous1 = index >= 1U ? scratch[index - 1U] : values.front();
        const auto previous2 = index >= 2U ? scratch[index - 2U] : values.front();
        const auto previous3 = index >= 3U ? scratch[index - 3U] : values.front();
        scratch[index] = plan.gain * values[index] + plan.coefficient1 * previous1 + plan.coefficient2 * previous2 +
                         plan.coefficient3 * previous3;
    }
    for (size_t reverse = count; reverse > 0U; --reverse) {
        const auto index = reverse - 1U;
        const auto following1 = index + 1U < count ? values[index + 1U] : scratch.back();
        const auto following2 = index + 2U < count ? values[index + 2U] : scratch.back();
        const auto following3 = index + 3U < count ? values[index + 3U] : scratch.back();
        values[index] = plan.gain * scratch[index] + plan.coefficient1 * following1 + plan.coefficient2 * following2 +
                        plan.coefficient3 * following3;
    }
}

// ---- Gaussian, High Pass, Unsharp Mask ---------------------------------------------------------------------

Result renderGaussian(const Result& input, double radius) {
    const auto margin = int(std::ceil(kGaussianMarginScale * radius));
    const auto bounds = input.bounds;
    const auto plan = makeGaussianLinePlan(radius, margin);
    Image output(bounds.width, bounds.height);
    const auto width = bounds.width, height = bounds.height;
    std::vector<float> horizontal(size_t(width) * size_t(height));
    std::vector<double> values, scratch;
    const std::array<int, 4> channels{3, 0, 1, 2};
    const uint8_t* sourceData = input.pixels.data();
    uint8_t* outputData = output.data();
    const auto sourceWidth = input.pixels.width();

    for (const auto channel : channels) {
        values.resize(size_t(width));
        scratch.resize(size_t(width));
        for (int32_t y = 0; y < height; ++y) {
            for (int32_t x = 0; x < width; ++x) {
                const auto sourceOffset = (size_t(y) * size_t(sourceWidth) + size_t(x)) * 4U;
                const auto alpha = sourceData[sourceOffset + 3U];
                values[size_t(x)] = channel == 3 ? double(alpha)
                                                 : double(sourceData[sourceOffset + size_t(channel)]) * double(alpha) / 255.0;
            }
            filterGaussianLine(values, scratch, plan);
            const auto rowOffset = size_t(y) * size_t(width);
            for (int32_t x = 0; x < width; ++x) horizontal[rowOffset + size_t(x)] = float(roundedByte(values[size_t(x)]));
        }
        values.resize(size_t(height));
        scratch.resize(size_t(height));
        for (int32_t x = 0; x < width; ++x) {
            for (int32_t y = 0; y < height; ++y) values[size_t(y)] = horizontal[size_t(y) * size_t(width) + size_t(x)];
            filterGaussianLine(values, scratch, plan);
            for (int32_t y = 0; y < height; ++y) {
                const auto outputOffset = (size_t(y) * size_t(width) + size_t(x)) * 4U;
                if (channel == 3) {
                    outputData[outputOffset + 3U] = roundedByte(values[size_t(y)]);
                    continue;
                }
                const auto alpha = outputData[outputOffset + 3U];
                const auto premultiplied = roundedByte(values[size_t(y)]);
                outputData[outputOffset + size_t(channel)] =
                    alpha == 0U ? uint8_t(0) : roundedByte(double(premultiplied) * 255.0 / double(alpha));
            }
        }
    }
    return Result{std::move(output), bounds};
}

Result renderStraightGaussian(const Result& input, double radius, bool unsharpKernel = false) {
    const auto margin = int(std::ceil(kGaussianMarginScale * radius));
    const auto plan = unsharpKernel ? makeUnsharpLinePlan(radius, margin) : makeHighPassLinePlan(radius, margin);
    Image output = input.pixels;
    const auto width = input.bounds.width, height = input.bounds.height;
    std::vector<float> horizontal(size_t(width) * size_t(height));
    std::vector<double> values, scratch;
    for (size_t channel = 0; channel < 3U; ++channel) {
        values.resize(size_t(width));
        scratch.resize(size_t(width));
        for (int32_t y = 0; y < height; ++y) {
            for (int32_t x = 0; x < width; ++x) values[size_t(x)] = input.pixels.pixel(x, y)[channel];
            filterGaussianLine(values, scratch, plan);
            const auto rowOffset = size_t(y) * size_t(width);
            for (int32_t x = 0; x < width; ++x) horizontal[rowOffset + size_t(x)] = roundedByte(values[size_t(x)]);
        }
        values.resize(size_t(height));
        scratch.resize(size_t(height));
        for (int32_t x = 0; x < width; ++x) {
            for (int32_t y = 0; y < height; ++y) values[size_t(y)] = horizontal[size_t(y) * size_t(width) + size_t(x)];
            filterGaussianLine(values, scratch, plan);
            for (int32_t y = 0; y < height; ++y) output.pixel(x, y)[channel] = roundedByte(values[size_t(y)]);
        }
    }
    return Result{std::move(output), input.bounds};
}

Result withHiddenColoursExtended(const Result& input);

Result renderHighPass(const Result& input, double radius) {
    const auto blurred = renderStraightGaussian(withHiddenColoursExtended(input), radius);
    Image output(input.bounds.width, input.bounds.height);
    for (int32_t y = 0; y < input.bounds.height; ++y) {
        for (int32_t x = 0; x < input.bounds.width; ++x) {
            const auto* source = input.pixels.pixel(x, y);
            const auto* lowFrequency = blurred.pixels.pixel(x, y);
            auto* destination = output.pixel(x, y);
            for (size_t c = 0; c < 3U; ++c)
                destination[c] = uint8_t(std::clamp(int(source[c]) - int(lowFrequency[c]) + 128, 0, 255));
            destination[3] = source[3];
        }
    }
    return Result{std::move(output), input.bounds};
}

Result renderUnsharpMask(const Result& input, double amountPercent, double radius, int32_t threshold) {
    const auto blurred = renderStraightGaussian(withHiddenColoursExtended(input), radius, true);
    Image output(input.bounds.width, input.bounds.height);
    for (int32_t y = 0; y < input.bounds.height; ++y) {
        for (int32_t x = 0; x < input.bounds.width; ++x) {
            const auto* source = input.pixels.pixel(x, y);
            const auto* lowFrequency = blurred.pixels.pixel(x, y);
            auto* destination = output.pixel(x, y);
            for (size_t c = 0; c < 3U; ++c) {
                const auto detail = int(source[c]) - int(lowFrequency[c]);
                // Scale the detail first, then remove Threshold from the signed adjustment (pinned by PS 27.8).
                const auto scaledDetail = int(double(detail) * amountPercent / 100.0);
                const auto magnitude = std::abs(scaledDetail);
                const auto adjustment =
                    magnitude <= threshold ? 0 : (scaledDetail < 0 ? -(magnitude - threshold) : magnitude - threshold);
                destination[c] = uint8_t(std::clamp(int(source[c]) + adjustment, 0, 255));
            }
            destination[3] = source[3];
        }
    }
    return Result{std::move(output), input.bounds};
}

// ---- Motion Blur -------------------------------------------------------------------------------------------

Result renderMotionBlur(const Result& input, int32_t angleDegrees, int32_t distancePixels) {
    constexpr int64_t kCoordinateScale = 65536;
    constexpr uint64_t kSampleWeight = uint64_t(kCoordinateScale) * kCoordinateScale;
    constexpr double kPi = 3.14159265358979323846;
    const auto radians = double(angleDegrees) * kPi / 180.0;
    const auto stepX = int64_t(std::llround(std::cos(radians) * kCoordinateScale));
    const auto stepY = int64_t(std::llround(-std::sin(radians) * kCoordinateScale));
    const auto firstSample = -distancePixels / 2;
    const auto lastSample = firstSample + distancePixels;
    const auto sampleCount = uint64_t(distancePixels) + 1U;
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto maximumX = int64_t(std::max(0, width - 1)) * kCoordinateScale;
    const auto maximumY = int64_t(std::max(0, height - 1)) * kCoordinateScale;
    Image output(width, height);
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            std::array<uint64_t, 3> premultiplied{};
            uint64_t alphaSum = 0U;
            for (auto sample = firstSample; sample <= lastSample; ++sample) {
                const auto sampleX = std::clamp<int64_t>(int64_t(x) * kCoordinateScale + int64_t(sample) * stepX, 0, maximumX);
                const auto sampleY = std::clamp<int64_t>(int64_t(y) * kCoordinateScale + int64_t(sample) * stepY, 0, maximumY);
                const auto x0 = int32_t(sampleX / kCoordinateScale);
                const auto y0 = int32_t(sampleY / kCoordinateScale);
                const auto x1 = std::min(width - 1, x0 + 1);
                const auto y1 = std::min(height - 1, y0 + 1);
                const auto fractionX = sampleX % kCoordinateScale;
                const auto fractionY = sampleY % kCoordinateScale;
                const std::array<uint64_t, 4> weights{
                    uint64_t(kCoordinateScale - fractionX) * uint64_t(kCoordinateScale - fractionY),
                    uint64_t(fractionX) * uint64_t(kCoordinateScale - fractionY),
                    uint64_t(kCoordinateScale - fractionX) * uint64_t(fractionY),
                    uint64_t(fractionX) * uint64_t(fractionY)};
                const std::array<const uint8_t*, 4> pixels{input.pixels.pixel(x0, y0), input.pixels.pixel(x1, y0),
                                                           input.pixels.pixel(x0, y1), input.pixels.pixel(x1, y1)};
                for (size_t corner = 0; corner < pixels.size(); ++corner) {
                    const auto alpha = uint64_t(pixels[corner][3]);
                    const auto alphaWeight = alpha * weights[corner];
                    alphaSum += alphaWeight;
                    for (size_t c = 0; c < 3U; ++c) premultiplied[c] += uint64_t(pixels[corner][c]) * alphaWeight;
                }
            }
            auto* destination = output.pixel(x, y);
            for (size_t c = 0; c < 3U; ++c)
                destination[c] = alphaSum == 0U
                                     ? uint8_t(0)
                                     : uint8_t(std::min<uint64_t>(255U, (premultiplied[c] + alphaSum / 2U) / alphaSum));
            const auto alphaDenominator = sampleCount * kSampleWeight;
            destination[3] = uint8_t(std::min<uint64_t>(255U, (alphaSum + alphaDenominator / 2U) / alphaDenominator));
        }
    }
    return Result{std::move(output), input.bounds};
}

// ---- transparent colour extension (Median, Dust & Scratches, Surface Blur, Plastic Wrap) ------------------

struct TransparentColorExtension {
    bool allTransparent = false;
    // Empty for an opaque input; otherwise the nearest visible pixel's linear index for every pixel.
    std::vector<uint32_t> nearestVisible;
};

uint8_t extendedStraightColorSample(const Result& input, const TransparentColorExtension& extension, int32_t x, int32_t y,
                                    size_t channel) {
    const auto* pixel = input.pixels.pixel(x, y);
    if (pixel[3] != 0U || extension.nearestVisible.empty()) return pixel[channel];
    const auto index = extension.nearestVisible[size_t(y) * size_t(input.bounds.width) + size_t(x)];
    return input.pixels.data()[size_t(index) * 4U + channel];
}

int64_t ceilDivide(int64_t numerator, int64_t denominator) {
    const auto quotient = numerator / denominator;
    const auto remainder = numerator % denominator;
    return quotient + (remainder > 0 ? 1 : 0);
}

TransparentColorExtension extendTransparentColors(const Result& input) {
    TransparentColorExtension extension;
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto pixelCount = uint64_t(width) * uint64_t(height);
    if (pixelCount == 0U) {
        extension.allTransparent = true;
        return extension;
    }
    bool sawVisible = false, sawTransparent = false;
    for (int32_t y = 0; y < height; ++y)
        for (int32_t x = 0; x < width; ++x) {
            const auto alpha = input.pixels.pixel(x, y)[3];
            sawVisible = sawVisible || alpha != 0U;
            sawTransparent = sawTransparent || alpha == 0U;
        }
    if (!sawVisible) {
        extension.allTransparent = true;
        return extension;
    }
    if (!sawTransparent) return extension;

    constexpr auto kNoSource = std::numeric_limits<uint32_t>::max();
    extension.nearestVisible.assign(size_t(pixelCount), kNoSource);
    // Nearest visible source on each row; an equal-distance choice favours the right.
    for (int32_t y = 0; y < height; ++y) {
        int32_t left = -1;
        const auto rowOffset = size_t(y) * size_t(width);
        for (int32_t x = 0; x < width; ++x) {
            if (input.pixels.pixel(x, y)[3] != 0U) left = x;
            if (left >= 0) extension.nearestVisible[rowOffset + size_t(x)] = uint32_t(left);
        }
        int32_t right = -1;
        for (int32_t x = width; x-- > 0;) {
            if (input.pixels.pixel(x, y)[3] != 0U) right = x;
            if (right < 0) continue;
            auto& source = extension.nearestVisible[rowOffset + size_t(x)];
            if (source == kNoSource || right - x <= x - int32_t(source)) source = uint32_t(right);
        }
    }
    // The exact squared-Euclidean transform down each column, integer lower envelope; ties favour the later row.
    std::vector<uint32_t> rowSourceX(static_cast<size_t>(height));
    std::vector<int32_t> envelopeRows(static_cast<size_t>(height));
    std::vector<int64_t> envelopeStarts(static_cast<size_t>(height));
    for (int32_t x = 0; x < width; ++x) {
        for (int32_t y = 0; y < height; ++y)
            rowSourceX[size_t(y)] = extension.nearestVisible[size_t(y) * size_t(width) + size_t(x)];
        int32_t envelopeSize = 0;
        for (int32_t candidate = 0; candidate < height; ++candidate) {
            const auto candidateX = rowSourceX[size_t(candidate)];
            if (candidateX == kNoSource) continue;
            int64_t start = std::numeric_limits<int64_t>::min();
            while (envelopeSize > 0) {
                const auto previous = envelopeRows[size_t(envelopeSize - 1)];
                const auto previousX = rowSourceX[size_t(previous)];
                const auto candidateDx = int64_t(x) - int64_t(candidateX);
                const auto previousDx = int64_t(x) - int64_t(previousX);
                const auto numerator = candidateDx * candidateDx + int64_t(candidate) * candidate - previousDx * previousDx -
                                       int64_t(previous) * previous;
                const auto denominator = 2LL * (int64_t(candidate) - previous);
                start = ceilDivide(numerator, denominator);
                if (start > envelopeStarts[size_t(envelopeSize - 1)]) break;
                --envelopeSize;
            }
            if (envelopeSize == 0) start = std::numeric_limits<int64_t>::min();
            envelopeRows[size_t(envelopeSize)] = candidate;
            envelopeStarts[size_t(envelopeSize)] = start;
            ++envelopeSize;
        }
        if (envelopeSize == 0) continue;   // cannot happen: some row has a visible source
        int32_t selected = 0;
        for (int32_t y = 0; y < height; ++y) {
            while (selected + 1 < envelopeSize && envelopeStarts[size_t(selected + 1)] <= y) ++selected;
            const auto sourceY = envelopeRows[size_t(selected)];
            const auto sourceX = rowSourceX[size_t(sourceY)];
            const auto sourceIndex = uint64_t(sourceY) * uint64_t(width) + sourceX;
            extension.nearestVisible[size_t(y) * size_t(width) + size_t(x)] = uint32_t(sourceIndex);
        }
    }
    return extension;
}

/// The low-pass input of High Pass and Unsharp Mask. Patchy blurs the straight RGB stored under transparent
/// pixels; premultiplied storage has none left (it reads as black, which darkens the low-pass near the edges of
/// the content), so the transparent pixels take the nearest visible colour, Median's extension rule. Photoshop's
/// previews agree (photoshop-smart-filter-high-pass.psd: flat 128 up to the edges).
Result withHiddenColoursExtended(const Result& input) {
    const auto extension = extendTransparentColors(input);
    if (extension.allTransparent || extension.nearestVisible.empty()) return input;
    Result out = input;
    for (int32_t y = 0; y < input.bounds.height; ++y)
        for (int32_t x = 0; x < input.bounds.width; ++x) {
            auto* p = out.pixels.pixel(x, y);
            if (p[3] != 0) continue;
            for (size_t c = 0; c < 3; ++c) p[c] = extendedStraightColorSample(input, extension, x, y, c);
        }
    return out;
}

// ---- Median, Dust & Scratches, Surface Blur ----------------------------------------------------------------

struct WindowValueHistogram {
    std::array<uint32_t, 256> bins{};
    std::array<uint32_t, 16> coarse{};
    void add(uint8_t value) {
        ++bins[value];
        ++coarse[size_t(value >> 4U)];
    }
    void remove(uint8_t value) {
        --bins[value];
        --coarse[size_t(value >> 4U)];
    }
    uint8_t median(uint32_t rank) const {
        size_t group = 0;
        while (group + 1U < coarse.size() && rank > coarse[group]) {
            rank -= coarse[group];
            ++group;
        }
        const auto first = group * 16U;
        size_t within = 0;
        while (within + 1U < 16U && rank > bins[first + within]) {
            rank -= bins[first + within];
            ++within;
        }
        return uint8_t(first + within);
    }
};

uint8_t roundedWeightedAverage(int64_t weightSum, int64_t weightedSum) {
    if (weightSum <= 0 || weightedSum < 0) return 0;
    const auto w = uint64_t(weightSum);
    const auto s = uint64_t(weightedSum);
    auto quotient = s / w;
    const auto remainder = s % w;
    const auto complement = w - remainder;
    if (remainder > complement || (remainder == complement && (quotient & 1U) != 0U)) ++quotient;
    return uint8_t(std::min<uint64_t>(255U, quotient));
}

// Surface Blur's range weight: the triangle 5*threshold - 2*|d|, kept while positive, indexed by |d|.
std::array<int32_t, 256> surfaceBlurDeltaWeights(int32_t threshold) {
    const auto weightBase = 5 * threshold;
    const auto maximumDelta = (weightBase - 1) / 2;
    std::array<int32_t, 256> weights{};
    for (int32_t delta = 0; delta < 256; ++delta) weights[size_t(delta)] = delta <= maximumDelta ? weightBase - 2 * delta : 0;
    return weights;
}

template <typename Sample>
void filterSquareMedianChannel(const Result& input, Image& output, size_t channel, int32_t radius, Sample&& sample) {
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto diameter = radius * 2 + 1;
    const auto windowArea = uint32_t(diameter) * uint32_t(diameter);
    const auto medianRank = windowArea / 2U + 1U;
    const auto clampX = [width](int32_t x) { return std::clamp(x, 0, width - 1); };
    const auto clampY = [height](int32_t y) { return std::clamp(y, 0, height - 1); };
    // One window histogram, updated one pixel value at a time along a serpentine traversal.
    WindowValueHistogram window;
    for (int32_t dy = -radius; dy <= radius; ++dy) {
        const auto sy = clampY(dy);
        for (int32_t dx = -radius; dx <= radius; ++dx) window.add(sample(clampX(dx), sy));
    }
    int32_t x = 0;
    int32_t direction = 1;
    for (int32_t y = 0; y < height; ++y) {
        while (true) {
            output.pixel(x, y)[channel] = window.median(medianRank);
            const auto nextX = x + direction;
            if (nextX < 0 || nextX >= width) break;
            const auto leavingX = direction > 0 ? clampX(x - radius) : clampX(x + radius);
            const auto enteringX = direction > 0 ? clampX(nextX + radius) : clampX(nextX - radius);
            for (int32_t dy = -radius; dy <= radius; ++dy) {
                const auto sy = clampY(y + dy);
                window.remove(sample(leavingX, sy));
                window.add(sample(enteringX, sy));
            }
            x = nextX;
        }
        if (y + 1 < height) {
            const auto leavingY = clampY(y - radius);
            const auto enteringY = clampY(y + 1 + radius);
            for (int32_t dx = -radius; dx <= radius; ++dx) {
                const auto sx = clampX(x + dx);
                window.remove(sample(sx, leavingY));
                window.add(sample(sx, enteringY));
            }
        }
        direction = -direction;
    }
}

template <typename Sample>
void filterSquareSurfaceChannel(const Result& input, Image& output, size_t channel, int32_t radius, int32_t threshold,
                                Sample&& sample) {
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto clampX = [width](int32_t x) { return std::clamp(x, 0, width - 1); };
    const auto clampY = [height](int32_t y) { return std::clamp(y, 0, height - 1); };
    const auto weights = surfaceBlurDeltaWeights(threshold);
    const auto rowStride = size_t(width);
    std::vector<uint8_t> plane(rowStride * size_t(height));
    for (int32_t y = 0; y < height; ++y)
        for (int32_t x = 0; x < width; ++x) plane[size_t(y) * rowStride + size_t(x)] = sample(x, y);
    const auto planeRow = [&plane, rowStride](int32_t y) { return plane.data() + size_t(y) * rowStride; };

    if (radius <= kSurfaceBlurDirectMaximumRadius) {
        for (int32_t y = 0; y < height; ++y) {
            const auto* centerRow = planeRow(y);
            for (int32_t x = 0; x < width; ++x) {
                const auto center = int32_t(centerRow[size_t(x)]);
                int64_t weightSum = 0, weightedSum = 0;
                for (int32_t dy = -radius; dy <= radius; ++dy) {
                    const auto* row = planeRow(clampY(y + dy));
                    for (int32_t dx = -radius; dx <= radius; ++dx) {
                        const auto value = int32_t(row[size_t(clampX(x + dx))]);
                        const auto weight = weights[size_t(std::abs(value - center))];
                        weightSum += weight;
                        weightedSum += int64_t(weight) * value;
                    }
                }
                output.pixel(x, y)[channel] = roundedWeightedAverage(weightSum, weightedSum);
            }
        }
        return;
    }

    // Large radii: per intensity level present as a centre value, box-sum the level's weight-transformed plane.
    std::array<bool, 256> present{};
    for (const auto value : plane) present[value] = true;
    std::vector<uint32_t> columnWeight(static_cast<size_t>(width)), columnWeighted(static_cast<size_t>(width));
    for (int32_t center = 0; center < 256; ++center) {
        if (!present[size_t(center)]) continue;
        std::array<uint32_t, 256> weightLut{}, weightedLut{};
        for (int32_t value = 0; value < 256; ++value) {
            const auto weight = uint32_t(weights[size_t(std::abs(value - center))]);
            weightLut[size_t(value)] = weight;
            weightedLut[size_t(value)] = weight * uint32_t(value);
        }
        std::fill(columnWeight.begin(), columnWeight.end(), 0U);
        std::fill(columnWeighted.begin(), columnWeighted.end(), 0U);
        for (int32_t dy = -radius; dy <= radius; ++dy) {
            const auto* row = planeRow(clampY(dy));
            for (int32_t x = 0; x < width; ++x) {
                const auto value = row[size_t(x)];
                columnWeight[size_t(x)] += weightLut[value];
                columnWeighted[size_t(x)] += weightedLut[value];
            }
        }
        for (int32_t y = 0; y < height; ++y) {
            if (y > 0) {
                const auto* leaving = planeRow(clampY(y - 1 - radius));
                const auto* entering = planeRow(clampY(y + radius));
                for (int32_t x = 0; x < width; ++x) {
                    const auto lv = leaving[size_t(x)];
                    const auto ev = entering[size_t(x)];
                    columnWeight[size_t(x)] += weightLut[ev] - weightLut[lv];
                    columnWeighted[size_t(x)] += weightedLut[ev] - weightedLut[lv];
                }
            }
            int64_t windowWeight = 0, windowWeighted = 0;
            for (int32_t dx = -radius; dx <= radius; ++dx) {
                const auto sx = size_t(clampX(dx));
                windowWeight += columnWeight[sx];
                windowWeighted += columnWeighted[sx];
            }
            const auto* centerRow = planeRow(y);
            for (int32_t x = 0; x < width; ++x) {
                if (x > 0) {
                    const auto leavingX = size_t(clampX(x - 1 - radius));
                    const auto enteringX = size_t(clampX(x + radius));
                    windowWeight += int64_t(columnWeight[enteringX]) - int64_t(columnWeight[leavingX]);
                    windowWeighted += int64_t(columnWeighted[enteringX]) - int64_t(columnWeighted[leavingX]);
                }
                if (int32_t(centerRow[size_t(x)]) == center)
                    output.pixel(x, y)[channel] = roundedWeightedAverage(windowWeight, windowWeighted);
            }
        }
    }
}

Result renderMedian(const Result& input, double radius) {
    if (pixelsEmpty(input)) return input;
    const auto effectiveRadius = std::max(1, int32_t(std::floor(radius)));
    const auto extension = extendTransparentColors(input);
    Image output = input.pixels;
    filterSquareMedianChannel(input, output, 3U, effectiveRadius,
                              [&input](int32_t x, int32_t y) { return input.pixels.pixel(x, y)[3]; });
    // With no visible colour source the hidden RGB is kept rather than manufactured.
    if (extension.allTransparent) return Result{std::move(output), input.bounds};
    for (size_t channel = 0; channel < 3U; ++channel)
        filterSquareMedianChannel(input, output, channel, effectiveRadius, [&input, &extension, channel](int32_t x, int32_t y) {
            return extendedStraightColorSample(input, extension, x, y, channel);
        });
    return Result{std::move(output), input.bounds};
}

Result renderSurfaceBlur(const Result& input, double radius, int32_t threshold) {
    if (pixelsEmpty(input)) return input;
    const auto effectiveRadius = std::max(1, int32_t(std::floor(radius + 0.5)));
    const auto extension = extendTransparentColors(input);
    Image output = input.pixels;
    filterSquareSurfaceChannel(input, output, 3U, effectiveRadius, threshold,
                               [&input](int32_t x, int32_t y) { return input.pixels.pixel(x, y)[3]; });
    if (extension.allTransparent) return Result{std::move(output), input.bounds};
    for (size_t channel = 0; channel < 3U; ++channel)
        filterSquareSurfaceChannel(input, output, channel, effectiveRadius, threshold,
                                   [&input, &extension, channel](int32_t x, int32_t y) {
                                       return extendedStraightColorSample(input, extension, x, y, channel);
                                   });
    return Result{std::move(output), input.bounds};
}

Result renderDustAndScratches(const Result& input, int32_t radius, int32_t threshold) {
    if (pixelsEmpty(input)) return input;
    const auto extension = extendTransparentColors(input);
    if (extension.allTransparent) return input;
    Image output = input.pixels;
    for (size_t channel = 0; channel < 3U; ++channel)
        filterSquareMedianChannel(input, output, channel, radius, [&input, &extension, channel](int32_t x, int32_t y) {
            return extendedStraightColorSample(input, extension, x, y, channel);
        });
    for (int32_t y = 0; y < input.bounds.height; ++y) {
        for (int32_t x = 0; x < input.bounds.width; ++x) {
            auto* destination = output.pixel(x, y);
            const std::array<uint8_t, 3> median{destination[0], destination[1], destination[2]};
            std::array<uint8_t, 3> source{};
            int32_t difference = 0;
            for (size_t c = 0; c < source.size(); ++c) {
                source[c] = extendedStraightColorSample(input, extension, x, y, c);
                difference = std::max(difference, std::abs(int32_t(source[c]) - int32_t(median[c])));
            }
            const auto replace = difference > threshold;
            for (size_t c = 0; c < source.size(); ++c) destination[c] = replace ? median[c] : source[c];
            // Straight RGB only; alpha stays byte-exact.
            destination[3] = input.pixels.pixel(x, y)[3];
        }
    }
    return Result{std::move(output), input.bounds};
}

// ---- Plastic Wrap ------------------------------------------------------------------------------------------

Result renderPlasticWrap(const Result& input, int32_t highlightStrength, int32_t detail, int32_t smoothness) {
    if (pixelsEmpty(input)) return input;
    const auto extension = extendTransparentColors(input);
    if (extension.allTransparent) return input;
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto count = size_t(width) * size_t(height);
    std::vector<uint16_t> luminance(count), horizontal(count), heightField(count);
    const auto indexOf = [width](int32_t x, int32_t y) { return size_t(y) * size_t(width) + size_t(x); };

    for (int32_t y = 0; y < height; ++y)
        for (int32_t x = 0; x < width; ++x) {
            const auto red = uint32_t(extendedStraightColorSample(input, extension, x, y, 0U));
            const auto green = uint32_t(extendedStraightColorSample(input, extension, x, y, 1U));
            const auto blue = uint32_t(extendedStraightColorSample(input, extension, x, y, 2U));
            const auto alpha = uint32_t(input.pixels.pixel(x, y)[3]);
            const auto straightLuminance = (77U * red + 150U * green + 29U * blue + 128U) >> 8U;
            luminance[indexOf(x, y)] = uint16_t((straightLuminance * alpha + 127U) / 255U);
        }

    const auto radius = 1 + (smoothness - 1) / 3;
    const auto diameter = radius * 2 + 1;
    for (int32_t y = 0; y < height; ++y) {
        uint32_t sum = 0U;
        for (int32_t offset = -radius; offset <= radius; ++offset) sum += luminance[indexOf(std::clamp(offset, 0, width - 1), y)];
        for (int32_t x = 0; x < width; ++x) {
            horizontal[indexOf(x, y)] = uint16_t((sum + uint32_t(diameter / 2)) / uint32_t(diameter));
            const auto leaving = std::clamp(x - radius, 0, width - 1);
            const auto entering = std::clamp(x + radius + 1, 0, width - 1);
            sum += luminance[indexOf(entering, y)];
            sum -= luminance[indexOf(leaving, y)];
        }
    }
    for (int32_t x = 0; x < width; ++x) {
        uint32_t sum = 0U;
        for (int32_t offset = -radius; offset <= radius; ++offset) sum += horizontal[indexOf(x, std::clamp(offset, 0, height - 1))];
        for (int32_t y = 0; y < height; ++y) {
            const auto smoothed = uint32_t((sum + uint32_t(diameter / 2)) / uint32_t(diameter));
            const auto source = uint32_t(luminance[indexOf(x, y)]);
            heightField[indexOf(x, y)] =
                uint16_t((smoothed * uint32_t(15 - detail) + source * uint32_t(detail) + 7U) / 15U);
            const auto leaving = std::clamp(y - radius, 0, height - 1);
            const auto entering = std::clamp(y + radius + 1, 0, height - 1);
            sum += horizontal[indexOf(x, entering)];
            sum -= horizontal[indexOf(x, leaving)];
        }
    }

    Image output = input.pixels;
    // One fixed local height-field treatment: no content-adaptive choices.
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const auto left = heightField[indexOf(std::max(0, x - 1), y)];
            const auto right = heightField[indexOf(std::min(width - 1, x + 1), y)];
            const auto up = heightField[indexOf(x, std::max(0, y - 1))];
            const auto down = heightField[indexOf(x, std::min(height - 1, y + 1))];
            const auto gradientX = int32_t(right) - left;
            const auto gradientY = int32_t(down) - up;
            const auto facing = -3 * gradientX - 4 * gradientY;
            const auto edge = std::abs(gradientX) + std::abs(gradientY);
            const auto relief = std::clamp(facing / 3, -112, 112);
            const auto ridgeSignal = std::clamp(edge * 8, 0, 255);
            const auto specularSignal = std::clamp(std::max(0, facing) + ridgeSignal, 0, 255);
            const auto curvedSpecular = (specularSignal * 2 + (specularSignal * specularSignal + 127) / 255 + 1) / 3;
            const auto shine = (curvedSpecular * highlightStrength + 10) / 20;
            const auto* source = input.pixels.pixel(x, y);
            auto* destination = output.pixel(x, y);
            for (size_t c = 0; c < 3U; ++c) {
                const auto shaded = std::clamp(int(source[c]) + relief, 0, 255);
                destination[c] = uint8_t(std::clamp(shaded + ((255 - shaded) * shine + 127) / 255, 0, 255));
            }
            destination[3] = source[3];
        }
    }
    return Result{std::move(output), input.bounds};
}

// ---- Box Blur ----------------------------------------------------------------------------------------------

// The destructive Box Blur math: alpha-weighted separable box average in doubles, edge-clamped.
Result renderBoxBlurDirect(const Result& input, int32_t radius) {
    const auto width = input.pixels.width(), height = input.pixels.height();
    Result result = input;
    const auto taps = 2 * radius + 1;
    const auto axisWeightSum = double(taps);
    const auto totalWeight = axisWeightSum * axisWeightSum;
    const auto rowStride = size_t(width) * 4U;
    std::vector<double> hRows(rowStride * size_t(taps), 0.0);
    int hRowsBuiltThrough = -1;
    const auto buildHRow = [&](int32_t sourceY, double* out) {
        std::fill(out, out + rowStride, 0.0);
        for (int dx = -radius; dx <= radius; ++dx)
            for (int32_t x = 0; x < width; ++x) {
                const auto sx = std::clamp<int32_t>(x + dx, 0, width - 1);
                const auto* px = input.pixels.pixel(sx, sourceY);
                const auto alpha = double(px[3]) / 255.0;
                auto* accum = out + size_t(x) * 4U;
                for (int c = 0; c < 3; ++c) accum[c] += double(px[c]) * alpha;
                accum[3] += alpha;
            }
    };
    const auto hRowFor = [&](int32_t sourceY) -> const double* { return hRows.data() + size_t(sourceY % taps) * rowStride; };
    std::vector<double> vAccum(rowStride);
    for (int32_t y = 0; y < height; ++y) {
        const auto neededThrough = std::min<int32_t>(height - 1, y + radius);
        while (hRowsBuiltThrough < neededThrough) {
            ++hRowsBuiltThrough;
            buildHRow(hRowsBuiltThrough, hRows.data() + size_t(hRowsBuiltThrough % taps) * rowStride);
        }
        std::fill(vAccum.begin(), vAccum.end(), 0.0);
        for (int dy = -radius; dy <= radius; ++dy) {
            const auto sy = std::clamp<int32_t>(y + dy, 0, height - 1);
            const auto* hRow = hRowFor(sy);
            for (size_t i = 0; i < rowStride; ++i) vAccum[i] += hRow[i];
        }
        for (int32_t x = 0; x < width; ++x) {
            const auto* accum = vAccum.data() + size_t(x) * 4U;
            auto* dst = result.pixels.pixel(x, y);
            const auto alphaSum = accum[3];
            for (int c = 0; c < 3; ++c) {
                const auto value = alphaSum > 0.000001 ? accum[c] / alphaSum : 0.0;
                dst[c] = clampLong(std::lround(value));
            }
            dst[3] = clampLong(std::lround(alphaSum / totalWeight * 255.0));
        }
    }
    return result;
}

// Large radii: an exact integer sliding-window box average with the same edge-clamped sampling.
Result renderBoxBlurSliding(const Result& input, int32_t radius) {
    const auto width = input.pixels.width(), height = input.pixels.height();
    Result result = input;
    const auto taps = int64_t(2) * radius + 1;
    const auto totalWeight = double(taps) * double(taps);
    const auto rowStride = size_t(width) * 4U;
    const auto term = [&](int32_t x, int32_t y, int c) {
        const auto* px = input.pixels.pixel(x, y);
        return c < 3 ? int64_t(px[c]) * int64_t(px[3]) : int64_t(px[3]);
    };
    std::vector<int64_t> hRow(rowStride);
    const auto buildHRow = [&](int32_t sourceY) {
        std::array<int64_t, 4> window{};
        for (int32_t t = -radius; t <= radius; ++t) {
            const auto sx = std::clamp<int32_t>(t, 0, width - 1);
            for (int c = 0; c < 4; ++c) window[size_t(c)] += term(sx, sourceY, c);
        }
        for (int32_t x = 0; x < width; ++x) {
            auto* out = hRow.data() + size_t(x) * 4U;
            for (int c = 0; c < 4; ++c) out[c] = window[size_t(c)];
            const auto leaving = std::clamp<int32_t>(x - radius, 0, width - 1);
            const auto entering = std::clamp<int32_t>(x + 1 + radius, 0, width - 1);
            for (int c = 0; c < 4; ++c) window[size_t(c)] += term(entering, sourceY, c) - term(leaving, sourceY, c);
        }
    };
    std::vector<int64_t> vAccum(rowStride, 0);
    const auto addRow = [&](int32_t sourceY, int64_t sign) {
        buildHRow(sourceY);
        for (size_t i = 0; i < rowStride; ++i) vAccum[i] += sign * hRow[i];
    };
    for (int32_t t = -radius; t <= radius; ++t) addRow(std::clamp<int32_t>(t, 0, height - 1), 1);
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const auto* accum = vAccum.data() + size_t(x) * 4U;
            auto* dst = result.pixels.pixel(x, y);
            const auto alphaSum = accum[3];
            for (int c = 0; c < 3; ++c) {
                const auto value = alphaSum > 0 ? double(accum[c]) / double(alphaSum) : 0.0;
                dst[c] = clampLong(std::lround(value));
            }
            dst[3] = clampLong(std::lround(double(alphaSum) / totalWeight));
        }
        if (y + 1 < height) {
            addRow(std::clamp<int32_t>(y - radius, 0, height - 1), -1);
            addRow(std::clamp<int32_t>(y + 1 + radius, 0, height - 1), 1);
        }
    }
    return result;
}

Result renderBoxBlur(const Result& input, int32_t radius) {
    if (pixelsEmpty(input)) return input;
    radius = std::clamp(radius, 1, int32_t(kMaximumBoxBlurRadius));
    if (radius <= kBoxBlurDirectMaximumRadius) return renderBoxBlurDirect(input, radius);
    return renderBoxBlurSliding(input, radius);
}

// ---- Emboss ------------------------------------------------------------------------------------------------

Result renderEmboss(const Result& input, int32_t angleDegrees, int32_t heightPixels, int32_t amountPercent) {
    if (pixelsEmpty(input)) return input;
    constexpr double kPi = 3.14159265358979323846;
    const auto w = input.pixels.width(), h = input.pixels.height();
    const auto sampled = [&](double x, double y, int channel) {
        x = std::clamp(x, 0.0, double(std::max<int32_t>(0, w - 1)));
        y = std::clamp(y, 0.0, double(std::max<int32_t>(0, h - 1)));
        const auto x0 = int32_t(std::floor(x)), y0 = int32_t(std::floor(y));
        const auto x1 = std::min<int32_t>(w - 1, x0 + 1), y1 = std::min<int32_t>(h - 1, y0 + 1);
        const auto tx = x - double(x0), ty = y - double(y0);
        const auto top = input.pixels.pixel(x0, y0)[channel] * (1.0 - tx) + input.pixels.pixel(x1, y0)[channel] * tx;
        const auto bottom = input.pixels.pixel(x0, y1)[channel] * (1.0 - tx) + input.pixels.pixel(x1, y1)[channel] * tx;
        return top * (1.0 - ty) + bottom * ty;
    };
    // Fitted to Photoshop's own Smart Filter preview (a red rectangle on white, 135 degrees, height 3, amount 150):
    // each channel is sampled half the height either side along the angle, the side toward the light counts as lit,
    // and the difference is scaled by the amount about middle grey (red's 55-level step gives about 128 + 84, green
    // and blue's larger ones clip). Not Patchy's grey relief, which differs from Photoshop's by some 46 levels there.
    const auto angle = double(angleDegrees) * kPi / 180.0;
    const auto distance = double(heightPixels) / 2.0;
    const auto offsetX = std::cos(angle) * distance;
    const auto offsetY = -std::sin(angle) * distance;
    const auto scale = double(amountPercent) / 100.0;
    Result result = input;
    for (int32_t y = 0; y < h; ++y)
        for (int32_t x = 0; x < w; ++x) {
            auto* px = result.pixels.pixel(x, y);
            for (int c = 0; c < 3; c++) {
                const auto lit = sampled(double(x) + offsetX, double(y) + offsetY, c);
                const auto dark = sampled(double(x) - offsetX, double(y) - offsetY, c);
                px[c] = clampLong(std::lround(128.0 + (lit - dark) * scale));
            }
        }
    return result;
}

// ---- Radial Blur (Spin) ------------------------------------------------------------------------------------

struct RadialBlurAccum {
    std::array<double, 3> premultipliedColor{0.0, 0.0, 0.0};
    double alpha = 0.0;
    double weight = 0.0;
};

void radialBlurAccumulatePixel(RadialBlurAccum& accum, const uint8_t* px, double weight) {
    if (weight <= 0.0) return;
    const auto alpha = double(px[3]) / 255.0;
    accum.weight += weight;
    accum.alpha += alpha * weight;
    for (size_t c = 0; c < 3; ++c) accum.premultipliedColor[c] += double(px[c]) * alpha * weight;
}

void radialBlurAccumulateSample(RadialBlurAccum& accum, const Image& original, double x, double y) {
    x = std::clamp(x, 0.0, double(std::max<int32_t>(0, original.width() - 1)));
    y = std::clamp(y, 0.0, double(std::max<int32_t>(0, original.height() - 1)));
    const auto x0 = int32_t(std::floor(x));
    const auto y0 = int32_t(std::floor(y));
    const auto x1 = std::min<int32_t>(original.width() - 1, x0 + 1);
    const auto y1 = std::min<int32_t>(original.height() - 1, y0 + 1);
    const auto tx = x - double(x0);
    const auto ty = y - double(y0);
    radialBlurAccumulatePixel(accum, original.pixel(x0, y0), (1.0 - tx) * (1.0 - ty));
    radialBlurAccumulatePixel(accum, original.pixel(x1, y0), tx * (1.0 - ty));
    radialBlurAccumulatePixel(accum, original.pixel(x0, y1), (1.0 - tx) * ty);
    radialBlurAccumulatePixel(accum, original.pixel(x1, y1), tx * ty);
}

void radialBlurWritePixel(Image& pixels, int32_t x, int32_t y, const RadialBlurAccum& accum) {
    auto* dst = pixels.pixel(x, y);
    const auto normalizedAlpha = accum.weight > 0.0 ? accum.alpha / accum.weight : 1.0;
    for (size_t c = 0; c < 3; ++c) {
        const auto value = accum.alpha > 0.000001 ? accum.premultipliedColor[c] / accum.alpha : 0.0;
        dst[c] = clampLong(std::lround(value));
    }
    dst[3] = clampLong(std::lround(normalizedAlpha * 255.0));
}

Result renderRadialBlur(const Result& input, int32_t amount, int32_t samples, double centerX, double centerY) {
    if (pixelsEmpty(input)) return input;
    constexpr double kPi = 3.14159265358979323846;
    Result result{Image(input.bounds.width, input.bounds.height), input.bounds};
    const auto clampedAmount = std::clamp(amount, 0, 100);
    const auto clampedSamples = std::clamp(samples, 4, 32);
    const auto sweep = double(clampedAmount) * 3.6 * kPi / 180.0;
    for (int32_t y = 0; y < input.bounds.height; ++y)
        for (int32_t x = 0; x < input.bounds.width; ++x) {
            const auto dx = double(x) - centerX;
            const auto dy = double(y) - centerY;
            RadialBlurAccum accum;
            for (int sample = 0; sample < clampedSamples; ++sample) {
                const auto t = clampedSamples <= 1 ? 0.0 : double(sample) / double(clampedSamples - 1) - 0.5;
                const auto angle = sweep * t;
                const auto sourceX = centerX + dx * std::cos(angle) - dy * std::sin(angle);
                const auto sourceY = centerY + dx * std::sin(angle) + dy * std::cos(angle);
                radialBlurAccumulateSample(accum, input.pixels, sourceX, sourceY);
            }
            radialBlurWritePixel(result.pixels, x, y, accum);
        }
    return result;
}

// ---- Add Noise ---------------------------------------------------------------------------------------------

// Patchy's filter_noise_hash (filter_engine.cpp), which its destructive add_noise and the Smart Filter share.
uint32_t addNoiseHash(int32_t x, int32_t y, uint32_t seed) {
    auto value = uint32_t(x + 16384) * 374761393U;
    value ^= uint32_t(y + 8192) * 668265263U;
    value ^= seed * 2246822519U;
    value ^= value >> 13U;
    value *= 1274126177U;
    value ^= value >> 16U;
    return value;
}

// RGB gains position-hashed deltas (buffer-local coordinates); alpha and bounds stay byte-identical.
Result renderAddNoise(const Result& input, double amountPercent, bool gaussian, bool monochromatic, int32_t seed) {
    if (pixelsEmpty(input)) return input;
    Result result = input;
    const auto range = std::clamp(amountPercent, kMinimumAddNoiseAmount, kMaximumAddNoiseAmount) * 2.55;
    const auto laneBase = uint32_t(std::clamp(seed, kMinimumAddNoiseSeed, kMaximumAddNoiseSeed)) * 16U;
    const auto unitFromHash = [](uint32_t hash) { return double(hash) * (2.0 / 4294967295.0) - 1.0; };
    const auto deltaForLane = [&](int32_t x, int32_t y, uint32_t lane) {
        if (!gaussian) return std::lround(unitFromHash(addNoiseHash(x, y, laneBase + lane * 4U)) * range);
        double sum = 0.0;   // sum of four uniforms
        for (uint32_t sample = 1; sample <= 4U; ++sample) sum += unitFromHash(addNoiseHash(x, y, laneBase + lane * 4U + sample));
        return std::lround(sum * 0.5 * range);
    };
    for (int32_t y = 0; y < result.bounds.height; ++y)
        for (int32_t x = 0; x < result.bounds.width; ++x) {
            auto* px = result.pixels.pixel(x, y);
            if (monochromatic) {
                const auto delta = deltaForLane(x, y, 3U);
                for (int c = 0; c < 3; ++c) px[c] = clampLong(long(px[c]) + delta);
            } else {
                for (uint32_t c = 0; c < 3U; ++c) px[c] = clampLong(long(px[c]) + deltaForLane(x, y, c));
            }
        }
    return result;
}

// ---- Mosaic ------------------------------------------------------------------------------------------------

// Alpha-weighted block means over a grid anchored at the input's local origin; bounds kept.
Result renderMosaic(const Result& input, int32_t cellSizePixels) {
    if (pixelsEmpty(input)) return input;
    Result result{Image(input.bounds.width, input.bounds.height), input.bounds};
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto cell = std::max<int32_t>(kMinimumMosaicCellSize, cellSizePixels);
    for (int32_t blockY = 0; blockY < height; blockY += cell) {
        const auto blockHeight = std::min(cell, height - blockY);
        for (int32_t blockX = 0; blockX < width; blockX += cell) {
            const auto blockWidth = std::min(cell, width - blockX);
            double weight = 0.0, alphaSum = 0.0;
            std::array<double, 3> premultiplied{};
            for (int32_t y = blockY; y < blockY + blockHeight; ++y)
                for (int32_t x = blockX; x < blockX + blockWidth; ++x) {
                    const auto* px = input.pixels.pixel(x, y);
                    const auto alpha = double(px[3]) / 255.0;
                    weight += 1.0;
                    alphaSum += alpha;
                    for (size_t c = 0; c < 3; ++c) premultiplied[c] += double(px[c]) * alpha;
                }
            std::array<uint8_t, 4> value{};
            for (size_t c = 0; c < 3; ++c) {
                const auto straight = alphaSum > 0.000001 ? premultiplied[c] / alphaSum : 0.0;
                value[c] = clampLong(std::lround(straight));
            }
            value[3] = clampLong(std::lround(weight > 0.0 ? alphaSum / weight * 255.0 : 0.0));
            for (int32_t y = blockY; y < blockY + blockHeight; ++y)
                for (int32_t x = blockX; x < blockX + blockWidth; ++x) std::copy(value.begin(), value.end(), result.pixels.pixel(x, y));
        }
    }
    return result;
}

// ---- stack plumbing ----------------------------------------------------------------------------------------

Image cropBuffer(const Image& source, PixelRect sourceBounds, PixelRect cropBounds) {
    Image cropped(cropBounds.width, cropBounds.height);
    const auto sx = cropBounds.x - sourceBounds.x, sy = cropBounds.y - sourceBounds.y;
    const auto rowBytes = size_t(cropBounds.width) * 4U;
    for (int32_t y = 0; y < cropBounds.height; ++y) {
        const auto* row = source.pixel(sx, sy + y);
        std::copy(row, row + rowBytes, cropped.pixel(0, y));
    }
    return cropped;
}

Result trimTransparentResult(Result result) {
    int32_t minX = result.bounds.width, minY = result.bounds.height, maxX = -1, maxY = -1;
    for (int32_t y = 0; y < result.bounds.height; ++y)
        for (int32_t x = 0; x < result.bounds.width; ++x) {
            if (result.pixels.pixel(x, y)[3] == 0U) continue;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    if (maxX < minX || maxY < minY) return result;
    const PixelRect cropped{result.bounds.x + minX, result.bounds.y + minY, maxX - minX + 1, maxY - minY + 1};
    if (cropped == result.bounds) return result;
    return Result{cropBuffer(result.pixels, result.bounds, cropped), cropped};
}

/// One entry's result over the result so far. Normal at 100% replaces; anything else composites the filtered
/// pixels source-over with opacity and blend, through NekoPhoto's own blend maths (premultiplied).
Result blendEntryResult(const Result& before, Result filtered, double opacity, BlendMode mode) {
    if (opacity >= 1.0 && mode == BlendMode::Normal) return filtered;
    const auto bounds = unionRect(before.bounds, filtered.bounds);
    Image output(bounds.width, bounds.height);
    const auto steps = coverageSteps(float(std::clamp(opacity, 0.0, 1.0)));
    const auto premultiplied = [](const uint8_t* s, uint8_t* d) {
        const unsigned a = s[3];
        for (int c = 0; c < 3; ++c) d[c] = a == 255 ? s[c] : uint8_t((s[c] * a + 127) / 255);
        d[3] = s[3];
    };
    for (int32_t y = 0; y < bounds.height; ++y)
        for (int32_t x = 0; x < bounds.width; ++x) {
            const auto* destination = sampleResult(before, bounds.x + x, bounds.y + y);
            const auto* source = sampleResult(filtered, bounds.x + x, bounds.y + y);
            uint8_t d[4] = {0, 0, 0, 0}, s[4] = {0, 0, 0, 0};
            if (destination) premultiplied(destination, d);
            if (source) premultiplied(source, s);
            if (s[3] != 0) compositePixelSteps(mode, s, steps, d);
            // Back to straight colour for the next kernel.
            auto* out = output.pixel(x, y);
            const unsigned a = d[3];
            for (int c = 0; c < 3; ++c)
                out[c] = a == 0 ? uint8_t(0) : a == 255 ? d[c] : uint8_t(std::min(255u, (d[c] * 255u + a / 2) / a));
            out[3] = d[3];
        }
    return Result{std::move(output), bounds};
}

uint8_t sampleFilterMask(const SmartFilterStack& stack, int32_t documentX, int32_t documentY) {
    if (!stack.maskEnabled || !stack.mask) return 255;
    const auto localX = int64_t(documentX) - stack.maskBounds.x;
    const auto localY = int64_t(documentY) - stack.maskBounds.y;
    const auto& mask = *stack.mask;
    if (!mask.isEmpty() && localX >= 0 && localY >= 0 && localX < stack.maskBounds.width && localY < stack.maskBounds.height &&
        localX < mask.width() && localY < mask.height())
        return mask.at(int32_t(localX), int32_t(localY));
    return stack.maskDefault;
}

/// The shared mask, once, between the unfiltered pixels (embedded in the canvas) and the filtered stack; trimmed.
Result applyStackMask(const Result& base, const Result& filtered, const SmartFilterStack& stack) {
    constexpr uint64_t kMaskScale = 255U;
    const auto bounds = unionRect(base.bounds, filtered.bounds);
    Image output(bounds.width, bounds.height);
    int32_t minX = bounds.width, minY = bounds.height, maxX = -1, maxY = -1;
    for (int32_t y = 0; y < bounds.height; ++y) {
        const auto documentY = bounds.y + y;
        for (int32_t x = 0; x < bounds.width; ++x) {
            const auto documentX = bounds.x + x;
            const auto* destination = sampleResult(base, documentX, documentY);
            const auto* source = sampleResult(filtered, documentX, documentY);
            const auto effectWeight = uint64_t(sampleFilterMask(stack, documentX, documentY));
            const auto beforeWeight = kMaskScale - effectWeight;
            const auto destinationAlpha = uint64_t(destination ? destination[3] : 0U);
            const auto sourceAlpha = uint64_t(source ? source[3] : 0U);
            const auto alphaNumerator = destinationAlpha * beforeWeight + sourceAlpha * effectWeight;
            auto* pixel = output.pixel(x, y);
            for (size_t c = 0; c < 3U; ++c) {
                if (alphaNumerator == 0U) {
                    pixel[c] = 0;
                    continue;
                }
                const auto destinationColor = uint64_t(destination ? destination[c] : 0U);
                const auto sourceColor = uint64_t(source ? source[c] : 0U);
                const auto numerator = destinationColor * destinationAlpha * beforeWeight + sourceColor * sourceAlpha * effectWeight;
                pixel[c] = uint8_t(std::min<uint64_t>(255U, (numerator + alphaNumerator / 2U) / alphaNumerator));
            }
            pixel[3] = uint8_t(std::min<uint64_t>(255U, (alphaNumerator + kMaskScale / 2U) / kMaskScale));
            if (pixel[3] != 0U) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    PixelRect cropped;
    if (maxX < minX || maxY < minY) cropped = intersectRect(base.bounds, bounds);
    else cropped = PixelRect{bounds.x + minX, bounds.y + minY, maxX - minX + 1, maxY - minY + 1};
    if (cropped == bounds || cropped.empty()) return Result{std::move(output), bounds};
    return Result{cropBuffer(output, bounds, cropped), cropped};
}

// ---- premultiplied <-> straight boundaries -----------------------------------------------------------------

Result toStraight(const PlacedRaster& in) {
    Result r;
    if (!in.image) return r;
    r.pixels = *in.image;
    unpremultiply(r.pixels);
    r.bounds = in.bounds();
    return r;
}

PlacedRaster toPlaced(Result r) {
    premultiply(r.pixels);
    PlacedRaster out;
    out.x = r.bounds.x;
    out.y = r.bounds.y;
    out.image = std::make_shared<Image>(std::move(r.pixels));
    return out;
}

PixelRect canvasOr(const PixelRect& canvas, const Result& r) { return canvas.empty() ? r.bounds : canvas; }

// Growing filters: embed in the canvas, filter, trim.
Result gaussianStep(const Result& r, const PixelRect& canvas, double radius) {
    return trimTransparentResult(renderGaussian(embedInFilterCanvas(r, canvasOr(canvas, r)), radius));
}
Result surfaceStep(const Result& r, const PixelRect& canvas, double radius, int32_t threshold) {
    return trimTransparentResult(renderSurfaceBlur(embedInFilterCanvas(r, canvasOr(canvas, r)), radius, threshold));
}
Result motionStep(const Result& r, const PixelRect& canvas, int32_t angle, int32_t distance) {
    return trimTransparentResult(renderMotionBlur(embedInFilterCanvas(r, canvasOr(canvas, r)), angle, distance));
}
Result boxStep(const Result& r, const PixelRect& canvas, double radius) {
    return trimTransparentResult(renderBoxBlur(embedInFilterCanvas(r, canvasOr(canvas, r)), int32_t(std::floor(radius))));
}
Result radialStep(const Result& r, const PixelRect& canvas, int32_t amount, int32_t samples) {
    const auto content = r.bounds;
    auto input = embedInFilterCanvas(r, canvasOr(canvas, r));
    // The sweep pivots on the CONTENT centre in canvas buffer coordinates, never the canvas centre.
    const auto centerX = double(content.x - input.bounds.x) + double(std::max<int32_t>(0, content.width - 1)) * 0.5;
    const auto centerY = double(content.y - input.bounds.y) + double(std::max<int32_t>(0, content.height - 1)) * 0.5;
    return trimTransparentResult(renderRadialBlur(input, amount, samples, centerX, centerY));
}

bool inRange(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

/// Patchy's validate_stack for one entry's parameters (out-of-range values make it refuse the stack).
struct ParametersValid {
    bool operator()(const std::monostate&) const { return false; }
    bool operator()(const smartfilter::GaussianBlur& p) const { return inRange(p.radius, kMinimumGaussianRadius, kMaximumGaussianRadius); }
    bool operator()(const smartfilter::HighPass& p) const { return inRange(p.radius, kMinimumGaussianRadius, kMaximumGaussianRadius); }
    bool operator()(const smartfilter::Median& p) const { return inRange(p.radius, kMinimumMedianRadius, kMaximumMedianRadius); }
    bool operator()(const smartfilter::DustAndScratches& p) const {
        return p.radius >= kMinimumDustAndScratchesRadius && p.radius <= kMaximumDustAndScratchesRadius &&
               p.threshold >= kMinimumDustAndScratchesThreshold && p.threshold <= kMaximumDustAndScratchesThreshold;
    }
    bool operator()(const smartfilter::SurfaceBlur& p) const {
        return inRange(p.radius, kMinimumSurfaceBlurRadius, kMaximumSurfaceBlurRadius) && p.threshold >= kMinimumSurfaceBlurThreshold &&
               p.threshold <= kMaximumSurfaceBlurThreshold;
    }
    bool operator()(const smartfilter::UnsharpMask& p) const {
        return inRange(p.amount, kMinimumUnsharpMaskAmount, kMaximumUnsharpMaskAmount) &&
               inRange(p.radius, kMinimumGaussianRadius, kMaximumGaussianRadius) && p.threshold >= kMinimumUnsharpMaskThreshold &&
               p.threshold <= kMaximumUnsharpMaskThreshold;
    }
    bool operator()(const smartfilter::MotionBlur& p) const {
        return p.angle >= kMinimumMotionBlurAngle && p.angle <= kMaximumMotionBlurAngle && p.distance >= kMinimumMotionBlurDistance &&
               p.distance <= kMaximumMotionBlurDistance;
    }
    bool operator()(const smartfilter::PlasticWrap& p) const {
        return p.highlight >= kMinimumPlasticWrapHighlightStrength && p.highlight <= kMaximumPlasticWrapHighlightStrength &&
               p.detail >= kMinimumPlasticWrapDetail && p.detail <= kMaximumPlasticWrapDetail &&
               p.smoothness >= kMinimumPlasticWrapSmoothness && p.smoothness <= kMaximumPlasticWrapSmoothness;
    }
    bool operator()(const smartfilter::Mosaic& p) const { return p.cellSize >= kMinimumMosaicCellSize && p.cellSize <= kMaximumMosaicCellSize; }
    bool operator()(const smartfilter::Emboss& p) const {
        return p.angle >= kMinimumEmbossAngle && p.angle <= kMaximumEmbossAngle && p.height >= kMinimumEmbossHeight &&
               p.height <= kMaximumEmbossHeight && p.amount >= kMinimumEmbossAmount && p.amount <= kMaximumEmbossAmount;
    }
    bool operator()(const smartfilter::BoxBlur& p) const { return inRange(p.radius, kMinimumBoxBlurRadius, kMaximumBoxBlurRadius); }
    bool operator()(const smartfilter::RadialBlur& p) const {
        return p.amount >= kMinimumRadialBlurAmount && p.amount <= kMaximumRadialBlurAmount &&
               (p.samples == 8 || p.samples == 16 || p.samples == 32);
    }
    bool operator()(const smartfilter::AddNoise& p) const {
        return inRange(p.amount, kMinimumAddNoiseAmount, kMaximumAddNoiseAmount) && p.seed >= kMinimumAddNoiseSeed &&
               p.seed <= kMaximumAddNoiseSeed;
    }
};

struct RunEntry {
    const Result& current;
    const PixelRect& canvas;
    Result operator()(const std::monostate&) const { return current; }
    Result operator()(const smartfilter::GaussianBlur& p) const { return gaussianStep(current, canvas, p.radius); }
    Result operator()(const smartfilter::HighPass& p) const { return renderHighPass(current, p.radius); }
    Result operator()(const smartfilter::Median& p) const {
        // Photoshop's window sees the canvas past the layer's edge (transparent there), not the layer's own edge
        // repeated: a rectangle filling its layer loses its corners. The bounds stay the layer's.
        const int r = std::max(1, int(std::floor(p.radius)));
        const PixelRect grown = intersectRect({current.bounds.x - r, current.bounds.y - r, current.bounds.width + 2 * r, current.bounds.height + 2 * r}, canvas);
        if (grown.empty() || grown == current.bounds) return renderMedian(current, p.radius);
        const Result wide = renderMedian(embedInFilterCanvas(current, grown), p.radius);
        return Result{cropBuffer(wide.pixels, wide.bounds, current.bounds), current.bounds};
    }
    Result operator()(const smartfilter::DustAndScratches& p) const {
        // Median's window, so Median's edge (the canvas past the layer, transparent).
        const int r = std::max(1, int(p.radius));
        const PixelRect grown = intersectRect({current.bounds.x - r, current.bounds.y - r, current.bounds.width + 2 * r, current.bounds.height + 2 * r}, canvas);
        if (grown.empty() || grown == current.bounds) return renderDustAndScratches(current, p.radius, p.threshold);
        const Result wide = renderDustAndScratches(embedInFilterCanvas(current, grown), p.radius, p.threshold);
        return Result{cropBuffer(wide.pixels, wide.bounds, current.bounds), current.bounds};
    }
    Result operator()(const smartfilter::SurfaceBlur& p) const { return surfaceStep(current, canvas, p.radius, p.threshold); }
    Result operator()(const smartfilter::UnsharpMask& p) const {
        Result out = renderUnsharpMask(current, p.amount, p.radius, p.threshold);
        // Photoshop sharpens transparency too (a half-transparent band beside an opaque one comes out opaque): the
        // same unsharp mask run over alpha as a channel.
        // Past the layer its low-pass sees the canvas's transparency (zero), as Median's window does.
        const int r = int(std::ceil(p.radius * 3)) + 1;
        PixelRect grown = intersectRect({current.bounds.x - r, current.bounds.y - r, current.bounds.width + 2 * r, current.bounds.height + 2 * r}, canvas);
        if (grown.empty()) grown = current.bounds;
        Result alpha{Image(grown.width, grown.height), grown};
        for (int y = 0; y < grown.height; y++)
            for (int x = 0; x < grown.width; x++) {
                const uint8_t* c = sampleResult(current, grown.x + x, grown.y + y);
                uint8_t* q = alpha.pixels.pixel(x, y);
                q[0] = q[1] = q[2] = c ? c[3] : 0; q[3] = 255;
            }
        const Result sharpened = renderUnsharpMask(alpha, p.amount, p.radius, p.threshold);
        for (int y = 0; y < out.pixels.height(); y++)
            for (int x = 0; x < out.pixels.width(); x++)
                out.pixels.pixel(x, y)[3] = sampleResult(sharpened, out.bounds.x + x, out.bounds.y + y)[0];
        return out;
    }
    Result operator()(const smartfilter::MotionBlur& p) const { return motionStep(current, canvas, p.angle, p.distance); }
    Result operator()(const smartfilter::PlasticWrap& p) const { return renderPlasticWrap(current, p.highlight, p.detail, p.smoothness); }
    Result operator()(const smartfilter::Mosaic& p) const { return renderMosaic(current, p.cellSize); }
    Result operator()(const smartfilter::Emboss& p) const { return renderEmboss(current, p.angle, p.height, p.amount); }
    Result operator()(const smartfilter::BoxBlur& p) const { return boxStep(current, canvas, p.radius); }
    Result operator()(const smartfilter::RadialBlur& p) const { return radialStep(current, canvas, p.amount, p.samples); }
    Result operator()(const smartfilter::AddNoise& p) const {
        return renderAddNoise(current, p.amount, p.gaussian, p.monochromatic, p.seed);
    }
};

} // namespace

// ---- the filters -------------------------------------------------------------------------------------------
// Parameters are clamped to Photoshop's dialog ranges (Patchy refuses values outside them).

PlacedRaster smartGaussianBlur(const PlacedRaster& in, const PixelRect& canvas, double radius) {
    if (!in.image) return in;
    return toPlaced(gaussianStep(toStraight(in), canvas, std::clamp(radius, kMinimumGaussianRadius, kMaximumGaussianRadius)));
}

PlacedRaster smartHighPass(const PlacedRaster& in, double radius) {
    if (!in.image) return in;
    return toPlaced(renderHighPass(toStraight(in), std::clamp(radius, kMinimumGaussianRadius, kMaximumGaussianRadius)));
}

PlacedRaster smartMedian(const PlacedRaster& in, double radius) {
    if (!in.image) return in;
    return toPlaced(renderMedian(toStraight(in), std::clamp(radius, kMinimumMedianRadius, kMaximumMedianRadius)));
}

PlacedRaster smartDustAndScratches(const PlacedRaster& in, int32_t radius, int32_t threshold) {
    if (!in.image) return in;
    return toPlaced(renderDustAndScratches(toStraight(in),
                                           std::clamp(radius, kMinimumDustAndScratchesRadius, kMaximumDustAndScratchesRadius),
                                           std::clamp(threshold, kMinimumDustAndScratchesThreshold, kMaximumDustAndScratchesThreshold)));
}

PlacedRaster smartSurfaceBlur(const PlacedRaster& in, const PixelRect& canvas, double radius, int32_t threshold) {
    if (!in.image) return in;
    return toPlaced(surfaceStep(toStraight(in), canvas, std::clamp(radius, kMinimumSurfaceBlurRadius, kMaximumSurfaceBlurRadius),
                                std::clamp(threshold, kMinimumSurfaceBlurThreshold, kMaximumSurfaceBlurThreshold)));
}

PlacedRaster smartUnsharpMask(const PlacedRaster& in, double amount, double radius, int32_t threshold) {
    if (!in.image) return in;
    return toPlaced(renderUnsharpMask(toStraight(in), std::clamp(amount, kMinimumUnsharpMaskAmount, kMaximumUnsharpMaskAmount),
                                      std::clamp(radius, kMinimumGaussianRadius, kMaximumGaussianRadius),
                                      std::clamp(threshold, kMinimumUnsharpMaskThreshold, kMaximumUnsharpMaskThreshold)));
}

PlacedRaster smartMotionBlur(const PlacedRaster& in, const PixelRect& canvas, int32_t angle, int32_t distance) {
    if (!in.image) return in;
    return toPlaced(motionStep(toStraight(in), canvas, std::clamp(angle, kMinimumMotionBlurAngle, kMaximumMotionBlurAngle),
                               std::clamp(distance, kMinimumMotionBlurDistance, kMaximumMotionBlurDistance)));
}

PlacedRaster smartPlasticWrap(const PlacedRaster& in, int32_t highlight, int32_t detail, int32_t smoothness) {
    if (!in.image) return in;
    return toPlaced(renderPlasticWrap(toStraight(in),
                                      std::clamp(highlight, kMinimumPlasticWrapHighlightStrength, kMaximumPlasticWrapHighlightStrength),
                                      std::clamp(detail, kMinimumPlasticWrapDetail, kMaximumPlasticWrapDetail),
                                      std::clamp(smoothness, kMinimumPlasticWrapSmoothness, kMaximumPlasticWrapSmoothness)));
}

PlacedRaster smartMosaic(const PlacedRaster& in, int32_t cellSize) {
    if (!in.image) return in;
    return toPlaced(renderMosaic(toStraight(in), std::clamp(cellSize, kMinimumMosaicCellSize, kMaximumMosaicCellSize)));
}

PlacedRaster smartEmboss(const PlacedRaster& in, int32_t angle, int32_t height, int32_t amount) {
    if (!in.image) return in;
    return toPlaced(renderEmboss(toStraight(in), std::clamp(angle, kMinimumEmbossAngle, kMaximumEmbossAngle),
                                 std::clamp(height, kMinimumEmbossHeight, kMaximumEmbossHeight),
                                 std::clamp(amount, kMinimumEmbossAmount, kMaximumEmbossAmount)));
}

PlacedRaster smartBoxBlur(const PlacedRaster& in, const PixelRect& canvas, double radius) {
    if (!in.image) return in;
    return toPlaced(boxStep(toStraight(in), canvas, std::clamp(radius, kMinimumBoxBlurRadius, kMaximumBoxBlurRadius)));
}

PlacedRaster smartRadialBlur(const PlacedRaster& in, const PixelRect& canvas, int32_t amount, int32_t samples) {
    if (!in.image) return in;
    return toPlaced(radialStep(toStraight(in), canvas, std::clamp(amount, kMinimumRadialBlurAmount, kMaximumRadialBlurAmount),
                               std::clamp(samples, 4, 32)));
}

PlacedRaster smartAddNoise(const PlacedRaster& in, double amount, bool gaussian, bool monochromatic, int32_t seed) {
    if (!in.image) return in;
    return toPlaced(renderAddNoise(toStraight(in), amount, gaussian, monochromatic, seed));
}

std::optional<PlacedRaster> renderSmartFilterStack(const PlacedRaster& placed, const PixelRect& canvas, const SmartFilterStack& stack) {
    if (!stack.supported) return std::nullopt;
    for (const auto& entry : stack.entries) {
        if (std::holds_alternative<std::monostate>(entry.parameters)) return std::nullopt;
        if (!entry.enabled) continue;
        if (!std::visit(ParametersValid{}, entry.parameters) || !std::isfinite(entry.opacity) || entry.opacity < 0.0 ||
            entry.opacity > 1.0)
            return std::nullopt;
    }
    if (!stack.enabled || !placed.image || placed.image->isEmpty()) return placed;

    const auto active = std::count_if(stack.entries.begin(), stack.entries.end(),
                                      [](const SmartFilterEntry& e) { return e.enabled && e.opacity > 0.0; });
    Result current = toStraight(placed);
    if (active == 0) return toPlaced(trimTransparentResult(std::move(current)));

    const PixelRect filterCanvas = canvas.empty() ? current.bounds : canvas;
    const Result base = embedInFilterCanvas(current, filterCanvas);
    for (const auto& entry : stack.entries) {
        if (!entry.enabled || entry.opacity <= 0.0) continue;
        Result filtered = std::visit(RunEntry{current, filterCanvas}, entry.parameters);
        current = blendEntryResult(current, std::move(filtered), entry.opacity, entry.blend);
    }
    return toPlaced(applyStackMask(base, current, stack));
}

} // namespace compositor
