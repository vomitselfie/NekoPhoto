#include "compositor/histogram.h"
#include "compositor/depth.h"
#include "compositor/modeedit.h"
#include "compositor/render.h"
#include <algorithm>
#include <cmath>

namespace compositor {

namespace {

template <class Img> int channelsOf(const Img& image) {
    if constexpr (std::is_same_v<Img, Image>) { (void)image; return 4; }
    else return image.channels();
}

template <class Img> std::shared_ptr<Img> subsampledTyped(const Img& image, int step) {
    const int w = (image.width() + step - 1) / step, h = (image.height() + step - 1) / step;
    const int n = channelsOf(image);
    std::shared_ptr<Img> out;
    if constexpr (std::is_same_v<Img, Image>) out = std::make_shared<Img>(w, h);
    else out = std::make_shared<Img>(w, h, n);
    for (int y = 0; y < h; y++) {
        const auto* src = image.row(y * step);
        auto* dst = out->row(y);
        for (int x = 0; x < w; x++) std::copy_n(src + size_t(x) * size_t(step) * size_t(n), n, dst + size_t(x) * size_t(n));
    }
    return out;
}

/// The 8-bit RGBA an RGB histogram counts.
std::shared_ptr<const Image> eightBit(const AnyImage& image, const TransferCurve& curve) {
    if (image.u8()) return image.u8();
    if (image.u16()) return narrowImage(*image.u16());
    if (image.f32()) return encodeImage8(*image.f32(), curve);
    return nullptr;
}

} // namespace

AnyImage subsampled(const AnyImage& image, int step) {
    if (!image || step <= 1) return image;
    if (image.c8()) return ImageC8Ptr(subsampledTyped(*image.c8(), step));
    if (image.u8()) return ImagePtr(subsampledTyped(*image.u8(), step));
    if (image.u16()) return Image16Ptr(subsampledTyped(*image.u16(), step));
    if (image.f32()) return ImageFPtr(subsampledTyped(*image.f32(), step));
    return image;
}

AnyImage renderComposite(const Document& document, double scale) {
    RenderOptions options;
    options.scale = scale > 0 ? scale : 1;
    if (document.colorMode == ColorMode::RGB && document.sampleType == SampleType::F32) {
        const int w = std::max(1, int(std::ceil(document.width * options.scale - 1e-9)));
        const int h = std::max(1, int(std::ceil(document.height * options.scale - 1e-9)));
        auto out = std::make_shared<ImageF>(w, h);
        renderF(document, options, *out);
        return ImageFPtr(out);
    }
    return renderNative(document, options);
}

int histogramCacheLevel(int width, int height, double budget) {
    int level = 1;
    double pixels = double(std::max(0, width)) * double(std::max(0, height));
    while (pixels > budget && level < 8) { pixels /= 4; level++; }
    return level;
}

ImageHistogram imageHistogram(const AnyImage& source, ColorMode mode, const TransferCurve& curve, int cacheLevel) {
    ImageHistogram result;
    result.mode = mode;
    result.cacheLevel = std::clamp(cacheLevel, 1, 8);
    const AnyImage image = subsampled(source, 1 << (result.cacheLevel - 1));
    if (!image) return result;
    if (mode != ColorMode::RGB) {
        result.channels = levelsHistogramInMode(image, mode);
        if (mode == ColorMode::CMYK && !result.channels[1].empty()) {
            // Photoshop's CMYK composite counts the four inks together, as its RGB one counts the three channels.
            for (int i = 0; i < 256; i++) {
                double sum = 0;
                for (int c = 1; c <= 4; c++) sum += result.channels[size_t(c)][size_t(i)];
                result.channels[0][size_t(i)] = sum / 4;
            }
        }
        return result;
    }
    const auto eight = eightBit(image, curve);
    if (!eight) return result;
    const auto levels = levelsHistogram(*eight, nullptr);
    for (int c = 0; c < 4; c++) result.channels[size_t(c)] = levels[size_t(c)];
    result.luminosity.assign(256, 0.0);
    for (int y = 0; y < eight->height(); y++) {
        const uint8_t* p = eight->row(y);
        for (int x = 0; x < eight->width(); x++, p += 4) {
            if (!p[3]) continue;
            const double a = p[3];
            const double r = std::min(255.0, std::round(p[0] * 255.0 / a)), g = std::min(255.0, std::round(p[1] * 255.0 / a)),
                         b = std::min(255.0, std::round(p[2] * 255.0 / a));
            const int level = std::clamp(int(std::lround(0.30 * r + 0.59 * g + 0.11 * b)), 0, 255);
            result.luminosity[size_t(level)] += a / 255.0;
        }
    }
    return result;
}

HistogramStats histogramStats(const std::vector<double>& bins) {
    HistogramStats stats;
    if (bins.size() != 256) return stats;
    double sum = 0, total = 0;
    for (int i = 0; i < 256; i++) { total += bins[size_t(i)]; sum += i * bins[size_t(i)]; }
    if (total <= 0) return stats;
    stats.pixels = total;
    stats.mean = sum / total;
    double var = 0;
    for (int i = 0; i < 256; i++) var += bins[size_t(i)] * (i - stats.mean) * (i - stats.mean);
    stats.stdDev = std::sqrt(var / total);
    double run = 0;
    for (int i = 0; i < 256; i++) {
        run += bins[size_t(i)];
        if (run >= total / 2) { stats.median = i; break; }
    }
    return stats;
}

double histogramCount(const std::vector<double>& bins, int from, int to) {
    if (bins.size() != 256) return 0;
    if (from > to) std::swap(from, to);
    double count = 0;
    for (int i = std::max(0, from); i <= std::min(255, to); i++) count += bins[size_t(i)];
    return count;
}

double histogramPercentile(const std::vector<double>& bins, int to) {
    const double total = histogramCount(bins, 0, 255);
    return total > 0 ? histogramCount(bins, 0, to) / total * 100 : 0;
}

AdjustmentSettings clippingSettings(const AdjustmentSettings& settings) {
    AdjustmentSettings out = settings;
    if (out.kind == AdjustmentKind::Levels)
        for (auto& range : out.levels.ranges) { range.outputBlack = 0; range.outputWhite = 255; }
    return out;
}

AnyImage adjustedAny(const AdjustmentSettings& settings, const AnyImage& image, ColorMode mode, const ColorProfile& profile, const TransferCurve& curve) {
    if (!image) return {};
    if (mode != ColorMode::RGB) return adjustedInMode(settings, image, mode, profile);
    if (image.u8()) {
        auto out = std::make_shared<Image>(*image.u8());
        applyAdjustment(settings, *out, Rect(0, 0, out->width(), out->height()), 1);
        return ImagePtr(out);
    }
    if (image.u16()) {
        auto out = std::make_shared<Image16>(*image.u16());
        applyAdjustment(settings, *out, Rect(0, 0, out->width(), out->height()), 1);
        return Image16Ptr(out);
    }
    if (image.f32()) {
        auto out = std::make_shared<ImageF>(*image.f32());
        applyAdjustment(settings, *out, Rect(0, 0, out->width(), out->height()), 1, curve);
        return ImageFPtr(out);
    }
    return {};
}

namespace {

/// Which of up to four colour channels sit at the top (white point) or bottom of their range, a bit per channel;
/// -1 for a transparent pixel.
template <class T> int clippedBits(const T* p, int colours, double full, bool whitePoint) {
    const double a = double(p[colours]);
    if (a <= 0) return -1;
    int bits = 0;
    for (int k = 0; k < colours; k++) {
        double v = double(p[k]) / a;
        if constexpr (std::is_floating_point_v<T>) {
            if (whitePoint ? v >= 1 - 1e-6 : v <= 1e-6) bits |= 1 << k;
        } else {
            const double level = std::round(std::min(1.0, v) * full);
            if (whitePoint ? level >= full : level <= 0) bits |= 1 << k;
        }
    }
    return bits;
}

} // namespace

std::shared_ptr<Image> clippingDisplay(const AnyImage& adjusted, ColorMode mode, bool whitePoint) {
    if (!adjusted) return nullptr;
    auto out = std::make_shared<Image>(adjusted.width(), adjusted.height());
    const int colours = colorModeColorChannels(mode);
    const int all = (1 << colours) - 1;
    auto colour = [&](int bits, uint8_t* o) {
        int r = 0, g = 0, b = 0;
        if (bits > 0) {
            if (bits == all) r = g = b = 255;
            else if (mode == ColorMode::RGB) { r = bits & 1 ? 255 : 0; g = bits & 2 ? 255 : 0; b = bits & 4 ? 255 : 0; }
            else if (mode == ColorMode::CMYK) {
                // An ink at its extreme shows as its RGB complement's channel; the black plate alone as gray.
                r = bits & 1 ? 255 : 0; g = bits & 2 ? 255 : 0; b = bits & 4 ? 255 : 0;
                if (!(bits & 7)) r = g = b = 128;
            } else {
                // Lab: lightness as gray, a as magenta, b as yellow.
                if (bits & 1) r = g = b = 255;
                else { r = 255; g = bits & 4 ? 255 : 0; b = bits & 2 ? 255 : 0; }
            }
        }
        if (bits < 0) r = g = b = 0;
        if (!whitePoint) { r = 255 - r; g = 255 - g; b = 255 - b; }
        o[0] = uint8_t(r); o[1] = uint8_t(g); o[2] = uint8_t(b); o[3] = 255;
    };
    auto run = [&](const auto& typed, double full) {
        const int n = channelsOf(typed);
        for (int y = 0; y < typed.height(); y++) {
            const auto* p = typed.row(y);
            uint8_t* o = out->row(y);
            for (int x = 0; x < typed.width(); x++, p += n, o += 4) colour(clippedBits(p, colours, full, whitePoint), o);
        }
    };
    if (adjusted.c8()) run(*adjusted.c8(), 255);
    else if (adjusted.u8()) run(*adjusted.u8(), 255);
    else if (adjusted.u16()) run(*adjusted.u16(), double(one16));
    else if (adjusted.f32()) run(*adjusted.f32(), 1);
    return out;
}

} // namespace compositor
