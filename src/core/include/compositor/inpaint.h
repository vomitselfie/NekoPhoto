// Content-aware fill: texture for a hole synthesised from the known pixels around it, so edges and
// patterns continue across it. Wexler, Shechtman & Irani's coherent synthesis (2007) driven by
// PatchMatch nearest-neighbour fields (Barnes et al. 2009), coarse to fine with patch voting; patches
// match on colour and gradient, the field's dominant offsets (He & Sun 2012) are offered everywhere so
// repeating patterns stay in phase, and several random starts compete at the coarsest level.
#pragma once
#include "image.h"
#include <cstdint>

namespace compositor {

struct InpaintOptions {
    int patchRadius = 2;      // 5x5 patches
    int iterations = 6;       // nearest-neighbour passes per pyramid level
    int seeds = 3;            // random starts at the coarsest level; the best-explaining field goes on
    uint32_t seed = 1;
};

/// Fills the pixels of `image` where `hole` is nonzero from its opaque, unselected pixels. Returns false when
/// there is nothing to copy from (no fully known patch anywhere), leaving the image untouched. `visible`, when
/// given at the image's size, limits the sources to the pixels it marks (128 and up): what a layer mask hides
/// is not copied from.
bool contentFill(Image& image, const GrayImage& hole, const InpaintOptions& options = {}, const GrayImage* visible = nullptr);

} // namespace compositor
