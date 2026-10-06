# Content-Aware Fill and Content-Aware Move

Both build on the same fill (`src/core/src/inpaint.cpp`: classic exemplar-based inpainting, Criminisi, Perez and
Toyama 2003) and on the healing membrane (`compositor/heal.h`). The fill works inward from the selection's edge,
most structured edge first; for each 9x9 patch it scores every candidate in a window of 96 pixels around it and copies
the best, then a membrane on the low-pass band evens out the tone. It is deterministic: the same selection gives the
same fill every time. It deliberately differs from Photoshop's fill (no patch rotation, scaling or mirroring, no
hand-painted sampling area); see [legal-boundaries.md](legal-boundaries.md).

## Content-Aware Fill

Edit > Content-Aware Fill… (Shift+F5) needs a selection on a visible pixel layer. The dialog shows the document
with the selection in red and the sampling area in green:

- **Auto** copies from a window around each patch: 96 pixels each way, so up to about 105 pixels past the
  selection. This is what `pixels.contentAwareFill` does by default.
- **Wide Area** scans a window of 192 pixels each way instead. It finds sources further away and is slower.

There is no hand-painted (Custom) sampling area: see [legal-boundaries.md](legal-boundaries.md).

What a layer mask hides is never copied from, whichever area is chosen. **Output To** fills the current layer or
puts only the filled pixels, by the selection's coverage, on a new layer above it. **Preview** shows the fill on
the canvas without committing it (the preview is always of the current layer).

Automation: `pixels.contentAwareFill` with `sampling` (`auto`, or `all` for Wide Area) and `output` (`current` or
`new`).

## Content-Aware Move

The healing tool (J) has Content-Aware Move as its last Type. Select what should move (any selection tool, or
press outside the selection and draw round it: the tool lassos), then drag the selection to where it should go.

1. **Move** fills the hole the selection leaves (grown by two pixels, so no fringe stays behind) from its
   surroundings; **Extend** leaves the original where it was.
2. The patch lands at the offset with its tone adapted to the new place by the healing membrane: fully at its
   edge, fading over a feather to the Adaptation's share in its middle (Very Strict none, Very Loose all).
3. A thin rim across the patch's edge is resynthesised from the new surroundings only, so the seam disappears
   without spreading the patch's old background along it. Rim and feather grow with the Adaptation and the
   patch's size.

The selection follows the patch, as in Photoshop, and the whole move is one undo step. The layer grows when the
patch lands past its edge. A tight selection works best: background brought along inside a loose selection
keeps its own colour in the patch's middle unless the Adaptation is loose.

Automation: `pixels.contentAwareMove` with `dx`, `dy`, `mode` (`move` or `extend`) and `adaptation` (0..4).
Core: `compositor::contentAwareMove` in `compositor/contentmove.h`, tested in `tests/heal_tests.cpp`.
