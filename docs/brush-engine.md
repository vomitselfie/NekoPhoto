# The brush engine

How a stroke gets from the pen to the pixels, what the brush dynamics are, how imported brushes land in them, and the
harness that measures all of it. The code: `src/core/include/compositor/brushsample.h`, `brushsmoothing.h`, `brushdynamics.h`,
`tipbrush.h`, `brush.h` (the round tip and the stroke's grid), `mypaint.h`, the importers in `src/core/src/abr.cpp`,
`procreate.cpp` and `sut.cpp`, and the harness in `tests/brush_harness.*`.

```
pointer events ──> BrushSample (raw) ──> smoothing (optional) ──> BrushSampleTrack (derived) ──> engine
                                                                        ├─ round tip: the position
                                                                        ├─ tip brushes: dynamics ──> dabs ──> coverage
                                                                        └─ MyPaint: pressure, tilt, time (stroke_to)
```

## Samples

`BrushSample` is one pointer event, the same for every engine.

Raw, as the device reported it (`CanvasWidget::tabletEvent`, or `mouseSample` for a mouse):

| Field | Meaning |
|---|---|
| `position` | document pixels |
| `time` | seconds; only differences matter |
| `pressure` | 0..1; a mouse reports 0.5 |
| `tiltX`, `tiltY` | degrees from upright, as Qt reports them (about ±60) |
| `twist` | the barrel's rotation in degrees (Qt's `rotation()`) |
| `tangentialPressure` | the airbrush wheel, -1..1 |
| `stylus` | the pressure, tilt and twist are a pen's; false for a mouse |
| `eraser` | the pen's eraser end |
| `twistReported` | the pen reports its barrel's twist (Qt's Rotation capability; in a recorded stroke, a sample with `twist`) |
| `viewScale` | screen points per document pixel where the stroke was drawn: the canvas's zoom (`Viewport::pointsPerPixel`), 1 at 100% and in a replay that does not say |

Nothing is folded in at capture: pressure is not turned into size there, and tilt stays in degrees. Each engine reads
what it uses.

`BrushSampleTrack` derives the rest from the samples before:

| Field | How |
|---|---|
| `dt` | seconds since the previous sample; 1/120 when the times do not say (the first sample, equal or missing times) |
| `speed` | document pixels per second, smoothed with a 20 ms time constant |
| `screenSpeed` | screen points per second: each step times its `viewScale`, smoothed the same way; equal to `speed` at 100% |
| `acceleration` | of that speed |
| `direction` | radians of travel, unwrapped |
| `tiltMagnitude` | 0 upright to 1 at 60 degrees or more |
| `tiltAzimuth` | radians the pen leans towards, unwrapped; kept while the pen is upright |
| `twistAngle` | the twist in radians, unwrapped |
| `distance` | document pixels since the stroke began |
| `progress` | 0..1 along the stroke when its whole length is known (`deriveStroke`, a replay); -1 while painting |

**Two speeds.** `speed` is measured in the document, so the same hand motion reads four times as fast at 400% as at
100%; `screenSpeed` is measured on the screen and reads the same at any zoom. Native brushes keep the document's
(Speed, and Mouse speed as pressure); Procreate's speed settings read the screen's (ScreenSpeed), as Procreate measures
them. The view has no rotation, and a rotated or flipped view would not change either speed, since it keeps lengths:
only the scale enters.

Angles are unwrapped (`unwrapAngle`) so a twist going from 179 to -179 degrees reads as two degrees of turn, not 358.
`interpolate` blends two samples along the unwrapped values, which is what a tip brush uses between events.

Qt delivers tablet events one by one (it does not coalesce them unless `AA_CompressTabletEvents` is set), so every
report reaches the brush; there is no platform batch to unpack.

A recorded stroke is JSON (`recordedStrokeFromJson`, `recordedStrokeToJson`):

```json
{"format": "nekophoto-stroke", "version": 1, "stylus": true,
 "samples": [{"t": 0, "x": 100, "y": 100, "pressure": 0.1, "tiltX": 0, "tiltY": 0, "twist": 0, "tangentialPressure": 0, "viewScale": 2}]}
```

(`viewScale` is written only when it is not 1.)

A bare array of samples also reads; missing fields take their defaults, and a stroke with `"stylus": false` reads with a
mouse's neutral values whatever it holds.

## Smoothing

`BrushStabilizer` (`brushsmoothing.h`) is its own stage between the pen's raw samples and the stroke. Raw samples go in;
the samples the stroke follows come out, none or several per report, and only then does `BrushSampleTrack` derive speed,
direction and the rest from them, so every engine and every dynamic reads the smoothed motion:

```
raw sample ──> input smoothing ──> pressure smoothing ──> stroke stabiliser ──> BrushSampleTrack ──> dynamics ──> dabs
```

Three controls, each with its own filter and constants, since position, pressure and angles are different signals
(`BrushSmoothing`; every amount 0..100, all 0 by default):

- **Input smoothing** (`input`): a One Euro filter on the position. Its cutoff at rest falls from 25 Hz at 1% to 1 Hz
  at 100% and rises by 0.05 Hz per screen point a second of the pen's speed (smoothed at 1 Hz), so a resting pen is
  held still and a moving one is followed closely. The tilt and the twist are filtered here too, with a slower cutoff
  of their own (12 Hz down to 0.6 Hz, no speed term) and with circular maths: the tilt as a vector, so its azimuth
  turns the short way round, and the twist on its unwrapped angle, so 359 and 1 degrees average to 0, not 180.
- **Pressure smoothing** (`pressure`): a one-pole low pass on the pressure and the airbrush wheel, with a time
  constant of up to 120 ms. It leaves the position alone, and the position filters leave the pressure alone.
