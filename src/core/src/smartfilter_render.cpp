// Smart Filter kernels and the stack renderer at 8 bits.
//
// A port of Patchy's src/filters/smart_filter_renderer.cpp (MIT, see src/third_party/patchy_psd/README.md), with
// the helpers it uses from filter_support.hpp, rect_utils.hpp and filter_engine.cpp's noise hash. Patchy's buffers
// are STRAIGHT RGBA8; NekoPhoto's are premultiplied, so every public entry point unpremultiplies on the way in and
// premultiplies on the way out, and the kernels in between run on straight colour exactly as Patchy's do. Progress
// reporting and cancellation are dropped; the arithmetic, the rounding and the edge rules are kept as they were.
//
// The kernels are written once for both depths (smartfilter_engine.h). This file holds what is the 8-bit side's
// own: the Gaussian line plans with their Photoshop calibrations, the parameter checks and the noise hash (shared
// with the 16-bit kernels through smartfilter_kernels.h), the byte arithmetic the engine is instantiated with
// (`Depth8`, Surface Blur's integer weights among it), and the public entry points.
//
// Patent design constraint carried over from Patchy (its docs/legal-constraints.md): Median and Dust & Scratches
// keep exactly ONE plain window histogram updated a single pixel value at a time (never merged or slid column
// histograms), and Surface Blur uses no value histogram at all (direct accumulation at small radii, per-level box
// sums at large radii).
#include "nekophoto/smartfilter.h"
#include "nekophoto/blend.h"
#include "smartfilter_engine.h"
#include "smartfilter_kernels.h"
#include "nekophoto/workcounters.h"

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

namespace nekophoto {

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
constexpr double kDirectGaussianMaximumRadius = 8.0;
constexpr int32_t kSurfaceBlurDirectMaximumRadius = 8;

/// Patchy's FilterRenderResult: straight RGBA8 pixels (stride = width * 4) placed at `bounds`.
} // namespace

// ---- Gaussian line plans (shared with the 16-bit kernels, smartfilter_kernels.h) -----------------------------

namespace smartfilter_detail {

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

} // namespace smartfilter_detail

namespace {

using namespace smartfilter_detail;

uint8_t clampLong(long value) { return uint8_t(std::clamp(value, 0L, 255L)); }

/// The engine's 8-bit instance (smartfilter_engine.h): straight RGBA8 with Patchy's byte arithmetic.
struct Depth8 {
    using Sample = uint8_t;
    using Pixels = Image;
    using Placed = PlacedRaster;
    static constexpr uint32_t one = 255;
    static constexpr double oneD = 255.0;
    static constexpr double level = 1.0;
    static uint8_t round(double value) { return roundedByte(value); }
    static uint8_t store(double value) { return clampLong(std::lround(value)); }
    static uint8_t narrow(uint8_t value) { return value; }
    static uint8_t widen(uint8_t value) { return value; }

    static uint8_t premultiplySample(uint8_t sample, uint32_t alpha) { return uint8_t((sample * alpha + 127) / 255); }
    static unsigned coverageSteps(float opacity) { return nekophoto::coverageSteps(opacity); }
    static void composite(BlendMode mode, const uint8_t* source, unsigned steps, uint8_t* destination) {
        compositePixelSteps(mode, source, steps, destination);
    }

    /// Motion Blur sums exactly in integers and rounds half up.
    using MotionSum = uint64_t;
    static uint8_t motionColour(uint64_t premultiplied, uint64_t alphaSum) {
        return alphaSum == 0U ? uint8_t(0) : uint8_t(std::min<uint64_t>(255U, (premultiplied + alphaSum / 2U) / alphaSum));
    }
    static uint8_t motionAlpha(uint64_t alphaSum, int32_t distancePixels) {
        constexpr uint64_t kSampleWeight = uint64_t(65536) * 65536;
        const auto alphaDenominator = (uint64_t(distancePixels) + 1U) * kSampleWeight;
        return uint8_t(std::min<uint64_t>(255U, (alphaSum + alphaDenominator / 2U) / alphaDenominator));
    }

