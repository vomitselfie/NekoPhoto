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

The strokes match: the same strokes painted by both builds differ only on the rim of the hard one, by the
eighth of a pixel every stamped tip already had (`large_stamped_dabs_match_the_general_path`).

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
