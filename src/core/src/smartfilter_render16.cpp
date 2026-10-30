// Smart Filter kernels at 16 bits: the thirteen filters of smartfilter_render.cpp on 15-bit straight colour
// (0..32768, Photoshop's range), for smart objects in 16-bit documents.
//
// Each kernel is the 8-bit one with its byte arithmetic carried over to 15 bits: the same Gaussian line plans (the
// Photoshop captures are shared, smartfilter_kernels.h), the same edge rules, sampling and blending, with every
// rounding to a byte becoming a rounding to the 15-bit grid, and every level-valued constant (High Pass's middle grey,
// thresholds, Surface Blur's weight triangle, Add Noise's amplitude) scaled by one 8-bit level, 32768 / 255. On an
// 8-bit-sourced image each agrees with its 8-bit twin to within the 8-bit twin's own rounding: docs/smart-objects.md
// gives the measured figures (smartfilter_tests). The kernels are the 8-bit file's own code (smartfilter_engine.h),
// instantiated here over `Depth16`; what this file holds is the arithmetic that differs at 15 bits.
//
// Two kernels decide on something coarser than a sample and keep that decision at 8-bit precision, as Photoshop's
// 8-bit look defines it: Plastic Wrap finds its relief and highlight from the luminance rounded to 8 bits and shades
// the 16-bit colour with them; thresholds (Dust & Scratches, Unsharp Mask) compare against the midpoint between two
// 8-bit levels, so a difference an 8-bit image calls "over the threshold" is over it here too.
//
// The patent design constraint of the 8-bit file holds here as well: Median and Dust & Scratches keep exactly ONE
// plain window histogram (32,769 bins, with its running group counts) updated a single pixel value at a time, and
// Surface Blur uses no value histogram (direct accumulation at small radii, per-level box sums at large radii, the
// levels being the 256 8-bit ones with each pixel's result interpolated between the two about its value).
#include "nekophoto/smartfilter.h"
#include "nekophoto/blend.h"
#include "nekophoto/depth.h"
#include "smartfilter_engine.h"
#include "smartfilter_kernels.h"
#include "nekophoto/workcounters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace nekophoto {

namespace {

using namespace smartfilter_detail;

constexpr uint32_t kOne = 32768;
constexpr double kOneD = 32768.0;
/// One 8-bit level in 15-bit units.
constexpr double kLevel = kOneD / 255.0;

uint16_t q16(double value) {
    if (!(value > 0.0)) return 0;
    if (value >= kOneD) return uint16_t(kOne);
    return uint16_t(std::floor(value + 0.5));
}

/// Whether `image` is 8-bit colour widened (as when an 8-bit document is converted): every colour sample on the 8-bit
/// grid, give or take what unpremultiplying at 16 bits moved it (half a 15-bit step over the alpha, so more where the
/// pixel is nearly transparent). Then Unsharp Mask's low-pass takes the 8-bit kernel's byte arithmetic (below).
bool colourOnEightBitGrid(const Image16& image) {
    for (int32_t y = 0; y < image.height(); ++y) {
        const uint16_t* row = image.row(y);
        for (int32_t x = 0; x < image.width(); ++x) {
            const uint16_t* p = row + size_t(x) * 4U;
            if (p[3] == 0) continue;
            const int tolerance = int(std::ceil(16384.0 / double(p[3]))) + 1;
            for (size_t c = 0; c < 3U; ++c)
                if (std::abs(int(widen8(narrow16(p[c]))) - int(p[c])) > tolerance) return false;
        }
    }
    return true;
}

/// The engine's 16-bit instance (smartfilter_engine.h): straight 15-bit RGBA.
struct Depth16 {
    using Sample = uint16_t;
    using Pixels = Image16;
    using Placed = PlacedRaster16;
    static constexpr uint32_t one = kOne;
    static constexpr double oneD = kOneD;
    static constexpr double level = kLevel;
    static uint16_t round(double value) { return q16(value); }
    static uint16_t store(double value) { return q16(value); }
    static uint8_t narrow(uint16_t value) { return narrow16(value); }
    static uint16_t widen(uint8_t value) { return widen8(value); }

    static uint16_t premultiplySample(uint16_t sample, uint32_t alpha) { return uint16_t(mul15(sample, alpha)); }
    static unsigned coverageSteps(float opacity) { return coverageSteps16(opacity); }
    static void composite(BlendMode mode, const uint16_t* source, unsigned steps, uint16_t* destination) {
        compositePixelSteps16(mode, source, steps, destination);
    }

