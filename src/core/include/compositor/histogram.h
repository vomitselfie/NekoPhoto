// Photoshop's Histogram panel and the clipping display of Levels and Curves (docs/features.md).
//
// The panel counts the same 256 bins as the Levels histogram (levelsHistogram, levelsHistogramInMode) whatever the
// depth: 16-bit samples rounded to 8 bits, 32-bit colour encoded at exposure 0 through the document's curve. A
// cache level above 1 counts a reduced copy, one pixel in 2^(level-1) each way, as Photoshop's cached histogram does.
#pragma once
#include "adjustments.h"
#include "colormgmt.h"
#include "colormodes.h"
#include "colorprofile.h"
#include "document.h"
#include "imaget.h"
#include <array>
#include <memory>
#include <vector>

namespace compositor {

/// The bins of one image at its mode: slot 0 the composite (RGB: the three channels together; CMYK: the four inks
/// together; empty in Lab, which has none), slots 1 to 4 the channels as stored (R, G, B; C, M, Y, K; L, a, b) and
/// in RGB the luminosity (0.30 R + 0.59 G + 0.11 B, as Photoshop weighs it). 256 bins each, weighted by alpha.
struct ImageHistogram {
    ColorMode mode = ColorMode::RGB;
    std::array<std::vector<double>, 5> channels;
    std::vector<double> luminosity;
    /// The cache level counted (1: every pixel).
    int cacheLevel = 1;
    bool empty() const { return channels[1].empty(); }
};

/// The histogram of `image` (RGB at any depth, or a CMYK or Lab buffer at `mode`'s layout); `curve` encodes 32-bit
/// colour. `cacheLevel` above 1 counts every 2^(level-1)-th pixel each way.
ImageHistogram imageHistogram(const AnyImage& image, ColorMode mode, const TransferCurve& curve, int cacheLevel = 1);

/// The document's composite at its own depth and mode, `scale` output pixels per document pixel: what the panel's
/// Entire Image counts (a cache level renders at 1 / 2^(level-1), through the layers' mips).
AnyImage renderComposite(const Document& document, double scale);

/// The smallest cache level at which a `width` x `height` image counts no more than `budget` pixels.
int histogramCacheLevel(int width, int height, double budget);

/// Every `step`-th pixel each way (nearest, no filtering): what a cache level counts.
AnyImage subsampled(const AnyImage& image, int step);

/// The statistics the panel shows for a set of bins: Mean, Std Dev, Median and Pixels (the weights' sum).
struct HistogramStats {
    double mean = 0, stdDev = 0, pixels = 0;
    int median = 0;
};
HistogramStats histogramStats(const std::vector<double>& bins);
/// Count of levels `from`..`to` and the percentage of pixels at or below `to` (Photoshop's Count and Percentile).
double histogramCount(const std::vector<double>& bins, int from, int to);
double histogramPercentile(const std::vector<double>& bins, int to);

/// What the clipping display decides on: `settings` with Levels' output range opened to 0..255 (the display shows
/// what the input sliders clip); other kinds unchanged.
AdjustmentSettings clippingSettings(const AdjustmentSettings& settings);

/// `image` adjusted by `settings` at its own depth and mode (RGB at 8, 16 or 32 bits, CMYK or Lab), or null when the
/// kind does not apply there.
AnyImage adjustedAny(const AdjustmentSettings& settings, const AnyImage& image, ColorMode mode, const ColorProfile& profile, const TransferCurve& curve);

/// Photoshop's clipping display of an adjusted image: for the white point, black everywhere but the channels that
/// reach the top (white where every channel does); for the black point, white everywhere but the channels that reach
/// the bottom (black where every one does). CMYK inks show as their RGB complements, Lab's lightness as gray and a
/// and b as magenta and yellow. Opaque 8-bit RGBA, transparent pixels as the background.
std::shared_ptr<Image> clippingDisplay(const AnyImage& adjusted, ColorMode mode, bool whitePoint);

} // namespace compositor
