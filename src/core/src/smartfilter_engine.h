// The Smart Filter kernels and the stack renderer, written once for both depths: smartfilter_render.cpp
// instantiates them over straight 8-bit RGBA (`Depth8`), smartfilter_render16.cpp over straight 15-bit RGBA
// (`Depth16`, 0..32768). Internal to the core; include it from those two files only.
//
// A depth is a trait struct the templates read:
//   Sample, Pixels, Placed           the sample type, the straight buffer (Image / Image16) and the placed raster
//   one, oneD                        full scale (255 / 32768), as an integer and as a double
//   level                            one 8-bit level in samples (1 / 32768 / 255): level-valued constants scale by it
//   round(v)                         a double to the sample grid, rounded half up and clamped
//   store(v)                         the same as the filters that came from Patchy's destructive code store it
//   narrow(s), widen(b)              a sample to an 8-bit level and back (identities at 8 bits)
//   premultiplySample, coverageSteps, composite   NekoPhoto's blend maths at the depth (blendEntryResult)
//   MotionSum, motionColour, motionAlpha          Motion Blur's accumulator and its rounding
//   Histogram                        Median's one window histogram
//   surfacePlane                     Surface Blur over one plane (the two depths weigh differently)
//   shade                            Plastic Wrap's shading of one colour sample
//   onByteGrid, Sharpen              Unsharp Mask's low-pass choice and its detail step
//   addNoise                         one Add Noise delta on a sample
//   draws(parameters)                whether the filter is drawn at the depth
// Everything else (the edge rules, the sampling, the traversal orders, the plumbing of the stack) is the same
// code at both depths, so a fix lands once.
//
// The patent design constraint of smartfilter_render.cpp holds for every instance: Median and Dust & Scratches keep
// exactly ONE plain window histogram updated a single pixel value at a time, and Surface Blur uses no value
// histogram at all.
#pragma once
#include "nekophoto/smartfilter.h"
#include "nekophoto/workcounters.h"
#include "smartfilter_kernels.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace nekophoto {

namespace {

/// Patchy's FilterRenderResult: straight RGBA pixels (4 samples a pixel, no row padding) placed at `bounds`.
template <class D>
struct Result {
    typename D::Pixels pixels;
    PixelRect bounds;
};

template <class D>
bool pixelsEmpty(const Result<D>& r) { return r.pixels.isEmpty(); }

inline PixelRect intersectRect(PixelRect a, PixelRect b) {
    const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.x + a.width, b.x + b.width), y1 = std::min(a.y + a.height, b.y + b.height);
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

inline PixelRect unionRect(PixelRect a, PixelRect b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.width, b.x + b.width), y1 = std::max(a.y + a.height, b.y + b.height);
    return {x0, y0, x1 - x0, y1 - y0};
}

template <class D>
const typename D::Sample* sampleResult(const Result<D>& result, int32_t documentX, int32_t documentY) {
    const auto localX = int64_t(documentX) - result.bounds.x;
    const auto localY = int64_t(documentY) - result.bounds.y;
    if (localX < 0 || localY < 0 || localX >= result.bounds.width || localY >= result.bounds.height) return nullptr;
    return result.pixels.pixel(int32_t(localX), int32_t(localY));
}

template <class Pixels>
Pixels cropBuffer(const Pixels& source, PixelRect sourceBounds, PixelRect cropBounds) {
    Pixels cropped(cropBounds.width, cropBounds.height);
    if (cropped.isEmpty()) return cropped;
    const auto sx = cropBounds.x - sourceBounds.x, sy = cropBounds.y - sourceBounds.y;
    const size_t samples = size_t(cropBounds.width) * 4U;
    for (int32_t y = 0; y < cropBounds.height; ++y) {
        const auto* row = source.pixel(sx, sy + y);
        std::copy(row, row + samples, cropped.pixel(0, y));
    }
    return cropped;
}

template <class D>
Result<D> embedInFilterCanvas(const Result<D>& placed, PixelRect canvasBounds) {
    if (canvasBounds.empty() || placed.bounds == canvasBounds) return placed;
    typename D::Pixels canvas(canvasBounds.width, canvasBounds.height);
    if (canvas.isEmpty()) return placed;   // larger than a buffer may be
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
    return Result<D>{std::move(canvas), canvasBounds};
}

template <class D>
Result<D> trimTransparentResult(Result<D> result) {
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
    return Result<D>{cropBuffer(result.pixels, result.bounds, cropped), cropped};
}

// ---- Gaussian, High Pass, Unsharp Mask ---------------------------------------------------------------------

template <class D>
Result<D> renderGaussian(const Result<D>& input, double radius) {
    const auto margin = int(std::ceil(smartfilter_detail::gaussianMarginScale * radius));
    const auto bounds = input.bounds;
    const auto plan = smartfilter_detail::makeGaussianLinePlan(radius, margin);
    typename D::Pixels output(bounds.width, bounds.height);
    const auto width = bounds.width, height = bounds.height;
    std::vector<float> horizontal(size_t(width) * size_t(height));
    std::vector<double> values, scratch;
    const std::array<int, 4> channels{3, 0, 1, 2};
    for (const auto channel : channels) {
        values.resize(size_t(width));
        scratch.resize(size_t(width));
        for (int32_t y = 0; y < height; ++y) {
            const auto* row = input.pixels.row(y);
            for (int32_t x = 0; x < width; ++x) {
                const auto* p = row + size_t(x) * 4U;
                values[size_t(x)] = channel == 3 ? double(p[3]) : double(p[channel]) * double(p[3]) / D::oneD;
            }
            smartfilter_detail::filterGaussianLine(values, scratch, plan);
            const auto rowOffset = size_t(y) * size_t(width);
            for (int32_t x = 0; x < width; ++x) horizontal[rowOffset + size_t(x)] = float(D::round(values[size_t(x)]));
        }
        values.resize(size_t(height));
        scratch.resize(size_t(height));
        for (int32_t x = 0; x < width; ++x) {
            for (int32_t y = 0; y < height; ++y) values[size_t(y)] = horizontal[size_t(y) * size_t(width) + size_t(x)];
            smartfilter_detail::filterGaussianLine(values, scratch, plan);
            for (int32_t y = 0; y < height; ++y) {
                auto* out = output.pixel(x, y);
                if (channel == 3) {
                    out[3] = D::round(values[size_t(y)]);
                    continue;
                }
                const auto alpha = out[3];
                const auto premultiplied = D::round(values[size_t(y)]);
                out[channel] = alpha == 0U ? typename D::Sample(0) : D::round(double(premultiplied) * D::oneD / double(alpha));
            }
        }
    }
    return Result<D>{std::move(output), bounds};
}