    /// Motion Blur sums in doubles and rounds to the 15-bit grid.
    using MotionSum = double;
    static uint16_t motionColour(double premultiplied, double alphaSum) { return alphaSum <= 0 ? uint16_t(0) : q16(premultiplied / alphaSum); }
    static uint16_t motionAlpha(double alphaSum, int32_t distancePixels) {
        constexpr double kSampleWeight = double(65536) * double(65536);
        return q16(alphaSum / ((double(distancePixels) + 1.0) * kSampleWeight));
    }

    /// One plain window histogram over every 15-bit value, with running counts per group of 128 values to find a rank.
    using Histogram = WindowValueHistogram<size_t(kOne) + 1U, 7>;
    static void surfacePlane(const std::vector<uint16_t>& plane, int32_t width, int32_t height, int32_t radius, int32_t threshold, Image16& output,
                             size_t channel);

    /// Plastic Wrap's shading on the 16-bit colour (the relief and the shine come from the 8-bit height field).
    static uint16_t shade(uint16_t source, int32_t relief, int32_t shine) {
        const double shaded = std::clamp(double(source) + relief * kLevel, 0.0, kOneD);
        return q16(shaded + (kOneD - shaded) * shine / 255.0);
    }

    // The 8-bit kernel rounds its low-pass to whole levels (after each pass), and the amount multiplies that rounding:
    // at 150% half a level of it is most of a level in the result, and at 400% two. So a 16-bit low-pass, however exact,
    // lands two to four levels from the 8-bit look on 8-bit-sourced pixels. On colour that lies on the 8-bit grid the
    // low-pass therefore runs in 8-bit levels, as the 8-bit kernel's does, and only the detail's scaling and threshold
    // stay continuous; off the grid (16-bit pixels proper) it is exact at 15 bits, with no byte steps for the amount
    // to magnify.
    static bool onByteGrid(const Image16& image) { return colourOnEightBitGrid(image); }
    struct Sharpen {
        double amountPercent;
        // The 8-bit kernel keeps a scaled detail of more than `threshold` whole levels (it truncates, so from
        // threshold + 1 up) and removes the threshold from it; here the cut sits halfway, at threshold + 0.5, and the
        // adjustment starts from nothing there.
        double cut, removed;
        Sharpen(double amount, int32_t threshold)
            : amountPercent(amount), cut((double(threshold) + 0.5) * kLevel), removed((double(threshold) + 0.5) * kLevel) {}
        uint16_t operator()(uint16_t source, uint16_t lowFrequency) const {
            const double detail = double(source) - double(lowFrequency);
            const double scaled = detail * amountPercent / 100.0;
            const double magnitude = std::abs(scaled);
            const double adjustment = magnitude <= cut ? 0.0 : (scaled < 0 ? -(magnitude - removed) : magnitude - removed);
            return q16(double(source) + adjustment);
        }
    };

    /// Add Noise: the 8-bit amplitude carried to 15 bits, the deltas not rounded to whole levels.
    static uint16_t addNoise(uint16_t sample, double delta) { return q16(double(sample) + delta); }

