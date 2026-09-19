// Content-aware fill: texture for a hole synthesised from the known pixels around it, so edges and
// patterns continue across it. Wexler, Shechtman & Irani's coherent synthesis (2007) driven by
// PatchMatch nearest-neighbour fields (Barnes et al. 2009), coarse to fine with patch voting.
#pragma once
#include "image.h"
#include <cstdint>

namespace compositor {

struct InpaintOptions {
    int patchRadius = 2;      // 5x5 patches
    int iterations = 6;       // nearest-neighbour passes per pyramid level
    uint32_t seed = 1;
};

/// Fills the pixels of `image` where `hole` is nonzero from its opaque, unselected pixels. Returns false when
/// there is nothing to copy from (no fully known patch anywhere), leaving the image untouched.
bool contentFill(Image& image, const GrayImage& hole, const InpaintOptions& options = {});

} // namespace compositor