/// The straight low-pass of High Pass and Unsharp Mask, each pass rounded to the sample grid. `byteGrid`: the input is
/// on the 8-bit grid and both passes run in 8-bit levels, rounding to whole levels as the 8-bit kernel does, so the
/// low-pass is the 8-bit one exactly (widened); at 8 bits that is the same thing.
template <class D>
Result<D> renderStraightGaussian(const Result<D>& input, double radius, bool unsharpKernel = false, bool byteGrid = false) {
    const auto margin = int(std::ceil(smartfilter_detail::gaussianMarginScale * radius));
    const auto plan = unsharpKernel ? smartfilter_detail::makeUnsharpLinePlan(radius, margin) : smartfilter_detail::makeHighPassLinePlan(radius, margin);
    typename D::Pixels output = input.pixels;
    const auto width = input.bounds.width, height = input.bounds.height;
    std::vector<float> horizontal(size_t(width) * size_t(height));
    std::vector<double> values, scratch;
    for (size_t channel = 0; channel < 3U; ++channel) {
        values.resize(size_t(width));
        scratch.resize(size_t(width));
        for (int32_t y = 0; y < height; ++y) {
            for (int32_t x = 0; x < width; ++x) {
                const auto v = input.pixels.pixel(x, y)[channel];
                values[size_t(x)] = byteGrid ? double(D::narrow(v)) : double(v);
            }
            smartfilter_detail::filterGaussianLine(values, scratch, plan);
            const auto rowOffset = size_t(y) * size_t(width);
            for (int32_t x = 0; x < width; ++x)
                horizontal[rowOffset + size_t(x)] = byteGrid ? float(smartfilter_detail::roundedByte(values[size_t(x)])) : float(D::round(values[size_t(x)]));
        }
        values.resize(size_t(height));
        scratch.resize(size_t(height));
        for (int32_t x = 0; x < width; ++x) {
            for (int32_t y = 0; y < height; ++y) values[size_t(y)] = horizontal[size_t(y) * size_t(width) + size_t(x)];
            smartfilter_detail::filterGaussianLine(values, scratch, plan);
            for (int32_t y = 0; y < height; ++y)
                output.pixel(x, y)[channel] = byteGrid ? D::widen(smartfilter_detail::roundedByte(values[size_t(y)])) : D::round(values[size_t(y)]);
        }
    }
    return Result<D>{std::move(output), input.bounds};
}

// ---- transparent colour extension (Median, Dust & Scratches, Surface Blur, Plastic Wrap) ------------------

struct TransparentColorExtension {
    bool allTransparent = false;
    // Empty for an opaque input; otherwise the nearest visible pixel's linear index for every pixel.
    std::vector<uint32_t> nearestVisible;
};

template <class D>
typename D::Sample extendedStraightColorSample(const Result<D>& input, const TransparentColorExtension& extension, int32_t x, int32_t y,
                                               size_t channel) {
    const auto* pixel = input.pixels.pixel(x, y);
    if (pixel[3] != 0U || extension.nearestVisible.empty()) return pixel[channel];
    const auto index = extension.nearestVisible[size_t(y) * size_t(input.bounds.width) + size_t(x)];
    return input.pixels.data()[size_t(index) * 4U + channel];
}

inline int64_t ceilDivide(int64_t numerator, int64_t denominator) {
    const auto quotient = numerator / denominator;
    const auto remainder = numerator % denominator;
    return quotient + (remainder > 0 ? 1 : 0);
}

