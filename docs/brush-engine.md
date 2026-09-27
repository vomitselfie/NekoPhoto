# The brush engine

How a stroke gets from the pen to the pixels, what the brush dynamics are, how imported brushes land in them, and the
harness that measures all of it. The code: `src/core/include/compositor/brushsample.h`, `brushdynamics.h`,
`tipbrush.h`, `brush.h` (the round tip and the stroke's grid), `mypaint.h`, the importers in `src/core/src/abr.cpp`,
`procreate.cpp` and `sut.cpp`, and the harness in `tests/brush_harness.*`.

```
pointer events ──> BrushSample (raw) ──> BrushSampleTrack (derived) ──> engine
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

Nothing is folded in at capture: pressure is not turned into size there, and tilt stays in degrees. Each engine reads
what it uses.

`BrushSampleTrack` derives the rest from the samples before:

| Field | How |
|---|---|
| `dt` | seconds since the previous sample; 1/120 when the times do not say (the first sample, equal or missing times) |
| `speed` | document pixels per second, smoothed with a 20 ms time constant |
| `acceleration` | of that speed |
| `direction` | radians of travel, unwrapped |
| `tiltMagnitude` | 0 upright to 1 at 60 degrees or more |
| `tiltAzimuth` | radians the pen leans towards, unwrapped; kept while the pen is upright |
| `twistAngle` | the twist in radians, unwrapped |
| `distance` | document pixels since the stroke began |
| `progress` | 0..1 along the stroke when its whole length is known (`deriveStroke`, a replay); -1 while painting |

Angles are unwrapped (`unwrapAngle`) so a twist going from 179 to -179 degrees reads as two degrees of turn, not 358.
`interpolate` blends two samples along the unwrapped values, which is what a tip brush uses between events.

Qt delivers tablet events one by one (it does not coalesce them unless `AA_CompressTabletEvents` is set), so every
report reaches the brush; there is no platform batch to unpack.

A recorded stroke is JSON (`recordedStrokeFromJson`, `recordedStrokeToJson`):

```json
{"format": "nekophoto-stroke", "version": 1, "stylus": true,
 "samples": [{"t": 0, "x": 100, "y": 100, "pressure": 0.1, "tiltX": 0, "tiltY": 0, "twist": 0, "tangentialPressure": 0}]}
```

A bare array of samples also reads; missing fields take their defaults, and a stroke with `"stylus": false` reads with a
mouse's neutral values whatever it holds.

## The engines

- **Round tip** (`BrushStroke`): takes the sample's position. Size, hardness and opacity are the options bar's; it has
  no dynamics.
- **MyPaint** (`MyPaintStroke`): libmypaint's first-generation `mypaint_brush_stroke_to`, on purpose (the newer entry
  point reads uninitialised memory in 1.6), fed from the sample: pressure as given (a mouse's 0.5, as in MyPaint), tilt
  as `tiltX / 60` and `tiltY / 60` clamped to -1..1, and `dt`. The presets' own settings do the rest.
- **Tip brushes** (`TipStroke`): the stamp engine for imported and image brushes. Dabs are placed every
  `spacing × size` along the path and stamped into the stroke's coverage, so colour, opacity, the selection, erasing,
  masks, the preview and undo are the round tip's. Everything that varies per dab comes from the dynamics.

A tip brush reads pressure only from a stylus: a mouse is full pressure, as in Photoshop, unless the brush turns on
Mouse speed as pressure.

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
| Speed | `speed / scale`, `scale` in document pixels per second (default 2000) |
| Tilt | `tiltMagnitude` |
| TiltDirection | `tiltAzimuth` as a fraction of a turn |
| Twist | `twistAngle` as a fraction of a turn |
| Random | a draw from the stroke's seeded generator; on angles, -1..1 |
| StrokeProgress | `distance / (scale × diameter)` with a `scale`; else `progress` when the stroke's length is known; else 25 diameters, Photoshop's default Fade |
| Roll | `twistAngle` as a fraction of a turn when the pen reports its twist, else the stroke's `direction`: a tip that turns with the barrel follows the stroke on a pen without one |

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
- **Mouse speed as pressure** (`mousePressureFromSpeed`, off by default, labelled simulated). For a mouse only:
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
curve with a minimum, and the two options. The brush's other mappings stay as they are. The changes are saved to the
brush's folder with a new preview.

## Importers

What each format's settings become. What a format holds that has no mapping yet is listed in the import's notes.

**Photoshop (`.abr` 6–10).** A control (`bVTy`) drives its target from the minimum up, and a jitter follows it:

| Photoshop | Mappings |
|---|---|
| Size jitter, control Pen Pressure / Pen Tilt / Fade, Minimum Diameter | Pressure → Size (`offset` minimum, `depth` 1 − minimum); Tilt → Size (full upright, the minimum lying flat); StrokeProgress → Size over the fade's steps × spacing; Random → Size |
| Angle jitter, control Pen Tilt, Direction | Random → Angle (jitter × 180 degrees); TiltDirection → Angle (depth 360); Direction sets `followStroke` |
| Roundness jitter and control, Minimum Roundness | the same shapes on Roundness |
| Scatter, control Pen Pressure | `scatter`; Pressure → Scatter |
| Transfer: Flow jitter and control | on Flow |
| Transfer: Opacity jitter and control | on Opacity (the most a dab builds up to) |

Texture, dual brush, colour dynamics, wet edges, noise and build-up are noted as left out. Versions 1 and 2 hold no
dynamics.

**Procreate (`.brushset`, `.brush`).** Each setting below becomes a mapping; every scaling lives in one block at the top
of `procreate.cpp` (`namespace scaling`), so the reference brushes made in Procreate can tune each in one place. Curves
are the identity unless the row says otherwise. Confidence: *high* where the meaning is plain from the setting and real
brushes agree, *assumed* where the direction or scale is a reading that the reference brushes (below) still have to
confirm.

| Procreate | Input → target | Offset, depth | Confidence |
|---|---|---|---|
| `dynamicsPressureSize` p | Pressure → Size | 1 − p, p | high |
| `dynamicsJitterSize` j | Random → Size | 1, −j | high |
| `dynamicsPressureOpacity` p | Pressure → Flow (Procreate's opacity is per dab) | 1 − p, p | high |
| `dynamicsJitterOpacity` j | Random → Flow | 1, −j | high |
| `shapeScatter` s | Random → Angle | 0, s × 180 degrees | high |
| `dynamicsSpeedSize` a, −1..1 | Speed → Size, full at `fullSpeed` | 1, a: grows with speed when positive, shrinks when negative | sign and scale assumed |
| `dynamicsSpeedOpacity` a, −1..1 | Speed → Opacity, full at `fullSpeed` | positive: 1 − a, a (slow strokes lighter); negative: 1, a (fast strokes lighter) | sign and scale assumed |
| `plotSpacingSpeed` a, 0.. | Speed → Spacing, full at `fullSpeed` | 1, a: the spacing widens with speed | direction high, scale assumed |
| `dynamicsTiltSize` a, −1..1 | Tilt → Size, tilt curve | 1, a: grows as the pen leans | sign assumed |
| `dynamicsTiltOpacity` a | Tilt → Opacity, tilt curve | 1, −a: lighter as the pen leans | direction assumed |
| `dynamicsTiltBleed` a | Tilt → Flow, tilt curve | 1, −`tiltBleedFlow` × a (0.5 × a): each dab thins | meaning and scale assumed |
| `dynamicsTiltShapeRoundness` a, `…Minimum` m | Tilt → Roundness, tilt curve | 1, −a × (1 − m); nothing while m is 1, as in nearly every brush | high |
| `shapeAzimuth` | TiltDirection → Angle | 0, −360: the tip's x axis points the way the pen leans | meaning high, which axis assumed |
| `shapeRoll` | Roll → Angle (the barrel's twist, else the stroke's direction); `followStroke` off | 0, −360: the tip turns with the barrel | meaning high, sign assumed |

The tip's angle turns counterclockwise on screen, while the azimuth, the twist and the stroke's direction turn
clockwise in the document's y-down frame, hence the depth of −360. The sum on an angle is continuous across a turn: the
harness's twist wrap (340 through 359, 0 and 1 to 20 degrees) paints without a jump, and `brush_dynamics_tests` holds
the tip's angle to the barrel's quarter-degree steps across 359 → 0 → 1. A brush with both `shapeRoll` and a rotation
that follows the stroke stops following the stroke on its own, since Roll follows it where the pen has no twist.
`shapeRollMode` (not in any brush seen so far) is listed as not carried over.

`fullSpeed` is 1500 document pixels per second: the speed at which a speed setting has its whole effect. Procreate
measures speed on the screen, not in the document, so this is a guess to tune.

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

**Clip Studio (`.sut`).** `BrushSizeEffector` with pressure → Pressure → Size from the effector's minimum;
`BrushOpacityEffector` or `BrushFlowEffector` with pressure → Pressure → Flow from 0. The effector's own curve is not
decoded yet (it needs Clip Studio to make reference files), so the response is linear; when it is, it goes into the
mapping's curve with no change to the engine.

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
input; a tight light-flow tip with density by spacing and mouse speed as pressure), the tips the importers make of their
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

`render_hash_tests` keeps its own brush scenes, and `brush_dynamics_tests` covers curves, the combination rule, circular
targets, the inputs and the migration of old presets.

## Automation

`brush.stroke` takes, besides `points`, `pressure` and `pressures`: `tilts` (`[tiltX, tiltY]` degrees per point),
`twists` (degrees per point; with them the pen reports its twist, which Roll reads), `times` (seconds per point; 8 ms apart by default) and `seed` (the tip brushes' jitter).
Any pen field makes the stroke a stylus's. A recorded action keeps them (and, for a preset, the times and the seed), so
a stroke replays exactly.

## 16 bits

A stroke on a 16-bit document (`BrushStroke(layer, mask, settings, canvas, SampleType::U16, selection)`, in
`brush_u16.cpp`) keeps everything at 0..32768: the working pixels or mask, the coverage grid, the selection, the dab
profile table and the stamps. The samples and the dynamics are depth-free doubles, so every engine places the same
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

- Clip Studio's effector curves, texture coordinate modes (canvas, stroke, dab), tilt and twist shaping the tip's
  geometry, stabilisation, a continuous swept round brush, and MyPaint's newer inputs.
