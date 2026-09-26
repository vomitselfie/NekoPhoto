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

## Render hashes

The matching correctness gate is `render_hash_tests` (in ctest): 133 scenes hashed with FNV-1a 64 against
`tests/render_hashes.txt`, each rendered on the worker pool and serially. After an intentional rendering change:

```bash
COMPOSITOR_UPDATE_RENDER_HASHES=1 build/tests/render_hash_tests
```