/// The nearest-visible-pixel map (it depends only on whether alpha is zero).
template <class D>
TransparentColorExtension extendTransparentColors(const Result<D>& input) {
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
template <class D>
Result<D> withHiddenColoursExtended(const Result<D>& input) {
    const auto extension = extendTransparentColors(input);
    if (extension.allTransparent || extension.nearestVisible.empty()) return input;
    Result<D> out = input;
    for (int32_t y = 0; y < input.bounds.height; ++y)
        for (int32_t x = 0; x < input.bounds.width; ++x) {
            auto* p = out.pixels.pixel(x, y);
            if (p[3] != 0) continue;
            for (size_t c = 0; c < 3; ++c) p[c] = extendedStraightColorSample(input, extension, x, y, c);
        }
    return out;
}

template <class D>
Result<D> renderHighPass(const Result<D>& input, double radius) {
    const auto blurred = renderStraightGaussian(withHiddenColoursExtended(input), radius);
    typename D::Pixels output(input.bounds.width, input.bounds.height);
    constexpr double middle = 128.0 * D::level;
    for (int32_t y = 0; y < input.bounds.height; ++y)
        for (int32_t x = 0; x < input.bounds.width; ++x) {
            const auto* source = input.pixels.pixel(x, y);
            const auto* lowFrequency = blurred.pixels.pixel(x, y);
            auto* destination = output.pixel(x, y);
            for (size_t c = 0; c < 3U; ++c) destination[c] = D::round(double(source[c]) - double(lowFrequency[c]) + middle);
            destination[3] = source[3];
        }
    return Result<D>{std::move(output), input.bounds};
}

template <class D>
Result<D> renderUnsharpMask(const Result<D>& input, double amountPercent, double radius, int32_t threshold) {
    const Result<D> extended = withHiddenColoursExtended(input);
    const auto blurred = renderStraightGaussian(extended, radius, true, D::onByteGrid(extended.pixels));
    typename D::Pixels output(input.bounds.width, input.bounds.height);
    const typename D::Sharpen sharpen(amountPercent, threshold);
    for (int32_t y = 0; y < input.bounds.height; ++y)
        for (int32_t x = 0; x < input.bounds.width; ++x) {
            const auto* source = input.pixels.pixel(x, y);
            const auto* lowFrequency = blurred.pixels.pixel(x, y);
            auto* destination = output.pixel(x, y);
            for (size_t c = 0; c < 3U; ++c) destination[c] = sharpen(source[c], lowFrequency[c]);
            destination[3] = source[3];
        }
    return Result<D>{std::move(output), input.bounds};
}

// ---- Motion Blur -------------------------------------------------------------------------------------------

template <class D>
Result<D> renderMotionBlur(const Result<D>& input, int32_t angleDegrees, int32_t distancePixels) {
    using Sum = typename D::MotionSum;
    constexpr int64_t kCoordinateScale = 65536;
    constexpr double kPi = 3.14159265358979323846;
    const auto radians = double(angleDegrees) * kPi / 180.0;
    const auto stepX = int64_t(std::llround(std::cos(radians) * kCoordinateScale));
    const auto stepY = int64_t(std::llround(-std::sin(radians) * kCoordinateScale));
    const auto firstSample = -distancePixels / 2;
    const auto lastSample = firstSample + distancePixels;
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto maximumX = int64_t(std::max(0, width - 1)) * kCoordinateScale;
    const auto maximumY = int64_t(std::max(0, height - 1)) * kCoordinateScale;
    typename D::Pixels output(width, height);
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            std::array<Sum, 3> premultiplied{};
            Sum alphaSum = 0;
            for (auto sample = firstSample; sample <= lastSample; ++sample) {
                const auto sampleX = std::clamp<int64_t>(int64_t(x) * kCoordinateScale + int64_t(sample) * stepX, 0, maximumX);
                const auto sampleY = std::clamp<int64_t>(int64_t(y) * kCoordinateScale + int64_t(sample) * stepY, 0, maximumY);
                const auto x0 = int32_t(sampleX / kCoordinateScale);
                const auto y0 = int32_t(sampleY / kCoordinateScale);
                const auto x1 = std::min(width - 1, x0 + 1);
                const auto y1 = std::min(height - 1, y0 + 1);
                const auto fractionX = sampleX % kCoordinateScale;
                const auto fractionY = sampleY % kCoordinateScale;
                const std::array<Sum, 4> weights{Sum(kCoordinateScale - fractionX) * Sum(kCoordinateScale - fractionY),
                                                 Sum(fractionX) * Sum(kCoordinateScale - fractionY),
                                                 Sum(kCoordinateScale - fractionX) * Sum(fractionY), Sum(fractionX) * Sum(fractionY)};
                const std::array<const typename D::Sample*, 4> pixels{input.pixels.pixel(x0, y0), input.pixels.pixel(x1, y0),
                                                                      input.pixels.pixel(x0, y1), input.pixels.pixel(x1, y1)};
                for (size_t corner = 0; corner < pixels.size(); ++corner) {
                    const Sum alphaWeight = Sum(pixels[corner][3]) * weights[corner];
                    alphaSum += alphaWeight;
                    for (size_t c = 0; c < 3U; ++c) premultiplied[c] += Sum(pixels[corner][c]) * alphaWeight;
                }
            }
            auto* destination = output.pixel(x, y);
            for (size_t c = 0; c < 3U; ++c) destination[c] = D::motionColour(premultiplied[c], alphaSum);
            destination[3] = D::motionAlpha(alphaSum, distancePixels);
        }
    }
    return Result<D>{std::move(output), input.bounds};
}

// ---- Median, Dust & Scratches, Surface Blur ----------------------------------------------------------------

/// One plain window histogram over every sample value (`Values` of them), with running counts per group of
/// 2^`GroupBits` values to find a rank.
template <size_t Values, unsigned GroupBits>
struct WindowValueHistogram {
    static constexpr size_t group = size_t(1) << GroupBits;
    static constexpr size_t groups = (Values + group - 1) / group;
    // The bins on the stack while they are small (256 at 8 bits), on the heap at 16 bits (32,769).
    std::conditional_t<(Values <= 256), std::array<uint32_t, Values>, std::vector<uint32_t>> bins{};
    std::array<uint32_t, groups> coarse{};
    WindowValueHistogram() {
        if constexpr (Values > 256) bins.assign(Values, 0U);
    }
    template <class V>
    void add(V value) {
        ++bins[value];
        ++coarse[size_t(value >> GroupBits)];
    }
    template <class V>
    void remove(V value) {
        --bins[value];
        --coarse[size_t(value >> GroupBits)];
    }
    size_t median(uint32_t rank) const {
        size_t g = 0;
        while (g + 1U < coarse.size() && rank > coarse[g]) {
            rank -= coarse[g];
            ++g;
        }
        const auto first = g * group;
        size_t within = 0;
        while (within + 1U < group && first + within + 1U < bins.size() && rank > bins[first + within]) {
            rank -= bins[first + within];
            ++within;
        }
        return first + within;
    }
};

