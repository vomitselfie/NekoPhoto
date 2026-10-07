// The Filter menu's destructive filters on a layer's pixels: Gaussian Blur,
// Motion Blur, Add Noise, Lens Correction (plus Invert in adjustments.h), and
// the filters drawn on the layer's grid at any depth and layout (applyGridFilter).
// Blurs spread past the layer's edges: the caller grows the grid first
// (growForBlur) and trims the empty margin afterwards (trimToPixels).
#pragma once
#include "blur.h"
#include "document.h"
#include "colormodes.h"
#include "imaget.h"
#include <cstdint>

namespace compositor {

class TransferCurve;

enum class FilterKind {
    GaussianBlur, MotionBlur, AddNoise, LensCorrection,
    // Drawn by the Smart Filter kernels (smartfilter.h), as Photoshop's filters of those names.
    BoxBlur, RadialBlur, SurfaceBlur, DustAndScratches, Median, UnsharpMask, HighPass, Emboss, Mosaic,
    // Ported from PhotoCraft (filters_photocraft.cpp).
    Twirl, Pinch, Spherize, Wave, Ripple, PolarCoordinates, ZigZag, Shear, Maximum, Minimum, Offset, Clouds, DifferenceClouds,
    FindEdges,
};
inline constexpr int filterKindCount = int(FilterKind::FindEdges) + 1;
const char* filterKindName(FilterKind kind);
/// Whether `kind` is one of applyGridFilter's (everything past Lens Correction).
constexpr bool isGridFilter(FilterKind kind) { return int(kind) > int(FilterKind::LensCorrection); }
/// Whether `kind` runs without a dialog, as in Photoshop (Clouds, Difference Clouds, Find Edges).
constexpr bool filterRunsDirectly(FilterKind kind) {
    return kind == FilterKind::Clouds || kind == FilterKind::DifferenceClouds || kind == FilterKind::FindEdges;
}

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
    bool bicubic = false;   // Lens Correction resamples bicubically instead of bilinearly (the Mac's way)
    FilterSettings normalized() const;

    // The grid filters' settings (applyGridFilter). `radius`, `angle` and `amount` above serve them too, in each
    // filter's own units and range (normalizedFor).
    /// Dust & Scratches 0-255, Surface Blur 2-255, Unsharp Mask 0-255 (levels).
    int threshold = 0;
    /// Emboss height in pixels, 1-100.
    int height = 3;
    /// Mosaic cell size in pixels, 2-200.
    int cellSize = 10;
    /// Radial Blur quality: 0 Draft, 1 Good, 2 Best.
    int quality = 1;
    /// The filter's choice: Spherize's mode (0 Normal, 1 Horizontal Only, 2 Vertical Only), Ripple's size (0 Small,
    /// 1 Medium, 2 Large), Polar Coordinates' direction (0 Rectangular to Polar, 1 Polar to Rectangular), ZigZag's
    /// style (0 Around Center, 1 Out From Center, 2 Pond Ripples), Wave's type (0 Sine, 1 Triangle, 2 Square),
    /// Minimum's and Maximum's Preserve (0 Squareness, 1 Roundness).
    int style = 0;
    /// Undefined Areas (Wave, Shear, Offset): 0 Wrap Around, 1 Repeat Edge Pixels, 2 Set to Transparent (Offset only).
    int undefinedAreas = 0;
    /// ZigZag's ridges, 0-20.
    double ridges = 5;
    /// Wave: generators 1-999, wavelength 1-999 and amplitude 1-999 pixels (minimum and maximum).
    int generators = 5;
    double wavelengthMin = 10, wavelengthMax = 120, amplitudeMin = 5, amplitudeMax = 35;
    /// Offset in pixels, right and down.
    int horizontal = 0, vertical = 0;

