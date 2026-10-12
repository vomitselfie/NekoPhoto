// Content-Aware Move (Photoshop's tool in the healing group): the selected pixels are lifted, the hole they
// leave is synthesised from its surroundings (inpaint.h), and the patch lands dx, dy away with its tone adapted to
// the new place (heal.h) and its rim resynthesised so the seam disappears. Extend leaves the original where it was.
#pragma once
#include "colormodes.h"
#include "image.h"
#include "imaget.h"
#include <cstdint>

namespace nekophoto {

struct ContentMoveOptions {
    bool extend = false;       // Photoshop's Extend mode: the source stays, only the copy lands
    /// Photoshop's Structure/Color adaptation folded into one: 0 very strict (the patch lands as it was, a thin
    /// seam blended) .. 4 very loose (its tone follows the new surroundings, a wide rim resynthesised).
    int adaptation = 2;
    uint32_t seed = 1;
    /// What an `Image` or four-channel `Image16`'s samples are: RGB, or Lab (L, a, b). CMYK is told by its five channels.
    ColorMode colorMode = ColorMode::RGB;
};

/// In place over premultiplied RGBA; `selection` (the image's size) marks what moves. Returns false when the
/// selection is empty, the offset is zero or lands wholly off the image, or the hole cannot be filled.
/// `visible` as for contentFill: what it hides is not copied from.
bool contentAwareMove(Image& image, const GrayImage& selection, int dx, int dy, const ContentMoveOptions& options = {},
                      const GrayImage* visible = nullptr);
/// The same at 16 bits: decisions on 8-bit masks and the image rounded to 8 bits, the moved, filled and blended
/// pixels at 16 bits.
bool contentAwareMove(Image16& image, const Gray16& selection, int dx, int dy, const ContentMoveOptions& options = {},
                      const Gray16* visible = nullptr);
/// In CMYK: five samples, the inks stored inverted, then alpha (8-bit `ImageC8`, and the Image16 overload above when it
/// has five channels). The hole is filled with the inks of the patches it copies (inpaint.h), the patch carries every
/// sample and its tone is adapted per ink by the Healing Brush's CMYK membrane (heal.h), at the image's own depth.
bool contentAwareMove(ImageC8& image, const GrayImage& selection, int dx, int dy, const ContentMoveOptions& options = {},
                      const GrayImage* visible = nullptr);

} // namespace nekophoto