template <class D, typename Sample>
void filterSquareMedianChannel(const Result<D>& input, typename D::Pixels& output, size_t channel, int32_t radius, Sample&& sample) {
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto diameter = radius * 2 + 1;
    const auto windowArea = uint32_t(diameter) * uint32_t(diameter);
    const auto medianRank = windowArea / 2U + 1U;
    const auto clampX = [width](int32_t x) { return std::clamp(x, 0, width - 1); };
    const auto clampY = [height](int32_t y) { return std::clamp(y, 0, height - 1); };
    // One window histogram, updated one pixel value at a time along a serpentine traversal.
    typename D::Histogram window;
    for (int32_t dy = -radius; dy <= radius; ++dy) {
        const auto sy = clampY(dy);
        for (int32_t dx = -radius; dx <= radius; ++dx) window.add(sample(clampX(dx), sy));
    }
    int32_t x = 0;
    int32_t direction = 1;
    for (int32_t y = 0; y < height; ++y) {
        while (true) {
            output.pixel(x, y)[channel] = typename D::Sample(window.median(medianRank));
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

/// One channel's plane through the depth's Surface Blur (`D::surfacePlane`).
template <class D, typename Sample>
void filterSquareSurfaceChannel(const Result<D>& input, typename D::Pixels& output, size_t channel, int32_t radius, int32_t threshold,
                                Sample&& sample) {
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto rowStride = size_t(width);
    std::vector<typename D::Sample> plane(rowStride * size_t(height));
    for (int32_t y = 0; y < height; ++y)
        for (int32_t x = 0; x < width; ++x) plane[size_t(y) * rowStride + size_t(x)] = sample(x, y);
    D::surfacePlane(plane, width, height, radius, threshold, output, channel);
}

template <class D>
Result<D> renderMedian(const Result<D>& input, double radius) {
    if (pixelsEmpty(input)) return input;
    const auto effectiveRadius = std::max(1, int32_t(std::floor(radius)));
    const auto extension = extendTransparentColors(input);
    typename D::Pixels output = input.pixels;
    filterSquareMedianChannel(input, output, 3U, effectiveRadius,
                              [&input](int32_t x, int32_t y) { return input.pixels.pixel(x, y)[3]; });
    // With no visible colour source the hidden RGB is kept rather than manufactured.
    if (extension.allTransparent) return Result<D>{std::move(output), input.bounds};
    for (size_t channel = 0; channel < 3U; ++channel)
        filterSquareMedianChannel(input, output, channel, effectiveRadius, [&input, &extension, channel](int32_t x, int32_t y) {
            return extendedStraightColorSample(input, extension, x, y, channel);
        });
    return Result<D>{std::move(output), input.bounds};
}

template <class D>
Result<D> renderSurfaceBlur(const Result<D>& input, double radius, int32_t threshold) {
    if (pixelsEmpty(input)) return input;
    const auto effectiveRadius = std::max(1, int32_t(std::floor(radius + 0.5)));
    const auto extension = extendTransparentColors(input);
    typename D::Pixels output = input.pixels;
    filterSquareSurfaceChannel(input, output, 3U, effectiveRadius, threshold,
                               [&input](int32_t x, int32_t y) { return input.pixels.pixel(x, y)[3]; });
    if (extension.allTransparent) return Result<D>{std::move(output), input.bounds};
    for (size_t channel = 0; channel < 3U; ++channel)
        filterSquareSurfaceChannel(input, output, channel, effectiveRadius, threshold,
                                   [&input, &extension, channel](int32_t x, int32_t y) {
                                       return extendedStraightColorSample(input, extension, x, y, channel);
                                   });
    return Result<D>{std::move(output), input.bounds};
}

template <class D>
Result<D> renderDustAndScratches(const Result<D>& input, int32_t radius, int32_t threshold) {
    using Sample = typename D::Sample;
    if (pixelsEmpty(input)) return input;
    const auto extension = extendTransparentColors(input);
    if (extension.allTransparent) return input;
    typename D::Pixels output = input.pixels;
    for (size_t channel = 0; channel < 3U; ++channel)
        filterSquareMedianChannel(input, output, channel, radius, [&input, &extension, channel](int32_t x, int32_t y) {
            return extendedStraightColorSample(input, extension, x, y, channel);
        });
    // More than `threshold` whole levels apart: past the midpoint to the next level (at 8 bits, the next level).
    const double cut = (double(threshold) + 0.5) * D::level;
    for (int32_t y = 0; y < input.bounds.height; ++y) {
        for (int32_t x = 0; x < input.bounds.width; ++x) {
            auto* destination = output.pixel(x, y);
            const std::array<Sample, 3> median{destination[0], destination[1], destination[2]};
            std::array<Sample, 3> source{};
            int32_t difference = 0;
            for (size_t c = 0; c < source.size(); ++c) {
                source[c] = extendedStraightColorSample(input, extension, x, y, c);
                difference = std::max(difference, std::abs(int32_t(source[c]) - int32_t(median[c])));
            }
            const auto replace = double(difference) > cut;
            for (size_t c = 0; c < source.size(); ++c) destination[c] = replace ? median[c] : source[c];
            // Straight RGB only; alpha stays exact.
            destination[3] = input.pixels.pixel(x, y)[3];
        }
    }
    return Result<D>{std::move(output), input.bounds};
}

// ---- Plastic Wrap ------------------------------------------------------------------------------------------

template <class D>
Result<D> renderPlasticWrap(const Result<D>& input, int32_t highlightStrength, int32_t detail, int32_t smoothness) {
    if (pixelsEmpty(input)) return input;
    const auto extension = extendTransparentColors(input);
    if (extension.allTransparent) return input;
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto count = size_t(width) * size_t(height);
    std::vector<uint16_t> luminance(count), horizontal(count), heightField(count);
    const auto indexOf = [width](int32_t x, int32_t y) { return size_t(y) * size_t(width) + size_t(x); };

    // The height field at 8-bit precision at every depth, as Photoshop's 8-bit look defines it.
    for (int32_t y = 0; y < height; ++y)
        for (int32_t x = 0; x < width; ++x) {
            const auto red = uint32_t(D::narrow(extendedStraightColorSample(input, extension, x, y, 0U)));
            const auto green = uint32_t(D::narrow(extendedStraightColorSample(input, extension, x, y, 1U)));
            const auto blue = uint32_t(D::narrow(extendedStraightColorSample(input, extension, x, y, 2U)));
            const auto alpha = uint32_t(D::narrow(input.pixels.pixel(x, y)[3]));
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

    typename D::Pixels output = input.pixels;
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
            for (size_t c = 0; c < 3U; ++c) destination[c] = D::shade(source[c], relief, shine);
            destination[3] = source[3];
        }
    }
    return Result<D>{std::move(output), input.bounds};
}

// ---- Box Blur ----------------------------------------------------------------------------------------------

// The destructive Box Blur math: alpha-weighted separable box average in doubles, edge-clamped.
template <class D>
Result<D> renderBoxBlurDirect(const Result<D>& input, int32_t radius) {
    const auto width = input.pixels.width(), height = input.pixels.height();
    Result<D> result = input;
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
                const auto alpha = double(px[3]) / D::oneD;
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
            for (int c = 0; c < 3; ++c) dst[c] = D::store(alphaSum > 0.000001 ? accum[c] / alphaSum : 0.0);
            dst[3] = D::store(alphaSum / totalWeight * D::oneD);
        }
    }
    return result;
}

