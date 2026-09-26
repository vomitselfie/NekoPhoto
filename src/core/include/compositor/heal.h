// Healing: filling a spot smoothly from its surroundings. The membrane (the smooth spread of the
// boundary values across the hole) is evaluated with mean-value coordinates over the polygon of known
// pixels around the hole (Farbman et al. 2009): no iterative solve, exact on linear ramps, milliseconds
// for a brush spot. Spot healing follows spot_heal in Compositor/Rendering/HealPixels.h (the reference) with this
// membrane in place of its relaxation sweeps.
#pragma once
#include "image.h"
#include <cstdint>

namespace compositor {

/// Interpolates `values` (`channels` floats per pixel) smoothly over the pixels where `hole` is nonzero
/// from the values just outside the hole (`known` nonzero there). Hole pixels whose whole boundary is
/// unknown get the mean of the known boundary values.
void membraneFill(float* values, int channels, const uint8_t* hole, const uint8_t* known, int width, int height);

/// Spot healing in place over premultiplied RGBA; `coverage` (the image's size) marks the spot.
/// Mode 0, Content-Aware, synthesises the spot from its surroundings (compositor/inpaint.h) so edges continue
/// through it; mode 1, Create Texture, fills smoothly and adds grain matching the detail around it; mode 2,
/// Proximity Match, copies the closest nearby patch whose surrounding ring matches the spot's. Copied texture
/// is membrane-blended to meet the surrounding tone, and the result replaces the original by coverage x opacity.
/// `visible`, when given at the image's size, marks the pixels that may be copied from or blended towards
/// (a layer mask's visible pixels, 128 and up): what it hides counts as unknown, like transparent pixels, so
/// a dab at the edge of a cut-out closes with the subject and not with the old background.
void spotHeal(Image& image, const GrayImage& coverage, float opacity, int mode, uint32_t seed, const GrayImage* visible = nullptr);

/// The Healing Brush and the Patch tool in place over premultiplied RGBA: `source` (the image's size, already
/// placed where it lands) is copied into the pixels `coverage` marks, its tone shifted to meet the pixels around
/// them (the difference along the edge membrane-filled across), and the result replaces the original by
/// coverage x opacity. `visible` as for spotHeal: what it hides is not blended towards.
void healFrom(Image& image, const Image& source, const GrayImage& coverage, float opacity, const GrayImage* visible = nullptr);

} // namespace compositor
