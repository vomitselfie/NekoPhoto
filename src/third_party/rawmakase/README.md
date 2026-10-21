# RAWmakase's measured Camera Raw tables

Four tables from [RAWmakase](https://github.com/pch/rawmakase) (`crates/rawmakase-engine/src/develop/*.bin` at commit
7c19ab8d78fc91ebc18dd9514b4f83e405c27953), unmodified, copyright (c) 2026 RAWmakase contributors, MIT
([LICENSES/RAWmakase-MIT.txt](../../../LICENSES/RAWmakase-MIT.txt)). `src/core/CMakeLists.txt` compiles each into the
core with `embed_blob.cmake`; `src/core/src/rawmeasured.cpp` reads them (a port of the Rust that reads them there), and
`src/core/src/rawtone.cpp` renders the Camera Raw panels through them ([docs/camera-raw.md](../../../docs/camera-raw.md#fitted-responses)).

## Where they come from

RAWmakase fitted every table to Camera Raw 18.7 (Photoshop 2026) renders of synthetic chart DNGs it writes itself: an
invented camera with made-up matrices and a linear profile (its `tests/color/fit_chart.rs` and `tests/color/chart.rs`),
rendered with each setting, read back as 16-bit ProPhoto RGB patch means and fitted by its `scripts/corpus/color-mixer.py`
(`fit`, `fit-vibrance`), `scripts/corpus/parametric-curve.py` and `scripts/corpus/color-grading.py`. Its
`docs/color-mixer.md` and `docs/tone-controls.md` describe the measurements. None of them is derived from the Adobe DNG
SDK or from Adobe camera profiles: they are measurements of Camera Raw's output, as NekoPhoto's own parity corpus is.
Two parts were measured on photos rather than charts (RAWmakase's first tables, Camera Raw renders of nine of its own
photos from a Fujifilm X100F and Sony A7 II and A7CR): Saturation's and Vibrance's ±50 tables (tables 48 to 51 of
`color_mixer_chart.bin`, and the ±25 and ±50 grids of `vibrance_chart.bin`), and the cells of the band tables the chart's
colours do not reach. They are the change each slider makes to Camera Raw's own default render of those photos.

RAWmakase's checkout here is shallow, so the tables' history could not be read from git; the provenance above is what its
scripts and documents at that commit say.

## Format

Little-endian throughout.

- **`color_mixer_chart.bin`** (404,352 bytes): 52 tables of int16 at 1/8000 per step. Each is three channels (hue
  shift in turns, log2 of the saturation factor, log2 of the value factor) over a grid of 36 hues x 6 saturations x 6
  values of HSV in linear ProPhoto RGB (saturations spaced by the square root, values by v^0.45; hue-major, value
  fastest). Tables 0-47 are the colour mixer's bands, `(band * 3 + kind) * 2 + sign` with kind 0 hue, 1 saturation,
  2 luminance and sign 0 for -100, 1 for +100; 48 and 49 are Saturation -50 and +50, 50 and 51 Vibrance -50 and +50.
- **`vibrance_chart.bin`** (62,208 bytes): Vibrance's grids in the same layout at -100, -75, -50, -25, +25, +50, +75
  and +100.
- **`parametric.bin`** (59,280 bytes): 228 curves of 65 float32 samples over 0..1 of gamma-2.2 encoding, each a change
  to the identity: Shadows (8 amounts x 5 shadow/midtone split ratios, over 0 to the midtone split), Highlights
  (8 x 5, over the midtone split to 1), Darks and Lights (8 amounts x 7 midtone splits each), then Darks and Lights
  together at the default splits (6 x 6).
- **`color_grade_curves.bin`** (146,432 bytes): float32. Shadows', Midtones' and Highlights' two tint profiles over
  5 Blending x 17 Balance values x 64 samples of x^(1/2.2); Global's three profiles; each region's coefficients per
  channel and term over 12 hues x saturation 25, 50, 75 and 100; each region's Luminance gains at -100 to +100
  (without 0) over the 64 samples.