// Large radii: an exact integer sliding-window box average with the same edge-clamped sampling.
template <class D>
Result<D> renderBoxBlurSliding(const Result<D>& input, int32_t radius) {
    const auto width = input.pixels.width(), height = input.pixels.height();
    Result<D> result = input;
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
            for (int c = 0; c < 3; ++c) dst[c] = D::store(alphaSum > 0 ? double(accum[c]) / double(alphaSum) : 0.0);
            dst[3] = D::store(double(alphaSum) / totalWeight);
        }
        if (y + 1 < height) {
            addRow(std::clamp<int32_t>(y - radius, 0, height - 1), -1);
            addRow(std::clamp<int32_t>(y + 1 + radius, 0, height - 1), 1);
        }
    }
    return result;
}

template <class D>
Result<D> renderBoxBlur(const Result<D>& input, int32_t radius) {
    if (pixelsEmpty(input)) return input;
    radius = std::clamp(radius, 1, smartfilter_detail::boxBlurMaximumRadius);
    if (radius <= smartfilter_detail::boxBlurDirectMaximumRadius) return renderBoxBlurDirect(input, radius);
    return renderBoxBlurSliding(input, radius);
}

// ---- Emboss ------------------------------------------------------------------------------------------------

template <class D>
Result<D> renderEmboss(const Result<D>& input, int32_t angleDegrees, int32_t heightPixels, int32_t amountPercent) {
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
    constexpr double middle = 128.0 * D::level;
    Result<D> result = input;
    for (int32_t y = 0; y < h; ++y)
        for (int32_t x = 0; x < w; ++x) {
            auto* px = result.pixels.pixel(x, y);
            for (int c = 0; c < 3; c++) {
                const auto lit = sampled(double(x) + offsetX, double(y) + offsetY, c);
                const auto dark = sampled(double(x) - offsetX, double(y) - offsetY, c);
                px[c] = D::store(middle + (lit - dark) * scale);
            }
        }
    return result;
}

// ---- Radial Blur (Spin and Zoom) ---------------------------------------------------------------------------

struct RadialBlurAccum {
    std::array<double, 3> premultipliedColor{0.0, 0.0, 0.0};
    double alpha = 0.0;
    double weight = 0.0;
};

template <class D>
void radialBlurAccumulatePixel(RadialBlurAccum& accum, const typename D::Sample* px, double weight) {
    if (weight <= 0.0) return;
    const auto alpha = double(px[3]) / D::oneD;
    accum.weight += weight;
    accum.alpha += alpha * weight;
    for (size_t c = 0; c < 3; ++c) accum.premultipliedColor[c] += double(px[c]) * alpha * weight;
}

template <class D>
void radialBlurAccumulateSample(RadialBlurAccum& accum, const typename D::Pixels& original, double x, double y) {
    x = std::clamp(x, 0.0, double(std::max<int32_t>(0, original.width() - 1)));
    y = std::clamp(y, 0.0, double(std::max<int32_t>(0, original.height() - 1)));
    const auto x0 = int32_t(std::floor(x));
    const auto y0 = int32_t(std::floor(y));
    const auto x1 = std::min<int32_t>(original.width() - 1, x0 + 1);
    const auto y1 = std::min<int32_t>(original.height() - 1, y0 + 1);
    const auto tx = x - double(x0);
    const auto ty = y - double(y0);
    radialBlurAccumulatePixel<D>(accum, original.pixel(x0, y0), (1.0 - tx) * (1.0 - ty));
    radialBlurAccumulatePixel<D>(accum, original.pixel(x1, y0), tx * (1.0 - ty));
    radialBlurAccumulatePixel<D>(accum, original.pixel(x0, y1), (1.0 - tx) * ty);
    radialBlurAccumulatePixel<D>(accum, original.pixel(x1, y1), tx * ty);
}

