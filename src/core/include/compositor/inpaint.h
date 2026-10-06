// Content-aware fill: the hole is filled from the known pixels around it so edges and patterns continue across
// it, by classic exemplar-based inpainting (Criminisi, Perez & Toyama 2003; Efros & Leung 1999).
//
// Legal boundary (docs/legal-boundaries.md, "Content-Aware Fill"): the hole is filled from its boundary inward in a
// fixed, deterministic priority order, and every target patch takes the best source patch found by an EXHAUSTIVE
// scan of a bounded window of the same image at the working resolution (integer L1 distance over the target's
// known pixels plus a fixed per-pixel penalty on the source offset; first in scan order wins a tie). Each target's
// search is independent of every other patch's chosen offset. Never add: an offset field searched by propagation
// from neighbouring patches or by perturbing an offset (random search), iterated (US 8285055); candidate lists that
// are precomputed, pruned, or kept past their own target, or belief propagation over patch labels (US 8340463);
// patch optimisation under user feature constraints (US 8355592); a patch map computed at a reduced resolution and
// upsampled (US 9396530: no coarse-to-fine offset maps); learned object boundaries or per-object masks
// (US 10825148); sources from other images (US 9697595); patch rotation, scale or mirroring; gradient-domain colour
// adaptation (US 9058699); a user-painted sampling region. The tone match is the healing membrane (heal.h) on the
// low-pass band: boundary offsets interpolated across the hole, never composited gradients.
#pragma once
#include "image.h"
#include "imaget.h"
#include <cstdint>

namespace compositor {

struct InpaintOptions {
    int patchRadius = 4;      // 9x9 patches
    /// Half-size of the window scanned around each target patch. Every source patch in the window is scored.
    int searchRadius = 96;
    /// Added to a candidate's distance per pixel of Manhattan offset between the source and the target: a fixed
    /// geometric preference for nearby sources that keeps a texture's phase continuous across a large hole.
    int offsetPenalty = 2;
    /// Kept for callers that pass one; the fill is deterministic and reads no random numbers.
    uint32_t seed = 1;
    /// With `visible` given: the window widens to `wideSearchRadius` (Content-Aware Fill's "All" sampling), still
    /// scanned exhaustively. `visible` limits the sources either way.
    bool sampleWholeVisible = false;
    int wideSearchRadius = 192;
    /// The low-pass tone match after the fill (see contentFill).
    bool toneMatch = true;
};

/// Fills the pixels of `image` where `hole` is nonzero from its opaque, unselected pixels. Returns false when
/// there is nothing to copy from (no fully known patch anywhere near), leaving the image untouched. `visible`, when
/// given at the image's size, limits the sources to the pixels it marks (128 and up): what a layer mask hides
/// is not copied from.
bool contentFill(Image& image, const GrayImage& hole, const InpaintOptions& options = {}, const GrayImage* visible = nullptr);
/// The same at 16 bits: the fill order and the patches are chosen on the image rounded to 8 bits, and the hole takes
/// the 16-bit pixels they point at, so the copied texture keeps its 16 bits.
bool contentFill(Image16& image, const Gray16& hole, const InpaintOptions& options = {}, const Gray16* visible = nullptr);
/// With the decisions' masks at 8 bits (the hole nonzero, the sources 128 and up).
bool contentFill16(Image16& image, const GrayImage& hole, const InpaintOptions& options = {}, const GrayImage* visible = nullptr);

} // namespace compositor