    /// Each filter's defaults (Photoshop's dialog defaults).
    static FilterSettings defaults(FilterKind kind);
    /// The settings held inside `kind`'s ranges (non-finite values take the defaults).
    FilterSettings normalizedFor(FilterKind kind) const;
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

/// What a grid filter needs beyond its settings.
struct GridFilterContext {
    /// The colour mode of the image's samples.
    ColorMode mode = ColorMode::RGB;
    /// The distortions' reference rectangle on the image's grid (Photoshop's: the selection's bounds); empty: the
    /// whole grid. The distortions, Offset and the Radial Blur change only inside it.
    int boundsX = 0, boundsY = 0, boundsWidth = 0, boundsHeight = 0;
    /// Where the grid's top-left pixel lies in the document, and the document's longest side (Clouds' pattern is
    /// fixed to the document, its scale to the document's size).
    int originX = 0, originY = 0, documentSide = 0;
    /// Clouds: the foreground and background colours, straight sRGB 0..1.
    double foreground[3] = {0, 0, 0}, background[3] = {1, 1, 1};
    /// Wave's and Clouds' pattern.
    uint32_t seed = 0;
};

/// How far a grid filter reaches past the layer, in layer pixels (the caller grows the grid by it).
int gridFilterMargin(FilterKind kind, const FilterSettings& settings);
/// Whether a grid filter grows past the layer's pixels, so the result is trimmed to them afterwards.
bool gridFilterTrims(FilterKind kind);
/// Runs a grid filter (isGridFilter) on `image` of any depth and layout (RGB 8, 16 or 32 bits; CMYK or Lab at 8 or
/// 16): a new buffer of the same size and layout, or null when the filter is not drawn at that depth and mode
/// (supports(): "filter.<name>"). The Smart Filter kernels draw the filters smartfilter.h names, the PhotoCraft
/// ports the rest.
AnyImage applyGridFilter(FilterKind kind, const AnyImage& image, const FilterSettings& settings, const GridFilterContext& context);
/// The selection's bounds on a grid: the nonzero coverage, as the context's bounds; false when nothing is covered.
bool coverageBounds(const AnyGray& coverage, GridFilterContext& context);

/// Selection coverage resampled into a layer's pixel grid (`pixelToDocument` maps the grid to the document).
std::shared_ptr<GrayImage> selectionInGrid(const GrayImage& selection, const Affine& pixelToDocument, int width, int height);
/// coverage x adjusted + (1 - coverage) x original, per pixel, into `adjusted`.
void blendThroughCoverage(Image& adjusted, const Image& original, const GrayImage& coverage);
void blendThroughCoverage(GrayImage& adjusted, const GrayImage& original, const GrayImage& coverage);

// The same at 16 bits (filters_u16.cpp): Add Noise draws the same pattern for a seed, Lens Correction the same taps.
std::shared_ptr<Image16> growImage(const Image16& image, const LayerTransform& transform, int margin, LayerTransform& grownTransform);
std::shared_ptr<Image16> trimToPixels(const Image16& image, const LayerTransform& transform, LayerTransform& trimmedTransform);
void applyFilter(FilterKind kind, Image16& image, const FilterSettings& settings, double scale = 1, uint32_t seed = 0);
std::shared_ptr<Gray16> selectionInGrid(const Gray16& selection, const Affine& pixelToDocument, int width, int height);
void blendThroughCoverage(Image16& adjusted, const Image16& original, const Gray16& coverage);
void blendThroughCoverage(Gray16& adjusted, const Gray16& original, const Gray16& coverage);

// The same at 32 bits (filters_f32.cpp), on premultiplied linear float: the blurs average light as it is, Add Noise
// adds the same pattern to the colour encoded through `curve` (the document's, encodedTransfer), Lens Correction
// resamples with exact weights. Coverage blends in float.
std::shared_ptr<ImageF> growImage(const ImageF& image, const LayerTransform& transform, int margin, LayerTransform& grownTransform);
std::shared_ptr<ImageF> trimToPixels(const ImageF& image, const LayerTransform& transform, LayerTransform& trimmedTransform);
/// The same over any depth and layout (4 or 5 samples; pixels_any.cpp); `empty` set when no pixel has alpha.
AnyImage trimToPixelsAny(const AnyImage& image, const LayerTransform& transform, LayerTransform& trimmedTransform, bool* empty = nullptr);
/// Layer Mask > Apply over any depth and layout: every sample times the mask (`mask` at the image's size, or 1 x 1) at
/// the depth's own rounding; null when the mask is not at the image's depth.
AnyImage applyMaskAny(const AnyImage& image, const AnyGray& mask);
void applyFilter(FilterKind kind, ImageF& image, const FilterSettings& settings, const TransferCurve& curve, double scale = 1, uint32_t seed = 0);
std::shared_ptr<GrayF> selectionInGrid(const GrayF& selection, const Affine& pixelToDocument, int width, int height);
void blendThroughCoverage(ImageF& adjusted, const ImageF& original, const GrayF& coverage);
void blendThroughCoverage(GrayF& adjusted, const GrayF& original, const GrayF& coverage);

} // namespace compositor
