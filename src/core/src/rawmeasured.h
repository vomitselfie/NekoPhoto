// Camera Raw responses measured by RAWmakase (https://github.com/pch/rawmakase, MIT, LICENSES/RAWmakase-MIT.txt): the
// tables in src/third_party/rawmakase, fitted there to Camera Raw 18.7's renders of synthetic chart DNGs (its
// scripts/corpus/color-mixer.py, parametric-curve.py and color-grading.py; README.md beside the tables). The readers
// below are ports of its crates/rawmakase-engine/src/develop/color_mixer.rs, parametric.rs and color_grade_curves.rs at
// commit 7c19ab8d78fc91ebc18dd9514b4f83e405c27953.
//
// Every colour here is linear ProPhoto RGB in display light, where rawtone.h's ToneStage holds a colour between its
// steps; curves take and give values with the sRGB transfer, as the ToneStage's tone steps do.
#pragma once
#include <array>
#include <optional>
#include <vector>

namespace nekophoto::rawtone::measured {

/// One change per cell of the colour mixer's grid (36 hues x 6 saturations x 6 values of HSV in linear ProPhoto RGB):
/// hue shift in turns, log2 of the saturation factor, log2 of the value factor.
using Grid = std::vector<std::array<float, 3>>;

/// `rgb` (linear ProPhoto RGB) through a grid of changes, trilinear between the cells.
void applyGrid(const Grid& grid, double rgb[3]);

/// The colour mixer's bands, Saturation and Vibrance as one lookup. `bands` holds each band's hue, saturation and
/// luminance (index 0, 1, 2) as -1..1; `saturation` and `vibrance` are -1..1. A control left out (its `use` flag off)
/// takes no part, so the caller can keep its own response for it.
class ColorMixer {
public:
    ColorMixer(const std::array<std::array<double, 8>, 3>& bands, double saturation, double vibrance, bool useBands, bool useSaturation,
               bool useVibrance);
    bool active() const { return active_; }
    void apply(double rgb[3]) const;

private:
    bool active_ = false;
    Grid delta_;
    /// How far colours fade to their luminance after the lookup (Saturation below -50), and the lookup without
    /// Saturation whose result sets that luminance (none: the colour's own).
    double saturationGray_ = 0;
    std::optional<Grid> graySource_;
};

/// The Tone Curve's parametric curve (regions -1..1, splits 0..1 in order) at `size` + 1 points over 0..1 of encoded
/// values, made monotone. Empty when every region is 0.
std::vector<float> parametricCurve(const std::array<double, 4>& regions, const std::array<double, 3>& splits, int size);

/// Colour Grading as one gain curve per channel of linear ProPhoto RGB. Each wheel is {hue 0..1 (a turn), saturation
/// 0..1, luminance -1..1}, in the order Shadows, Midtones, Highlights, Global; Blending is 0..1, Balance -1..1.
class GradeCurves {
public:
    GradeCurves(const std::array<std::array<double, 3>, 4>& wheels, double blending, double balance);
    bool active() const { return active_; }
    void apply(double rgb[3]) const;

private:
    bool active_ = false;
    std::vector<std::array<float, 3>> gain_;
};

} // namespace nekophoto::rawtone::measured
