# Brush latency on Linux

`nekophoto --bench-brush <preset>` paints five strokes through the real canvas with synthetic mouse events
(one every 8 ms, 80 moves each) and prints, per stroke, how long the press takes to show its first paint,
the median and slowest move, and the release. Each time covers the event handler and the repaint that
follows, not the display server's frame pacing. `--bench-size WxH` sets the document (default 2000 × 2000),
`--bench-opaque` paints on the opaque image layer instead of a blank layer above it. It also reports after
how many events the first paint appears.

```sh
QT_QPA_PLATFORM=offscreen COMPOSITOR_WINDOW_SIZE=1400x1000 QT_SCALE_FACTOR=2.25 \
    ./build/src/app/nekophoto --bench-brush classic/dry_brush --bench-size 1086x1448
```

(`round` is the plain brush; use a scratch `XDG_CONFIG_HOME` and `XDG_DATA_HOME` to leave your own settings
alone.) `--bench-brush-size`, `--bench-hardness`, `--bench-eraser`, `--bench-zoom`, `--bench-moves` and
`--bench-reach` shape the stroke; `--bench-burst N` delivers N moves between two repaints, as a fast mouse
does, and the `moves:` line then gives the mean cost per move, which must stay under 1 ms to keep pace with
a 1000 Hz mouse.

## September 2026 pass

Measured on a 12-core laptop (Ryzen AI 9 HX 370) at a 2.25 display scale, the scale of its panel, with
the MyPaint Dry brush on a blank layer above a photo. Medians over five strokes; the first stroke after
launch in brackets.

| 1086 × 1448 document | Before | After |
|---|---:|---:|
| Paint appears | after the first move | on the press |
| Press | 16.7 ms (32) | 1.7 ms (2.7) |
| Move | 1.9 ms | 1.0 ms |
| Release | 14.2 ms | 3.6 ms |

| 4096 × 4096 document | Before | After |
|---|---:|---:|
| Press | 18–24 ms | 4–5 ms |
| Release | 13–17 ms | 5 ms |

What changed, largest effect first:

- **A MyPaint click paints.** Presets that place dabs only by distance travelled painted nothing until the
  pen moved; the press now gets one dab (`MyPaintStroke::strokeTo` lends a dab rate for that event).
- **Slow tracking catches up.** Presets with slow position tracking (Dry brush has 2.0) follow the pointer
  with a lag and only move on input, so with a mouse held still the paint stopped short of it. While the
  pointer rests, the last input repeats every 16 ms until `MyPaintStroke::settled()`.
- **No whole-view renders around a stroke.** A MyPaint press reported an empty changed area, which the
  canvas read as "everything"; the commit on release did the same. The press now reports only what it
  painted, and a release commits without rendering at all when the preview was exact (every brush but Spot
  Healing, on a layer at its own size on whole pixels): `documentChangedAsShown`.
- **No copy of the canvas image per paint.** Setting the pixel ratio on a copy of the cached view made Qt
  deep-copy it on every paint.
- **Smaller repaints.** Hovering with a brush repainted the whole view for the outline, and every layer
  change did too; now only the outline's old and new area, and layer changes only while a transform box is
  shown.
- **Cheaper stroke setup.** Image memory comes from calloc (zero pages cost nothing until written), large
  copies, crops, layer placement and the painted-area scan run on every core, a blank layer is neither
  copied nor scanned, and a layer smaller than the canvas is copied at its own size rather than the
  canvas's.
- **libmypaint warmed.** Its one-time setup (about 9 ms) runs shortly after the window appears
  (`warmBrushEngines`) instead of in the first press.

## Large strokes (September 2026)

A big brush or eraser swept across a long canvas trailed behind the pointer, further the longer the stroke.
Wayland delivers every pointer event (up to 1000 a second from a gaming mouse, and Qt does not merge them
there as it does on X11), and each one cost more than a millisecond, so events queued up. Two causes:

