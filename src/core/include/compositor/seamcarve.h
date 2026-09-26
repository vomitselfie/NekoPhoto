// Content-aware scaling by seam carving (Avidan & Shamir 2007): connected paths of low-energy pixels, one per
// row (or column), are removed to narrow an image or duplicated to widen it, so the parts that carry detail keep
// their proportions. Several disjoint seams come out of each cumulative-energy pass (the lowest ends first,
// traced back while they stay clear of the seams already taken), which makes a 12-megapixel image 20% narrower
// in about a second instead of the minutes of one seam per pass.
#pragma once
#include "image.h"

namespace compositor {

struct SeamCarveOptions {
    /// Pixels to keep (nonzero, the image's size): seams avoid them while any other path exists.
    const GrayImage* protect = nullptr;
    /// The share of the current width (or height) taken out in one pass: smaller is closer to one seam at a
    /// time, larger is faster.
    double passFraction = 1.0 / 32;
};

/// `image` (premultiplied RGBA) carved to `width` x `height`: seams removed where it shrinks, the lowest-energy
/// seams duplicated (each at most once per round, rounds of up to half the size) where it grows. Width first,
/// then height. Returns an empty image for a size below 1 or above maxImageSide.
Image seamCarve(const Image& image, int width, int height, const SeamCarveOptions& options = {});

} // namespace compositor
