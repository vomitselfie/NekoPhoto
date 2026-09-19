# Pixel maths review: faster and better algorithms for what we have

Six parallel reviews of the pixel pipeline (compositing, filters, adjustments,
selections, painting, geometry), September 2026. Research only; nothing here
is implemented yet. Every item names the file and line it refers to, the
expected gain, whether output stays identical, and the effort in days.

Measurements are on a 4000 × 3000 RGBA document, Ryzen AI 9 HX 370, `-O2`.

## Progress

Updated as batches land (see the "Pixel maths batch" commits). "Done" means
the item is in `src/core` with tests; a note says where it differs from the
plan.

| Batch | Items landed |
|---|---|
| 1, the bug list | All fourteen rows below, except the wand's premultiplied compare, which stays for parity with the Mac (see the correctness notes). |
| 2, quick wins | 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13 (5 is superseded by batch 3a). |
| 3a, blurs | 5, 15, 21, C25 (Gaussian: FIR in bands at σ ≤ 6, three 8.8 fixed-point box passes with running column sums above; motion blur by shear). |
| 3b, selections | 14, 24, 25, C6 (Expand/Contract by exact EDT; Feather, Smooth, Border; signed-area rasterizer; raster ants). |
| 4, compositing | 16, 17, 29 (the on-grid Normal row path, scalar `compositeSpanNormal` for now), C8. |
| 5a/5b, geometry and painting | 18, 19, 36 (the shared const base and bounded scans; no copy-on-write tiles), 37 (hard tips only). |
| G'MIC | Steps 1 and 2 of the plan at the end of this document. |
| 7, adjustments and the wand | 20 (compare once per pixel in a vectorised range test, fill over the match map, sampled pixels cached per document revision), 26 (tetrahedral 8.8 cube, exact 511-entry Colorize table), 39 (Levels/Curves/Exposure layers are transfers; a run at full opacity in Normal mode with no masks composes into one table applied in place), C2 (a "Photoshop saturation curve" checkbox and `saturationCurve` in the manifest, off by default for Mac parity). Also fixed on the way: the integer Normal blend for adjustment layers rounded negative deltas towards zero, so a darkening layer at full opacity was one level too light. |
| 13, G'MIC step 3 and the preview model | G'MIC step 3: libgmic in-process behind `COMPOSITOR_WITH_LIBGMIC` (one interpreter kept warm with the catalogue), opt-in at runtime with `COMPOSITOR_GMIC_INPROCESS=1` because libgmic 4.0.5 crashes inside `sharpen` when called as a library while the executable handles it. C11: PP-HumanSeg (OpenCV's model zoo, 192 px, 6 ms) is a model in the list and, when downloaded, gives Remove Background an instant coarse preview while the chosen model runs. |
| 12, painting frames and blur accuracy | 31 (on a document-aligned grid a dab is a precomputed tile at one of 4x4 subpixel phases merged row by row; `stampedDabs` in BrushSettings turns it off for comparison), 32 (`RenderCache`: while one layer is edited the layers below are kept composited and, when every layer above is a plain Normal pixel layer, those are flattened once, so a frame is backdrop + layer + one blend; the canvas keys it on the session's document revision), 34 with C26 (Deriche's fourth-order recursive Gaussian above sigma 6, row-major banded column pass, double state: within a level of the true kernel at every sigma, where the three boxes drifted by several; about 25 % slower than the boxes at 12 MP, 125 ms). C27 not needed. |
| 11, liquify | 33 and C4: Liquify keeps a displacement field over the touched box and resamples the untouched original through it (bicubic) after every push, so a long stroke never blurs and a stroke pushed back lands on a sharp edge again; the falloff is Gustafsson's forward warp shaped by the brush hardness, shrinking with the drag length. Smudge is unchanged (it mixes paint rather than warping). |
| 10, content-aware fill | 38: `src/core/src/inpaint.cpp` synthesises a hole coarse to fine with PatchMatch nearest-neighbour fields and patch voting (Wexler/Barnes); Content-Aware Fill and the brush's Content-Aware mode use it (the latter adds the membrane over a synthesised band, so tone still matches); Proximity Match keeps the rigid nearest patch. From C19: 5x5 patches, random search around the current best, vote weights that fall off from the best covering patch and never collapse. C18 (dominant offsets), C20 (structure sparsity) and C23 (Criminisi priorities) were not needed. 200x200 hole in a 12 MP image: 665 -> 50 ms; 600x600: 266 ms. |
| 9, healing | 23 by way of C22: the membrane is evaluated with mean-value coordinates over the polygon of known pixels around the hole (hierarchically sampled boundary, rows in parallel), exact on ramps, replacing the relaxation sweeps; C21's sine-transform solve was not needed. Spot healing lives in `src/core/src/heal.cpp` (`spotHeal`), held to the C reference in `tests/heal_tests.cpp`. 4000x3000, 300 px spot: 1.3 s -> 16 ms. |
| 8, resampling | 27 (High = Catmull-Rom point sampling on the mip level nearest 1x; Smooth = bilinear on the floor level, as before), 28 (the cache holds weak references only and a 400 MB budget with least-recently-used eviction; it used to keep every source alive through its level-0 entry), 30 (pure scaling, which is what Image Size and unrotated distorts are, goes through a separable Lanczos-3 (High) or triangle (Smooth) resample with the kernel widened by the reduction, no mips), 35 (8.8/10-bit fixed-point bilinear and bicubic in 32-bit lanes), C3 (the fixed-point layout; a full `pshufb`/`pmaddwd` pipeline is not needed at these sizes), C5 (vectorised halving, 4000x3000 in 1.5 ms), C29 (Lens Correction: a Bicubic checkbox and `bicubic` over automation, off by default for parity with the reference). |
| 6, matting | 22, C10, C13, C14, C15, C17, C28 (the coefficient grid is 2× or 4× coarser by size). C13 differs: the guide is R, G, B only. With the mask as a fourth guide channel the filter fits the mask exactly (a_M → 1) and nothing moves, so the multichannel form uses the colour alone. C9: MODNet's preprocessing is recognised by file name (`modnet*.onnx`), but no download entry exists because the official release has no fixed-shape ONNX asset. |

Still open: C16, C24, C30 (C18, C20, C23 folded into 38's notes; C27 not needed).

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

## Second pass: Chinese literature and engineering write-ups

A second round of five reviews restricted to Chinese-language sources and work
from Chinese groups. Most of the practical material comes from the ImageShop
engineering blog (石林 / laviewpbt, who has spent a decade reproducing and
SSE-optimising Photoshop's own algorithms) and from a handful of papers by
Kaiming He, Jian Sun and their successors. Mainland hosts often refuse
connections from this machine, so several posts were read through mirrors.
Blog code is unlicensed unless noted: treat it as reference and re-derive.

### What changes the plan

| # | Finding | Source | Effect on the plan | Days |
|---|---|---|---|---|
| C1 | **Photoshop's opacity + blend-mode compositing may omit the `As·(1−Ab)·Cs` term** that `blend.cpp:105-107` implements from the PDF spec: a probe of Photoshop gives `Cr·Ar = Cb·Ab·(1−As) + As·F(Cs,Cb)`. The two agree for Normal and for opaque bases only. | laviewpbt, 「PS算法理论探讨一」*How Photoshop composites two 32-bit images*, 2021 | Verify with a small PSD probe before locking #17's integer formulas; adopt whichever matches | ½ |
| C2 | **Photoshop's saturation slider is not `s·(1+Δ)`**: with `L=(max+min)/510` and HSL `S`, for `inc ≥ 0`: `α = (inc+S ≥ 1) ? S : 1−inc; α = 1/α − 1; C' = C + (C−255L)·α`; for `inc < 0`: `C' = 255L + (C−255L)·(1+inc)`. | 阿发伯 (maozefa) 2012, reposted 2014 | Option for `adjustments.cpp:414`; diverges from the Mac's Core Image behaviour, so make it a product choice | ¼ |
| C3 | **Bicubic at bilinear cost**: 8-bit fractional fixed point (coordinate ≪ 16, top byte indexes a 513-entry kernel LUT), per-column weight tables, `pshufb` interleave so `pmaddwd` sums two taps of one channel, `packus` instead of clamps, border/interior split. 720p→1080p in 8.5 ms single-threaded. | laviewpbt, SSE 优化系列十八 (2018) and 短道速滑五 (2020) | The u8 SIMD layout for #30/#27/#35; makes "High" = Catmull-Rom essentially free | 1½ |
| C4 | **Liquify's forward warp** is Gustafsson 1993 §4.4: `u = x − ((r²ₘₐₓ − |x−c|²)/((r²ₘₐₓ − |x−c|²) + |m−c|²))² · (m−c)`; local scale `r' = r·(1 − a(1 − r²/R²))`. Our `warpstroke.cpp:104-107` uses a smoothstep that doesn't shrink with drag length as Photoshop's does. | xiaotie 2009; laviewpbt 眼睛放大 2014 | Fold into #33's displacement field; Bloat, Pucker and Twirl fall out of the same form | ½ |
| C5 | **Halving with SIMD**: `(p00+p01+p10+p11+2)>>2` via `pmaddubsw`/`phaddw`; the `std::min` clamps in `halveImage` (`image.cpp:78-93`) are what stop auto-vectorisation. A `[1 4 6 4 1]` blur-then-decimate in 16-bit is a cheaper anti-aliased halve than Lanczos for #28. | laviewpbt 2020, 2019 | 3–5× on mip builds, identical output | ¼ |
| C6 | **Expand/Contract/Smooth**: confirms Photoshop's kernels are circular; Smooth = "more than half the pixels within the radius are selected" = a box count, O(N) and radius-independent with the `(uint8)255 → (int8)−1` counting trick; **Feather N = Gaussian blur with σ = N** on the coverage (a difference layer against Photoshop is pure black). | laviewpbt EDM/erode posts 2018–2021; aiuai.cn | Keep the FH distance transform (#14); add Smooth and Feather, two missing Photoshop features, from the same EDT and blur | ¾ |
| C7 | **Magic-wand tolerance consensus**: `|R−R0| ≤ T ∧ |G−G0| ≤ T ∧ |B−B0| ≤ T` on straight RGB, seed colour = 3×3 or 5×5 mean. | myexception.cn; several reproductions | Confirms the unpremultiply fix and the Sample Size option already in the bug list | 0 |
| C8 | **Exact `/255` in SIMD**: `(x + 1 + (x>>8)) >> 8` (floor) and `(x + 128 + ((x+128)>>8)) >> 8` (round), add-and-shift only. | laviewpbt SSE 系列十七 (2018) | Same cost as #29's `((t+128)·257)>>16`; either | 0 |

### Matting and background removal

| # | Finding | Source | Effect | Days |
|---|---|---|---|---|
| C9 | **MODNet** (Apache-2.0, 512², ~7 M params, 50–150 ms on CPU) loads in OpenCV DNN from the official fixed-shape export (the PaddleSeg export fails with a Concat shape assert, opencv #23288). **PP-MattingV2** (Apache-2.0, 8.95 M params, 18 % lower error than MODNet) needs a fixed-shape export plus onnxsim. Both are portrait-only and sharper on hair than IS-Net. | Ke et al., CityU + SenseTime, AAAI 2022; Baidu PaddleSeg 2.7 | Add as "Portrait" models next to IS-Net; skip the min-max stretch at `subject.cpp:82-85` for alpha-output models; aspect-preserving resize to a multiple of 32 | 2 |
| C10 | **u2net_human_seg** (Apache-2.0, 320², rembg release) runs today with no code change. | Qin Xuebin | One `ModelStore` entry | 0.1 |
| C11 | **PP-HumanSeg** (Apache-2.0, 192², 5.6 ms) ships in OpenCV's own model zoo. | Baidu | Instant coarse mask for a live preview, refined afterwards | ½ |
| C12 | **BiRefNet** is confirmed unusable in OpenCV DNN: `deform_conv2d` has no importer path and Swin's `roll` produces silent garbage. RVM is GPL-3 and also fails; AEMatter has non-commercial weights; MAT is CC BY-NC. | — | Stays excluded; would need onnxruntime | — |
| C13 | **Multichannel guided filter with guide (R,G,B,M)**: `a_k = (Σ_k + εU)⁻¹(mean(I·p) − μ_k·p̄_k)`, `q = aᵀI + b`; the mask becomes its own regressor and colour edges invisible in luma drive the matte. Ten box means and a 4×4 solve per pixel instead of the luma-only filter at `matte.cpp:95-109`. | He's multichannel GIF; the closed-form twin of Fast Deep Matting's feathering block (CASIA, ACM MM 2017) | Visibly better hair on same-luma backgrounds | ½–1 |
| C14 | **Halo-free guided filters**: weighted GIF `a_k = cov/(var + λ/Γ_k)` and gradient-domain GIF with an edge-aware `γ_k` so `a → 1` on edges. One extra box mean at `matte.cpp:104-105`. | Li Zhengguo, TIP 2015; Kou, Wen, Li, TIP 2015 | Removes the grey band the plain filter leaks over flat backgrounds | ½ each |
| C15 | **Shift Edge hardens the matte**: `matte.cpp:118-130` blurs then thresholds at slope 1/0.001. Grey-level dilation or erosion by |s| (van Herk running max/min) moves every iso-contour and keeps the ramp. | laviewpbt SSE max filter | A quality bug fix | ½ |
| C16 | **Trimap band + local matting**: unknown band `|d| ≤ w` from the mask's distance transform, width from the ramp's gradient, solved with Shared Sampling (1–2 s at 12 MP, parallel) or the large-kernel matting Laplacian. | Alibaba SHM (ACM MM 2018); SCUT 2023; He/Sun/Tang CVPR 2010 | The largest hair-quality jump; also the input ViTMatte (MIT) would need | 3–4 |
| C17 | Fast guided filter done right (subsample the guide and mask, upsample the coefficients, evaluate against the full-resolution guide). The current `limit` path upsamples the result and throws the detail away. | He & Sun 2015; laviewpbt 2017 | Confirms #22 | 1 |

### Inpainting, healing and content-aware fill

| # | Finding | Source | Effect | Days |
|---|---|---|---|---|
| C18 | **Dominant patch offsets**: the 2-D histogram of nearest-neighbour offsets between 8×8 patches is sparse; keep the K≈60 peaks and label hole pixels with one offset each by α-expansion graph cut (`E_d = 0` if the shifted pixel is known, else ∞; `E_s` = colour difference at the seam). Structures like bricks, railings and text rows continue across the hole. | He Kaiming & Sun Jian, MSRA, ECCV 2012 | Replaces the 120-candidate polar search in `HealPixels.c:162-188` with K data-driven offsets, and seeds #38's nearest-neighbour field; the graph cut gives coherent seams where voting blurs | 2 + 2 |
| C19 | **PatchMatch inpainting, engineering corrections**: drop the source→target field (2× faster); 5×5 patches; pyramid sizes `(W+1)/2`; random search around the original centre (sharper); gradient channels `g = (a/2 − b/2) + 128` in the SSD; the distance-to-similarity vote table in most public ports collapses to zero past ~40 % of range and produces garbage votes; run 2–3 seeds and keep the best. | laviewpbt, 2024 (20 days of notes) | Design constraints for #38 | 0 |
| C20 | **Patch structure sparsity**: priority `P = C·ρ` where `ρ` measures how concentrated a patch's similarities are (edges and corners first); fill by a sparse convex combination of the top candidates rather than one copy. | Xu Zongben & Sun Jian, XJTU, TIP 2010 | A priority heap for `ContentFill.c:50-55` and a weighted blend at `:81`, removing the speckle | 1½ |
| C21 | **Exact Poisson solve by sine transforms**: row and column DSTs with eigenvalues `2cos(πi/(W+1)) + 2cos(πj/(H+1)) − 4`, exact for any mask, O(N log N), about 100 lines. | laviewpbt's reading of OpenCV `seamlessClone`, 2024 | Replaces the SOR sweeps at `HealPixels.c:54-113`; identical solution, no tuning | 1 |
| C22 | **Mean-value coordinates** for spot heals: interpolate the ring difference with `w_i = (tan(α_{i−1}/2) + tan(α_i/2)) / ‖p_i − x‖`; no solve at all for holes up to ~10⁴ px. | Farbman 2009; fafa1899/MVCImageBlend; NUAA 2019 | Microseconds per heal, near-harmonic result | ½ |
| C23 | Criminisi priority fixes: regularised confidence `Rc = (1−ω)C + ω`, adaptive block size from local variance; gains of 0.4–3 dB on small test images. | NTU 2005; SCU 2018; Liaoning TU 2023 | Only the confidence and radius rules worth carrying into #38 | ½ |
| C24 | **Deep inpainting through OpenCV DNN**: AOT-GAN (SYSU + MSRA, Apache-2.0, 15 M params, plain and dilated convolutions, any resolution, ~1–2 s per 512² crop on CPU) is the one model that is both permissively licensed and importer-clean. CoordFill (BSD-3, 90 ms at 2048² but weaker texture) is worth a test. ZITS needs OpenCV 5's DFT; MAT is non-commercial; MI-GAN's weights descend from a non-commercial model; LaMa (Samsung) is the OpenCV 5 fallback. | — | An optional "Content-Aware Fill (AI)" behind the same model-download switch as Remove Background | 2–3 |

### Suggested adjustments to the order

- Before #17 (integer blends), spend half a day on C1's Photoshop probe so the formulas are right the first time.
- Bundle C5 with the `MipCache` fix, C8 with #29, C3 with #30, C4 with #33.
- Do C6's Smooth and Feather with #14; C7 with the wand fix.
- Matting: C15, C13, C14 first (2 days, no new models), then C9/C10 model entries, then C16.
- Healing: C22 first (half a day), then #38 shaped by C19 with C18's offsets seeding it, then C21 for large fills.

### Blurs and the guided filter

| # | Finding | Source | Effect | Days |
|---|---|---|---|---|
| C25 | **Box blur without transposes ("lazy" column sums)**: keep `int32 colSum[x]` over the vertical window, add the entering row and subtract the leaving one per output row, then a running horizontal sum over `colSum` gives the whole 2-D box in one row-major sweep; no transpose, no float buffers. 3000×2000 grey: 39 ms C, ~6 ms SSE, single thread. An original Chinese formulation (2009). | laviewpbt 2009; ImageShop SSE 系列十三 (2018) | Replaces `boxBlurRows` plus both transposes and the 16-byte-per-pixel float buffers (`filters.cpp:81-109, 177-184`): ~5–7× on the large-sigma path and 4× less memory, beating #5 and #21; also the box engine for #15's motion blur | ½–1 |
| C26 | **Recursive Gaussian with a row-major vertical pass**: the IIR's vertical pass runs y-outer, x-inner using the three rows already written, so it is sequential in memory and SIMD across x with no transpose; float RGBA interleaved so one vector is one pixel, our exact layout. 3000×2000 RGB: 370 ms C, 75 ms SSE, single thread. Confirms that float Young–van Vliet breaks above radius ≈ 75 while Deriche does not. | ImageShop SSE 系列二 parts 1–2 (2017) | The engineering layout for #34 (Deriche as the single path) | 1–2 |
| C27 | **Integer extended-binomial Gaussian**: the same Kovesi box widths we use plus `{1,−4,6,−4,1}` recursions in int32, 3000×2000 grey in 11–13 ms; "no visible difference for 8-bit at 4th order". | ImageShop 2022 | An alternative integer layout for #21 | 1–2 |
| C28 | **Fast guided filter at s = 5** with one integral image reused for the six means and the coefficients (not the result) upsampled in split passes: 3000×2000 RGB 55 ms single thread. | ImageShop SSE 系列三 (2017) | Confirms #22 and that our `limit` path is wrong | 1 |
| C29 | **Bicubic for Lens Correction** with fixed-point 4×4 weight tables and separable SIMD accumulation, ~1.5–2× the cost of fixed-point bilinear. | ImageShop 2020 | Fixes the corner aliasing at strong distortion: 325 ms → ~30–40 ms with rows in parallel | 1 |
| C30 | Side-window filtering (eight half windows, keep the one closest to the input) removes halos at hard edges at 2–3× the cost of one box; thin strands narrower than the radius can pick the wrong side. | Yin, Gong, Qiu, Shenzhen U., CVPR 2019 oral | Optional refinement after C13/C14 | 1½–2 |
| — | Motion blur and Add Noise: nothing in the Chinese sources beats #15 and #1; the naive O(distance) line samplers there are slower than ours. | — | — | — |

## G'MIC: the open-source filter library used as a plugin

G'MIC (GREYC's Magic for Image Computing, from the GREYC lab at the University
of Caen and CNRS) is the filter framework that GIMP, Krita, Paint.NET, digiKam
and Photoshop users install as a plugin: more than 500 filters (sharpening,
denoising, artistic and painterly looks, film emulation, deformations, patch-based
inpainting, frames, and so on) driven by a scripting language. It is installed on
this machine (version 4.0.5 with `gmic`, `gmic_qt`, `libgmic` and its headers)
and packaged by Arch and Ubuntu.

Licensing: `libgmic` is dual-licensed CeCILL-C (LGPL-like, linking from an MIT
program is fine) or CeCILL v2.1; the `gmic_qt` plugin front end is CeCILL
(GPL-like), so it can only be run as a separate program, never embedded.

Plan, as three deliverables:

1. **Filter > G'MIC…** bridge through the `gmic` executable (an optional runtime
   dependency found on PATH): the active layer's pixels inside the selection go
   out as PNG, the command runs, the result comes back through the same commit
   path the built-in filters use, with a downscaled live preview, a searchable
   list of curated filters with parameter widgets, and a free-form command box.
   The automation socket gets `pixels.gmic {command}`. About 2 days.
2. **The whole catalogue**: parse the `#@gui` descriptors of the G'MIC standard
   library (name, folder, and typed parameters: float, int, bool, choice, colour,
   text, separator) to build the parameter panel for every filter automatically,
   which is exactly what `gmic_qt` does, with the same "update filters" download
   of the community definitions. About 3–4 days.
3. **In-process `libgmic`** behind a CMake option, for previews without the PNG
   round trip, keeping the executable path as the fallback. About 1 day.

This gives the editor the plugin ecosystem people expect without taking on any
of its code.
