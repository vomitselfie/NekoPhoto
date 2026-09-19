// Magic Wand: which pixels of a premultiplied RGBA image match the colour at a seed. The contract is
// wand_mask's in Compositor/Rendering/WandPixels.h (the reference this is held to): the reference colour
// is the rounded average over a (2 * radius + 1)^2 square around the seed, a pixel matches when every
// channel, alpha included, is within `tolerance` of it, and contiguous fills 4-connected from the seed.
#pragma once
#include "image.h"

namespace compositor {

/// Writes 255 for selected and 0 elsewhere into `mask` (the image's size) and returns the count selected.
/// Every pixel is compared once, in a branch-free range test the compiler vectorises; the contiguous fill
/// then walks the match map rather than re-testing colours.
long wandMask(const Image& pixels, int seedX, int seedY, int radius, int tolerance, bool contiguous, GrayImage& mask);

} // namespace compositor
