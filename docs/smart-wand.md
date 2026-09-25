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
- the alpha difference.

A pixel's cost is the worst step on its best path from the click (a bottleneck path, found with a bucket
queue), in the classic wand's tolerance units. Tolerance only thresholds this field, with a two-level soft
band for antialiasing, so changing the tolerance right after a click re-selects at once from the same field
and replaces that Magic Wand step. The field is computed to twice the tolerance (at least 64) and again
only if the tolerance goes past that.

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
| **Neighbour steps ×8, click distance ×0.7, texture (shipped)** | **0.860** | **0.996** | **137** |

The shipped variant is right at the default tolerance where the classic wand is not (two near colours with a
sharp edge, a small region, a shadowed object), reaches every answer at some tolerance, and is right over
twice the tolerance range, so the slider is much easier to set. It matches the classic wand on line-art fills
(it still reaches antialiased lines). What did not earn its place: the multi-scale edge barrier (the neighbour
step is a sharper edge signal; the blurred scales stop fills short of line art), and summing costs along the
path (a long walk through gentle shading costs as much as crossing an edge). `wand_bench dump DIR` writes
every scene and selection; `wand_bench time IMAGE.png` times the preparation and a click.

On a real character sheet, a click on a black top selects the top where the classic wand also runs along every
connected black outline (11,219 pixels against 29,054). A 4096 × 4096 image takes about 150 ms to prepare
(once per layer state) and 100 to 250 ms per click.

## Limits and next steps

- Same-coloured regions that touch (a black beanie and its black outline) join; no colour rule separates them.
- The click's patch (3 to 11 pixels) cannot see a pattern coarser than itself: on a texture atlas with a grid,
  a click selects one grid cell. Milestone 3 (region statistics at larger scales) is where that belongs.
- Next, from the plan: positive and negative evidence (Alt-click to keep a region out, Milestone 2), region
  statistics and superpixels for large canvases (3 and 4), boundary matting (5).
