# Pixel maths review: faster and better algorithms for what we have

Six parallel reviews of the pixel pipeline (compositing, filters, adjustments,
selections, painting, geometry), September 2026. Research only; nothing here
is implemented yet. Every item names the file and line it refers to, the
expected gain, whether output stays identical, and the effort in days.

Measurements are on a 4000 × 3000 RGBA document, Ryzen AI 9 HX 370, `-O2`.

## Bugs and waste found on the way

These are not optimisations; they are things that are wrong or needlessly slow
today, all cheap to fix.

| Where | What | Effect | Fix |
|---|---|---|---|
| `warp.cpp:100`, `render.cpp:227` | The source image is copied into `MipCache` under a temporary pointer, so the mip chain is rebuilt on every warp or resample and never reused | A full-image copy plus mip build per distort-preview step | Pass the caller's `ImagePtr` (¼ d) |
| `EditorSession.cpp:1603` | `endWarp` appends every warp point without `takeDirtyRect`, so each append recomposes the union of the whole stroke | Long Liquify strokes spend seconds in write-back, O(N²) | Clear the dirty rect per append (¼ d) |
| `selection.cpp:166` | `wand_trace` returning "too detailed" (−2, above 8 M edges) is swallowed | The selection exists but draws no marching ants; the Mac shows an error | Surface it, then see raster ants below |
| `adjustments.cpp:437` | The Hue/Saturation 33³ cube is built with `parallelRows(0, 33)` whose default 24-rows-per-thread minimum puts it on one thread | 2–7 ms per slider tick, single core | `minRowsPerThread = 1` (0.05 d, 4–8×) |
| `EditorSession.cpp:1400-1408` | Clone Stamp flattens the whole document once per stroke even for one untransformed layer | Stroke start latency on big documents | Sample the layer directly when it is alone |
| `EditorSession.cpp:1602` | The warp result is deep-copied although it is already a `shared_ptr<const Image>` | A wasted full-layer copy per stroke | Drop the copy |
| `HealPixels.c:137` vs `:232` | Any nonzero coverage counts as hole, but the result is blended by coverage × opacity | Soft healing tips leave a half-healed ring | Coverage ≥ 128 is hole, else ring (½ d) |
| `render.cpp:306, 321` | `resizeDocument` resamples with each layer's own sampling, not the Image Size dialog's choice | The Resampling combo has no effect on layers | Pass the dialog's sampling |
| `transform.h:29`, `warp.cpp:68-71` | `radians()` via `fmod` leaves 90° corners at 300 + 1e-14; floor/ceil then widens the warp by a pixel | A soft extra column on rotated layers | Snap corners to 1e-6 before framing |
| `image.cpp:189-192` | `MipCache::shared()` mutates its map without a mutex | A data race if anything renders off the main thread | A mutex around `level()` costs nothing |
| `filters.cpp:193` | `Image source = image;` in motion blur is dead | A 48 MB copy per run | Delete |
| `render.cpp:118` | `row[x] * outside / 255` truncates while every other `/255` rounds | A −½ LSB bias on mask edges | Add 127 |
| `EditorSession.cpp:2120` | The wand passes sample radius 0 | The Mac's 3×3 and 5×5 Sample Size option is missing | Expose it |
| `WandPixels.h:5-8` | The wand compares premultiplied RGBA | A 50 %-alpha pixel of the same hue differs by up to 128, so fills stop early on soft edges; Photoshop compares straight colour | Unpremultiply in the compare |

## Ranked: quick wins (½ day or less each)

