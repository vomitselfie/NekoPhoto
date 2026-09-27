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

## Render hashes

The matching correctness gate is `render_hash_tests` (in ctest): 235 scenes (133 at 8 bits, 102 at 16 bits) hashed with FNV-1a 64 against
`tests/render_hashes.txt`, each rendered on the worker pool and serially. After an intentional rendering change:

```bash
COMPOSITOR_UPDATE_RENDER_HASHES=1 build/tests/render_hash_tests
```