template <class D>
Result<D> renderRadialBlur(const Result<D>& input, int32_t amount, int32_t samples, double centerX, double centerY, bool zoom) {
    if (pixelsEmpty(input)) return input;
    Result<D> result{typename D::Pixels(input.bounds.width, input.bounds.height), input.bounds};
    const auto clampedAmount = std::clamp(amount, 0, 100);
    const auto clampedSamples = std::clamp(samples, 4, 32);
    for (int32_t y = 0; y < input.bounds.height; ++y)
        for (int32_t x = 0; x < input.bounds.width; ++x) {
            const auto dx = double(x) - centerX;
            const auto dy = double(y) - centerY;
            RadialBlurAccum accum;
            for (int sample = 0; sample < clampedSamples; ++sample) {
                double sx = 0, sy = 0;
                smartfilter_detail::radialBlurSource(dx, dy, clampedAmount, clampedSamples, zoom, sample, sx, sy);
                radialBlurAccumulateSample<D>(accum, input.pixels, centerX + sx, centerY + sy);
            }
            auto* dst = result.pixels.pixel(x, y);
            const auto normalizedAlpha = accum.weight > 0.0 ? accum.alpha / accum.weight : 1.0;
            for (size_t c = 0; c < 3; ++c) dst[c] = D::store(accum.alpha > 0.000001 ? accum.premultipliedColor[c] / accum.alpha : 0.0);
            dst[3] = D::store(normalizedAlpha * D::oneD);
        }
    return result;
}

// ---- Add Noise ---------------------------------------------------------------------------------------------

// RGB gains position-hashed deltas (buffer-local coordinates); alpha and bounds stay exact.
template <class D>
Result<D> renderAddNoise(const Result<D>& input, double amountPercent, bool gaussian, bool monochromatic, int32_t seed) {
    using namespace smartfilter_detail;
    if (pixelsEmpty(input)) return input;
    Result<D> result = input;
    // The amplitude in 8-bit levels, carried to the depth.
    const auto range = std::clamp(amountPercent, addNoiseMinimumAmount, addNoiseMaximumAmount) * 2.55 * D::level;
    const auto laneBase = uint32_t(std::clamp(seed, addNoiseMinimumSeed, addNoiseMaximumSeed)) * 16U;
    const auto unitFromHash = [](uint32_t hash) { return double(hash) * (2.0 / 4294967295.0) - 1.0; };
    const auto deltaForLane = [&](int32_t x, int32_t y, uint32_t lane) {
        if (!gaussian) return unitFromHash(addNoiseHash(x, y, laneBase + lane * 4U)) * range;
        double sum = 0.0;   // sum of four uniforms
        for (uint32_t sample = 1; sample <= 4U; ++sample) sum += unitFromHash(addNoiseHash(x, y, laneBase + lane * 4U + sample));
        return sum * 0.5 * range;
    };
    for (int32_t y = 0; y < result.bounds.height; ++y)
        for (int32_t x = 0; x < result.bounds.width; ++x) {
            auto* px = result.pixels.pixel(x, y);
            if (monochromatic) {
                const auto delta = deltaForLane(x, y, 3U);
                for (int c = 0; c < 3; ++c) px[c] = D::addNoise(px[c], delta);
            } else {
                for (uint32_t c = 0; c < 3U; ++c) px[c] = D::addNoise(px[c], deltaForLane(x, y, c));
            }
        }
    return result;
}

// ---- Mosaic ------------------------------------------------------------------------------------------------

// Alpha-weighted block means over a grid anchored at the input's local origin; bounds kept.
template <class D>
Result<D> renderMosaic(const Result<D>& input, int32_t cellSizePixels) {
    if (pixelsEmpty(input)) return input;
    Result<D> result{typename D::Pixels(input.bounds.width, input.bounds.height), input.bounds};
    const auto width = input.bounds.width, height = input.bounds.height;
    const auto cell = std::max<int32_t>(smartfilter_detail::mosaicMinimumCellSize, cellSizePixels);
    for (int32_t blockY = 0; blockY < height; blockY += cell) {
        const auto blockHeight = std::min(cell, height - blockY);
        for (int32_t blockX = 0; blockX < width; blockX += cell) {
            const auto blockWidth = std::min(cell, width - blockX);
            double weight = 0.0, alphaSum = 0.0;
            std::array<double, 3> premultiplied{};
            for (int32_t y = blockY; y < blockY + blockHeight; ++y)
                for (int32_t x = blockX; x < blockX + blockWidth; ++x) {
                    const auto* px = input.pixels.pixel(x, y);
                    const auto alpha = double(px[3]) / D::oneD;
                    weight += 1.0;
                    alphaSum += alpha;
                    for (size_t c = 0; c < 3; ++c) premultiplied[c] += double(px[c]) * alpha;
                }
            std::array<typename D::Sample, 4> value{};
            for (size_t c = 0; c < 3; ++c) value[c] = D::store(alphaSum > 0.000001 ? premultiplied[c] / alphaSum : 0.0);
            value[3] = D::store(weight > 0.0 ? alphaSum / weight * D::oneD : 0.0);
            for (int32_t y = blockY; y < blockY + blockHeight; ++y)
                for (int32_t x = blockX; x < blockX + blockWidth; ++x) std::copy(value.begin(), value.end(), result.pixels.pixel(x, y));
        }
    }
    return result;
}

// ---- stack plumbing ----------------------------------------------------------------------------------------

