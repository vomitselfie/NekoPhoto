// Adjustments and filters on a CMYK or Lab document's own samples (docs/color-modes.md, "Adjustments and filters").
//
// Nothing here goes through RGB: a CMYK pixel's inks and a Lab pixel's L, a and b are changed as stored. Colours the
// settings hold as sRGB (a Gradient Map's ends, a Photo Filter's colour) are converted through the document's profile
// once; where a kind decides on brightness (Threshold, Gradient Map) a CMYK pixel's lightness is read through the
// profile, for deciding only. The layouts are the documents': 8-bit CMYK `ImageC8` (5 samples), 8-bit Lab `Image`,
// 16-bit `Image16` with 5 (CMYK) or 4 (Lab) samples.
#pragma once
#include "adjustments.h"
#include "colorprofile.h"
#include "filters.h"

namespace compositor {

/// Whether Image > Adjustments and adjustment layers apply `kind` in a document of `mode` (every kind in RGB). The
/// kinds Photoshop offers there (adjustmentOfferedInMode) but Color Lookup, whose tables are RGB.
bool adjustmentAppliesInMode(AdjustmentKind kind, ColorMode mode);

/// Applies `settings` to a buffer at a CMYK or Lab layout, in place; false (the pixels untouched) for a kind that does
/// not apply in the mode. `profile` is the document's (the CMYK profile; ignored in Lab).
bool applyAdjustmentMode(const AdjustmentSettings& settings, ImageC8& cmyk, const ColorProfile& profile);
bool applyAdjustmentLab(const AdjustmentSettings& settings, Image& lab);
bool applyAdjustmentMode(const AdjustmentSettings& settings, Image16& image, ColorMode mode, const ColorProfile& profile);
/// A copy of `image` (at `mode`'s layout) adjusted, or null when the kind does not apply or the layout is not the mode's.
AnyImage adjustedInMode(const AdjustmentSettings& settings, const AnyImage& image, ColorMode mode, const ColorProfile& profile);

/// Levels' histograms of a buffer at `mode`'s layout, weighted by `coverage` (on its grid, or none): the composite (the
/// inks' mean in CMYK; empty in Lab, which has none) and each channel as stored (C, M, Y, K; L, a, b in slots 1 to 3),
/// 256 bins each.
std::array<std::vector<double>, 5> levelsHistogramInMode(const AnyImage& image, ColorMode mode, const AnyGray& coverage = {});

/// Color Balance on a CMYK buffer's stored cyan, magenta and yellow as red, green and blue (black kept): the RGB
/// kernel's maths (adjustments_more.cpp).
void applyColorBalanceStoredCmy(ImageC8& image, const ColorBalanceSettings& settings);
void applyColorBalanceStoredCmy(Image16& image, const ColorBalanceSettings& settings);

/// Whether the Filter menu's `kind` runs in a document of `mode`: all four built-in filters do.
bool filterAppliesInMode(FilterKind kind, ColorMode mode);
/// A copy of `image` (at `mode`'s layout) filtered: the blurs and Lens Correction on every sample as the RGB kernels
/// weigh them, Add Noise on each ink (monochromatic: the same amount on every ink) or on L, a and b (monochromatic:
/// L alone). Null when the layout is not the mode's.
AnyImage filteredInMode(FilterKind kind, const AnyImage& image, ColorMode mode, const FilterSettings& settings, double scale = 1, uint32_t seed = 0);

// Helpers over any depth and layout (4 or 5 samples), for the pixel edits of every mode.
/// growImage for any buffer: padded by `margin` transparent pixels, the placement kept.
AnyImage growImageAny(const AnyImage& image, const LayerTransform& transform, int margin, LayerTransform& grownTransform);
/// selectionInGrid for coverage of any depth.
AnyGray selectionInGridAny(const AnyGray& selection, const Affine& pixelToDocument, int width, int height);
/// coverage x adjusted + (1 - coverage) x original for any layout; `adjusted` and `original` of the same layout and
/// `coverage` at their depth. Returns the blend (a new buffer), or `adjusted` when they do not match.
AnyImage blendThroughCoverageAny(const AnyImage& adjusted, const AnyImage& original, const AnyGray& coverage);

} // namespace compositor
