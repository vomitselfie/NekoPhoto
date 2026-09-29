// Smart Filter kernels at 16 bits: the thirteen filters of smartfilter_render.cpp on 15-bit straight colour
// (0..32768, Photoshop's range), for smart objects in 16-bit documents.
//
// Each kernel is the 8-bit one with its byte arithmetic carried over to 15 bits: the same Gaussian line plans (the
// Photoshop captures are shared, smartfilter_kernels.h), the same edge rules, sampling and blending, with every
// rounding to a byte becoming a rounding to the 15-bit grid, and every level-valued constant (High Pass's middle grey,
// thresholds, Surface Blur's weight triangle, Add Noise's amplitude) scaled by one 8-bit level, 32768 / 255. On an
// 8-bit-sourced image each agrees with its 8-bit twin to within the 8-bit twin's own rounding: docs/smart-objects.md
// gives the measured figures (smartfilter_tests).
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
#include "compositor/smartfilter.h"
#include "compositor/blend.h"
#include "compositor/depth.h"
#include "smartfilter_kernels.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace compositor {

namespace {

using namespace smartfilter_detail;

constexpr uint32_t kOne = 32768;
constexpr double kOneD = 32768.0;
/// One 8-bit level in 15-bit units.
constexpr double kLevel = kOneD / 255.0;

/// Straight 15-bit RGBA placed at `bounds`.
struct Result16 {
    Image16 pixels;
    PixelRect bounds;
};

bool pixelsEmpty(const Result16& r) { return r.pixels.isEmpty(); }

uint16_t q16(double value) {
    if (!(value > 0.0)) return 0;
    if (value >= kOneD) return uint16_t(kOne);
    return uint16_t(std::floor(value + 0.5));
}

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

const uint16_t* sampleResult(const Result16& result, int32_t documentX, int32_t documentY) {
    const auto localX = int64_t(documentX) - result.bounds.x;
    const auto localY = int64_t(documentY) - result.bounds.y;
    if (localX < 0 || localY < 0 || localX >= result.bounds.width || localY >= result.bounds.height) return nullptr;
    return result.pixels.pixel(int32_t(localX), int32_t(localY));
}

Image16 cropBuffer(const Image16& source, PixelRect sourceBounds, PixelRect cropBounds) {
    Image16 cropped(cropBounds.width, cropBounds.height);
    if (cropped.isEmpty()) return cropped;
    const auto sx = cropBounds.x - sourceBounds.x, sy = cropBounds.y - sourceBounds.y;
    const size_t samples = size_t(cropBounds.width) * 4U;
    for (int32_t y = 0; y < cropBounds.height; ++y) {
        const auto* row = source.pixel(sx, sy + y);
        std::copy(row, row + samples, cropped.pixel(0, y));
    }
    return cropped;
}

Result16 embedInFilterCanvas(const Result16& placed, PixelRect canvasBounds) {
    if (canvasBounds.empty() || placed.bounds == canvasBounds) return placed;
    Image16 canvas(canvasBounds.width, canvasBounds.height);
    if (canvas.isEmpty()) return placed;
    const auto copied = intersectRect(placed.bounds, canvasBounds);
    if (!copied.empty()) {
        const int sx = copied.x - placed.bounds.x, sy = copied.y - placed.bounds.y;
        const int dx = copied.x - canvasBounds.x, dy = copied.y - canvasBounds.y;
        const size_t samples = size_t(copied.width) * 4U;
        for (int32_t y = 0; y < copied.height; ++y) {
            const auto* source = placed.pixels.pixel(sx, sy + y);
            std::copy(source, source + samples, canvas.pixel(dx, dy + y));
        }
    }
    return Result16{std::move(canvas), canvasBounds};
}

Result16 trimTransparentResult(Result16 result) {
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
    return Result16{cropBuffer(result.pixels, result.bounds, cropped), cropped};
}

// ---- Gaussian, High Pass, Unsharp Mask ---------------------------------------------------------------------

Result16 renderGaussian(const Result16& input, double radius) {
    const auto margin = int(std::ceil(gaussianMarginScale * radius));
    const auto bounds = input.bounds;
    const auto plan = makeGaussianLinePlan(radius, margin);
    Image16 output(bounds.width, bounds.height);
    const auto width = bounds.width, height = bounds.height;
    std::vector<float> horizontal(size_t(width) * size_t(height));
    std::vector<double> values, scratch;
    const std::array<int, 4> channels{3, 0, 1, 2};
    for (const auto channel : channels) {
        values.resize(size_t(width));
        scratch.resize(size_t(width));
        for (int32_t y = 0; y < height; ++y) {
            const uint16_t* row = input.pixels.row(y);
            for (int32_t x = 0; x < width; ++x) {
                const uint16_t* p = row + size_t(x) * 4U;
                values[size_t(x)] = channel == 3 ? double(p[3]) : double(p[channel]) * double(p[3]) / kOneD;
            }
            filterGaussianLine(values, scratch, plan);
            const auto rowOffset = size_t(y) * size_t(width);
            for (int32_t x = 0; x < width; ++x) horizontal[rowOffset + size_t(x)] = float(q16(values[size_t(x)]));
        }
        values.resize(size_t(height));
        scratch.resize(size_t(height));
        for (int32_t x = 0; x < width; ++x) {
            for (int32_t y = 0; y < height; ++y) values[size_t(y)] = horizontal[size_t(y) * size_t(width) + size_t(x)];
            filterGaussianLine(values, scratch, plan);
            for (int32_t y = 0; y < height; ++y) {
                uint16_t* out = output.pixel(x, y);
                if (channel == 3) {
                    out[3] = q16(values[size_t(y)]);
                    continue;
                }
                const auto alpha = out[3];
                const auto premultiplied = q16(values[size_t(y)]);
                out[channel] = alpha == 0U ? uint16_t(0) : q16(double(premultiplied) * kOneD / double(alpha));
            }
        }
    }
    return Result16{std::move(output), bounds};
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

uint8_t roundedByte(double value) {
    if (!(value > 0.0)) return 0;
    if (value >= 255.0) return 255;
    return uint8_t(std::floor(value + 0.5));
}

/// `byteGrid`: the input is on the 8-bit grid and both passes run in 8-bit levels, rounding to whole levels as the 8-bit
/// kernel does, so the low-pass is the 8-bit one exactly (widened).
Result16 renderStraightGaussian(const Result16& input, double radius, bool unsharpKernel = false, bool byteGrid = false) {
    const auto margin = int(std::ceil(gaussianMarginScale * radius));
    const auto plan = unsharpKernel ? makeUnsharpLinePlan(radius, margin) : makeHighPassLinePlan(radius, margin);
    Image16 output = input.pixels;
    const auto width = input.bounds.width, height = input.bounds.height;
    std::vector<float> horizontal(size_t(width) * size_t(height));
    std::vector<double> values, scratch;
    for (size_t channel = 0; channel < 3U; ++channel) {
        values.resize(size_t(width));
        scratch.resize(size_t(width));
        for (int32_t y = 0; y < height; ++y) {
            for (int32_t x = 0; x < width; ++x) {
                const uint16_t v = input.pixels.pixel(x, y)[channel];
                values[size_t(x)] = byteGrid ? double(narrow16(v)) : double(v);
            }
            filterGaussianLine(values, scratch, plan);
            const auto rowOffset = size_t(y) * size_t(width);
            for (int32_t x = 0; x < width; ++x)
                horizontal[rowOffset + size_t(x)] = byteGrid ? float(roundedByte(values[size_t(x)])) : float(q16(values[size_t(x)]));
        }
        values.resize(size_t(height));
        scratch.resize(size_t(height));
        for (int32_t x = 0; x < width; ++x) {
            for (int32_t y = 0; y < height; ++y) values[size_t(y)] = horizontal[size_t(y) * size_t(width) + size_t(x)];
            filterGaussianLine(values, scratch, plan);
            for (int32_t y = 0; y < height; ++y)
                output.pixel(x, y)[channel] = byteGrid ? widen8(roundedByte(values[size_t(y)])) : q16(values[size_t(y)]);
        }
    }
    return Result16{std::move(output), input.bounds};
}

// ---- transparent colour extension --------------------------------------------------------------------------

struct TransparentColorExtension {
    bool allTransparent = false;
    std::vector<uint32_t> nearestVisible;
};

uint16_t extendedStraightColorSample(const Result16& input, const TransparentColorExtension& extension, int32_t x, int32_t y, size_t channel) {
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

/// The 8-bit file's nearest-visible-pixel map, unchanged (it depends on alpha being zero or not only).
TransparentColorExtension extendTransparentColors(const Result16& input) {
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
    std::vector<uint32_t> rowSourceX(static_cast<size_t>(height));
    std::vector<int32_t> envelopeRows(static_cast<size_t>(height));
    std::vector<int64_t> envelopeStarts(static_cast<size_t>(height));
    for (int32_t x = 0; x < width; ++x) {
        for (int32_t y = 0; y < height; ++y) rowSourceX[size_t(y)] = extension.nearestVisible[size_t(y) * size_t(width) + size_t(x)];
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
        if (envelopeSize == 0) continue;
        int32_t selected = 0;
        for (int32_t y = 0; y < height; ++y) {
            while (selected + 1 < envelopeSize && envelopeStarts[size_t(selected + 1)] <= y) ++selected;
            const auto sourceY = envelopeRows[size_t(selected)];
            const auto sourceX = rowSourceX[size_t(sourceY)];
            extension.nearestVisible[size_t(y) * size_t(width) + size_t(x)] = uint32_t(uint64_t(sourceY) * uint64_t(width) + sourceX);
        }
    }
    return extension;
}

Result16 withHiddenColoursExtended(const Result16& input) {
    const auto extension = extendTransparentColors(input);
    if (extension.allTransparent || extension.nearestVisible.empty()) return input;
    Result16 out = input;
    for (int32_t y = 0; y < input.bounds.height; ++y)
        for (int32_t x = 0; x < input.bounds.width; ++x) {
            auto* p = out.pixels.pixel(x, y);
            if (p[3] != 0) continue;
            for (size_t c = 0; c < 3; ++c) p[c] = extendedStraightColorSample(input, extension, x, y, c);
        }
    return out;
}

Result16 renderHighPass(const Result16& input, double radius) {
    const auto blurred = renderStraightGaussian(withHiddenColoursExtended(input), radius);
    Image16 output(input.bounds.width, input.bounds.height);
    constexpr double middle = 128.0 * kLevel;
    for (int32_t y = 0; y < input.bounds.height; ++y)
        for (int32_t x = 0; x < input.bounds.width; ++x) {
            const auto* source = input.pixels.pixel(x, y);
            const auto* lowFrequency = blurred.pixels.pixel(x, y);
            auto* destination = output.pixel(x, y);
            for (size_t c = 0; c < 3U; ++c) destination[c] = q16(double(source[c]) - double(lowFrequency[c]) + middle);
            destination[3] = source[3];
        }
    return Result16{std::move(output), input.bounds};
}

// The 8-bit kernel rounds its low-pass to whole levels (after each pass), and the amount multiplies that rounding: at
// 150% half a level of it is most of a level in the result, and at 400% two. So a 16-bit low-pass, however exact, lands
// two to four levels from the 8-bit look on 8-bit-sourced pixels. On colour that lies on the 8-bit grid the low-pass
// therefore runs in 8-bit levels, as the 8-bit kernel's does, and only the detail's scaling and threshold stay
// continuous; off the grid (16-bit pixels proper) it is exact at 15 bits, with no byte steps for the amount to magnify.
Result16 renderUnsharpMask(const Result16& input, double amountPercent, double radius, int32_t threshold) {
    const Result16 extended = withHiddenColoursExtended(input);
    const auto blurred = renderStraightGaussian(extended, radius, true, colourOnEightBitGrid(extended.pixels));
    Image16 output(input.bounds.width, input.bounds.height);
    // The 8-bit kernel keeps a scaled detail of more than `threshold` whole levels (it truncates, so from threshold + 1
    // up) and removes the threshold from it; here the cut sits halfway, at threshold + 0.5, and the adjustment starts
    // from nothing there.
    const double cut = (double(threshold) + 0.5) * kLevel;
    const double removed = (double(threshold) + 0.5) * kLevel;
    for (int32_t y = 0; y < input.bounds.height; ++y)
        for (int32_t x = 0; x < input.bounds.width; ++x) {
            const auto* source = input.pixels.pixel(x, y);
            const auto* lowFrequency = blurred.pixels.pixel(x, y);
            auto* destination = output.pixel(x, y);
            for (size_t c = 0; c < 3U; ++c) {
                const double detail = double(source[c]) - double(lowFrequency[c]);
                const double scaled = detail * amountPercent / 100.0;
                const double magnitude = std::abs(scaled);
                const double adjustment = magnitude <= cut ? 0.0 : (scaled < 0 ? -(magnitude - removed) : magnitude - removed);
                destination[c] = q16(double(source[c]) + adjustment);
            }
            destination[3] = source[3];
        }
    return Result16{std::move(output), input.bounds};
}

// ---- Motion Blur -------------------------------------------------------------------------------------------

Result16 renderMotionBlur(const Result16& input, int32_t angleDegrees, int32_t distancePixels) {
    constexpr int64_t kCoordinateScale = 65536;
    constexpr double kSampleWeight = double(kCoordinateScale) * double(kCoordinateScale);
    constexpr double kPi = 3.14159265358979323846;
    const auto radians = double(angleDegrees) * kPi / 180.0;
    const auto stepX = int64_t(std::llround(std::cos(radians) * kCoordinateScale));
    const auto stepY = int64_t(std::llround(-std::sin(radians) * kCoordinateScale));
    const auto firstSample = -distancePixels / 2;
    const auto lastSample = firstSample + distancePixels;
    const auto sampleCount = double(distancePixels) + 1.0;
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto maximumX = int64_t(std::max(0, width - 1)) * kCoordinateScale;
    const auto maximumY = int64_t(std::max(0, height - 1)) * kCoordinateScale;
    Image16 output(width, height);
    for (int32_t y = 0; y < height; ++y)
        for (int32_t x = 0; x < width; ++x) {
            std::array<double, 3> premultiplied{};
            double alphaSum = 0;
            for (auto sample = firstSample; sample <= lastSample; ++sample) {
                const auto sampleX = std::clamp<int64_t>(int64_t(x) * kCoordinateScale + int64_t(sample) * stepX, 0, maximumX);
                const auto sampleY = std::clamp<int64_t>(int64_t(y) * kCoordinateScale + int64_t(sample) * stepY, 0, maximumY);
                const auto x0 = int32_t(sampleX / kCoordinateScale);
                const auto y0 = int32_t(sampleY / kCoordinateScale);
                const auto x1 = std::min(width - 1, x0 + 1);
                const auto y1 = std::min(height - 1, y0 + 1);
                const auto fractionX = double(sampleX % kCoordinateScale);
                const auto fractionY = double(sampleY % kCoordinateScale);
                const double scale = double(kCoordinateScale);
                const std::array<double, 4> weights{(scale - fractionX) * (scale - fractionY), fractionX * (scale - fractionY),
                                                    (scale - fractionX) * fractionY, fractionX * fractionY};
                const std::array<const uint16_t*, 4> pixels{input.pixels.pixel(x0, y0), input.pixels.pixel(x1, y0),
                                                            input.pixels.pixel(x0, y1), input.pixels.pixel(x1, y1)};
                for (size_t corner = 0; corner < pixels.size(); ++corner) {
                    const double alphaWeight = double(pixels[corner][3]) * weights[corner];
                    alphaSum += alphaWeight;
                    for (size_t c = 0; c < 3U; ++c) premultiplied[c] += double(pixels[corner][c]) * alphaWeight;
                }
            }
            auto* destination = output.pixel(x, y);
            for (size_t c = 0; c < 3U; ++c) destination[c] = alphaSum <= 0 ? uint16_t(0) : q16(premultiplied[c] / alphaSum);
            destination[3] = q16(alphaSum / (sampleCount * kSampleWeight));
        }
    return Result16{std::move(output), input.bounds};
}

// ---- Median, Dust & Scratches, Surface Blur ----------------------------------------------------------------

/// One plain window histogram over every 15-bit value, with running counts per group of 128 values to find a rank.
struct WindowValueHistogram {
    std::vector<uint32_t> bins = std::vector<uint32_t>(size_t(kOne) + 1U, 0U);
    std::array<uint32_t, (kOne >> 7U) + 1U> coarse{};
    void add(uint16_t value) {
        ++bins[value];
        ++coarse[size_t(value >> 7U)];
    }
    void remove(uint16_t value) {
        --bins[value];
        --coarse[size_t(value >> 7U)];
    }
    uint16_t median(uint32_t rank) const {
        size_t group = 0;
        while (group + 1U < coarse.size() && rank > coarse[group]) {
            rank -= coarse[group];
            ++group;
        }
        const auto first = group * 128U;
        size_t within = 0;
        while (within + 1U < 128U && first + within + 1U < bins.size() && rank > bins[first + within]) {
            rank -= bins[first + within];
            ++within;
        }
        return uint16_t(first + within);
    }
};

template <typename Sample>
void filterSquareMedianChannel(const Result16& input, Image16& output, size_t channel, int32_t radius, Sample&& sample) {
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto diameter = radius * 2 + 1;
    const auto windowArea = uint32_t(diameter) * uint32_t(diameter);
    const auto medianRank = windowArea / 2U + 1U;
    const auto clampX = [width](int32_t x) { return std::clamp(x, 0, width - 1); };
    const auto clampY = [height](int32_t y) { return std::clamp(y, 0, height - 1); };
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

template <typename Sample>
void filterSquareSurfaceChannel(const Result16& input, Image16& output, size_t channel, int32_t radius, int32_t threshold, Sample&& sample) {
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto clampX = [width](int32_t x) { return std::clamp(x, 0, width - 1); };
    const auto clampY = [height](int32_t y) { return std::clamp(y, 0, height - 1); };
    const auto rowStride = size_t(width);
    std::vector<uint16_t> plane(rowStride * size_t(height));
    for (int32_t y = 0; y < height; ++y)
        for (int32_t x = 0; x < width; ++x) plane[size_t(y) * rowStride + size_t(x)] = sample(x, y);
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

Result16 renderMedian(const Result16& input, double radius) {
    if (pixelsEmpty(input)) return input;
    const auto effectiveRadius = std::max(1, int32_t(std::floor(radius)));
    const auto extension = extendTransparentColors(input);
    Image16 output = input.pixels;
    filterSquareMedianChannel(input, output, 3U, effectiveRadius, [&input](int32_t x, int32_t y) { return input.pixels.pixel(x, y)[3]; });
    if (extension.allTransparent) return Result16{std::move(output), input.bounds};
    for (size_t channel = 0; channel < 3U; ++channel)
        filterSquareMedianChannel(input, output, channel, effectiveRadius, [&input, &extension, channel](int32_t x, int32_t y) {
            return extendedStraightColorSample(input, extension, x, y, channel);
        });
    return Result16{std::move(output), input.bounds};
}

Result16 renderSurfaceBlur(const Result16& input, double radius, int32_t threshold) {
    if (pixelsEmpty(input)) return input;
    const auto effectiveRadius = std::max(1, int32_t(std::floor(radius + 0.5)));
    const auto extension = extendTransparentColors(input);
    Image16 output = input.pixels;
    filterSquareSurfaceChannel(input, output, 3U, effectiveRadius, threshold, [&input](int32_t x, int32_t y) { return input.pixels.pixel(x, y)[3]; });
    if (extension.allTransparent) return Result16{std::move(output), input.bounds};
    for (size_t channel = 0; channel < 3U; ++channel)
        filterSquareSurfaceChannel(input, output, channel, effectiveRadius, threshold, [&input, &extension, channel](int32_t x, int32_t y) {
            return extendedStraightColorSample(input, extension, x, y, channel);
        });
    return Result16{std::move(output), input.bounds};
}

Result16 renderDustAndScratches(const Result16& input, int32_t radius, int32_t threshold) {
    if (pixelsEmpty(input)) return input;
    const auto extension = extendTransparentColors(input);
    if (extension.allTransparent) return input;
    Image16 output = input.pixels;
    for (size_t channel = 0; channel < 3U; ++channel)
        filterSquareMedianChannel(input, output, channel, radius, [&input, &extension, channel](int32_t x, int32_t y) {
            return extendedStraightColorSample(input, extension, x, y, channel);
        });
    // More than `threshold` whole levels apart: past the midpoint to the next level.
    const double cut = (double(threshold) + 0.5) * kLevel;
    for (int32_t y = 0; y < input.bounds.height; ++y)
        for (int32_t x = 0; x < input.bounds.width; ++x) {
            auto* destination = output.pixel(x, y);
            const std::array<uint16_t, 3> median{destination[0], destination[1], destination[2]};
            std::array<uint16_t, 3> source{};
            int32_t difference = 0;
            for (size_t c = 0; c < source.size(); ++c) {
                source[c] = extendedStraightColorSample(input, extension, x, y, c);
                difference = std::max(difference, std::abs(int32_t(source[c]) - int32_t(median[c])));
            }
            const auto replace = double(difference) > cut;
            for (size_t c = 0; c < source.size(); ++c) destination[c] = replace ? median[c] : source[c];
            destination[3] = input.pixels.pixel(x, y)[3];
        }
    return Result16{std::move(output), input.bounds};
}

// ---- Plastic Wrap ------------------------------------------------------------------------------------------

Result16 renderPlasticWrap(const Result16& input, int32_t highlightStrength, int32_t detail, int32_t smoothness) {
    if (pixelsEmpty(input)) return input;
    const auto extension = extendTransparentColors(input);
    if (extension.allTransparent) return input;
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto count = size_t(width) * size_t(height);
    std::vector<uint16_t> luminance(count), horizontal(count), heightField(count);
    const auto indexOf = [width](int32_t x, int32_t y) { return size_t(y) * size_t(width) + size_t(x); };
    // The height field at 8-bit precision, exactly as the 8-bit kernel finds it.
    for (int32_t y = 0; y < height; ++y)
        for (int32_t x = 0; x < width; ++x) {
            const auto red = uint32_t(narrow16(extendedStraightColorSample(input, extension, x, y, 0U)));
            const auto green = uint32_t(narrow16(extendedStraightColorSample(input, extension, x, y, 1U)));
            const auto blue = uint32_t(narrow16(extendedStraightColorSample(input, extension, x, y, 2U)));
            const auto alpha = uint32_t(narrow16(input.pixels.pixel(x, y)[3]));
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
            heightField[indexOf(x, y)] = uint16_t((smoothed * uint32_t(15 - detail) + source * uint32_t(detail) + 7U) / 15U);
            const auto leaving = std::clamp(y - radius, 0, height - 1);
            const auto entering = std::clamp(y + radius + 1, 0, height - 1);
            sum += horizontal[indexOf(x, entering)];
            sum -= horizontal[indexOf(x, leaving)];
        }
    }
    Image16 output = input.pixels;
    for (int32_t y = 0; y < height; ++y)
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
            // The shading on the 16-bit colour.
            for (size_t c = 0; c < 3U; ++c) {
                const double shaded = std::clamp(double(source[c]) + relief * kLevel, 0.0, kOneD);
                destination[c] = q16(shaded + (kOneD - shaded) * shine / 255.0);
            }
            destination[3] = source[3];
        }
    return Result16{std::move(output), input.bounds};
}

// ---- Box Blur ----------------------------------------------------------------------------------------------

Result16 renderBoxBlurDirect(const Result16& input, int32_t radius) {
    const auto width = input.pixels.width(), height = input.pixels.height();
    Result16 result = input;
    const auto taps = 2 * radius + 1;
    const auto totalWeight = double(taps) * double(taps);
    const auto rowStride = size_t(width) * 4U;
    std::vector<double> hRows(rowStride * size_t(taps), 0.0);
    int hRowsBuiltThrough = -1;
    const auto buildHRow = [&](int32_t sourceY, double* out) {
        std::fill(out, out + rowStride, 0.0);
        for (int dx = -radius; dx <= radius; ++dx)
            for (int32_t x = 0; x < width; ++x) {
                const auto sx = std::clamp<int32_t>(x + dx, 0, width - 1);
                const auto* px = input.pixels.pixel(sx, sourceY);
                const auto alpha = double(px[3]) / kOneD;
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
            const auto* hRow = hRowFor(std::clamp<int32_t>(y + dy, 0, height - 1));
            for (size_t i = 0; i < rowStride; ++i) vAccum[i] += hRow[i];
        }
        for (int32_t x = 0; x < width; ++x) {
            const auto* accum = vAccum.data() + size_t(x) * 4U;
            auto* dst = result.pixels.pixel(x, y);
            const auto alphaSum = accum[3];
            for (int c = 0; c < 3; ++c) dst[c] = q16(alphaSum > 0.000001 ? accum[c] / alphaSum : 0.0);
            dst[3] = q16(alphaSum / totalWeight * kOneD);
        }
    }
    return result;
}

Result16 renderBoxBlurSliding(const Result16& input, int32_t radius) {
    const auto width = input.pixels.width(), height = input.pixels.height();
    Result16 result = input;
    const auto totalWeight = double(int64_t(2) * radius + 1) * double(int64_t(2) * radius + 1);
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
            for (int c = 0; c < 3; ++c) dst[c] = q16(alphaSum > 0 ? double(accum[c]) / double(alphaSum) : 0.0);
            dst[3] = q16(double(alphaSum) / totalWeight);
        }
        if (y + 1 < height) {
            addRow(std::clamp<int32_t>(y - radius, 0, height - 1), -1);
            addRow(std::clamp<int32_t>(y + 1 + radius, 0, height - 1), 1);
        }
    }
    return result;
}

Result16 renderBoxBlur(const Result16& input, int32_t radius) {
    if (pixelsEmpty(input)) return input;
    radius = std::clamp(radius, 1, boxBlurMaximumRadius);
    if (radius <= boxBlurDirectMaximumRadius) return renderBoxBlurDirect(input, radius);
    return renderBoxBlurSliding(input, radius);
}

// ---- Emboss ------------------------------------------------------------------------------------------------

Result16 renderEmboss(const Result16& input, int32_t angleDegrees, int32_t heightPixels, int32_t amountPercent) {
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
    const auto angle = double(angleDegrees) * kPi / 180.0;
    const auto distance = double(heightPixels) / 2.0;
    const auto offsetX = std::cos(angle) * distance;
    const auto offsetY = -std::sin(angle) * distance;
    const auto scale = double(amountPercent) / 100.0;
    constexpr double middle = 128.0 * kLevel;
    Result16 result = input;
    for (int32_t y = 0; y < h; ++y)
        for (int32_t x = 0; x < w; ++x) {
            auto* px = result.pixels.pixel(x, y);
            for (int c = 0; c < 3; c++) {
                const auto lit = sampled(double(x) + offsetX, double(y) + offsetY, c);
                const auto dark = sampled(double(x) - offsetX, double(y) - offsetY, c);
                px[c] = q16(middle + (lit - dark) * scale);
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

void radialBlurAccumulatePixel(RadialBlurAccum& accum, const uint16_t* px, double weight) {
    if (weight <= 0.0) return;
    const auto alpha = double(px[3]) / kOneD;
    accum.weight += weight;
    accum.alpha += alpha * weight;
    for (size_t c = 0; c < 3; ++c) accum.premultipliedColor[c] += double(px[c]) * alpha * weight;
}

void radialBlurAccumulateSample(RadialBlurAccum& accum, const Image16& original, double x, double y) {
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

Result16 renderRadialBlur(const Result16& input, int32_t amount, int32_t samples, double centerX, double centerY) {
    if (pixelsEmpty(input)) return input;
    constexpr double kPi = 3.14159265358979323846;
    Result16 result{Image16(input.bounds.width, input.bounds.height), input.bounds};
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
            auto* dst = result.pixels.pixel(x, y);
            const auto normalizedAlpha = accum.weight > 0.0 ? accum.alpha / accum.weight : 1.0;
            for (size_t c = 0; c < 3; ++c) dst[c] = q16(accum.alpha > 0.000001 ? accum.premultipliedColor[c] / accum.alpha : 0.0);
            dst[3] = q16(normalizedAlpha * kOneD);
        }
    return result;
}

// ---- Add Noise ---------------------------------------------------------------------------------------------

Result16 renderAddNoise(const Result16& input, double amountPercent, bool gaussian, bool monochromatic, int32_t seed) {
    if (pixelsEmpty(input)) return input;
    Result16 result = input;
    // The 8-bit amplitude in levels, carried to 15 bits (the deltas are not rounded to whole levels).
    const auto range = std::clamp(amountPercent, addNoiseMinimumAmount, addNoiseMaximumAmount) * 2.55 * kLevel;
    const auto laneBase = uint32_t(std::clamp(seed, addNoiseMinimumSeed, addNoiseMaximumSeed)) * 16U;
    const auto unitFromHash = [](uint32_t hash) { return double(hash) * (2.0 / 4294967295.0) - 1.0; };
    const auto deltaForLane = [&](int32_t x, int32_t y, uint32_t lane) {
        if (!gaussian) return unitFromHash(addNoiseHash(x, y, laneBase + lane * 4U)) * range;
        double sum = 0.0;
        for (uint32_t sample = 1; sample <= 4U; ++sample) sum += unitFromHash(addNoiseHash(x, y, laneBase + lane * 4U + sample));
        return sum * 0.5 * range;
    };
    for (int32_t y = 0; y < result.bounds.height; ++y)
        for (int32_t x = 0; x < result.bounds.width; ++x) {
            auto* px = result.pixels.pixel(x, y);
            if (monochromatic) {
                const auto delta = deltaForLane(x, y, 3U);
                for (int c = 0; c < 3; ++c) px[c] = q16(double(px[c]) + delta);
            } else {
                for (uint32_t c = 0; c < 3U; ++c) px[c] = q16(double(px[c]) + deltaForLane(x, y, c));
            }
        }
    return result;
}

// ---- Mosaic ------------------------------------------------------------------------------------------------

Result16 renderMosaic(const Result16& input, int32_t cellSizePixels) {
    if (pixelsEmpty(input)) return input;
    Result16 result{Image16(input.bounds.width, input.bounds.height), input.bounds};
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto cell = std::max<int32_t>(mosaicMinimumCellSize, cellSizePixels);
    for (int32_t blockY = 0; blockY < height; blockY += cell) {
        const auto blockHeight = std::min(cell, height - blockY);
        for (int32_t blockX = 0; blockX < width; blockX += cell) {
            const auto blockWidth = std::min(cell, width - blockX);
            double weight = 0.0, alphaSum = 0.0;
            std::array<double, 3> premultiplied{};
            for (int32_t y = blockY; y < blockY + blockHeight; ++y)
                for (int32_t x = blockX; x < blockX + blockWidth; ++x) {
                    const auto* px = input.pixels.pixel(x, y);
                    const auto alpha = double(px[3]) / kOneD;
                    weight += 1.0;
                    alphaSum += alpha;
                    for (size_t c = 0; c < 3; ++c) premultiplied[c] += double(px[c]) * alpha;
                }
            std::array<uint16_t, 4> value{};
            for (size_t c = 0; c < 3; ++c) value[c] = q16(alphaSum > 0.000001 ? premultiplied[c] / alphaSum : 0.0);
            value[3] = q16(weight > 0.0 ? alphaSum / weight * kOneD : 0.0);
            for (int32_t y = blockY; y < blockY + blockHeight; ++y)
                for (int32_t x = blockX; x < blockX + blockWidth; ++x) std::copy(value.begin(), value.end(), result.pixels.pixel(x, y));
        }
    }
    return result;
}

// ---- stack plumbing ----------------------------------------------------------------------------------------

Result16 blendEntryResult(const Result16& before, Result16 filtered, double opacity, BlendMode mode) {
    if (opacity >= 1.0 && mode == BlendMode::Normal) return filtered;
    const auto bounds = unionRect(before.bounds, filtered.bounds);
    Image16 output(bounds.width, bounds.height);
    const auto steps = coverageSteps16(float(std::clamp(opacity, 0.0, 1.0)));
    const auto premultiplied = [](const uint16_t* s, uint16_t* d) {
        const uint32_t a = s[3];
        for (int c = 0; c < 3; ++c) d[c] = a == kOne ? s[c] : uint16_t(mul15(s[c], a));
        d[3] = s[3];
    };
    for (int32_t y = 0; y < bounds.height; ++y)
        for (int32_t x = 0; x < bounds.width; ++x) {
            const auto* destination = sampleResult(before, bounds.x + x, bounds.y + y);
            const auto* source = sampleResult(filtered, bounds.x + x, bounds.y + y);
            uint16_t d[4] = {0, 0, 0, 0}, s[4] = {0, 0, 0, 0};
            if (destination) premultiplied(destination, d);
            if (source) premultiplied(source, s);
            if (s[3] != 0) compositePixelSteps16(mode, s, steps, d);
            auto* out = output.pixel(x, y);
            const uint32_t a = d[3];
            for (int c = 0; c < 3; ++c)
                out[c] = a == 0 ? uint16_t(0) : a == kOne ? d[c] : uint16_t(std::min(kOne, (uint32_t(d[c]) * kOne + a / 2) / a));
            out[3] = d[3];
        }
    return Result16{std::move(output), bounds};
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

Result16 applyStackMask(const Result16& base, const Result16& filtered, const SmartFilterStack& stack) {
    constexpr uint64_t kMaskScale = 255U;
    const auto bounds = unionRect(base.bounds, filtered.bounds);
    Image16 output(bounds.width, bounds.height);
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
                pixel[c] = uint16_t(std::min<uint64_t>(kOne, (numerator + alphaNumerator / 2U) / alphaNumerator));
            }
            pixel[3] = uint16_t(std::min<uint64_t>(kOne, (alphaNumerator + kMaskScale / 2U) / kMaskScale));
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
    if (cropped == bounds || cropped.empty()) return Result16{std::move(output), bounds};
    return Result16{cropBuffer(output, bounds, cropped), cropped};
}

Result16 toStraight(const PlacedRaster16& in) {
    Result16 r;
    if (!in.image) return r;
    r.pixels = *in.image;
    unpremultiply(r.pixels);
    r.bounds = in.bounds();
    return r;
}

PlacedRaster16 toPlaced(Result16 r) {
    premultiply(r.pixels);
    PlacedRaster16 out;
    out.x = r.bounds.x;
    out.y = r.bounds.y;
    out.image = std::make_shared<Image16>(std::move(r.pixels));
    return out;
}

PixelRect canvasOr(const PixelRect& canvas, const Result16& r) { return canvas.empty() ? r.bounds : canvas; }

struct RunEntry {
    const Result16& current;
    const PixelRect& canvas;
    Result16 operator()(const std::monostate&) const { return current; }
    Result16 operator()(const smartfilter::GaussianBlur& p) const {
        return trimTransparentResult(renderGaussian(embedInFilterCanvas(current, canvasOr(canvas, current)), p.radius));
    }
    Result16 operator()(const smartfilter::HighPass& p) const { return renderHighPass(current, p.radius); }
    Result16 operator()(const smartfilter::Median& p) const {
        const int r = std::max(1, int(std::floor(p.radius)));
        PixelRect grown = intersectRect({current.bounds.x - r, current.bounds.y - r, current.bounds.width + 2 * r, current.bounds.height + 2 * r}, canvas);
        grown = unionRect(grown, current.bounds);
        if (grown.empty() || grown == current.bounds) return renderMedian(current, p.radius);
        const Result16 wide = renderMedian(embedInFilterCanvas(current, grown), p.radius);
        return Result16{cropBuffer(wide.pixels, wide.bounds, current.bounds), current.bounds};
    }
    Result16 operator()(const smartfilter::DustAndScratches& p) const {
        const int r = std::max(1, int(p.radius));
        PixelRect grown = intersectRect({current.bounds.x - r, current.bounds.y - r, current.bounds.width + 2 * r, current.bounds.height + 2 * r}, canvas);
        grown = unionRect(grown, current.bounds);
        if (grown.empty() || grown == current.bounds) return renderDustAndScratches(current, p.radius, p.threshold);
        const Result16 wide = renderDustAndScratches(embedInFilterCanvas(current, grown), p.radius, p.threshold);
        return Result16{cropBuffer(wide.pixels, wide.bounds, current.bounds), current.bounds};
    }
    Result16 operator()(const smartfilter::SurfaceBlur& p) const {
        return trimTransparentResult(renderSurfaceBlur(embedInFilterCanvas(current, canvasOr(canvas, current)), p.radius, p.threshold));
    }
    Result16 operator()(const smartfilter::UnsharpMask& p) const {
        Result16 out = renderUnsharpMask(current, p.amount, p.radius, p.threshold);
        // Alpha sharpened as a channel, its low-pass seeing the canvas's transparency past the layer.
        const int r = int(std::ceil(p.radius * 3)) + 1;
        PixelRect grown = intersectRect({current.bounds.x - r, current.bounds.y - r, current.bounds.width + 2 * r, current.bounds.height + 2 * r}, canvas);
        grown = unionRect(grown, current.bounds);
        Result16 alpha{Image16(grown.width, grown.height), grown};
        for (int y = 0; y < grown.height; y++)
            for (int x = 0; x < grown.width; x++) {
                const uint16_t* c = sampleResult(current, grown.x + x, grown.y + y);
                uint16_t* q = alpha.pixels.pixel(x, y);
                q[0] = q[1] = q[2] = c ? c[3] : 0;
                q[3] = uint16_t(kOne);
            }
        const Result16 sharpened = renderUnsharpMask(alpha, p.amount, p.radius, p.threshold);
        for (int y = 0; y < out.pixels.height(); y++)
            for (int x = 0; x < out.pixels.width(); x++) out.pixels.pixel(x, y)[3] = sampleResult(sharpened, out.bounds.x + x, out.bounds.y + y)[0];
        return out;
    }
    Result16 operator()(const smartfilter::MotionBlur& p) const {
        return trimTransparentResult(renderMotionBlur(embedInFilterCanvas(current, canvasOr(canvas, current)), p.angle, p.distance));
    }
    Result16 operator()(const smartfilter::PlasticWrap& p) const { return renderPlasticWrap(current, p.highlight, p.detail, p.smoothness); }
    Result16 operator()(const smartfilter::Mosaic& p) const { return renderMosaic(current, p.cellSize); }
    Result16 operator()(const smartfilter::Emboss& p) const { return renderEmboss(current, p.angle, p.height, p.amount); }
    Result16 operator()(const smartfilter::BoxBlur& p) const {
        return trimTransparentResult(renderBoxBlur(embedInFilterCanvas(current, canvasOr(canvas, current)), int32_t(std::floor(p.radius))));
    }
    Result16 operator()(const smartfilter::RadialBlur& p) const {
        const auto content = current.bounds;
        auto input = embedInFilterCanvas(current, canvasOr(canvas, current));
        const auto centerX = double(content.x - input.bounds.x) + double(std::max<int32_t>(0, content.width - 1)) * 0.5;
        const auto centerY = double(content.y - input.bounds.y) + double(std::max<int32_t>(0, content.height - 1)) * 0.5;
        return trimTransparentResult(renderRadialBlur(input, p.amount, p.samples, centerX, centerY));
    }
    Result16 operator()(const smartfilter::AddNoise& p) const { return renderAddNoise(current, p.amount, p.gaussian, p.monochromatic, p.seed); }
};

} // namespace

// Every filter NekoPhoto draws as a Smart Filter is drawn at 16 bits.
bool smartFilterDrawsAt16(const SmartFilterParameters& parameters) {
    return !std::holds_alternative<std::monostate>(parameters);
}

std::optional<PlacedRaster16> renderSmartFilterStack(const PlacedRaster16& placed, const PixelRect& canvas, const SmartFilterStack& stack) {
    if (!stack.supported) return std::nullopt;
    for (const auto& entry : stack.entries) {
        if (std::holds_alternative<std::monostate>(entry.parameters) || !smartFilterDrawsAt16(entry.parameters)) return std::nullopt;
        if (!entry.enabled) continue;
        if (!parametersValid(entry.parameters) || !std::isfinite(entry.opacity) || entry.opacity < 0.0 || entry.opacity > 1.0) return std::nullopt;
    }
    if (!stack.enabled || !placed.image || placed.image->isEmpty()) return placed;
    const auto active = std::count_if(stack.entries.begin(), stack.entries.end(), [](const SmartFilterEntry& e) { return e.enabled && e.opacity > 0.0; });
    Result16 current = toStraight(placed);
    if (active == 0) return toPlaced(trimTransparentResult(std::move(current)));
    const PixelRect filterCanvas = canvas.empty() ? current.bounds : canvas;
    const Result16 base = embedInFilterCanvas(current, filterCanvas);
    for (const auto& entry : stack.entries) {
        if (!entry.enabled || entry.opacity <= 0.0) continue;
        Result16 filtered = std::visit(RunEntry{current, filterCanvas}, entry.parameters);
        current = blendEntryResult(current, std::move(filtered), entry.opacity, entry.blend);
    }
    return toPlaced(applyStackMask(base, current, stack));
}

} // namespace compositor