    static bool draws(const SmartFilterParameters& parameters) { return smartFilterDrawsAt16(parameters); }
};

// ---- Surface Blur ------------------------------------------------------------------------------------------

/// Surface Blur's range weight, the 8-bit triangle 5 * threshold - 2 * |d| in levels, times 255 * 255 / 32768 so
/// that it is a whole number for any 15-bit difference: 5 * threshold * 32768 - 510 * |d|, kept while positive.
int64_t surfaceWeight(int32_t threshold, int64_t twiceScaledDelta) {
    const int64_t w = int64_t(5) * threshold * int64_t(kOne) - std::abs(twiceScaledDelta);
    return w > 0 ? w : 0;
}

uint16_t roundedAverage(int64_t weightSum, int64_t weightedSum) {
    if (weightSum <= 0 || weightedSum < 0) return 0;
    return q16(double(weightedSum) / double(weightSum));
}

void Depth16::surfacePlane(const std::vector<uint16_t>& plane, int32_t width, int32_t height, int32_t radius, int32_t threshold, Image16& output,
                           size_t channel) {
    const auto clampX = [width](int32_t x) { return std::clamp(x, 0, width - 1); };
    const auto clampY = [height](int32_t y) { return std::clamp(y, 0, height - 1); };
    const auto rowStride = size_t(width);
    const auto planeRow = [&plane, rowStride](int32_t y) { return plane.data() + size_t(y) * rowStride; };

    if (radius <= surfaceBlurDirectMaximumRadius) {
        // |d| in 15-bit units, times 510: the weight's whole-number form.
        std::vector<int64_t> weights(size_t(kOne) + 1U);
        for (size_t d = 0; d < weights.size(); ++d) weights[d] = surfaceWeight(threshold, int64_t(d) * 510);
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
                        weightedSum += weight * value;
                    }
                }
                output.pixel(x, y)[channel] = roundedAverage(weightSum, weightedSum);
            }
        }
        return;
    }

    // Large radii: box sums per 8-bit level k taken as the centre value (k * 32768 / 255, whose weights are whole
    // numbers: 510 * (v - k * 32768 / 255) = 510 * v - 65536 * k), each pixel's result interpolated between the two
    // levels about its own value.
    std::vector<double> accumulated(rowStride * size_t(height), 0.0);
    std::array<bool, 256> needed{};
    const auto levelOf = [](uint16_t value, double& fraction) {
        const double position = double(value) / kLevel;
        const int k = std::min(254, int(std::floor(position)));
        fraction = std::clamp(position - k, 0.0, 1.0);
        return k;
    };
    for (const auto value : plane) {
        double t = 0;
        const int k = levelOf(value, t);
        needed[size_t(k)] = true;
        needed[size_t(k + 1)] = true;
    }
    std::vector<int64_t> columnWeight(static_cast<size_t>(width)), columnWeighted(static_cast<size_t>(width));
    std::vector<int64_t> weightLut(size_t(kOne) + 1U), weightedLut(size_t(kOne) + 1U);
    for (int32_t level = 0; level < 256; ++level) {
        if (!needed[size_t(level)]) continue;
        for (size_t value = 0; value < weightLut.size(); ++value) {
            weightLut[value] = surfaceWeight(threshold, int64_t(value) * 510 - int64_t(level) * 65536);
            weightedLut[value] = weightLut[value] * int64_t(value);
        }
        std::fill(columnWeight.begin(), columnWeight.end(), 0);
        std::fill(columnWeighted.begin(), columnWeighted.end(), 0);
        for (int32_t dy = -radius; dy <= radius; ++dy) {
            const auto* row = planeRow(clampY(dy));
            for (int32_t x = 0; x < width; ++x) {
                columnWeight[size_t(x)] += weightLut[row[size_t(x)]];
                columnWeighted[size_t(x)] += weightedLut[row[size_t(x)]];
            }
        }
        for (int32_t y = 0; y < height; ++y) {
            if (y > 0) {
                const auto* leaving = planeRow(clampY(y - 1 - radius));
                const auto* entering = planeRow(clampY(y + radius));
                for (int32_t x = 0; x < width; ++x) {
                    columnWeight[size_t(x)] += weightLut[entering[size_t(x)]] - weightLut[leaving[size_t(x)]];
                    columnWeighted[size_t(x)] += weightedLut[entering[size_t(x)]] - weightedLut[leaving[size_t(x)]];
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
                    windowWeight += columnWeight[enteringX] - columnWeight[leavingX];
                    windowWeighted += columnWeighted[enteringX] - columnWeighted[leavingX];
                }
                double t = 0;
                const int k = levelOf(centerRow[size_t(x)], t);
                if (k != level && k + 1 != level) continue;
                const double share = k == level ? 1.0 - t : t;
                if (share <= 0) continue;
                const double average = windowWeight > 0 ? double(windowWeighted) / double(windowWeight) : 0.0;
                accumulated[size_t(y) * rowStride + size_t(x)] += share * average;
            }
        }
    }
    for (int32_t y = 0; y < height; ++y)
        for (int32_t x = 0; x < width; ++x) output.pixel(x, y)[channel] = q16(accumulated[size_t(y) * rowStride + size_t(x)]);
}

} // namespace

// Every filter NekoPhoto draws as a Smart Filter is drawn at 16 bits.
bool smartFilterDrawsAt16(const SmartFilterParameters& parameters) {
    return !std::holds_alternative<std::monostate>(parameters);
}

std::optional<PlacedRaster16> renderSmartFilterStack(const PlacedRaster16& placed, const PixelRect& canvas, const SmartFilterStack& stack) {
    return renderStack<Depth16>(placed, canvas, stack);
}

} // namespace nekophoto