| # | Change | Gain | Output | Days |
|---|---|---|---|---|
| 1 | Add Noise: row-parallel, compute the normal once when monochromatic, inverse-CDF LUT for the Gaussian, reciprocal-alpha table (`NoisePixels.c`) | 20–40× (681 → ~20 ms) | Parallel and mono: identical; LUT: new pattern | ½ |
| 2 | Lens Correction: row-parallel, float or fixed-point bilinear instead of double with four `lround` (`LensPixels.c`) | 15–20× (325 ms → ~20 ms) | ±1 LSB | ½ |
| 3 | Levels / Curves / Exposure: 256-entry byte LUT per channel plus a 16.16 reciprocal-alpha table instead of a float divide and float interpolation per channel (`LevelsPixels.c:3-16`) | 8–15× | Identical at alpha 255, ≤1 LSB otherwise | ½ |
| 4 | Selection bounds cached on the selection; `isEmpty` via early exit; word-wide scans in `nonzeroBounds` (`document.cpp:108`, `image.cpp:37`) | 10–20×; halves the scans callers do today | Identical | ½ |
| 5 | Gaussian blur: replace the two naive transposes (65 % of the time) with a 16-column-band vertical pass (`filters.cpp:102-109`) | 1.6× (216 → ~130 ms) | Identical | ½ |
| 6 | Compositing: hoist `1/sx`, `1/sy`, `1/factor` per layer and skip edge anti-aliasing on the interior span of each row (`render.cpp:195-208`) | 1.5–2× on the rotated/scaled path | No visible change | ½ |
| 7 | Adjustment layers in Normal mode blend in premultiplied space, `d' = d + ((a − d) m + 128) >> 8`, instead of six float divides (`render.cpp:486-495`) | ~5× on that pass | ±1 | ⅓ |
| 8 | Mask mips through the existing `MipCache::level(GrayPtr)` instead of rebuilding per frame (`render.cpp:103-108`) | Removes a halving chain per frame for masked folders | Identical | ⅓ |
| 9 | Invert as one 32-bit op, `(v & 0xFF000000) | (a·0x010101 − (v & 0xFFFFFF))` (`adjustments.cpp:479`) | 4–8×, vectorises | Identical | 0.1 |
| 10 | Gradient Map with one divide: luma is linear in premultiplied channels (`AdjustPixels.c:12-19`) | 2–3× on soft edges | Index may move ±1 | ¼ |
| 11 | Build flags `-march=x86-64-v2` (or `-msse4.1`) so `roundf`, `floor`, `fminf` become single instructions across the C kernels | Free | Identical | 0.1 |
| 12 | `coverageFromLayer` copies alpha directly for identity or integer-translate transforms (`selection.cpp:178-190`) | 4–10× | Identical | ½ |
| 13 | Selection combine loops over flat bytes without the pre-copy; add Intersect (Shift+Alt) | 1.5–2×, plus a missing mode | Identical | ¼ |

## Ranked: medium (1–2 days each)

