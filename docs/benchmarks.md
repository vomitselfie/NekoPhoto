# Core benchmarks

The baseline for the high-bit-depth work (`docs/high-bit-depth-plan.md`, P0). Later phases compare against these
numbers on the same machine: P1 must stay within ±2%.

## Rerun

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench_core
env -u DISPLAY -u WAYLAND_DISPLAY build/tests/bench_core [runs=9] [name filter]
```

`bench_core` is not part of ctest. Each line is the median of the runs (min and max beside it). What it times
(`tests/bench_core.cpp`):

- **render**: a 4000 x 3000 document with a base, six 2400 x 1800 layers in Normal, Multiply, Screen, Overlay, Soft
  Light and Color (two masked), a full-size layer clipped to the last, an isolated Hard Light group of two layers,
  and a Levels and a Curves adjustment layer; flattened at 1:1, at 0.25, and a 1024 x 768 region at 1:1.
- **brush stroke**: one 80 px round-brush stroke at 80% across a 4000 x 3000 layer (901 points, `appendAll`, flush and
  commit), hard and soft.
- **gaussian blur**: `applyFilter` on a 4000 x 3000 opaque image, radius 2 and 20.
- **adjustments**: Levels, Curves and Hue/Saturation through `applyAdjustment` on the same image.

## Results

2026-09-26, commit after NekoPhoto 1.5.3; AMD Ryzen AI 9 HX 370 (24 worker threads), Manjaro, GCC 16, Release,
median of 9 runs:

| Operation | Median (ms) |
|---|---:|
| render 4000x3000, 12 layers | 206.7 |
| render 4000x3000 at 0.25 | 19.6 |
| render 1024x768 region at 1 | 30.3 |
| brush stroke d80, hardness 1 | 19.5 |
| brush stroke d80, hardness 0.3 | 44.7 |
| gaussian blur r2 | 49.9 |
| gaussian blur r20 | 85.2 |
| levels | 1.7 |
| curves | 1.7 |
| hue/saturation | 18.6 |

Laptop timings drift with power state and temperature by a few percent between sessions; compare runs taken back to
back on the same power profile.

## P2: 16 bits per channel

2026-09-27, the P2 branch against ae37011 (NekoPhoto 1.6.1 with P1), both built in the same session, three alternating
rounds of `bench_core 9` (each figure the median of the three round medians):

| Operation | ae37011 (ms) | P2 (ms) | Change |
|---|---:|---:|---:|
| render 4000x3000, 12 layers | 212.96 | 212.49 | -0.2% |
| render 4000x3000 at 0.25 | 21.61 | 20.61 | -4.6% |
| render 1024x768 region at 1 | 33.58 | 33.66 | +0.2% |
| brush stroke d80, hardness 1 | 22.79 | 23.07 | +1.2% |
| brush stroke d80, hardness 0.3 | 53.66 | 53.20 | -0.9% |
| gaussian blur r2 | 52.93 | 51.60 | -2.5% |
| gaussian blur r20 | 85.84 | 86.90 | +1.2% |
| levels | 1.75 | 1.75 | 0.0% |
| curves | 1.76 | 1.80 | +2.3% |
| hue/saturation | 18.83 | 18.79 | -0.2% |

The lines past 2% were run again in six alternating rounds (25 runs each for the adjustments): curves 1.735 → 1.740
(+0.3%), hue/saturation 18.86 → 19.27 on an earlier build (+2.1%, before the 16-bit sources moved to the end of the
library) and 18.83 → 18.79 after. The 8-bit code is the same instructions as before, compared function by function
between the two builds (render_exec_u8, blend_u8, resample, the brushes, blur, filters and adjustments identical;
in render.cpp only `render()`'s depth switch differs), so what remains is placement in the binary and the laptop's
drift.

The same document at 16 bits (`convertSampleType`), new lines of `bench_core`:

| Operation | Median (ms) |
|---|---:|
| render16 4000x3000, 12 layers | 230.0 |
| render16 4000x3000 to display (reduced to 8 bits for the canvas) | 261.5 |
| render16 4000x3000 at 0.25 | 33.3 |
| render16 1024x768 region at 1 | 26.0 |

The 16-bit render includes the two adjustment layers, drawn at 8 bits and applied as a difference.

## Layer counts

`bench_core` also times edits on documents of 1,000, 5,000 and 10,000 layers: 32 x 32 pixel layers sharing one
raster, a folder for every ten, and a history already holding 100 steps. Each edit is timed as the app makes it,
inside one history step (`begin`/`end`, what `beginEdit`/`endEdit` do): renaming the middle layer, toggling its
visibility, moving the bottom layer to the middle of the stack, an empty step (the snapshot and comparison alone), an
undo and a redo together, building the render plan, and looking every layer up by id with `indexOf`.

2026-09-27, with other builds running on the machine (the 4000 x 3000 render was about 30% slower than above), median
of 9 runs, three rounds:

| Operation | 1,000 (ms) | 5,000 (ms) | 10,000 (ms) |
|---|---:|---:|---:|
| rename | 0.75 | 4.89 | 10.55 |
| visibility | 0.74 | 4.82 | 10.63 |
| move | 0.76 | 4.94 | 10.65 |
| beginEdit/endEdit | 0.38 | 3.73 | 8.02 |
| undo + redo | 0.34 | 2.88 | 7.07 |
| plan build | 0.55 | 5.15 | 12.84 |
| find each by id | 1.22 | 27.46 | 130.57 |

Each single edit grows about linearly with the layers. Before this table, keeping the history within its byte
limit walked every layer of every snapshot after each edit, undo and redo: 92 ms to rename a layer and 174 ms for an
undo and a redo at 10,000 layers. Each history step now lists its buffers once when it is recorded.

Finding every layer by id one after another is quadratic (`indexOf` is a linear search): 130 ms for all 10,000, about
13 µs per lookup. No single edit makes that many lookups, so it is left as it is; a pass that needs them should build
its own id-to-index map, as the render plan does.

The existing lines, A/B against 68435cb in the same session (three alternating rounds of `bench_core 9`, the lines
past 2% rerun in six to ten alternating rounds of 25 to 51 runs): every line within ±2% but render at 0.25, brush
stroke d80 hardness 1 and curves in the first rounds (+3.1%, +8.4%, +7.2%), which the reruns put at -11.8%, +1.1% and
-2.8%: the machine's load, as the only change on those paths is one uncontended mutex per top-level parallel loop.

## P3a: editing at 16 bits

2026-09-27, P3a against 68435cb, four alternating rounds of `bench_core 9` (median of round medians). The 8-bit
object code of the benched functions is identical to 68435cb, so the 8-bit differences (brush -8% to +3%, blur r2
+2.9%) are the desktop's load; render16 now draws its two adjustment layers natively.

| Operation | 68435cb (ms) | P3a (ms) |
|---|---:|---:|
| render 4000x3000, 12 layers | 205.3 | 205.6 |
| gaussian blur r2 / r20 | 51.3 / 84.1 | 52.8 / 85.1 |
| levels / curves / hue/saturation | 1.69 / 1.78 / 19.3 | 1.72 / 1.80 / 19.1 |
| render16 4000x3000, 12 layers | 231.6 | 214.0 |
| u16 gaussian blur r2 / r20 | | 53.5 / 87.0 |
| u16 levels / curves / hue/saturation | | 7.7 / 8.0 / 46.2 |

## P3b: painting at 16 bits

2026-09-27, P3b against 1efe1c2, alternating rounds on a loaded machine (load average 9 to 29 from other builds;
medians of round medians, `bench_core 9 "brush stroke d80"` ten rounds, `--bench-brush classic/dry_brush --bench-size
1086x1448` twelve rounds, per-stroke medians). The 8-bit differences are within the rounds' spread.

| Operation | 1efe1c2 | P3b |
|---|---:|---:|
| brush stroke d80 hardness 1 (ms) | 28.4 | 28.7 |
| brush stroke d80 hardness 0.3 (ms) | 70.4 | 66.5 |
| dry_brush press / move p50 / move p95 / release (ms) | 5.14 / 1.55 / 2.60 / 7.95 | 5.01 / 1.49 / 2.34 / 7.95 |
| u16 brush stroke d80 hardness 1 / 0.3 (ms) | | 27.3 / 36.5 |

## Mosh

2026-09-27, the same machine; `bench_core 9 mosh`: the heaviest Filter > Mosh effects ([mosh.md](mosh.md)) at their
defaults on a 4000x3000 layer, straight-colour conversion in and out included.

| Effect | Median (ms) |
|---|---:|
| mosh pixel-sort | 52.0 |
| mosh vhs | 104.2 |
| mosh crt | 69.2 |
| mosh hard-glitch | 95.1 |
| mosh glow (four passes) | 747.8 |
| mosh light-streak (five passes) | 704.4 |
| mosh feedback (ten passes) | 888.5 |
| mosh optical-flow | 228.8 |

The multi-pass effects keep their frames between passes at 16 bits a channel.

The fingerprints of every effect (defaults, seeded settings, a translucent layer) are in `tests/mosh_hashes.txt`,
checked by `mosh_tests`; `COMPOSITOR_UPDATE_MOSH_HASHES=1 build/tests/mosh_tests fingerprints` rewrites them.

## Worst-case brush

Report only, no threshold: `bench_core 3 worst` (it runs only when the filter names it, about half a minute a run). A
10-second recorded pen stroke (1201 reports at 120 a second, deterministic) with a tip brush that uses every per-dab
feature at once: a 200-pixel soft tip at 2% spacing, 16 dabs a step scattered half a size both ways, moving (Stroke)
grain, ScreenSpeed on size and opacity, tilt on roundness, the lean's direction and the barrel's roll on the angle and a
size jitter, on a 4096 x 4096 layer. Memory is the most the stroke held beyond what was held before it, sampled every
millisecond: malloc's bytes (glibc `mallinfo2`, which covers the pixel buffers' calloc) and the resident set.

2026-09-27, NekoPhoto 1.8.0 with the 1.8.1 brush work; AMD Ryzen AI 9 HX 370 (24 worker threads), GCC 16, Release,
median of 3 runs:

| Depth | Wall time (ms) | Samples/s | Dabs | Dabs/s | Peak temporary memory |
|---|---:|---:|---:|---:|---|
| 8 bits | 29 366 | 41 | 287 472 | 9 789 | 80 MiB allocated, 76 MiB resident |
| 16 bits | 29 438 | 41 | 287 472 | 9 765 | 160 MiB allocated, 150 MiB resident |

The stroke paints about three times slower than it was drawn: at these settings (16 dabs of 200 pixels every 4 pixels)
the engine cannot keep up with the pen. Both depths cost the same; the dab count, not the sample depth, sets the time.

The bench also prints a fingerprint of the painted pixels (FNV-1a 64 over the working image, taken after the timing):
`9bc4ba5c82371c2c` at 8 bits and `994448994d69ca4c` at 16. A change that is meant to be faster must leave both alone.

## Brush performance pass

2026-09-27, after NekoPhoto 1.8.1 (7fd93d8), same machine and compiler. Every change keeps the output byte for byte:
`brush_parity` (8-bit, u16 and synthetic scenes), `brush_grain_tests`, `render_hash_tests`, `depth_paint_tests` and
the worst case's fingerprints are unchanged.

`bench_core` gained four lines for it: `brush stroke d400 hardness 0` (a 400-pixel soft round brush along the d80 path)
and `tip brush textured d60` (a 64-pixel soft tip at 10% spacing, two dabs a step scattered a quarter size, canvas
grain, pressure on size and a size jitter, pressed along the same path at 120 reports a second), each at 8 and 16 bits.

**Profiles before.** Sampled at 1 kHz of CPU time over all threads (a `SIGPROF` sampler; `perf` is not installed):

- Worst case: 72% of the CPU in the worker pool waking and parking threads (condition variable broadcast and the
  futex behind it), 20% stamping dabs (the per-pixel loop 14%, the bilinear sample of the tip 5%, `lround` 2%). Every
  dab handed its rows to the pool on its own, and every hand-out woke all 23 workers.
- Round d400 soft: 75% waking and parking workers; the rest the 8-bit merge and the recompose.
- Round d80: the 8-bit soft merge (`row + t × (255 − row) / 255`) 18%, the recompose 8% at 8 bits and 20% at 16.
  The 8-bit merge ran slower than the 16-bit one: it did not vectorise.
- Textured tip d60: the per-pixel loop 33%, the bilinear sample 13%, `lround` 9%, the recompose 12%.
- Dry brush (MyPaint) through the canvas: libmypaint and the repaint; nothing in the stamp engines.

**Profiles after.** Worst case: the per-pixel loop 37%, the bilinear sample 17%, the rounding 13%, the pool 6%. Round
d400: the merge on the calling thread and the recompose; the pool's share is what the recompose's own hand-outs cost.

**The changes**, each measured against the one before (fastest of alternating rounds for the everyday lines, one run
for the worst case):

| Change | What it measured |
|---|---|
| The pool wakes one worker per spare chunk instead of all of them | round d400 soft 133 → 118 ms (8 bits), 142 → 125 ms (16) |
| Tip brushes place a step's dabs first, then draw them in bands of rows, each band through the dabs in order | worst case 29.4 → 4.6 s (8 bits), 29.1 → 4.6 s (16) |
| A leaner per-pixel loop: direct bilinear reads inside the tip, inline rounding, a grain factor table, a mask for power-of-two grains, only the span of each row the tip can land on | worst case 4.6 → 4.2 s |
| Pixels a dab cannot raise (at its ceiling, or so near that its largest step rounds to nothing) are passed over before sampling | worst case 4.2 → 2.6 s (8 bits), 2.8 s (16) |
| The rounding without a branch (the branch on the fraction made the textured tip 30% slower than `lround`) | worst case 2.6 → 2.0 s; textured tip 103 → 86 ms |
| The 8-bit stamp merge vectorises (the row length was read through a reference a byte store may alias) | round d80 hardness 1 19.2 → 12.1 ms, hardness 0.3 43.8 → 15.4 ms |
| A stamped dab merges on one core up to 2^20 pixels | round d400 soft 116 → 40 ms (8 bits), 126 → 71 ms (16) |

**Before and after.** 7fd93d8 against the pass, alternating rounds (six rounds of `bench_core 15 "brush "`, ten of 25
for the u16 d80 lines, two of `bench_core 3 worst`): the median of the round medians, and the fastest run.

| Operation | 7fd93d8 median (fastest) | After median (fastest) | Change (median) |
|---|---:|---:|---:|
| worst case, 8 bits (ms) | 31 928 (29 522) | 2 104 (2 045) | -93.4% |
| worst case, 16 bits (ms) | 32 662 (30 217) | 2 144 (2 085) | -93.4% |
| brush stroke d80 hardness 1 | 22.46 (17.57) | 14.45 (11.23) | -35.7% |
| brush stroke d80 hardness 0.3 | 46.64 (43.15) | 18.07 (15.50) | -61.3% |
| brush stroke d400 hardness 0 | 148.92 (133.48) | 45.81 (39.98) | -69.2% |
| tip brush textured d60 | 123.22 (101.69) | 97.20 (79.11) | -21.1% |
| u16 brush stroke d80 hardness 1 | 23.29 (20.17) | 23.07 (20.09) | -0.9% |
| u16 brush stroke d80 hardness 0.3 | 30.12 (26.47) | 30.33 (26.65) | +0.7% |
| u16 brush stroke d400 hardness 0 | 160.91 (140.13) | 80.34 (70.62) | -50.1% |
| u16 tip brush textured d60 | 125.22 (103.67) | 96.31 (78.32) | -23.1% |

The worst case now paints 287 472 dabs in about 2.1 s (about 135 000 dabs a second): the 10-second stroke paints in a
fifth of the time it took to draw, at either depth, with the same peak memory (80 and 160 MiB).

Brush latency through the canvas (`--bench-brush`, 1086 x 1448, `COMPOSITOR_WINDOW_SIZE=1400x1000
QT_SCALE_FACTOR=2.25`, eight alternating rounds, medians of the per-run medians; press / move p50 / move p95 /
release, ms):

| Brush | 7fd93d8 | After |
|---|---:|---:|
| classic/dry_brush | 1.96 / 1.15 / 1.43 / 4.70 | 2.06 / 1.07 / 1.42 / 4.58 |
| round 80 px, hardness 0.3 | 2.45 / 1.62 / 2.12 / 3.92 | 2.19 / 1.44 / 1.98 / 3.28 |
| round 400 px, hardness 0 | 5.01 / 5.36 / 6.27 / 4.83 | 4.94 / 5.43 / 6.55 / 4.87 |

Dry brush is libmypaint's and the large round brush's moves are the repaint's, so both stay where they were.

This laptop has two kinds of core (4 at 5.2 GHz, 8 at 3.3 GHz): a single-threaded line runs about 20% slower when the
scheduler puts its thread on a slow core, so round medians are bimodal. Compare many alternating rounds, and the
fastest runs beside the medians.

**Not done because the output would change:**

- Dividing by the dab's scale through a reciprocal (`× (1 / sx)`): `brush_parity` fails, and the worst case measured
  2.09 s against 2.01 s, no gain.
- Stepping the tip's u and v along a row by adding a per-pixel delta instead of computing them from the position:
  rounding differs in the last bits, which moves the bilinear taps.
- `floor(v + 0.5)` for the value's rounding: differs from `lround` just below a half.
- A stamp cache keyed by a quantised size, angle and roundness: every dab of the worst case has its own size, angle,
  roundness and scatter, so a cache would only help by snapping them, which changes every dab.
- Single precision in the per-pixel loop.

**What is left.** The worst case is now compute-bound in the per-pixel loop (two divisions, the bilinear sample and
the grain lookup per pixel, about 15 ns a pixel on a fast core, over 2.7 billion pixel visits). The recompose of each
sample's area (0.3 s of the 2.1) and placing the dabs (0.1 s) are next. A tip brush's batch goes to the pool only
from 65 536 pixels; lowering it to 4 096 did not help the textured tip measurably. The round brush's 8-bit recompose
has a branch per pixel and stays scalar.

## Work counters

The benchmarks time the work; `work_counter_tests` and the `work-counters` self-test (both in ctest) count it, so a
change that makes an operation do more work fails on any machine, loaded or not: pixels rendered per brush move and per
pan step, layers drawn per adjustment slider tick, reductions built, Smart Filters run, history bytes kept. What each
counter measures, how its bounds were chosen and how to update them: [work-counters.md](work-counters.md).

## Render hashes

The matching correctness gate is `render_hash_tests` (in ctest): 235 scenes (133 at 8 bits, 102 at 16 bits) hashed with FNV-1a 64 against
`tests/render_hashes.txt`, each rendered on the worker pool and serially. After an intentional rendering change:

```bash
COMPOSITOR_UPDATE_RENDER_HASHES=1 build/tests/render_hash_tests
```
