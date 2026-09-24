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
alone.)

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