| # | Change | Gain | Output | Days |
|---|---|---|---|---|
| 14 | **Expand/Contract via an exact Euclidean distance transform** (Felzenszwalb–Huttenlocher 2012, Meijster 2000): O(N), parallel, coverage `clamp(r + 0.5 − √d², 0, 1)`. Today is O(W·H·πr²) single-threaded; r = 100 on 12 MP takes hours (`selection.cpp:135-156`). Also gives Border and Smooth for free. | 50× at r = 10, ~5000× at r = 100 | Clean anti-aliased ramp instead of propagated grey levels | 1 |
| 15 | **Motion blur as shear → 1-D running-sum box → un-shear** (Paeth 1986). Today is O(N·d): d = 100 takes 1.7 s, d = 2000 about 33 s (`filters.cpp:188-225`). | 10× at d = 100, ~200× at d = 2000 | Comparable softness, not bit-identical | 1–1.5 |
| 16 | **Persistent thread pool with a tile queue** (256×32 tiles, atomic counter) instead of spawning `std::thread`s per `parallelRows` call (`parallel.h:17-31`), which costs 0.5–1 ms per layer per frame | Dominant fixed cost of interactive redraw gone | Identical | 1 |
| 17 | **Integer premultiplied blend formulas** for the 12 non-Normal modes (pixman/Cairo forms, PDF 32000-1 §11.3.5), templated on mode so the switch leaves the loop; nine modes need no division (`blend.cpp:89-115`) | 4–6× scalar, 10×+ with SIMD | ±1 LSB on semi-transparent pixels; goldens allow ±2 | 1½ |
| 18 | **Incremental homography stepping** in `warpImage`/`warpMask`/`warpCoverage`: `(X, Y, W) += (dX, dY, dW)` per pixel with one reciprocal, footprint from the analytic Jacobian, instead of three full projective maps and two `hypot` per pixel (`warp.cpp:106-118`) | 2–3× on every warp, distort preview, Image Size | Identical in practice (double) | 1 |
| 19 | **Brush dab from an r² lookup table** (incremental `dx² + dy²`, no `sqrt` or `exp` per pixel; MyPaint, Krita) (`brush.cpp:212-243`) | 3–8× per dab | ≤ 1/255 | 1 |
| 20 | Wand: branchless lo/hi compare that vectorises to `pminub`/`pmaxub`, tri-state visited mask so each pixel is tested once, and a cached sample render keyed by layer and transform (`WandPixels.c:9-85`) | 3–20× | Identical | 1 |
| 21 | Gaussian blur on integer running sums (uint8 → int32) instead of two 16-byte-per-pixel float buffers, which reach 3.2 GB at the pixel budget (Elboher & Werman 2011) | 3–4× overall, 4× less memory | ±1 LSB | 1–2 |
| 22 | Fast guided filter for matte refinement (He & Sun 2015): subsample the guide and mask by 4, box-filter at N/16, upsample the coefficients, evaluate against the full-resolution guide. Today's `limit` path upsamples the *result*, discarding the hair detail the filter exists to recover (`matte.cpp:91-93`). | 5–6× at full resolution | Better, not identical | 1 |
| 23 | Heal: a real multigrid V-cycle (bilinear prolongation, 2–3 sweeps per level, 3–5 cycles) instead of two nearest-neighbour levels with 40 sweeps (`HealPixels.c:54-113`; Briggs, *A Multigrid Tutorial*) | ~4× on the solve | Same solution | 1 |
| 24 | Signed-area coverage rasterizer for lasso, marquee and ellipse (stb_truetype v2, font-rs) with the ellipse flattened by sagitta tolerance; today every sub-scanline tests every edge and a 3000 px ellipse costs ~49 M edge tests with 5-level vertical AA (`selection.cpp:20-99`) | 10–50× on large ellipses | 256-level AA on shallow edges (better) | 1–2 |
| 25 | Marching ants as a raster pass over the viewport (boundary = mask ≠ shifted mask, dashed by `(x + y + phase) mod 8`) when the outline exceeds ~200 k edges, with the cached vector path below that (`CanvasWidget.cpp:283-331`) | Unbounded outline size; no "too detailed" failure | Screen-pixel ants at low zoom (Photoshop's look) | 1–2 |
| 26 | Hue/Saturation: tetrahedral integer interpolation (4 corners, uint16 cube), exact on the r = g, g = b, r = b creases where trilinear errs by up to 2 levels; Colorize via a 511-entry LUT keyed by max + min (`adjustments.cpp:401-471`; Kasson et al. 1995) | 2.5–4× per pixel | ≤ 2 levels more accurate | 1 |
| 27 | Bicubic (Catmull-Rom or Mitchell) magnification and a rounded or trilinear mip choice for `High` sampling, which is currently identical to `Smooth` (`transform.cpp:6-20`, `image.cpp:180-184`) | Less aliasing zoomed out; sharper zoomed in | Visible improvement | 1 |
| 28 | `MipCache` size budget (the Mac caps at 400 MB), mutex, optional Lanczos halving to match the Mac's vImage chain (`image.cpp:178-215`) | Bounded memory | Identical unless Lanczos | 1 |

## Ranked: larger (2 days and up)

| # | Change | Gain | Output | Days |
|---|---|---|---|---|
| 29 | **1:1 axis-aligned row path with SIMD Normal blend** (u16 lanes via GCC vector extensions, AVX2 behind a CPU check): when a layer sits at integer translation and 100 % scale, blend rows directly with `((t + 128)·257) >> 16`, which equals `round(t/255)` exactly (Blinn, *Three Wrongs Make a Right*; Skia `SkMulDiv255Round`) (`render.cpp:189-211`) | 5–10× on the common unrotated case; near memory bandwidth | Identical | 2 |
| 30 | **Separable two-pass resampling** (Lanczos-3 or Catmull-Rom, per-column weight tables, kernel widened by the reduction factor) for axis-aligned Image Size and `resampleLayer`, instead of routing every layer through the projective warp with box mips (`render.cpp:219-332`, `304-306`; Turkowski 1990, Chromium `image_operations.cc`) | Matches or beats today's speed with the Mac's "High" quality | Changes non-solid pixels; tests hold | 1½–2 |
| 31 | **Per-stroke precomputed dab stamp** with 4×4 subpixel variants and SIMD max/screen accumulation (Krita `KisDabCache`) | 50–100× per dab (≈ 0.05 ms for a 500 px brush) | ≤ ¼ px rim shift | 2 |
| 32 | **Composite caching around the edited layer**: keep the layers below as a backdrop and pre-flatten those above when all are Normal (source-over is associative), so a frame while painting is backdrop + active layer + one blend (`render.cpp:554-559`) | 5–20× while painting or dragging in tall stacks | ±1 on semi-transparent pixels | 2–3 |
| 33 | **Liquify as an accumulated displacement field**, resampling the original once with bicubic at preview and commit, instead of re-resampling the already-warped pixels ~40 times per pass (`warpstroke.cpp:82-120`; GIMP warp tool, Krita liquify worker) | Removes the cumulative blur; enables strength-after and reconstruct | Visibly sharper | 2–3 |
| 34 | Deriche order-4 recursive Gaussian as the single code path for all sigma: 0.03–0.05 % kernel error at every sigma versus 0.2–1 % for the FIR and 5–6 % for the three-box path above sigma 6, at the same speed (Deriche 1993; Getreuer, IPOL 2013). Young–van Vliet and Alvarez–Mazorra were checked and rejected: the former collapses above sigma ≈ 100, the latter is worse than three boxes. | Consistent kernel across the sigma switch | Small change on point highlights | 1–2 |
| 35 | Fixed-point 8.8 bilinear in u16 SIMD lanes (after #29) | 2–3× on sampling | ±1 | 1 |
| 36 | Copy-on-write stroke buffers: today every stroke copies `base`, `working` and `coverage` for the whole grid, 144 MB on a 16 MP document (`brush.cpp:79-101`) | 30–60 ms off stroke start on big documents | Identical | 2–3 |
| 37 | Brush spacing policy: a hard brush needs only `d = √(2r)` spacing for a ¼ px scallop (3× fewer dabs); soft build-up today depends on spacing, so a stroke's edge changes with speed (alpha-darken or MyPaint's `opaque_linearize` fixes it) | 3× fewer dabs; speed-independent edges | Visible; make it a setting | 1 |
| 38 | Spot healing and Content-Aware Fill on a shared **PatchMatch** core (Barnes et al. 2009; Wexler et al. 2007) with a coarse-to-fine pyramid and patch voting, keeping the membrane blend. Today healing picks one rigid source patch from a sparse 120-candidate search and cannot continue edges; Content-Aware Fill is greedy single-pixel onion-peel synthesis at ~3 s per megapixel of hole, single-threaded, with global random donors. | Edges and textures continue; parallel | Different (better) | 5–8 |
| 39 | Fused LUT for stacked Levels/Curves/Exposure adjustment layers: compose in float, quantise once | One pass instead of N; less rounding | ≤ 1 LSB more accurate | 2–3 |

## Correctness notes worth a decision

- **Coverage quantisation differs by blend mode**: Normal uses k/256, the other modes exact float, so a 50 % layer differs by ±1 between modes (`blend.cpp:79, 89-91`). Pick one; k/256 is what the SIMD paths will use.
- **Edge anti-aliasing uses `min(ex, ey)`** (`render.cpp:199`); true corner coverage is `ex·ey`, so layer corners can be up to 25 % too opaque.
- **Samples are rounded to 8 bits before the blend rounds again** (`render.cpp:209`); feeding 16-bit samples into the blend removes up to ½ LSB of error once the integer blends exist.
- **Luma coefficients**: Gradient Map and Grain use Rec. 709 on encoded values; Photoshop uses 0.299/0.587/0.114 there. Pure green differs by ~32 levels. Product decision.
- **Gamma**: everything (blend, mip, bilinear, adjustments) runs on sRGB-encoded bytes, which matches Core Graphics, the Mac app, and Photoshop's 8-bit default. Keep it.
- **Curves** uses the Fritsch–Butland monotone spline, the same as the Mac; Photoshop's spline overshoots and clips. Keep ours.
- **Lens Correction** is Brown–Conrady's k₁ term with inverse mapping; at k = −0.35 the corner sampling spacing reaches 2 px, so bilinear aliases there. Catmull-Rom or 2×2 supersampling fixes it; a radial LUT would not help (the polynomial is three flops).

## Suggested order

1. The bug list above, in one pass (about two days total, most items minutes).
2. Quick wins 1–13 (about four days) for broad, safe speedups.
3. #14 distance transform, #15 motion blur, #16 thread pool, #18 warp stepping, #19 brush LUT: the largest user-visible latency fixes (about five days).
4. #29 SIMD Normal path and #32 composite caching: interactive redraw on big documents.
5. Quality items as wanted: #30 resampling, #33 liquify field, #38 PatchMatch.

## Sources

W3C Compositing and Blending Level 1; PDF 32000-1:2008 §11.3.5; Porter & Duff 1984; Blinn, *Three Wrongs Make a Right*; pixman `pixman-combine32.c`; Skia `SkBlendMode.cpp`, `SkColorPriv.h`; Kovesi, *Fast Almost-Gaussian Filtering*, DICTA 2010; Deriche 1993 (INRIA RR-1893); Getreuer, IPOL 2013; Elboher & Werman 2011; Gwosdek et al. 2011; Paeth 1986; Marsaglia & Tsang 2000; He, Sun & Tang, *Guided Image Filtering*; He & Sun 2015 (arXiv:1505.00996); Kasson et al. 1995; Kang 1997; Fritsch & Carlson 1980; Fritsch & Butland 1984; Poynton, *Digital Video and HD*; Felzenszwalb & Huttenlocher 2012; Meijster et al. 2000; van Herk 1992; Gil & Werman 1993; Heckbert, *A Seed Fill Algorithm* (Graphics Gems 1990); stb_truetype v2 rasterizer; font-rs; Heckbert 1989, *Fundamentals of Texture Mapping and Image Warping*; Wolberg 1990; Greene & Heckbert 1986; Keys 1981; Mitchell & Netravali 1988; Turkowski 1990; Williams 1983; Chromium `skia/ext/image_operations.cc`; Pillow `Resample.c`; MyPaint `brushlib`; Krita `KisDabCache`, `KisLiquifyTransformWorker`; GIMP `gimpwarptool.c`; Pérez et al. 2003; Farbman et al. 2009; Barnes et al. 2009; Wexler et al. 2007; Briggs, *A Multigrid Tutorial*.
