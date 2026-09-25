# The edge-aware Magic Wand

Milestone 1 of the Smart Wand research plan (`NekoPhoto_Smart_Wand_2026_Research_Plan.md`, beside the
repository). The code is `src/core/src/smartwand.cpp`; the benchmark `tests/wand_bench.cpp`.

## What it does

A contiguous wand click builds a colour model from a small patch around the click (mean and spread in
OKLab), then computes, once, the cost of reaching every pixel from it:

- the distance from the click's colour, less one and a half times the patch's own spread, counted at 70%;
- the change from the neighbouring pixel (times 8), less twice the patch's spread, so shading passes a
  little at a time while a boundary is one big step, and a textured click steps over its own texture;
- with a textured click, how much smoother a neighbourhood is than the click's (texture does not run into
  flat colour of the same mean);
- the alpha difference;
- where the click is textured at a coarser scale (about 20 and 36 pixel windows, kept on a grid of 4 x 4
  cells), a second way in: a pixel whose colour is near one of the two colours the click's neighbourhood is
  made of (a 2-means over the window: cloth and grid, a weave's two threads) and whose own neighbourhood
  matches the click's in mean and spread (a Gaussian stand-in for comparing colour distributions) costs only
  that mismatch. A grid-lined piece of a texture atlas is taken whole, and the ground around it is not.

Right after a click, Shift-click adds a click that selects and Alt-click one that keeps out; each has its own
colour model and field, and a pixel is selected when its cheapest selecting cost is within tolerance and
below its cheapest keep-out cost, so the clicks compete for pixels (Milestone 2). They and tolerance changes
re-evaluate the one Magic Wand step.

A pixel's cost is the worst step on its best path from the click (a bottleneck path, found with a bucket
queue), in the classic wand's tolerance units. Tolerance only thresholds this field (up to 32 as it is, then
growing with the square, about 2,000 at 255: `wandCost`), with a two-level soft
band for antialiasing, so changing the tolerance right after a click re-selects at once from the same field
and replaces that Magic Wand step. The field is computed to twice the tolerance (at least 64) and again
only if the tolerance goes past that.

Because edges cost so much more than shading, a clean region stays the same over a wide stretch of tolerance;
that is what the benchmark's "range" column measures, and early testing found it read as the slider doing
nothing. So tolerance grows faster above 32 (the upper half reaches through the stronger boundaries: on the
texture atlas, 12 to 64 keep the sock, 80 adds its neighbours, 100 most of the sheet), and every change says in
the status bar how many pixels are selected and at what tolerance the selection grows next. A click on a folder
or an empty layer says why nothing was selected.

The tool's options bar has Edge Aware (on); off, or with Contiguous off, the classic per-channel wand runs.
`selection.wand` takes `edgeAware` over automation.

## Benchmark

`build/tests/wand_bench` scores nine synthetic scenes with known answers (pixels within a pixel of a true
boundary are not scored): a flat fill inside antialiased line art, the same with noise, two colours 20 levels
apart, JPEG-like blocks on those, a shaded region, soft skin on a similar ground, texture on its own mean
colour, a small region, and a strongly shadowed object. Means over the nine:

| Method | IoU at tolerance 32 | Best IoU | Tolerance range with IoU ≥ 0.95 |
|---|---:|---:|---:|
| Classic wand | 0.639 | 0.908 | 66 |
| OKLab distance from the click | 0.673 | 0.903 | 56 |
| + multi-scale edge barrier | 0.672 | 0.906 | 58 |
| Additive (geodesic sum) + edges | 0.605 | 0.874 | 60 |
| + texture term | 0.673 | 0.978 | 57 |
| **Neighbour steps ×8, click distance ×0.7, texture (Milestone 1)** | **0.860** | **0.996** | **137** |

A tenth scene, a grid-lined piece of a texture atlas on grey, was added for Milestone 3. Over all ten:

| Method | IoU at 32 | Best IoU | Range |
|---|---:|---:|---:|
| Classic wand | 0.577 | 0.855 | 59 |
| Milestone 1 | 0.776 | 0.899 | 123 |
| **+ coarse region statistics (shipped)** | **0.868** | **0.997** | **144** |

The other nine scenes score exactly as before: the coarse term only switches on for clicks textured at a
coarser scale. On the real texture atlas a click on a grid-lined sock takes the sock and stops at its stripes
and outline (before: one grid cell).

