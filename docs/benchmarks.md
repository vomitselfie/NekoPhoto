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

## Render hashes

The matching correctness gate is `render_hash_tests` (in ctest): 235 scenes (133 at 8 bits, 102 at 16 bits) hashed with FNV-1a 64 against
`tests/render_hashes.txt`, each rendered on the worker pool and serially. After an intentional rendering change:

```bash
COMPOSITOR_UPDATE_RENDER_HASHES=1 build/tests/render_hash_tests
```