- **Every move rendered the view.** The canvas composited each event's changed area at once (4-5 ms at
  2.25x with a 400 px brush). It now notes the area and renders everything noted once per frame.
- **Brushes over about 510 px painted every dab pixel by pixel.** Stamped tiles now cover them too
  (quarter-pixel phases for hard tips, half-pixel for soft ones, whose rim hides it; built as needed), and
  large dabs and the recomposite behind them run on every core.

Mean cost per move with 16 moves per repaint, 40% hardness unless noted, 3000 x 15000 canvas at 50% zoom
and 2.25x:

| Brush | Before | After |
|---|---:|---:|
| 400 px | 1.55 ms | 0.65 ms |
| 400 px eraser | 0.90 ms | 0.55 ms |
| 800 px | 8.20 ms | 0.98 ms |
| 800 px hard | 8.37 ms | 1.02 ms |
| 800 px eraser | 5.65 ms | 0.77 ms |
| 1500 px | 19.28 ms | 2.09 ms |
| 1500 px eraser | 13.70 ms | 1.59 ms |

Zoomed out, a stroke in progress showed only its first dab, or a garbled version of itself, until the
view was zoomed in. Layers drawn below about half size come from reduced copies cached per image
(`MipCache`), and a stroke paints its working image in place under the same pointer, so the copies went
stale. The stroke engines now bring the cached levels up to date over what they changed
(`MipCache::refresh`, from `BrushStroke::takeDirtyRect` and `WarpStroke::takeDirtyRect`); Smudge and
Liquify also render only what they changed instead of the whole view. The test
`zoomed_out_render_follows_a_stroke_in_progress` fails without it.

The strokes match: the same strokes painted by both builds differ only on the rim of the hard one, by the
eighth of a pixel every stamped tip already had (`large_stamped_dabs_match_the_general_path`).

## Smoothing (September 2026)

