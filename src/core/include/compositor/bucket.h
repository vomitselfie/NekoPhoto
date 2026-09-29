// The Paint Bucket's choice of pixels in a CMYK or Lab document (docs/color-modes.md, "Painting"): on the document's own
// samples, as Photoshop's bucket compares each channel. The contract is the Magic Wand's (wand.h) with the seed's colour
// taken at one pixel: a pixel matches when every sample, alpha included, premultiplied as stored, is within `tolerance`
// 8-bit levels of the seed's (at 16 bits the same fraction of 32768), and contiguous fills 4-connected from the seed.
#pragma once
#include "image.h"
#include "imaget.h"

namespace compositor {

/// Writes 255 for chosen and 0 elsewhere into `mask` (the image's size) and returns the count chosen; 0 when the seed
/// is outside. Any layout: 4 or 5 samples, 8 or 16 bits (not 32).
long bucketMask(const AnyImage& image, int seedX, int seedY, int tolerance, bool contiguous, GrayImage& mask);

} // namespace compositor