/// One entry's result over the result so far. Normal at 100% replaces; anything else composites the filtered
/// pixels source-over with opacity and blend, through NekoPhoto's own blend maths (premultiplied).
template <class D>
Result<D> blendEntryResult(const Result<D>& before, Result<D> filtered, double opacity, BlendMode mode) {
    using Sample = typename D::Sample;
    if (opacity >= 1.0 && mode == BlendMode::Normal) return filtered;
    const auto bounds = unionRect(before.bounds, filtered.bounds);
    typename D::Pixels output(bounds.width, bounds.height);
    const auto steps = D::coverageSteps(float(std::clamp(opacity, 0.0, 1.0)));
    const auto premultiplied = [](const Sample* s, Sample* d) {
        const uint32_t a = s[3];
        for (int c = 0; c < 3; ++c) d[c] = a == D::one ? s[c] : D::premultiplySample(s[c], a);
        d[3] = s[3];
    };
    for (int32_t y = 0; y < bounds.height; ++y)
        for (int32_t x = 0; x < bounds.width; ++x) {
            const auto* destination = sampleResult(before, bounds.x + x, bounds.y + y);
            const auto* source = sampleResult(filtered, bounds.x + x, bounds.y + y);
            Sample d[4] = {0, 0, 0, 0}, s[4] = {0, 0, 0, 0};
            if (destination) premultiplied(destination, d);
            if (source) premultiplied(source, s);
            if (s[3] != 0) D::composite(mode, s, steps, d);
            // Back to straight colour for the next kernel.
            auto* out = output.pixel(x, y);
            const uint32_t a = d[3];
            for (int c = 0; c < 3; ++c)
                out[c] = a == 0 ? Sample(0) : a == D::one ? d[c] : Sample(std::min(D::one, (uint32_t(d[c]) * D::one + a / 2) / a));
            out[3] = d[3];
        }
    return Result<D>{std::move(output), bounds};
}

inline uint8_t sampleFilterMask(const SmartFilterStack& stack, int32_t documentX, int32_t documentY) {
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
template <class D>
Result<D> applyStackMask(const Result<D>& base, const Result<D>& filtered, const SmartFilterStack& stack) {
    using Sample = typename D::Sample;
    constexpr uint64_t kMaskScale = 255U;
    const auto bounds = unionRect(base.bounds, filtered.bounds);
    typename D::Pixels output(bounds.width, bounds.height);
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
                pixel[c] = Sample(std::min<uint64_t>(D::one, (numerator + alphaNumerator / 2U) / alphaNumerator));
            }
            pixel[3] = Sample(std::min<uint64_t>(D::one, (alphaNumerator + kMaskScale / 2U) / kMaskScale));
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
    if (cropped == bounds || cropped.empty()) return Result<D>{std::move(output), bounds};
    return Result<D>{cropBuffer(output, bounds, cropped), cropped};
}

// ---- premultiplied <-> straight boundaries -----------------------------------------------------------------

template <class D>
Result<D> toStraight(const typename D::Placed& in) {
    Result<D> r;
    if (!in.image) return r;
    r.pixels = *in.image;
    unpremultiply(r.pixels);
    r.bounds = in.bounds();
    return r;
}

template <class D>
typename D::Placed toPlaced(Result<D> r) {
    premultiply(r.pixels);
    typename D::Placed out;
    out.x = r.bounds.x;
    out.y = r.bounds.y;
    out.image = std::make_shared<typename D::Pixels>(std::move(r.pixels));
    return out;
}

template <class D>
PixelRect canvasOr(const PixelRect& canvas, const Result<D>& r) { return canvas.empty() ? r.bounds : canvas; }

// Growing filters: embed in the canvas, filter, trim.
template <class D>
Result<D> gaussianStep(const Result<D>& r, const PixelRect& canvas, double radius) {
    return trimTransparentResult(renderGaussian(embedInFilterCanvas(r, canvasOr(canvas, r)), radius));
}
template <class D>
Result<D> surfaceStep(const Result<D>& r, const PixelRect& canvas, double radius, int32_t threshold) {
    return trimTransparentResult(renderSurfaceBlur(embedInFilterCanvas(r, canvasOr(canvas, r)), radius, threshold));
}
template <class D>
Result<D> motionStep(const Result<D>& r, const PixelRect& canvas, int32_t angle, int32_t distance) {
    return trimTransparentResult(renderMotionBlur(embedInFilterCanvas(r, canvasOr(canvas, r)), angle, distance));
}
template <class D>
Result<D> boxStep(const Result<D>& r, const PixelRect& canvas, double radius) {
    return trimTransparentResult(renderBoxBlur(embedInFilterCanvas(r, canvasOr(canvas, r)), int32_t(std::floor(radius))));
}
template <class D>
Result<D> radialStep(const Result<D>& r, const PixelRect& canvas, int32_t amount, int32_t samples, bool zoom) {
    const auto content = r.bounds;
    auto input = embedInFilterCanvas(r, canvasOr(canvas, r));
    // The sweep pivots on the CONTENT centre in canvas buffer coordinates, never the canvas centre.
    const auto centerX = double(content.x - input.bounds.x) + double(std::max<int32_t>(0, content.width - 1)) * 0.5;
    const auto centerY = double(content.y - input.bounds.y) + double(std::max<int32_t>(0, content.height - 1)) * 0.5;
    return trimTransparentResult(renderRadialBlur(input, amount, samples, centerX, centerY, zoom));
}