`--bench-smoothing input|stabilizer|pulled|pressure|all` runs `--bench-brush` with that smoothing at 50% (`all`: the
three filters with Catch-Up On Stroke End; see [brush-engine.md](brush-engine.md#smoothing)). Medians of five
interleaved runs of 5 strokes of 200 moves, 1086 × 1448, offscreen, against the build before smoothing:

| ms | round: press | move p50 | move p95 | release | dry brush: press | move p50 | move p95 | release |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| before | 2.19 | 0.25 | 0.31 | 2.47 | 2.14 | 0.33 | 0.45 | 3.22 |
| off | 1.85 | 0.20 | 0.27 | 2.32 | 2.12 | 0.31 | 0.43 | 3.32 |
| input | 1.79 | 0.21 | 0.31 | 2.63 | 2.14 | 0.33 | 0.44 | 2.90 |
| stabiliser | 1.91 | 0.23 | 0.30 | 2.56 | 1.84 | 0.32 | 0.45 | 2.80 |
| pulled string | 1.89 | 0.20 | 0.31 | 2.29 | 1.62 | 0.20 | 0.37 | 2.25 |
| pressure | 1.91 | 0.25 | 0.31 | 2.86 | 2.32 | 0.36 | 0.46 | 3.42 |
| all | 2.20 | 0.28 | 0.38 | 3.38 | 1.95 | 0.34 | 0.46 | 3.61 |

Input smoothing adds 0.01–0.02 ms to a move's median, inside the run-to-run noise (the budget was 1 ms); the filters
themselves cost 0.1 µs a report (input) to 0.4 µs (all three), per `brush_smoothing_tests`. Pulled String paints
less, since the brush stays a string's length behind the pen, so its moves are the cheapest. What smoothing does add
is the lag it is there for: the brush trailing the pen, measured in pixels in brush-engine.md, not in milliseconds of
work.

## Viewing: `--bench-view`

`nekophoto --bench-view` builds a document (`--bench-size`, default 4096 × 4096) with a photo-like base and
four soft paint layers, then times a full view render, six zoom steps in, forty pan steps, six zoom steps
out, and the undo and redo of a brush stroke. At a 2.25 display scale on the same laptop:

| 4096 × 4096, 5 layers | Before | After |
|---|---:|---:|
| Pan step | 54 ms | 11 ms |
| Undo of a stroke | 31 ms | 12.5 ms |
| Redo of a stroke | 31 ms | 12.8 ms |
| Wheel zoom step | 59 ms | 3.2 ms |
| Full render | 40 ms | unchanged |

- **Panning scrolls the cached view.** A pan by whole device pixels (pans are now rounded to them) moves
  what the cache already holds and renders only the strip that came into view.
- **A wheel or pinch zoom shows the last render scaled** and renders afresh once the gesture pauses for
  120 ms (`zoomSettle_` in `CanvasWidget`); zooms from the menu or keyboard render at once.
- **Undo and redo render only what they change.** A raster edit notes its area in its history entry
  (`DocumentHistory::noteRegion`); other steps compare the two documents (`changedArea`), which falls back
  to the whole canvas for a new canvas size, a new stacking order, a folder or an adjustment layer.

## Typing: `--bench-type`

`nekophoto --bench-type` types three paragraphs of 44 px text on the canvas with the Type tool, through key
events, then a paragraph box of 30 px text that wraps, and prints how long each keystroke takes to reach the
screen: the text's layout and raster (the layer is redrawn in full on every keystroke), the canvas render of the
changed area and the repaint. With `--screenshot out.png` it grabs the window while a selection shows (and
`out-box.png` for the paragraph box). Offscreen at 1400 × 900 on the desktop, the third paragraph's keystrokes took
4.6 to 6.6 ms each (median; about a third of it the layer's layout and raster), a wrapping box 6.6 ms. It ends by
sending an input method's events (にほんご composed, converted to 日本語, committed, then です) and checks the
text shows inline while composing, commits once and undoes by the commit; `out-ime.png` grabs the conversion.

## Adjustment drags: `--bench-adjust`

`nekophoto --bench-adjust` builds the view bench's document (`--bench-size`, five layers) at a depth and mode
(`--bench-depth 8|16|32`, `--bench-mode rgb|cmyk|lab`), fits it to the window, times five full view renders, then
drags a Levels and an Exposure adjustment layer's slider for thirty ticks each (one edit, as the Properties panel
does) and prints each tick's time to reach the screen. Offscreen at 1400 × 900 on the desktop (median, ms; the
machine was shared, so the 8-bit rows show the noise):

| Document | View before | View after | Levels tick before | Levels tick after |
|---|---:|---:|---:|---:|
| 3840 × 2160, 8-bit RGB | 11.2 | 11.5 | 11.0 | 10.5 |
| 7680 × 4320, 8-bit RGB | 11.7 | 10.5 | 9.3 | 9.5 |
| 3840 × 2160, 16-bit RGB | 18.4 | 18.1 | 25.6 | 24.8 |
| 7680 × 4320, 16-bit RGB | 72.0 | 17.7 | 82.6 | 25.8 |
| 7680 × 4320, 16-bit CMYK | 148 | 33.5 | 140 | 35.4 |
| 7680 × 4320, 16-bit Lab | 76.2 | 19.2 | 78.3 | 22.0 |

At fit zoom every layer is drawn from a reduction (level 2 here). The mip cache kept the level 1 it was built
from as well, four times larger: at 16 bits an 8K layer's two levels take 83 MB (104 MB in CMYK), five layers
passed the cache's 400 MB budget, and every frame evicted and rebuilt the reductions from the full-size pixels.
Over budget the cache now releases the levels kept only as steps to a deeper one before it evicts whole images,
so the cost follows the view again. The reductions are the same pixels (a released level is rebuilt, and a stroke's
refresh worked through, by the same halvings), so the view is unchanged and peak memory is the same. A deep
document still costs about twice an 8-bit one at the same view size: that is the 16-bit compositing itself.

### Where a deep document's time went

Profiled with `perf` on one core over the whole `--bench-adjust` run (3840 × 2160, five layers, fit). In 8-bit RGB
the Catmull-Rom point sampler takes about 45 % of the time. At 16 bits it took 71 %, because it summed each channel
in 64-bit scalars. CMYK and Lab layers use the bilinear sampler, which ran its 4 or 5 samples one float at a time.
16-bit Normal compositing ran in 64-bit scalars too. 16-bit drawing worked out the edge antialias with two divisions
for every pixel, where 8-bit skips that inside the layer. On each slider tick, Levels, Curves, Exposure and similar
adjustments filled three 32769-entry tables on a single thread. In 16-bit CMYK, turning the frame into the screen's
RGB took over 40 % of the time: Little CMS's float pipeline (per-pixel tone curves with `pow`, a 4-D CLUT).

The changes below leave every output bit unchanged; render_hash_tests, brush parity and the golden tests pass as
they are.

| Change | Instructions, whole run (16 RGB / CMYK / Lab) |
|---|---|
| Before | 145.6 G / 121.4 G / 150.1 G |
| 16-bit bicubic in 32-bit lanes (each row's sum split into high and low halves, recombined exactly) | 92.1 G / 121.2 G / — |
| CMYK/Lab bilinear: four samples in float lanes, same operation order | — / 115.3 G / 136.8 G |
| 16-bit Normal source-over in 32-bit lanes; skip the edge formula inside the layer (with a 1/1000 px margin) | 81.7 G / 110.9 G / 133.7 G |
| 16-bit adjustment tables filled on the pool | the same count, off the critical path |

Medians (ms; best of three runs on a shared machine, 1400 × 900 offscreen):

| Document | View before | View after | Levels tick before | Levels tick after | Exposure tick before | Exposure tick after |
|---|---:|---:|---:|---:|---:|---:|
| 8-bit RGB | 8.2 | 8.4 | 6.9 | 7.1 | 7.1 | 7.1 |
| 16-bit RGB | 17.1 | 11.1 | 24.0 | 12.5 | 26.5 | 12.5 |
| 16-bit CMYK | 29.7 | 26.5 | 32.1 | 28.2 | — | — |
| 16-bit Lab | 23.0 | 18.6 | 24.1 | 20.6 | 23.5 | 20.8 |

16-bit RGB is now within about 1.5× of 8-bit, which is roughly what twice the bytes costs. CMYK and Lab are still
about 2.5–3× slower, and the extra time is the display conversion. An 8-bit CMYK frame goes through Little CMS's
precalculated 8-bit grid. A 16-bit frame goes through the exact float pipeline, chosen for precision, and that is
roughly 1000 instructions a pixel. A per-thread memo keyed on the pixel's samples gained nothing on photo-like
content and was dropped. Closing the gap would need a design change, such as a precalculated 16-bit display grid,
which changes the displayed bytes, or splitting the pipeline so the final 8-bit tone curve becomes a threshold lookup.
Either one changes what the screen shows, so it was left alone. 32-bit was not changed.

### The frame below the dragged layer is kept

The tick times above were measured when every tick drew every layer again. A drag now marks the adjustment layer as
being dragged (`LayerOverride::adjusting`, set by `EditorSession` while an adjustment edit is open), and the canvas's
`RenderCache` keeps the frame as it stands just below that layer, with any folders open around it (`render_resume.h`):
the first tick draws everything and keeps that state, and each later tick copies it back and draws on from there, the
adjustment pass and the layers above it. A clipped adjustment layer goes on from inside its clipping stack, so the
base and the layers clipped under it are not drawn again either. Frames are the same bytes as uncached ones at every
depth and mode (`adjustment_drag_tests`), and `work_counter_tests` and the window's `work_counters_selftest` bound the
draws a tick at the layers above the dragged one. What a tick still costs is the view render itself: copying the kept
frame, the adjustment pass, the layers above, and for a deep, CMYK or Lab document the conversion to the screen.