- **The stroke stabiliser** (`stabilizer`, Photoshop's Smoothing): the brush trails the pen. Its reach is the amount in
  screen points (100 at 100%). Stepped every 2 document pixels of the pen's travel, so its curve does not depend on the
  report rate (60 and 240 reports a second of the same S curve stay within 0.26 pixels).

The stabiliser's options follow Photoshop's Smoothing options. The names and what each does are Adobe's; the
mechanics behind them are not documented, so each is marked with how it was read:

| Option | What it does here | Confidence |
|---|---|---|
| Smoothing 0..100 | With Stroke Catch-Up, the brush closes on the pen with a time constant of `0.25 s × (amount/100)²`, so it lags further the faster the pen moves | weakly inferred: Photoshop's curve is unknown; the constants are synthetic-only |
| Pulled String Mode (off) | The brush stays put while the pen moves within the reach and is dragged behind it, the string taut, once the pen goes further; a pen wandering inside the string leaves no mark | strongly inferred from Adobe's description ("paints only when the string is taut") |
| Stroke Catch-Up (on) | The brush keeps closing on a paused pen (the canvas ticks the stabiliser at 60 a second while the pen is still). Off, the brush moves only as the pen does, by `1 − e^(−step/reach)` of the gap per step, so a steady pen is trailed by the reach and a paused one leaves the brush where it is | strongly inferred for the behaviour; the distance rule is weakly inferred |
| Catch-Up On Stroke End (off) | The release paints on in a straight line from the brush to where the pen lifted, at the pace the brush had | strongly inferred |
| Adjust For Zoom (on) | The reach is in screen points, so it is shorter in the document when zoomed in and longer zoomed out; off, it is in document pixels | strongly inferred: Adobe says it decreases smoothing zoomed in and increases it zoomed out |

Defaults are Photoshop's as far as they are known (Stroke Catch-Up and Adjust For Zoom on, the other two off), except
the amount, which starts at 0 so a stroke paints as it did before smoothing existed. With every amount at 0 the stage
is skipped and the pen's samples reach the stroke as they came, which is why every parity scene is unchanged.

In the app the options bar's Smoothing field is the stabiliser's amount; Dynamics… (now open for every brush) holds its
four options and the two other filters. It applies to the Brush tool and the Eraser; a Shift-click straight line is
not smoothed. MyPaint's own slow tracking still applies on top, and a MyPaint stroke settling while the pen rests
repeats the last smoothed sample, not the pen's point. An action records the pen's raw points with the smoothing that
was on, so a replay smooths them the same way.

**Measured** (`brush_smoothing_tests` prints the table): 1.5 pixels of noise on each axis, 0.05 on the pressure and 3
degrees on the twist, added to clean paths reported 120 times a second; `dev` is the mean distance of the stroke's
samples from the clean path, `lag` the mean distance from the brush to where the clean pen was after each report
(which includes the noise, so it is 1.8 with smoothing off), `end` the distance from the stroke's end to the clean end.

| Setting | Line (520 px/s) dev / lag / end | S curve dev / lag | Circle dev / lag | Fast line (2000 px/s) lag |
|---|---|---|---|---|
| off | 1.16 / 1.79 / 0.71 | 1.15 / 1.75 | 1.38 / 2.04 | 1.90 |
| input 25 | 0.80 / 2.12 / 2.08 | 0.90 / 2.70 | 0.93 / 2.36 | 5.92 |
| input 100 | 0.79 / 2.68 / 2.39 | 0.88 / 3.26 | 0.88 / 2.92 | 7.25 |
| stabiliser 10 | 0.86 / 1.52 / 1.03 | 0.96 / 1.73 | 1.05 / 1.76 | 3.88 |
| stabiliser 25 | 0.60 / 6.75 / 7.12 | 0.63 / 6.63 | 0.72 / 6.12 | 25.1 |
| stabiliser 50 | 0.46 / 27.0 / 31.4 | 2.98 / 24.3 | 4.42 / 23.5 | 69.3 |
| stabiliser 50, no Stroke Catch-Up | 0.40 / 36.0 / 43.2 | 6.03 / 35.1 | 7.68 / 31.4 | 38.3 |
| stabiliser 50, Pulled String | 0.44 / 44.3 / 50.4 | 9.90 / 47.0 | 18.0 / 46.8 | 43.7 |
| stabiliser 50, Catch-Up On Stroke End | 0.46 / 27.0 / 0.71 | 2.97 / 24.3 | 4.40 / 23.5 | 69.3 |
| pressure 50 | positions unchanged; pressure noise 0.044 → 0.015 on the line | | | |

On curves a heavy stabiliser cuts inside the bend (the circle's deviation at 50), as a trailing brush does; on a line
it only steadies. The tests hold: at 0 every sample passes through unchanged; input smoothing at 25 cuts the
deviation by more than a fifth with a lag under 4 pixels at 500 pixels a second and under 8 at 2000; the stabiliser
lags more as it rises; Pulled String trails a taut line by exactly the reach; Catch-Up On Stroke End ends within the
noise of the pen; Stroke Catch-Up closes on a paused pen only when on; pressure smoothing leaves the positions
bit-identical; a twist jittering across 0 stays within 6 degrees of 0.

**Latency.** Each filter costs under half a microsecond per report (input smoothing 0.1 µs, all of them 0.4 µs,
`each_filter_costs_microseconds_per_report`). Through the canvas (`--bench-brush <preset> --bench-smoothing
input|stabilizer|pulled|pressure|all`), every mode's move median stays within the run-to-run noise of smoothing off.

## The engines

- **Round tip** (`BrushStroke`): takes the sample's position. Size, hardness and opacity are the options bar's; it has
  no dynamics.
- **MyPaint** (`MyPaintStroke`): libmypaint's first-generation `mypaint_brush_stroke_to`, on purpose (the newer entry
  point reads uninitialised memory in 1.6), fed from the sample: pressure as given (a mouse's 0.5, as in MyPaint), tilt
  as `tiltX / 60` and `tiltY / 60` clamped to -1..1, and `dt`. The presets' own settings do the rest.
- **Tip brushes** (`TipStroke`): the stamp engine for imported and image brushes. Dabs are placed every
  `spacing × size` along the path and stamped into the stroke's coverage, so colour, opacity, the selection, erasing,
  masks, the preview and undo are the round tip's. Everything that varies per dab comes from the dynamics. The dabs
  of one sample are placed first (dynamics, random draws, grain frame) and then drawn together: bands of rows go to
  the worker pool, and each band runs through the dabs in the order they were placed, so every pixel takes the same
  dabs in the same order as it would one dab at a time.

A tip brush reads pressure only from a stylus: a mouse is full pressure, as in Photoshop, unless the brush turns on
Mouse speed as pressure.

**The first dab waits for the direction** when anything about it turns with the stroke: Stroke grain, a tip that
follows the stroke (`followStroke`), or a Roll mapping on a pen that does not report its twist. The press has no
direction of its own (the track gives the first sample 0, along +x), so its dab is placed when the next sample that
moves arrives, at the press's point, with the pen as it was at the press and the direction of that first step; a click
that never moves is stamped by `TipStroke::finish`, facing +x. Up to 1.8.2 only Stroke grain waited, so a tip following
the stroke, or rolling with it, stamped its first dab facing +x: a stroke started in any other direction began with a
dab turned the wrong way (a square tip showed a crooked corner at the start). A tip that turns with nothing, or Roll on
a pen reporting its barrel, does not wait. `the_first_dab_points_the_way_the_stroke_goes` holds it.

## Dynamics

A tip brush's `dynamics` is a list of mappings. Each reads one input, shapes it through a curve, and scales the result
into a range:

```
output = offset + depth × curve(input)        (the range runs from offset to offset + depth; depth may be negative)
```

### Inputs

| Input | Value, 0..1 |
|---|---|
| Pressure | the pen's pressure (a mouse: 1, or its speed when simulated) |
| Speed | `speed / scale`, `scale` in document pixels per second (default 2000); shown as "Speed (in the document)" |
| ScreenSpeed | `screenSpeed / scale`, `scale` in screen points per second (default 2000); shown as "Speed (on screen)" |
| Tilt | `tiltMagnitude` |
| TiltDirection | `tiltAzimuth` as a fraction of a turn |
| Twist | `twistAngle` as a fraction of a turn |
| Random | a draw from the stroke's seeded generator; on angles, -1..1 |
| StrokeProgress | `distance / (scale × diameter)` with a `scale`; else `progress` when the stroke's length is known; else 25 diameters, Photoshop's default Fade |
| Roll | `twistAngle` as a fraction of a turn when the pen reports its twist, else the stroke's `direction`: a tip that turns with the barrel follows the stroke on a pen without one |
| StrokeRandom | one draw per stroke, the same for all its dabs (on angles, −1..1), from a generator of its own so the dabs' draws are unchanged: Procreate's Randomized |
| InitialDirection | the way the stroke set off, as a fraction of a turn; the first dab waits for it: Photoshop's Initial Direction |
| Wheel | the airbrush wheel, `(tangentialPressure + 1) / 2`: Photoshop's Stylus Wheel |

### Targets and the combination rule

| Target | Base | Range |
|---|---|---|
| Size | the brush size in pixels | 0..10000 |
| Flow | the brush's flow (each dab's alpha) | 0..1 |
| Opacity | 1: the most a dab builds the stroke up to | 0..1 |
| Angle | the tip's angle, degrees | circular |
| Roundness | the tip's roundness | 0.01..1 |
| Spacing | the brush's spacing, a fraction of the size | 0.01..10 |
| Scatter | the brush's scatter | 0..10 |
| GrainDepth | the grain's depth | 0..1 |
| GrainRotation | 0 degrees (the grain turned about the document's origin) | circular |

**Grain modes** (`grainMode`): the grain is always fixed to the document (Canvas), so a stroke reveals it. Stroke and
Dab grain, and any mapping onto GrainDepth or GrainRotation, are stored but not applied: the brush texture stays one
static, document-anchored mask that nothing the pen does can weight or turn ([legal-boundaries.md](legal-boundaries.md);
the engine forces Canvas in `TipStroke::TipStroke` and `mappingAllowed` skips the grain targets). `brush.json` keeps
`grainMode` ("canvas", "stroke", "dab") and `grainMovement`, so presets round-trip. Mappings of speed onto Flow or
Opacity are skipped the same way (no velocity-dependent deposition).

Stroke grain, as it worked before it was disabled (the code path remains in `TipStroke::dab`, unreachable):

- Its frame turns with the stroke's tangent smoothed over about two diameters of travel, so a corner or a jittered dab
  does not spin it. The path's direction is unwrapped from dab to dab, and the tangent follows that, so a path that turns
  more than half a turn within the window (a spiral tighter than the brush) keeps turning the grain forwards.
- The grain travels with each step between dabs, `grainMovement` (0..1) of it, turned into the frame of the moment. At
  1 the grain under the paper stays put and only turns about the dab as the frame catches up with a bend; at 0 it moves
  with the dab.
- The first dab waits for the stroke's direction (the next sample that moves), so the grain starts turned the way the
  stroke goes; `TipStroke::finish` stamps it for a click that never moves.

Before 1.8.1 the first dab was stamped facing +x, so a stroke going any other way spun its grain round over its first
diameters (half a turn for one drawn right to left); the grain advanced by the distance along the lagging tangent, so after
a corner it slid across the stroke by up to a step per dab; and the direction was unwrapped against the lagging tangent,
so a spiral tighter than the window turned the grain backwards. The torture fixture below found all three.

One rule for every brush and every importer:

- **Scalar targets:** `value = base × Π (offset + depth × curve(input))`, over the mappings on that target in the order
  listed, then clamped to the target's range. There is no additive term.
- **Circular targets** (Angle, GrainRotation): `value = base + Σ (offset + depth × curve(input))`, in degrees. A
  multiple of an angle means nothing, and a sum stays continuous across a turn: a mapping from TiltDirection with a depth
  of 360 turns the tip with the pen, with no jump where the azimuth wraps.

A Random mapping on an angle is centred: the draw runs -1..1 and the curve shapes its size and keeps its sign, so a jitter
of 60 turns up to 60 degrees either way. Elsewhere a jitter is a Random mapping with `offset` 1 and a negative `depth`:
the target drops by up to that fraction.

The spacing is measured in the size without its Random mappings, so a size jitter varies the dabs without making the
spacing stagger.

### Curves

A curve maps 0..1 to 0..1 through points: straight lines between them (Linear), or a monotone cubic through them
(Smooth: PCHIP, Fritsch–Carlson tangents), which cannot overshoot, so a curve never leaves the range its points span and
a rising curve never dips. No points is the identity.

### Randomness

Each stroke gets a seed (fresh per stroke in the app, recorded with an action's `brush.stroke` as `seed`, and fixed in
tests). Every dab draws a size, a flow and a signed angle value, then the flips and scatter it uses, in that order,
whether or not a mapping reads them; draws for Random mappings on the other targets come after, only when such a mapping
exists. So a fixed seed paints the same stroke on replay and in the harness, and brushes painted before the other targets
existed paint as they did.

### Options

- **Density by spacing** (`densityBySpacing`, off by default). For repeated source-over dabs, `1 - A = Π (1 - aᵢ)`; with
  the option on, a dab's alpha `a` is spread over the spacing actually used, `a(s) = 1 - (1 - a)^(s / r)`, where `r` is
  `densityReference` (25% by default) and `s` the step divided by the dab size. Applied per pixel through a 256-entry
  table. In the harness a light flow's interior alpha stays within about 1% from 2% to 50% spacing with the option on,
  and falls from 1.0 to 0.5 over the same range with it off.
- **Taper** (`taper`, and `mouseTaper` for a mouse when set; none by default). The stroke narrows (`size`, 0..1) and
  fades (`opacity`, 0..1) linearly over `start` document pixels from its first dab and over `end` pixels to its last:
  at a distance `d` along a stroke of length `L`, `f = min(1, d / start, (L − d) / end)`, and the dab's size is multiplied
  by `1 − size × (1 − f)`, its opacity by `1 − opacity × (1 − f)`; the spacing follows the tapered size, so the tip stays
  solid. The end is known only when the pen lifts, so with an end taper the stroke holds back the samples within `end`
  pixels of the pen and paints them, tapered, at `finish()`: the stroke's tip trails the pen by that length while
  drawing. Lengths are pixels, not diameters, since a taper in Procreate and in Clip Studio does not grow with the brush.
  A brush without a taper takes exactly the code path it took before (`no_taper_paints_as_before`; the parity baseline
  and render hashes are unchanged).
- **Mouse speed as pressure** (`mousePressureFromSpeed`): not applied (the engine clears it, a mouse paints at full
  pressure), since pressure can drive flow and opacity ([legal-boundaries.md](legal-boundaries.md)). It was, for a mouse only:
  `pressure = clamp(1.1 - speed / 1500, 0.25, 1) × min(1, 0.3 + 0.7 × distance / (2 × diameter))`: slow presses harder,
  a flick lifts, and the first two diameters ramp in. A stylus's pressure is never replaced.

### On disk

`brush.json` version 2 keeps the mappings:

```json
"dynamics": [
  {"input": "pressure", "target": "size", "offset": 0.25, "depth": 0.75, "curve": [[0, 0], [0.4, 0.7], [1, 1]], "smooth": true},
  {"input": "random", "target": "angle", "offset": 0, "depth": 60}
],
"densityBySpacing": false, "densityReference": 0.25, "mousePressureFromSpeed": false
```

A version 1 preset (with `pressureSize`, `minimumSize`, `pressureFlow`, `sizeJitter`, `flowJitter`, `angleJitter` and no
`dynamics`) opens with those as mappings (`legacyDynamics`), in the order the old engine multiplied them, and paints as
it did; saving writes version 2.

### Editing

The Dynamics… button in the Brush tool's options bar (for a tip brush) edits pressure on size and on flow, each as a
curve with a minimum, the two options, and Pen tilt shapes the tip with its flattest roundness. The brush's other
mappings stay as they are. The changes are saved to the brush's folder with a new preview.

**A tilted pencil** (`tiltShapesTip(flattest)`, for any tip brush, which Pen tilt shapes the tip adds): Tilt on roundness
from 1 upright to `flattest` at full tilt, and TiltDirection on the angle with a depth of −360, so the tip flattens as the
pen leans and its long side points the way it leans, like the side of a pencil. The harness paints it as `tip_tilt_shape`.

## Importers

What each format's settings become. What a format holds that has no mapping yet is listed in the import's notes.

**Confidence.** Every mapping below is tagged with how far its meaning is known, so a guess does not quietly become the
truth:

- **confirmed**: checked against the source application's own output (a brush painted there and measured). No dynamics
  mapping is confirmed yet: there is no Photoshop, Procreate or Clip Studio here to paint the reference brushes. What is
  confirmed is the reading of the files (field names, types and ranges from real brushes), which each row takes for
  granted.
- **strongly inferred**: the meaning is plain from the setting's name and the application's documented interface, and
  the values real brushes store agree with it.
- **weakly inferred**: a reading of a direction, sign, target or ramp that real brushes do not settle.
- **synthetic-only**: a number chosen here (a scale or a constant) that only the synthetic fixtures exercise. Those prove
  the reader and the engine, not the application.

**Photoshop (`.abr` 6–10).** A control (`bVTy`) drives its target from the minimum up, and a jitter follows it:

| Photoshop | Mappings | Confidence |
|---|---|---|
| Size control Pen Pressure, Minimum Diameter | Pressure → Size (`offset` minimum, `depth` 1 − minimum) | strongly inferred |
| Size control Pen Tilt | Tilt → Size (full upright, the minimum lying flat) | weakly inferred: which end of the lean is full |
| Size control Fade | StrokeProgress → Size over the fade's steps × spacing | strongly inferred |
| Size jitter | Random → Size | strongly inferred |
| Angle jitter | Random → Angle (jitter × 180 degrees) | strongly inferred |
| Angle control Pen Tilt | TiltDirection → Angle (depth 360) | weakly inferred: the sign is the opposite of Procreate's `shapeAzimuth` (−360) and neither is checked |
| Angle control Direction (`bVTy` 7) | `followStroke` | strongly inferred (earlier versions read it from 6, so Direction brushes did not follow the stroke) |
| Angle control Initial Direction (6) | InitialDirection → Angle (depth −360) | strongly inferred |
| Angle control Rotation (5) | Twist → Angle (depth −360) | strongly inferred; the sign weakly |
| Angle control Stylus Wheel (4) | Wheel → Angle (depth 360) | weakly inferred |
| Size, roundness, flow or opacity control Stylus Wheel (4) | Wheel → the target from the minimum up | strongly inferred |
| Roundness jitter, control and Minimum Roundness | the same shapes on Roundness | as for size: pressure, fade and jitter strongly inferred, tilt weakly |
| Scatter, control Pen Pressure | `scatter`; Pressure → Scatter | strongly inferred |
| Transfer: Flow jitter and control | on Flow | strongly inferred |
| Transfer: Opacity jitter and control | on Opacity (the most a dab builds up to) | strongly inferred: Photoshop's opacity caps the stroke, its flow is per dab |

Texture, dual brush, colour dynamics, wet edges, noise and build-up are noted as left out. Versions 1 and 2 hold no
dynamics. The control numbers (0 off, 1 fade, 2 pressure, 3 tilt, 4 stylus wheel, 5 rotation, 6 initial direction, 7
direction) are those Photoshop writes, as the Patchy editor's ABR reader documents them and its self-made Photoshop
2026 fixture (angle control 7) shows.

**Procreate (`.brushset`, `.brush`).** Each setting below becomes a mapping; every scaling lives in one block at the top
of `procreate.cpp` (`namespace scaling`), whose comments carry the same tags, so the reference brushes made in
Procreate can tune each in one place. Curves are the identity unless the row says otherwise.

| Procreate | Input → target | Offset, depth | Confidence |
|---|---|---|---|
| `dynamicsPressureSize` p | Pressure → Size, through `dynamicsPressureSizeCurve` | 1 − p, p | strongly inferred |
| `dynamicsJitterSize` j | Random → Size | 1, −j | strongly inferred |
| `dynamicsPressureOpacity` p | Pressure → Flow (Procreate's opacity is per dab), through `dynamicsPressureOpacityCurve` | 1 − p, p | strongly inferred; the target (flow, not the stroke's ceiling) weakly |
| the pressure curves (`dynamicsPressure…Curve`) | the mapping's curve: the points (stored as `"{x, y}"` strings, in any order) sorted, drawn smoothly (monotone cubic); a straight 0..1 line is no curve | | strongly inferred: Procreate's curve editor draws a smooth curve through its points; the exact spline is not checked |
| `dynamicsPressureBleed` a | Pressure → Flow, through its curve | 1 − 0.5 a, 0.5 a: each dab thins at light pressure | weakly inferred, as the tilt bleed |
| `jitterShapeRoundness` j | Random → Roundness | 1, −j | strongly inferred |
| `shapeRandomise` | StrokeRandom → Angle | 0, 180: each stroke's tip turned at random, every dab alike | strongly inferred |
| `plotSpacingJitter` j | Random → Spacing | 1, j: the gaps widen at random | weakly inferred: the scale |
| `pencilTaperStartLength`, `…EndLength`, `pencilTaperSize`, `pencilTaperOpacity` | `taper`: each length × 300 pixels (`taperFullLength`), size and opacity as they are | | lengths weakly inferred (pixels, not diameters; the 300 is a guess); size and opacity strongly |
| `taperStartLength`, `taperEndLength`, `taperSize`, `taperOpacity` (touch) | `mouseTaper`, the same way | | as the pencil's |
| `textureBrightness` b, `textureContrast` c, −1..1 | the grain image changed once: `(v − 0.5) × gain + 0.5 + b`, gain `1 + 3c` (c ≥ 0) or `1 + c` | | weakly inferred: the scale of contrast |
| `dynamicsJitterOpacity` j | Random → Flow | 1, −j | strongly inferred |
| `shapeScatter` s | Random → Angle | 0, s × 180 degrees | strongly inferred |
| `dynamicsSpeedSize` a, −1..1 | ScreenSpeed → Size, full at `fullSpeed` | 1, a: grows with speed when positive, shrinks when negative | sign weakly inferred; scale synthetic-only |
| `dynamicsSpeedOpacity` a, −1..1 | ScreenSpeed → Opacity, full at `fullSpeed` | positive: 1 − a, a (slow strokes lighter); negative: 1, a (fast strokes lighter) | sign weakly inferred; scale synthetic-only |
| `plotSpacingSpeed` a, 0.. | ScreenSpeed → Spacing, full at `fullSpeed` | 1, a: the spacing widens with speed | direction strongly inferred from real brush data; scale synthetic-only |
| `dynamicsTiltSize` a, −1..1 | Tilt → Size, tilt curve | 1, a: grows as the pen leans | weakly inferred: the sign |
| `dynamicsTiltOpacity` a | Tilt → Opacity, tilt curve | 1, −a: lighter as the pen leans | weakly inferred: the direction |
| `dynamicsTiltBleed` a | Tilt → Flow, tilt curve | 1, −`tiltBleedFlow` × a (0.5 × a): each dab thins | weakly inferred: the meaning; the 0.5 synthetic-only |
| `dynamicsTiltShapeRoundness` a, `…Minimum` m | Tilt → Roundness, tilt curve | 1, −a × (1 − m); nothing while m is 1, as in nearly every brush | strongly inferred |
| the tilt angles (`sizeTiltAngle` and the rest) | the Tilt input's curve (`tiltCurve`) | zero up to the angle, then straight up to full at 60 degrees | weakly inferred: needs a source reference |
| `shapeAzimuth` | TiltDirection → Angle | 0, −360: the tip's x axis points the way the pen leans | weakly inferred: the field and its meaning are plain, which axis and the sign are not |
| `shapeRoll` | Roll → Angle (the barrel's twist, else the stroke's direction); `followStroke` off | 0, −360: the tip turns with the barrel | weakly inferred: the sign, and how it combines with `shapeAzimuth` |
| `textureApplication` 0 (moving grain) | Canvas grain with an import note (moving grain is not applied) | not a mapping | weakly inferred: which value is moving comes from the earlier reader |
| `textureMovement` m | `grainMovement` m | not a mapping | synthetic-only: the scale |
| speed measured on the screen | ScreenSpeed, not Speed | | strongly inferred: Procreate works in screen space; its zoom reference is still to make |
| `maxSize` | 200 pixels at 1 | | weakly inferred: from Procreate's own thumbnails |

The constants in `scaling` are tagged the same: `fullSpeed` (1500 screen points per second) and `tiltBleedFlow` (0.5)
are synthetic-only; `tiltAngleRange` (a stored tilt angle of 1 is 90 degrees) is strongly inferred from Procreate's
0–90 degree tilt graph; `tiltFullAt` (60 degrees) is the engine's own Tilt input, not a reading of Procreate.

The tip's angle turns counterclockwise on screen, while the azimuth, the twist and the stroke's direction turn
clockwise in the document's y-down frame, hence the depth of −360. The sum on an angle is continuous across a turn: the
harness's twist wrap (340 through 359, 0 and 1 to 20 degrees) paints without a jump, and `brush_dynamics_tests` holds
the tip's angle to the barrel's quarter-degree steps across 359 → 0 → 1, and the twist, the roll, the azimuth, the
stroke's direction and the tip's resolved angle, dab by dab, to a degree a report through 358, 359, 0, 1 and 2 (and
−2..2, and back), with a replay across the wrap painting the same pixels. A brush with both `shapeRoll` and a rotation
that follows the stroke stops following the stroke on its own, since Roll follows it where the pen has no twist.
`shapeRollMode` (not in any brush seen so far) is listed as not carried over.

`fullSpeed` is 1500 screen points per second: the speed at which a speed setting has its whole effect. Procreate
measures speed on the screen, so the three speed settings read ScreenSpeed and respond the same to the same hand motion
at any zoom (`procreate_speed_responds_the_same_at_any_zoom` replays one screen stroke at 50, 100, 200 and 400% and
holds the resolved size, opacity and spacing equal at every sample). Before 1.8.1 they read the document's speed with
the same 1500, so at 100% nothing changed; the value itself is still a guess to tune. A Procreate brush imported
before 1.8.1 and saved in the library keeps its document-speed mappings until it is imported again.

**Tilt, as this reader takes it** (`scaling::tiltCurve`, the one place to change): Procreate's tilt is the pen's angle
from upright, the same as the Tilt input (`tiltMagnitude`, 0 upright to 1 at 60 degrees). Each tilt setting has a tilt
angle (`sizeTiltAngle`, `opacityTiltAngle`, `bleedTiltAngle`, `shapeRoundnessTiltAngle`; else `dynamicsTiltAngle`),
stored as a fraction of Procreate's 0–90 degree tilt graph and read as the lean from upright at which the setting starts
to count: the Tilt input's curve is zero up to it and rises straight to full at 60 degrees. Real brushes store 0.1 (9
degrees) for most, so a slight lean starts it. If the reference brushes show the angle is measured from the screen, or
the effect ramps differently, only `tiltCurve` changes. `dynamicsTiltCompression` (whether the grain scales with a tilted
size) and `dynamicsTiltGradation` are not carried over.

`dynamicsPressureSizeSpeed`, `dynamicsPressureOpacitySpeed` and `dynamicsPressureBleedSpeed` are not speed dynamics:
their values follow `dynamicsPressureResponse` (0.3, 0.6 and so on together in real brushes), so they read as how quickly
size, opacity and bleed catch up with a change of pressure. The engine has no such lag; they are left out and listed.

What a brush uses that has no mapping is listed in the import's notes, one line naming each setting with the number of
brushes using it (`notCarriedSettings` in `procreate.cpp`; a setting counts when it is off its neutral value, and a
roundness setting only while its minimum is below full).

Colour jitter and wet mixing are counted in the notes ("brushes with colour jitter", "brushes with wet mix"): they
paint in the chosen colour. `dynamicsFalloff` and `shapeCountJitter` are listed as not carried over.

**Clip Studio (`.sut`).** An effector blob is a header of eleven big-endian words (its size, 44; a marker; flags, bit
0x10 when pressure drives it; the minimum in percent; the ninth word the first curve block's length) and then curve
blocks: 12, the number of points, 16, and the points as big-endian doubles x, y. The first is the pressure curve; the
others (a four-point falling curve in every sample) are not read.

| Clip Studio | Mapping | Confidence |
|---|---|---|
| `BrushSizeEffector` with pressure, its minimum and curve | Pressure → Size from the minimum, through the curve (smooth) | strongly inferred for the minimum and the curve's place; the smooth drawing weakly |
| `BrushOpacityEffector` or `BrushFlowEffector` with pressure | Pressure → Flow from its minimum, through its curve | weakly inferred: opacity read as flow |
| `BrushUseIn`/`BrushInLength`, `BrushUseOut`/`BrushOutLength` (starting and ending) | `taper` in pixels, on size | strongly inferred for the lengths (unit 0, pixels; other units read as pixels); which settings it tapers (`BrushInOutTarget`) is not decoded, so size |
| `TextureBrightness`, `TextureContrast`, −100..100 | the texture image changed once, as Procreate's | weakly inferred |

## The parity harness

`tests/brush_harness.{h,cpp}` paints recorded strokes with a set of brushes and measures the result.

**Fixtures.** Made in code (`standardFixtures`): a pressure ramp (0 → 1 → 0), a pressure sine, a speed sweep (slow,
fast, slow at even timing), a tilt sweep, a twist sweep (a full turn, wrapping from 180 to -180 halfway), a straight
line, a circle, an S curve, corners, a fast flick (ten reports over 240 pixels), a long slow stroke (720 reports), an
azimuth sweep (the pen at 45 degrees from upright, leaning once round) and a twist wrap (the barrel from 340 through
359, 0 and 1 to 20 degrees); and the JSON files under `tests/brush_fixtures/` (a recorded pen hook with tilt, twist and tangential pressure, and a
mouse scribble with uneven timing). Every input a mapping can read has a fixture that moves it.

**Presets.** The round tip hard and soft, the eraser (on an opaque grey layer), tip brushes made in code (a square tip
following the stroke; a textured tip with jitters, scatter, count and pressure; a flat tip with a mapping on every pen
input; a tight light-flow tip with density by spacing and mouse speed as pressure; a tilted pencil), the tips the importers make of their
own tests' files (the Photoshop `.abr` "Leaf", Procreate's "Soft Ink", Clip Studio's "Soft Pencil" and "Spray" when the
build has SQLite; `tests/brush_import_fixtures.h` writes those files for both), and four MyPaint presets (pencil,
charcoal, dry brush, calligraphy) when the build has libmypaint. Tip brushes use a fixed seed.

**Metrics** (`measure`): where paint landed (bounding box), total and mean alpha, the width across the stroke at ten
stations along it (pixels at 10% alpha or more), the peak alpha at each, the mean distance from 90% to 10% of the peak
(the edge), and the start and end taper (the width at 5% and 95% over the median width).

**Baseline.** `brush_parity` (a ctest) paints every fixture with every preset, on the worker pool and serially (the two
must agree), and holds each render's FNV-1a hash to `tests/brush_parity_baseline.txt`, which also carries the
measurements so a change shows how a stroke moved, not only that it did. A second section, `u16/<fixture>/<preset>`,
holds the same scenes painted on a 16-bit layer (see 16 bits above); each test rewrites only its own section.
`COMPOSITOR_UPDATE_BRUSH_PARITY=1 build/tests/brush_parity` rewrites it after an intentional change. GCC and Clang
builds produce the same file. The test
also checks sample derivation (unwrapping, speed, progress), the density option across 2–50% spacing, and the mouse
speed option.

**Fixture brushes.** `tests/fixtures/brushes/procreate/` holds synthetic Procreate brushes (`syn_00_baseline.brush` and
copies with one setting changed, written by `make_fixtures.py` there) and a `manifest.json` that names, for each, the
stroke it is checked on, what is measured and the direction expected against the baseline: `up` or `down` (the width or
peak at the stations where the stroke's input is high, 4 and 5 of 10, over those where it is low, 0 and 9, against the
same for the baseline), `turns` (the width swings as the tip turns), `continuous` (no jump between neighbouring
stations) or `differs`. `synthetic_procreate_brushes_change_the_way_their_setting_says` checks every entry, and the
brushes' scenes on their strokes join the baseline (8 and 16 bits). They test this reader and the engine, not
Procreate's meaning: that is for brushes made in Procreate on an iPad with one setting each, which will replace the
assumptions in the table above.

`tests/local-fixtures/` (git-ignored) is for brush sets that may not be shared. With a `manifest.json` there:

```json
{"brushes": [{"set": "Some Set.brushset", "name": "Brush name", "input": "speed", "stroke": "speed_sweep",
              "measure": "peak", "expect": "down", "diameter": 120}]}
```

`local_third_party_brushes_when_present` imports each set, prints the importer's notes (what was not carried over), and
checks each brush against itself without its mappings from `input` (`diameter` paints it larger than the default 28
pixels, for spacings too fine to step at that size). Nothing from them enters the baseline; without the manifest the
test is skipped.

**By hand.** `build/tests/brush_parity_tool list` names the fixtures and presets; `dump <folder> [filter]` writes every
render as a PNG with `metrics.txt` and the fixtures as JSON; `render <stroke.json> <preset> <out.png>` paints one
recorded stroke.

**Moving grain under torture** (`brush_grain_tests`, removed with Stroke grain; kept here as the record). An asymmetric grain made in code (`tortureGrain`: a checkerboard,
stripes that brighten one way only, an L in one corner) in Stroke mode, on a plain tip and on one whose size follows
ScreenSpeed; painted along a straight line, a right-angled corner, an S curve, a circle of a turn and a quarter, a spiral
that ends tighter than the brush and a stroke drawn right to left that wobbles across ±180 degrees at every report; each
reported slowly (about every 2 points) and fast (about every 16), at 100% and 200%, and the slow ones again under a view
turned 30 degrees (the document path turned back, as a rotated view maps the same hand motion). `TipStroke::trace`
records every dab's centre, size, the path's direction, the grain's tangent and its offset, and the test holds:

- the grain under any point near a dab only turns about the dab before, never slides; under a tenth of a pixel on the
  straight strokes, under 2 pixels round the corner, the S and the circle, and on the spiral no faster than the path;
- the tangent never leaves the directions the path has taken (no spin at a corner or at the start), settles within 3
  degrees where the path holds a direction for three windows, and never steps back round the circle and the spiral;
- slow and fast reports: the grain's tangent within 8 degrees and its phase within a pixel plus 0.5% of the stroke's
  length, plus what the fast first chord's different start direction gives over the window;
- a turned view turns the grain's frame by exactly the turn and changes no distance;
- ScreenSpeed resolves each dab's size at 200% to the 100% stroke's at the same moment, to rounding;
- 8 and 16 bits within 4 levels (0.05% of samples beyond a level), and the worker pool and a serial run the same.

`brush_parity_tool grain <folder>` writes every torture scene as a PNG, the grain enlarged, and each scene's dabs as text.

`render_hash_tests` keeps its own brush scenes, and `brush_dynamics_tests` covers curves, the combination rule, circular
targets, the inputs and the migration of old presets.

**Baseline changes after 1.8.2** (the first dab's direction, above). 17 scenes changed at 8 bits and the same 17 at 16:
`tip_square` and `procreate_soft_ink` (both follow the stroke: Soft Ink has `shapeRotation`) on `straight_line`,
`circle`, `s_curve`, `corners`, `fast_flick`, `long_slow`, `pen_hook` and `mouse_scribble`, the fixtures whose first
step is not along +x; and `syn_12_roll` on `circle`, the one Roll scene without a reported twist (its `twist_sweep` and
`twist_wrap` report the barrel). Only the start moves: the bounding box and total alpha of the first dab, not the
stations along the stroke. The fixtures drawn along +x (the pressure, speed, tilt and twist sweeps) are unchanged. In
`render_hash_tests` the one scene with such a tip, `brush/tip_square`, changed for the same reason.

## Automation

`brush.stroke` takes, besides `points`, `pressure` and `pressures`: `tilts` (`[tiltX, tiltY]` degrees per point),
`twists` (degrees per point; with them the pen reports its twist, which Roll reads), `times` (seconds per point; 8 ms apart by default), `seed` (the tip brushes' jitter) and
`viewScale` (the zoom the stroke is taken as drawn at, 1 by default: 2 for 200%), which ScreenSpeed reads.
Any pen field makes the stroke a stylus's. A recorded action keeps them (for a preset, the times and the seed; the view
scale when it is not 1), so a stroke replays exactly. Smoothing is off unless asked for: `smoothing` (the stabiliser,
0..100), `pulledString`, `strokeCatchUp`, `catchUpOnEnd`, `adjustForZoom`, `inputSmoothing` and `pressureSmoothing`; a
stroke recorded with smoothing on carries them, with its times and view scale.

## 16 bits

A stroke on a 16-bit document (`BrushStroke(layer, mask, settings, canvas, SampleType::U16, selection)`) keeps
everything at 0..32768: the working pixels or mask, the coverage grid, the selection, the dab profile table and the
stamps. `BrushStroke` itself is depth-free (the grid's placement, the curve, the tail, the dirty area); the pixels are
a `StrokeRaster<S>` (`src/core/src/stroke_raster.h`), one template over a `StrokeOps<S>` policy that holds each
depth's arithmetic, instantiated in `brush.cpp` for 8 bits and `brush_u16.cpp` for 16. The samples and the dynamics are depth-free doubles, so every engine places the same
dabs at either depth; only what a dab writes differs:

- **Round tip.** The dab table holds 15-bit values; hard tips merge by max, soft ones by screen
  (`old + v × (1 − old)`, rounded at 15 bits), and the recompose is `base + (colour − base) × coverage × opacity` in
  15-bit integers.
- **Tip brushes.** `TipStroke::dab` writes the grid's own depth: each dab's alpha builds up towards its opacity at 15
  bits, and density by spacing has a table entry per 15-bit level (kept while the spacing ratio stays the same) instead
  of 256.
- **MyPaint.** libmypaint paints in 15-bit fixed point, which is the 16-bit document's range: a 16-bit layer's pixels
  go into its tiles as they are and come back as they are, so a MyPaint stroke on an 8-bit layer converted to 16 bits
  reduces to exactly the 8-bit stroke.
- **The rest of the stroke:** the eraser, masks and the Quick Mask, clone and processed sources (Clone Stamp, the
  Healing Brush, Blur, Sharpen, Dodge, Burn and Sponge read a 16-bit sample, or a `TiledSource16` rendered and
  processed at 16 bits), Smudge and Liquify (`WarpStroke` on a 16-bit image), gradients, lifting and moving selected
  pixels, healing, the preview (the renderer's `image16` and `maskImage16` overrides, and `MipCache` refreshing its
  16-bit levels in place) and the commit.

Against the 8-bit stroke on the same 8-bit-sourced layer, reduced to 8 bits: hard tips, MyPaint, clones, the healers,
toning, gradients and moved pixels are within a level. Soft tips and tip brushes differ by up to a few levels in a
small share of samples (at most 4 levels and 0.72% of samples in `depth_paint_tests`; 5 levels and 0.88% for a light
flow with density by spacing at 2% spacing in the parity harness): the 8-bit coverage rounds to 1/255 at every dab of a
screen build-up, and against the exact coverage in double precision the 8-bit stroke is up to 3 levels off in about
0.9% of its pixels where the 16-bit one is within a level. Density by spacing holds the interior alpha within 0.2%
across 2–50% spacing at 16 bits (1.2% at 8).

The parity harness paints every scene again on a 16-bit layer (`render16`) and holds its 16-bit samples to the
baseline's `u16/` section, with each scene's distance from its 8-bit render (`vs8=`).

## Not yet

- Clip Studio's tilt, velocity and random effectors (the flags beside pressure are not decoded), a continuous swept
  round brush, and MyPaint's newer inputs.
- Photoshop's texture, dual brush, wet edges, colour dynamics, noise and build-up. Texture and dual brush need a second
  image composed with the tip and a pattern section reader; wet edges a stroke-wide mask. Adobe has active patents on
  input-driven brush texture, dual brush hierarchies and colour dynamics, so any of these wants a claim check first.
- Procreate's fall-off (`dynamicsFalloff`), shape count jitter, grain zoom (`textureZoom`), grain offset jitter, the
  pressure response lag (`dynamicsPressure…Speed`) and StreamLine. Clip Studio's own
  stabilisation and Procreate's StreamLine are not read from imported brushes; the stabiliser is the tool's setting.
- Colour by tilt or pressure (Procreate's `dynamicsTilt/PressureHue`, `Saturation`, `Brightness`, `SecondaryColor` and the
  colour jitters): a stroke's colour is one value for the whole stroke, laid down through its coverage, so a colour per dab
  does not fit the dynamics layer as a target; it would need the stroke to carry colour per pixel. Listed as not carried
  over.