The shipped variant is right at the default tolerance where the classic wand is not (two near colours with a
sharp edge, a small region, a shadowed object), reaches every answer at some tolerance, and is right over
twice the tolerance range, so the slider is much easier to set. It matches the classic wand on line-art fills
(it still reaches antialiased lines). What did not earn its place: the multi-scale edge barrier (the neighbour
step is a sharper edge signal; the blurred scales stop fills short of line art), and summing costs along the
path (a long walk through gentle shading costs as much as crossing an edge). `wand_bench dump DIR` writes
every scene and selection; `wand_bench time IMAGE.png` times the preparation and a click.

On a real character sheet, a click on a black top selects the top where the classic wand also runs along every
connected black outline (11,219 pixels against 29,054). A 4096 × 4096 image takes about 150 ms to prepare
(once per layer state; about 240 ms with the coarse statistics) and 100 to 500 ms per click.

## Clearing a background around line art in one click

The case this was built for: an AI render or a painting with a flat or textured background, lines that are
smudged or antialiased, and the background to go. Selecting the background with the wand and deleting it
leaves the lines' fringe (pixels half line, half background) whole, a rim of the background's colour; the
workaround was to expand the selection by 2, smooth it by 3 and delete, which eats into the lines and still
leaves the rim.

Refine Edge (on) unmixes the wand's edge (`refineWandEdge`): within 3 pixels of it, each pixel is taken as
a mix of the selected colour nearby and the line's colour, pixel = a × background + (1 − a) × line, and
is selected by a. The line's colour is the most different colour nearby, except where that is itself a
faint stretch of the line (it lies on the way from the background to the line's colour over the whole
edge): then the whole edge's line colour is used, so thin and smudged lines unmix against black, not
against a dark blue. Delete right after that selection (`clearDecontaminated`) gives what is left the
line's colour at the alpha that is left: no rim.

`wand_bench edges` scores this on smudged black line art on blue, one click on the background, at
tolerance 32 (alpha error against the true coverage, the share of line cores eaten, and blue left on the
cleared side):

| Flow | Alpha error | Line eaten | Blue left |
|---|---:|---:|---:|
| Wand, delete | 0.458 | 0% | 83 |
| Wand, expand 2, smooth 3, delete | 0.444 | 12% | 81 |
| Wand with refined edge, delete | 0.082 | 0% | 29 |
| **Wand with refined edge, clean delete** | **0.082** | **0%** | **2.9** |

The clean delete applies when the selection is still the refined wand selection and the active layer
covers the canvas pixel for pixel (an opened image); otherwise Delete clears as before.

## One click for a background in many pockets: Contiguous off

A background cut into pockets by hair and figures (the baked checkerboard of an AI character sheet) needs a
click per pocket when the wand is contiguous. With Contiguous off (and Edge Aware on) the wand takes every
region that looks like the click instead:

- a pixel can start the selection when it is close to one of the click's colours by the classic measure (the
  largest channel difference; perceptual distance is too lenient about tints without edges to hold it back:
  pale skin is close to white in OKLab), the click's colour or, for a textured click, either colour of the
  pattern (a 2-means over its neighbourhood), and when its neighbourhood looks like the click's at the coarse
  scale (a checker pocket shows both colours; an eye white beside a line does not; for a flat click, flat);
- the search then fills each such region out to its edges as a click inside it would, crossing the pattern's
  soft cell boundaries.

On the character sheet, one click in a corner at tolerance 32 takes the whole checkerboard, every pocket,
and none of the eye whites, bone charms, skin or highlights; the result holds from tolerance 16 to 64. With
Refine Edge and Delete, the outlines come out clean over any colour. A few tiny or oddly mixed pockets can
still be missed (a Shift-click takes each).

## Tests

`core_tests` covers the wand on tiny and transparent layers, a click in a grid, keep-out clicks, and the
unmixed fringe with the clean delete; `wand_bench check` runs the ten-scene benchmark in CI and fails if the
shipped wand falls below its recorded scores (IoU at 32 at least 0.86, best at least 0.99).

## Limits and next steps

- Same-coloured regions that touch (a black beanie and its black outline) join; no colour rule separates them.
- A pattern of more than two colours is modelled as two (the 2-means); patterns coarser than about 36 pixels
  are not seen as texture.
- The unmixing assumes two colours meet at an edge; where three do (a line between two background colours) the
  fringe is unmixed against the nearer pair only.
- Not done from the plan: superpixels for very large canvases (4; a 4096 x 4096 click takes 0.1 to 0.5 s,
  so it has not been needed). Boundary matting (5) is the refined edge above, for line art; hair and fur in
  photographs are Remove Background's job.
