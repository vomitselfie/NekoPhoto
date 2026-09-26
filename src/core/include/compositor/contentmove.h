// Content-Aware Move (Photoshop's tool in the healing group): the selected pixels are lifted, the hole they
// leave is synthesised from its surroundings (inpaint.h), and the patch lands dx, dy away with its tone adapted to
// the new place (heal.h) and its rim resynthesised so the seam disappears. Extend leaves the original where it was.
#pragma once
#include "image.h"
#include <cstdint>

namespace compositor {

struct ContentMoveOptions {
    bool extend = false;       // Photoshop's Extend mode: the source stays, only the copy lands
    /// Photoshop's Structure/Color adaptation folded into one: 0 very strict (the patch lands as it was, a thin
    /// seam blended) .. 4 very loose (its tone follows the new surroundings, a wide rim resynthesised).
    int adaptation = 2;
    uint32_t seed = 1;
};

/// In place over premultiplied RGBA; `selection` (the image's size) marks what moves. Returns false when the
/// selection is empty, the offset is zero or lands wholly off the image, or the hole cannot be filled.
/// `visible` as for contentFill: what it hides is not copied from.
bool contentAwareMove(Image& image, const GrayImage& selection, int dx, int dy, const ContentMoveOptions& options = {},
                      const GrayImage* visible = nullptr);

} // namespace compositor
