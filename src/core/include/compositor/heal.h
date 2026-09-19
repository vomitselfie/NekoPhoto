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
void spotHeal(Image& image, const GrayImage& coverage, float opacity, int mode, uint32_t seed);

} // namespace compositor
