// The Filter menu's destructive filters on a layer's pixels: Gaussian Blur,
// Motion Blur, Add Noise, Lens Correction (plus Invert in adjustments.h).
// Blurs spread past the layer's edges: the caller grows the grid first
// (growForBlur) and trims the empty margin afterwards (trimToPixels).
#pragma once
#include "blur.h"
#include "document.h"
#include <cstdint>

namespace compositor {

enum class FilterKind { GaussianBlur, MotionBlur, AddNoise, LensCorrection };
const char* filterKindName(FilterKind kind);

struct FilterSettings {
    /// Gaussian Blur radius in layer pixels (the standard deviation), 0.1-250.
    double radius = 1;
    /// Motion Blur direction in degrees, counterclockwise from horizontal, -90..90.
    double angle = 0;
    /// Motion Blur streak length in layer pixels, 1-2000.
    double distance = 10;
    /// Add Noise strength as Photoshop's percentage, 0.1-400.
    double amount = 10;
    bool gaussian = false;
    bool monochromatic = false;
    /// Lens Correction's Remove Distortion, -100..100.
    double distortion = 0;
    FilterSettings normalized() const;
};

/// How far a blur reaches past the layer, in layer pixels.
double blurMargin(FilterKind kind, const FilterSettings& settings);

/// `image` placed at `transform`, padded by `margin` pixels of transparency on every side, with the transform
/// that keeps the pixels where they are. Fails (returns null) beyond the size limits.
std::shared_ptr<Image> growImage(const Image& image, const LayerTransform& transform, int margin, LayerTransform& grownTransform);
/// `image` cropped to its nonzero-alpha pixels, and the transform keeping them in place.
std::shared_ptr<Image> trimToPixels(const Image& image, const LayerTransform& transform, LayerTransform& trimmedTransform);

/// Runs a filter on premultiplied RGBA in place. `scale` is pixels in `image` per original layer pixel
/// (a downscaled preview blurs proportionally less); `seed` fixes Add Noise's pattern.
void applyFilter(FilterKind kind, Image& image, const FilterSettings& settings, double scale = 1, uint32_t seed = 0);

// gaussianBlur and motionBlur live in blur.h.

/// Selection coverage resampled into a layer's pixel grid (`pixelToDocument` maps the grid to the document).
std::shared_ptr<GrayImage> selectionInGrid(const GrayImage& selection, const Affine& pixelToDocument, int width, int height);
/// coverage x adjusted + (1 - coverage) x original, per pixel, into `adjusted`.
void blendThroughCoverage(Image& adjusted, const Image& original, const GrayImage& coverage);
void blendThroughCoverage(GrayImage& adjusted, const GrayImage& original, const GrayImage& coverage);

} // namespace compositor