    using Histogram = WindowValueHistogram<256, 4>;
    static void surfacePlane(const std::vector<uint8_t>& plane, int32_t width, int32_t height, int32_t radius, int32_t threshold, Image& output,
                             size_t channel);

    static uint8_t shade(uint8_t source, int32_t relief, int32_t shine) {
        const auto shaded = std::clamp(int(source) + relief, 0, 255);
        return uint8_t(std::clamp(shaded + ((255 - shaded) * shine + 127) / 255, 0, 255));
    }

    /// Unsharp Mask: the low-pass runs on whole levels as it is.
    static bool onByteGrid(const Image&) { return false; }
    struct Sharpen {
        double amountPercent;
        int32_t threshold;
        Sharpen(double amount, int32_t t) : amountPercent(amount), threshold(t) {}
        uint8_t operator()(uint8_t source, uint8_t lowFrequency) const {
            const auto detail = int(source) - int(lowFrequency);
            // Scale the detail first, then remove Threshold from the signed adjustment (pinned by PS 27.8).
            const auto scaledDetail = int(double(detail) * amountPercent / 100.0);
            const auto magnitude = std::abs(scaledDetail);
            const auto adjustment = magnitude <= threshold ? 0 : (scaledDetail < 0 ? -(magnitude - threshold) : magnitude - threshold);
            return uint8_t(std::clamp(int(source) + adjustment, 0, 255));
        }
    };

    /// Add Noise's deltas are whole levels.
    static uint8_t addNoise(uint8_t sample, double delta) { return clampLong(long(sample) + std::lround(delta)); }

