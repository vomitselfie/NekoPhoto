# Content-Aware Fill and Content-Aware Move

Both build on the same synthesis (`src/core/src/inpaint.cpp`: coherent PatchMatch synthesis, coarse to fine,
see the header of `compositor/inpaint.h`) and on the healing membrane (`compositor/heal.h`).

## Content-Aware Fill

Edit > Content-Aware Fill… (Shift+F5) needs a selection on a visible pixel layer. The dialog shows the document
with the selection in red and the sampling area in green:

- **Auto** copies from the neighbourhood the fill chooses itself: the selection with twice its size around it,
  64 to 384 pixels each way. This is what the fill always did, and what `pixels.contentAwareFill` does by default.
- **All of the Layer** copies from anywhere on the layer (up to 1024 pixels past the selection each way, to keep
  the pyramid bounded).
- **Custom** copies only from the area painted green. It starts as everything but the selection; the left button
  adds, the right button (or Alt) removes, at the Brush size.

What a layer mask hides is never copied from, whichever area is chosen. **Output To** fills the current layer or
puts only the filled pixels, by the selection's coverage, on a new layer above it. **Preview** shows the fill on
the canvas without committing it (the preview is always of the current layer).

Automation: `pixels.contentAwareFill` with `sampling` (`auto`, `all`, `custom`), `include` / `exclude` rectangle
lists for custom sampling, and `output` (`current` or `new`).

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