template <class D>
struct RunEntry {
    const Result<D>& current;
    const PixelRect& canvas;
    Result<D> operator()(const std::monostate&) const { return current; }
    Result<D> operator()(const smartfilter::GaussianBlur& p) const { return gaussianStep(current, canvas, p.radius); }
    Result<D> operator()(const smartfilter::HighPass& p) const { return renderHighPass(current, p.radius); }
    Result<D> operator()(const smartfilter::Median& p) const {
        // Photoshop's window sees the canvas past the layer's edge (transparent there), not the layer's own edge
        // repeated: a rectangle filling its layer loses its corners. The bounds stay the layer's.
        const int r = std::max(1, int(std::floor(p.radius)));
        PixelRect grown = intersectRect({current.bounds.x - r, current.bounds.y - r, current.bounds.width + 2 * r, current.bounds.height + 2 * r}, canvas);
        grown = unionRect(grown, current.bounds);   // never less than the layer (it may reach past the canvas)
        if (grown.empty() || grown == current.bounds) return renderMedian(current, p.radius);
        const Result<D> wide = renderMedian(embedInFilterCanvas(current, grown), p.radius);
        return Result<D>{cropBuffer(wide.pixels, wide.bounds, current.bounds), current.bounds};
    }
    Result<D> operator()(const smartfilter::DustAndScratches& p) const {
        // Median's window, so Median's edge (the canvas past the layer, transparent).
        const int r = std::max(1, int(p.radius));
        PixelRect grown = intersectRect({current.bounds.x - r, current.bounds.y - r, current.bounds.width + 2 * r, current.bounds.height + 2 * r}, canvas);
        grown = unionRect(grown, current.bounds);   // never less than the layer (it may reach past the canvas)
        if (grown.empty() || grown == current.bounds) return renderDustAndScratches(current, p.radius, p.threshold);
        const Result<D> wide = renderDustAndScratches(embedInFilterCanvas(current, grown), p.radius, p.threshold);
        return Result<D>{cropBuffer(wide.pixels, wide.bounds, current.bounds), current.bounds};
    }
    Result<D> operator()(const smartfilter::SurfaceBlur& p) const { return surfaceStep(current, canvas, p.radius, p.threshold); }
    Result<D> operator()(const smartfilter::UnsharpMask& p) const {
        Result<D> out = renderUnsharpMask(current, p.amount, p.radius, p.threshold);
        // Photoshop sharpens transparency too (a half-transparent band beside an opaque one comes out opaque): the
        // same unsharp mask run over alpha as a channel.
        // Past the layer its low-pass sees the canvas's transparency (zero), as Median's window does.
        const int r = int(std::ceil(p.radius * 3)) + 1;
        PixelRect grown = intersectRect({current.bounds.x - r, current.bounds.y - r, current.bounds.width + 2 * r, current.bounds.height + 2 * r}, canvas);
        grown = unionRect(grown, current.bounds);   // never less than the layer (it may reach past the canvas)
        Result<D> alpha{typename D::Pixels(grown.width, grown.height), grown};
        for (int y = 0; y < grown.height; y++)
            for (int x = 0; x < grown.width; x++) {
                const auto* c = sampleResult(current, grown.x + x, grown.y + y);
                auto* q = alpha.pixels.pixel(x, y);
                q[0] = q[1] = q[2] = c ? c[3] : 0;
                q[3] = typename D::Sample(D::one);
            }
        const Result<D> sharpened = renderUnsharpMask(alpha, p.amount, p.radius, p.threshold);
        for (int y = 0; y < out.pixels.height(); y++)
            for (int x = 0; x < out.pixels.width(); x++)
                out.pixels.pixel(x, y)[3] = sampleResult(sharpened, out.bounds.x + x, out.bounds.y + y)[0];
        return out;
    }
    Result<D> operator()(const smartfilter::MotionBlur& p) const { return motionStep(current, canvas, p.angle, p.distance); }
    Result<D> operator()(const smartfilter::PlasticWrap& p) const { return renderPlasticWrap(current, p.highlight, p.detail, p.smoothness); }
    Result<D> operator()(const smartfilter::Mosaic& p) const { return renderMosaic(current, p.cellSize); }
    Result<D> operator()(const smartfilter::Emboss& p) const { return renderEmboss(current, p.angle, p.height, p.amount); }
    Result<D> operator()(const smartfilter::BoxBlur& p) const { return boxStep(current, canvas, p.radius); }
    Result<D> operator()(const smartfilter::RadialBlur& p) const { return radialStep(current, canvas, p.amount, p.samples, p.zoom); }
    Result<D> operator()(const smartfilter::AddNoise& p) const {
        return renderAddNoise(current, p.amount, p.gaussian, p.monochromatic, p.seed);
    }
};

/// The whole stack at the depth (renderSmartFilterStack).
template <class D>
std::optional<typename D::Placed> renderStack(const typename D::Placed& placed, const PixelRect& canvas, const SmartFilterStack& stack) {
    if (!stack.supported) return std::nullopt;
    for (const auto& entry : stack.entries) {
        if (std::holds_alternative<std::monostate>(entry.parameters) || !D::draws(entry.parameters)) return std::nullopt;
        if (!entry.enabled) continue;
        if (!smartfilter_detail::parametersValid(entry.parameters) || !std::isfinite(entry.opacity) || entry.opacity < 0.0 ||
            entry.opacity > 1.0)
            return std::nullopt;
    }
    if (!stack.enabled || !placed.image || placed.image->isEmpty()) return placed;
    work::add(work::Counter::SmartFilterStacks);

    const auto active = std::count_if(stack.entries.begin(), stack.entries.end(),
                                      [](const SmartFilterEntry& e) { return e.enabled && e.opacity > 0.0; });
    Result<D> current = toStraight<D>(placed);
    if (active == 0) return toPlaced(trimTransparentResult(std::move(current)));

    const PixelRect filterCanvas = canvas.empty() ? current.bounds : canvas;
    const Result<D> base = embedInFilterCanvas(current, filterCanvas);
    for (const auto& entry : stack.entries) {
        if (!entry.enabled || entry.opacity <= 0.0) continue;
        work::add(work::Counter::SmartFilterPasses);
        Result<D> filtered = std::visit(RunEntry<D>{current, filterCanvas}, entry.parameters);
        current = blendEntryResult(current, std::move(filtered), entry.opacity, entry.blend);
    }
    return toPlaced(applyStackMask(base, current, stack));
}

} // namespace

} // namespace nekophoto