    static bool draws(const SmartFilterParameters&) { return true; }
};

// ---- Surface Blur ------------------------------------------------------------------------------------------

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

void Depth8::surfacePlane(const std::vector<uint8_t>& plane, int32_t width, int32_t height, int32_t radius, int32_t threshold, Image& output,
                          size_t channel) {
    const auto clampX = [width](int32_t x) { return std::clamp(x, 0, width - 1); };
    const auto clampY = [height](int32_t y) { return std::clamp(y, 0, height - 1); };
    const auto weights = surfaceBlurDeltaWeights(threshold);
    const auto rowStride = size_t(width);
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

// ---- parameters ------------------------------------------------------------------------------------------

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

} // namespace

bool smartfilter_detail::parametersValid(const SmartFilterParameters& parameters) { return std::visit(ParametersValid{}, parameters); }

// ---- the filters -------------------------------------------------------------------------------------------
// Parameters are clamped to Photoshop's dialog ranges (Patchy refuses values outside them).

PlacedRaster smartGaussianBlur(const PlacedRaster& in, const PixelRect& canvas, double radius) {
    if (!in.image) return in;
    return toPlaced(gaussianStep(toStraight<Depth8>(in), canvas, std::clamp(radius, kMinimumGaussianRadius, kMaximumGaussianRadius)));
}

PlacedRaster smartHighPass(const PlacedRaster& in, double radius) {
    if (!in.image) return in;
    return toPlaced(renderHighPass(toStraight<Depth8>(in), std::clamp(radius, kMinimumGaussianRadius, kMaximumGaussianRadius)));
}

PlacedRaster smartMedian(const PlacedRaster& in, double radius) {
    if (!in.image) return in;
    return toPlaced(renderMedian(toStraight<Depth8>(in), std::clamp(radius, kMinimumMedianRadius, kMaximumMedianRadius)));
}

PlacedRaster smartDustAndScratches(const PlacedRaster& in, int32_t radius, int32_t threshold) {
    if (!in.image) return in;
    return toPlaced(renderDustAndScratches(toStraight<Depth8>(in),
                                           std::clamp(radius, kMinimumDustAndScratchesRadius, kMaximumDustAndScratchesRadius),
                                           std::clamp(threshold, kMinimumDustAndScratchesThreshold, kMaximumDustAndScratchesThreshold)));
}

PlacedRaster smartSurfaceBlur(const PlacedRaster& in, const PixelRect& canvas, double radius, int32_t threshold) {
    if (!in.image) return in;
    return toPlaced(surfaceStep(toStraight<Depth8>(in), canvas, std::clamp(radius, kMinimumSurfaceBlurRadius, kMaximumSurfaceBlurRadius),
                                std::clamp(threshold, kMinimumSurfaceBlurThreshold, kMaximumSurfaceBlurThreshold)));
}

PlacedRaster smartUnsharpMask(const PlacedRaster& in, double amount, double radius, int32_t threshold) {
    if (!in.image) return in;
    return toPlaced(renderUnsharpMask(toStraight<Depth8>(in), std::clamp(amount, kMinimumUnsharpMaskAmount, kMaximumUnsharpMaskAmount),
                                      std::clamp(radius, kMinimumGaussianRadius, kMaximumGaussianRadius),
                                      std::clamp(threshold, kMinimumUnsharpMaskThreshold, kMaximumUnsharpMaskThreshold)));
}

PlacedRaster smartMotionBlur(const PlacedRaster& in, const PixelRect& canvas, int32_t angle, int32_t distance) {
    if (!in.image) return in;
    return toPlaced(motionStep(toStraight<Depth8>(in), canvas, std::clamp(angle, kMinimumMotionBlurAngle, kMaximumMotionBlurAngle),
                               std::clamp(distance, kMinimumMotionBlurDistance, kMaximumMotionBlurDistance)));
}

PlacedRaster smartPlasticWrap(const PlacedRaster& in, int32_t highlight, int32_t detail, int32_t smoothness) {
    if (!in.image) return in;
    return toPlaced(renderPlasticWrap(toStraight<Depth8>(in),
                                      std::clamp(highlight, kMinimumPlasticWrapHighlightStrength, kMaximumPlasticWrapHighlightStrength),
                                      std::clamp(detail, kMinimumPlasticWrapDetail, kMaximumPlasticWrapDetail),
                                      std::clamp(smoothness, kMinimumPlasticWrapSmoothness, kMaximumPlasticWrapSmoothness)));
}

PlacedRaster smartMosaic(const PlacedRaster& in, int32_t cellSize) {
    if (!in.image) return in;
    return toPlaced(renderMosaic(toStraight<Depth8>(in), std::clamp(cellSize, kMinimumMosaicCellSize, kMaximumMosaicCellSize)));
}

PlacedRaster smartEmboss(const PlacedRaster& in, int32_t angle, int32_t height, int32_t amount) {
    if (!in.image) return in;
    return toPlaced(renderEmboss(toStraight<Depth8>(in), std::clamp(angle, kMinimumEmbossAngle, kMaximumEmbossAngle),
                                 std::clamp(height, kMinimumEmbossHeight, kMaximumEmbossHeight),
                                 std::clamp(amount, kMinimumEmbossAmount, kMaximumEmbossAmount)));
}

PlacedRaster smartBoxBlur(const PlacedRaster& in, const PixelRect& canvas, double radius) {
    if (!in.image) return in;
    return toPlaced(boxStep(toStraight<Depth8>(in), canvas, std::clamp(radius, kMinimumBoxBlurRadius, kMaximumBoxBlurRadius)));
}

PlacedRaster smartRadialBlur(const PlacedRaster& in, const PixelRect& canvas, int32_t amount, int32_t samples, bool zoom) {
    if (!in.image) return in;
    return toPlaced(radialStep(toStraight<Depth8>(in), canvas, std::clamp(amount, kMinimumRadialBlurAmount, kMaximumRadialBlurAmount),
                               std::clamp(samples, 4, 32), zoom));
}

PlacedRaster smartAddNoise(const PlacedRaster& in, double amount, bool gaussian, bool monochromatic, int32_t seed) {
    if (!in.image) return in;
    return toPlaced(renderAddNoise(toStraight<Depth8>(in), amount, gaussian, monochromatic, seed));
}

std::optional<PlacedRaster> renderSmartFilterStack(const PlacedRaster& placed, const PixelRect& canvas, const SmartFilterStack& stack) {
    return renderStack<Depth8>(placed, canvas, stack);
}

} // namespace nekophoto