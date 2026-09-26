# Content-Aware Scale

Edit > Content-Aware Scale (Ctrl+Alt+Shift+C) resizes the active layer by seam carving (Avidan & Shamir,
2007) instead of resampling: connected paths of low-detail pixels, one per row or column, are taken out to
make the layer narrower or shorter, or duplicated to make it wider or taller. Skies, walls and backgrounds
give way; faces, buildings and text keep their proportions.

- **Width / Height**: the new size in percent (10–300 %). The width is carved first, then the height.
- **Protect the selection**: the selected pixels are kept; seams go around them while any other path exists.
  (Photoshop's Protect menu takes a saved alpha channel; here the current selection plays that part.)
- The preview is carved from a reduced copy of the layer, so it updates as you type. OK carves the full
  layer as one undo step. The layer keeps its top-left corner.

## How it works

`compositor/seamcarve.h` (core, Qt-free). The energy is the gradient magnitude over RGBA (premultiplied), with
a large cost on protected pixels. Rather than one seam per cumulative-energy pass, each pass takes up to 1/32
of the current width in disjoint seams: the lowest-cost bottom ends first, each traced back through the
cumulative map while it stays clear of the seams already taken (a seam that would run into one is dropped).
Growing picks the lowest seams the same way (rounds of up to half the width) and doubles each, the copy
averaged with its right-hand neighbour. Energy, removal and insertion run in parallel by rows; the heights
are carved on a transposed copy.

On a 24-thread desktop a 4000 x 3000 image narrowed by 20 % takes about 1.6 s (`seamcarve_tests`).

Automation: `pixels.contentAwareScale` (`width`/`height` or `widthPercent`/`heightPercent`,
`protectSelection`); MCP tool `pixels_content_aware_scale`.
