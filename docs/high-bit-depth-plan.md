# High bit depth and colour management: design plan

Status: design, for review before any code. Surveyed against `src/core` (~31.5k lines), `src/app`, `tests/`.

## 1. Where we are

- `compositor::Image` (`image.h`) is premultiplied RGBA8 (`PixelBytes`, explicit stride; `data()`/`row()`/`pixel()` hand out
  `uint8_t*`). `GrayImage` is 8-bit, stride = width. `ImagePtr`/`GrayPtr` are `shared_ptr<const>`, shared copy-on-write
  between the document, `MipCache` and history snapshots.
- Reach: 44 core and 21 app .cpp files take `Image&`/`ImagePtr`/`data()`; 41 core files touch pixel pointers. The pixel
  heavy files: `render.cpp`, `blend.cpp`, `smartfilter_render.cpp`, `layerstyle_render.cpp`, `adjustments.cpp`,
  `cameraraw.cpp`, `brush.cpp`, `affinity.cpp`, `psd.cpp`, `psd_writer.cpp`.
- The blend API (`blend.h`) is all bytes; its kernels are calibrated to Photoshop and gated by `blend_tests`,
  `golden_tests`, `kernel_tests`.
- PSD import already decodes 16/32-bit planes, then folds them to 8-bit with a note. Carried blocks are depth-aware;
  mask-section carry is limited to `depth == 8`.
- The project manifest (version 7) already has a `colorSpace` key. lcms2 is used only in `colour.cpp`,
  `colorlookup.cpp`, `affinity.cpp`. The canvas renders to `Image` → `toQImage` (RGBA8 premultiplied), no display
  transform.
- Budgets: 30,000 px a side, 100 MP per canvas/layer/mask, 1 GP of layers. At 16-bit 1 GP is 8 GB, at float 16 GB, so
  budgets must become byte budgets.

## 2. Principles

1. 8-bit is the fast path, not a special case of float: it keeps its byte kernels unchanged; dispatch is once per
   document or render call, never per pixel.
2. One depth and one mode per document (every layer, mask, channel, cache) — as Photoshop does; no mixed depths.
3. Add types, don't mutate `Image`: it stays exactly as it is (the U8 case); new code is templated over the sample type;
   files migrate one by one.
4. Colour conversion only at the edges (import, export, assign/convert, display, proof); the engine works in one space.

## 3. Pixel storage

- `enum class SampleType { U8, U16, F32 }`, `SampleTraits<S>`. U16 uses Photoshop's internal 0..32768 range (exact
  `x*a >> 15` premultiply and multiply, and the best chance of calibrating 16-bit blends later); PSD's 0..65535 is
  mapped at import/export.
- `ImageT<S>` (same API as `Image`, `S*` rows, a channel count: 4 for RGB/Lab, 5 for CMYK), `GrayImageT<S>` for masks
  and channels. `AnyImage = variant<ImagePtr, Image16Ptr, ImageFPtr>` with a `visit` helper; `Layer`, masks,
  selections and channels hold it. Existing 8-bit code calls `img.u8()`: every file keeps working on 8-bit only, and a
  feature not yet ported is refused for deeper documents with a notice.
- Premultiplied everywhere. F32 is linear-light by default, colour may exceed 1.0, alpha is clamped; non-Normal
  separable modes unpremultiply per span in float. CMYK/Lab are premultiplied on PSD's inverted ink values; Lab's a/b are
  stored offset (hidden behind accessors).
- Masks and selections follow the document depth (no banding in 16-bit feathers; PSD stores masks at document depth).
- Tiles: not in this project. All new kernels take spans `(rows, x0, x1)` so tiling stays possible later.

## 4. Rendering dispatch

- Split `render.cpp` into a depth-agnostic planner (tree walk, clipping, groups, knockouts, style scheduling, dirty rects)
  and a templated executor `RenderExec<S>`; `renderDocument` switches on the document's sample type once.
- `blend.cpp` becomes `blend_u8.cpp` byte for byte (LUTs included); `blend_u16.cpp` (15-bit integer separable modes,
  float for the non-separable and dodge/burn families) and `blend_f32.cpp` (W3C/PDF formulas, vectorised spans) are new.
- 8-bit guarantee: `RenderExec<U8>` is moved code, not rewritten — a no-functional-change step gated by render hashes and a
  ±2% bench.
- The canvas always gets an 8-bit display image: deeper executors end with `toDisplay<S>` fused with the display colour
  transform, so the canvas, thumbnails and `toQImage` keep their paths. `MipCache` keeps the document depth.

## 5. Colour management

- `ColorProfile { icc bytes, description, model }` on the `Document` (ICC kept verbatim for byte-exact resource 1039).
  A new `colormgmt.h/.cpp` owns lcms2: a thread-safe LRU transform cache and parallel `convertImage`.
- Working spaces: sRGB for new 8/16-bit, linear sRGB (Rec.709) for 32-bit; Adobe RGB, Display P3, ProPhoto selectable;
  CMYK defaults to a generic/US Web Coated profile, Lab D50.
- Conversion points: import (keep embedded profile by default, untagged = sRGB), paste/place (convert to the target),
  colour picker (document space), canvas (document → monitor, fused with depth reduction), soft proof (one lcms proofing
  transform, optional gamut warning), export (embed, or convert to sRGB for web), Assign (bytes only, one undo step),
  Convert (every raster, fill and style colour; one undo step).
- When the document profile equals the display profile, or no monitor profile is known (the default), the display
  transform is skipped: the 8-bit canvas stays exactly as today.
- Monitor profile: X11 `_ICC_PROFILE`; on Wayland a preference with a file chooser.
- Blending happens on the document's encoded values (gamma for 8/16, linear for 32), as Photoshop does.

## 6. CMYK and Lab modes

- `ColorMode { RGB, CMYK, Lab }` on the `Document`; CMYK 5 channels, Lab 4, premultiplied.
- CMYK: separable modes run the RGB kernels on inverted ink values (channel count 5), which is Photoshop's own model;
  non-separable modes go through Lab per span (documented as approximate). Lab: Normal, Dissolve, multiply/screen-like,
  Darken, Lighten.
- Filters and adjustments declare supported modes; the rest are greyed out (as in Photoshop). First set: Levels, Curves,
  Hue/Saturation, Invert, blurs, sharpen, noise, per-channel filters. Display: document → monitor through lcms.

## 7. Channels

- `Channel { id, name, image (AnyGray), kind Alpha|Spot, overlay colour/opacity, selectedAreas }` in
  `Document::channels`: saved selections and alpha channels (spot channels carried only, in v1).
- Colour channels (R, G, B / C, M, Y, K / L, a, b) are views, not storage. Single-channel editing is a session bitset
  honoured by brush, fill and filter write-back helpers; the all-channels case keeps the old path through a template flag.
- Display of one channel as grey, several as overlays, in the display step. Save Selection, Load Selection
  (add/subtract/intersect), New/Duplicate/Delete, Edit as Quick Mask. Apply Image and Calculations later.
- `ChannelsPanel` following the Layers panel; automation `channels.*` with the usual handler, description, MCP tool, docs and smoke coverage (CONTRIBUTING.md).

## 8. Formats, memory, undo

- PSD import keeps typed planes (16-bit, 32-bit), imports CMYK (mode 4) and Lab (mode 9) natively instead of converting,
  extra alpha channels (with resources 1006/1045 names) into `channels`, resource 1039 into the profile. Export writes the
  document's depth and mode; masks and channels at document depth. For exact round trips, unmodified 16-bit planes are
  re-emitted from a `CarriedPlane` of the original bytes (dropped on the first edit), so byte-exactness never depends on
  the 0..65535 ↔ 0..32768 mapping.
- Project format version 8: `sampleType`, `colorMode`, `profile` (ICC as its own package entry), `channels`; U16 layers as
  16-bit PNG, F32 as compressed raw floats. 8-bit sRGB documents keep writing version 7.
- RAW: LibRaw 16-bit output into U16 (or linear ProPhoto into F32) for deep documents; 8-bit stays the default. TIFF/PNG
  16-bit and ICC both ways; JPEG 8-bit with ICC, deeper documents dithered down on export.
- Budgets become bytes with the same meaning (100 MP at 8-bit = 400 MB per canvas; 1 GP = 4 GB of layers): a 16-bit
  document holds half the pixels, float a quarter; dialogs and automation say so.
- Undo: pixel edits store only the edited region's before-crop (`RegionPatch`) when it is under half the layer — the one
  history change; it also saves memory at 8-bit. History cap stays in bytes.

## 9. Phases (each shippable; gated by ctest, rpc smoke and Docker CI)

- **P0** Measure and freeze: render-hash test over all golden fixtures and blend modes (plus brush and filter outputs);
  bench targets for render, brush, blur, adjustments. Baselines committed.
- **P1** Typed images and the dispatch skeleton, no behaviour change: `SampleType`, `ImageT`, `AnyImage`; layers, masks
  and selection on `AnyImage` with `.u8()` everywhere; planner/executor split; `blend_u8.cpp`. Gate: identical hashes,
  benches within ±2%, PSD corpus byte-identical.
- **P2** 16-bit RGB core: `RenderExec<U16>`, `blend_u16`, Normal + separable modes, opacity, masks, clipping, groups,
  knockout, `MipCache`, display; PSD/PNG/TIFF 16-bit, Image ▸ Mode ▸ 8/16 Bits, project v8; a `supports(SampleType)`
  registry greys out what is not ported. Gate: 16-bit render of 8-bit-sourced documents within ±1 level; 16-bit PSD round
  trip byte-exact; 8-bit hashes unchanged.
- **P3** Painting and editing at 16-bit: brushes (tip and MyPaint surface writes), adjustments (65536-entry LUTs), blur,
  transforms, fill, gradient, selections. Each port unlocks its menu entry with a U16-vs-U8 test.
- **P4** Colour management: profiles, `colormgmt`, policy, Assign/Convert, display transform, soft proof, export
  embedding. Gate: conversions against lcms reference values; identity path bit-identical for untagged sRGB 8-bit.
- **P5** 32-bit float: `RenderExec<F32>`, linear working space, `blend_f32`, float storage, 32-bit PSD, RAW to float,
  an exposure/gamma view control.
- **P6** Channels panel and alpha channels (depth-independent: can move up to P2).
- **P7** CMYK and Lab modes.
- **P8** Long tail at depth: layer styles, smart filters, Camera Raw, content-aware fill and heal, liquify, Affinity.

## 10. Risks

Silent 8-bit regressions in P1 (hash gates, no-change refactor step); code size and compile time from three
instantiations (explicit instantiation files, small span kernels); undocumented 16-bit Photoshop rounding (aim within one
15-bit level, document it); feature-gap confusion while ports land (central `supports()` registry, clear tooltips and
automation errors); memory (a 100 MP float layer is 1.6 GB: byte budgets, region undo); Wayland monitor profiles
(preference fallback); CMYK non-separable blend semantics.

## 11. What to cut, in order

Lab mode; the 32-bit phase (keep 16-bit + colour management); the gamut warning; spot channels, Apply Image and
Calculations; CMYK non-separable modes (fall back to Normal with a notice); tiling. The minimum valuable product is
P0–P4 plus P6: 16-bit RGB, a colour-managed display with Assign/Convert and soft proof, lossless 16-bit PSD round trips,
and a Channels panel, with 8-bit documents untouched throughout.

## Status

**P1 landed (2026-09-27), no behaviour change.**

- `sampletype.h`: `SampleType`, `SampleTraits` (U16 one = 32768). `imaget.h`: `ImageT<S>` / `GrayImageT<S>` for U16 and
  F32 (a channel count, byte stride), `ImageOf<S>` / `GrayOf<S>` mapping U8 to the untouched `Image` / `GrayImage`,
  and `AnyImage` / `AnyGray` (a variant of the three shared pointers, `u8()`, `u16()`, `f32()`, depth-agnostic
  `width()`, `height()`, `identity()`, `visit`). `Image` was kept as it is rather than made an alias.
- `Asset::image`, `MaskAsset::image`, `Layer::shapeImage` / `textImage` / `smartImage` and `Selection::coverage` hold
  `AnyImage` / `AnyGray`; every reader goes through `.u8()` (null for a deeper buffer). Asset and mask thumbnails,
  `LayerOverride`, `RenderCache` and `DrawParams` stay 8-bit. `Document::sampleType` exists (always U8); the project
  and PSD formats are unchanged.
- `render.cpp` keeps the primitives (`drawLayer`, mask sampling, resampling, `resizeDocument`) and the entry point;
  `render_plan.h/.cpp` is the depth-agnostic `RenderPlan`; `render_exec_u8.cpp` is `RenderExec<SampleType::U8>`, the
  former `Renderer` body moved. `render()` switches on `document.sampleType` once. `blend.cpp` is now `blend_u8.cpp`.
- `supports.h`: `supports(feature, SampleType)` over a table in `supports.cpp`; an unlisted feature is 8-bit only.
- Gates: render hashes identical, full ctest, PSD corpus identical (117 files, 3675 carried blocks), GCC and Clang
  `-Werror`, rpc smoke; `bench_core 9` A/B against 1.6.1 within ±2% (medians of alternating rounds, render 201.7 →
  201.2 ms, brush 21.9 → 22.1 ms, blur r20 82.2 → 82.8 ms).

**P2 landed (2026-09-27): the 16-bit RGB core.** User-facing summary: [bit-depth.md](bit-depth.md).

- `RenderExec<U16>` (`render_exec_u16.cpp`) is a separate specialisation, so the U8 executor and its codegen are
  untouched: pixel layers in every mode, opacity, pixel and vector masks, clipping stacks, folders (pass-through,
  faded, isolated), artboards, strokes and Dissolve at 0..32768 with coverage at the same depth. Layer styles and
  adjustment layers are drawn by their 8-bit code and applied as a difference (`via8`), so pixels they leave alone
  keep 16 bits; they get native paths with P3 and P8. `drawLayer`, mask sampling, resampling, `LayerOverride`
  (`image16`, `maskImage16`), `RenderCache` (`backdrop16`, `above16`), `DrawParams16` and `MipCache` (16-bit levels)
  have their 16-bit forms. `render()` hands the canvas a 16-bit document reduced with rounding (`toDisplay<U16>`,
  no colour transform yet); `render16()` is the document at its depth.
- `blend_u16.cpp`: Normal and the product-form modes (Multiply, Screen, Overlay, Hard Light, Darken, Lighten,
  Difference, Exclusion, Linear Burn and Dodge, Subtract, Linear Light, Pin Light) are exact 15-bit integer maths on
  premultiplied samples; Color Dodge and Burn, Vivid Light, Soft Light, Divide, Hard Mix and the non-separable modes
  are float per pixel. Calibration against the 8-bit kernels: opaque pixels at full coverage are within one 8-bit
  level in every mode (Vivid Light two: Photoshop's 8-bit kernel rounds its divisor to a byte), and a soft, masked
  layer at an opacity keeps 99% of samples within a level; stacked scenes differ more where the 8-bit engine's own
  rounding (coverage in 1/256 steps, bytes between layers, 8-bit unpremultiplied clipping bases) is magnified by a
  dividing mode. Hard Mix, Darker Color and Lighter Color decide on the pixels rounded to 8 bits, so an 8-bit
  document converted to 16 bits chooses as its 8-bit render does.
- Formats: 16-bit RGB and grayscale PSDs open as 16-bit documents and export as 16-bit PSDs (layers in `Lr16`, zip
  with prediction); an unedited layer's channels come back byte for byte from `PsdLayerCarry::planes`, bound to the
  layer's pixels by their fingerprint (PSD to PSD; a PSB, or an edited layer, is written from 0..32768). The stored
  mask channels carry at their depth. 16-bit PNG read and write; TIFF at 16 bits through Qt's plugin (probed at run
  time); JPEG, WebP, TGA, ICO and GIF dither down with a notice. Project format 8 gains `sampleType` (`"u16"`, 16-bit
  PNGs); 8-bit documents write what they wrote before.
- Budgets are bytes: `Document::imagePixelBudget(type)`, `projectPixelBudgetAt(type)`, checked by the mode
  conversion, the PSD reader and the project loader and writer. `DocumentHistory::retainedBytes` counts buffers at
  their depth.
- The `supports()` table lists what P2 ports (rendering, the layer structure, masks, whole-layer transforms, canvas
  size and flip, import, save and export, the tools that do not touch pixels). The app greys every other menu entry
  and tool in a 16-bit document ("Not available in 16-bit yet"), automation refuses the other methods ("<method> is
  not available for 16-bit documents yet"), and the session's 8-bit-only entry points refuse with the same wording.
  Every edit brings buffers held at another depth to the document's (`conformToSampleType`), so a document keeps one
  depth even when an 8-bit path (a shape or text raster, an imported file) adds pixels.
- Gates: 8-bit render hashes identical (133 scenes) plus 58 16-bit scenes; full ctest; GCC and Clang `-Werror`; rpc
  smoke with a 16-bit section; PSD corpus plus K.psd identical (118 files, 3,975 carried blocks); a constructed
  16-bit PSD round trips byte for byte (`depth_format_tests`). No Photoshop-saved 16-bit PSD was available; the
  fixture is built in the test the way Photoshop lays one out.

**P3a landed (2026-09-27): editing at 16 bits, painting aside.** User-facing summary: [bit-depth.md](bit-depth.md).

- Adjustments: every `AdjustmentKind` on `Image16` (`adjustments_u16.cpp`): table kinds through 32769-entry tables
  built from their functions, colour kinds with their per-pixel maths on the exact straight colour, Hue/Saturation
  through a float 33-point cube; Posterize and Threshold decide as at 8 bits on 8-bit-sourced pixels. Adjustment
  layers render natively in `RenderExec<U16>` (no more `via8` for them; layer styles still use it).
- Filters: Gaussian and Motion Blur (and 16-bit masks), Add Noise, Lens Correction; grow/trim/selection helpers.
- Selections at document depth: combine, invert, expand/contract/border/smooth/feather on `Gray16`, Load as
  Selection, Quick Mask, offset; the wand and Quick Select read the 8-bit display render and widen their result.
- Pixel edits: fill, clear, clipboard, Free Transform of selected pixels, content-aware fill/move/scale (decisions
  on the 8-bit rounding, 16-bit pixels copied), Image Size, Crop, Trim, Distort, Warp and Warp Cage.
- Gates: `depth_edit_tests` (U16 vs U8 calibration per port), 44 new U16 render-hash scenes (8-bit hashes
  unchanged; `u16/adjust_layer/levels` changed as it is no longer drawn at 8 bits), full ctest, GCC and Clang
  `-Werror`, rpc smoke's 16-bit section, PSD corpus 118 files / 3,975 blocks at 8 and 16 bits. The 8-bit object
  code of blur, resample, render_exec_u8, blend_u8, brush, kernels, adjustments and filters matches 68435cb
  function for function (16-bit code lives in separate `_u16` units).
- Still gated: painting and brush tools, Patch, Paint Bucket, moving selected pixels with the Move tool, Camera
  Raw, G'MIC, Remove Background, merges, Apply Mask, layer styles, smart objects, vectors, text, timeline.
- For P3b: `BrushStroke` (fill-through at 8 bits, `liftSelection`, `commit`) needs a 16-bit raster, then the
  Move-tool pixel move, Paint Bucket, Patch, gradients and the healing/clone/smudge engines follow it.

**P3b landed (2026-09-27): painting and retouching at 16 bits.** User-facing summary: [bit-depth.md](bit-depth.md);
the engines: [brush-engine.md](brush-engine.md), "16 bits".

- `BrushStroke` takes the document's depth (`SampleType::U16` and a 16-bit selection): its working pixels, mask,
  coverage, selection, dab table and stamps are 0..32768 (`brush_u16.cpp`), and every step (dabs, the screen and max
  build-up, the recompose, clone and processed sources, lifting and moving selected pixels, gradients, healing, the
  commit) follows the 8-bit code with 15-bit samples. The 8-bit paths are unchanged; each entry point branches once.
- `TipStroke::dab` writes the grid's depth, with a density-by-spacing table per 15-bit level. MyPaint's tiles take a
  16-bit layer's samples as they are (libmypaint's fixed point is 0..32768) and write them back; `MipCache::refresh`
  has 16-bit forms. 16-bit `spotHeal`, `healFrom` (patch search and synthesis on the 8-bit rounding, the membrane and
  copied pixels at 16 bits), `toneImage`, `sharpenImage`, `fillGradient`, `WarpStroke` and `TiledSource16`.
- In the editor: the brush in every engine and the eraser on pixels, masks and the Quick Mask; Clone Stamp, Healing
  Brush, Spot Healing, Patch; Blur, Sharpen, Smudge, Liquify; Dodge, Burn, Sponge; gradients; the Paint Bucket
  (chosen on the canvas as shown, filled at 16 bits); moving selected pixels; Merge Down and Apply Layer Mask. Each has
  its own `supports()` line (`tool.brush`, `tool.spotHealing`, `tool.cloneStamp`, `tool.smudge`, `tool.dodge`,
  `tool.gradient`, `tool.paintBucket`, `edit.movePixels`, `layers.merge`, `layers.applyMask`); `supports()` answers
  8-bit questions without looking at the table.
- Calibration (`depth_paint_tests`, 8-bit-sourced layers, the 16-bit result reduced to 8 bits against the 8-bit
  result): hard tips, masks painted with hard tips, clones, Spot Healing in its three modes, Patch, Liquify, the six
  processed tools, gradients (linear, radial, multi-stop, on masks, through selections) and moved pixels are within a
  level. Soft tips reach 4 levels in at most 0.72% of samples, tip brushes 2 levels (0.06%), a replacing clone and the
  Healing Brush through a soft tip 3 and 2 levels (under 0.01%), and Smudge 2 levels (0.04%): the 8-bit coverage rounds
  to 1/255 at every step of a screen build-up (and Smudge rounds the pixels it picks up again), and against the exact
  coverage in double precision the 8-bit soft stroke is up to 3 levels off in 0.9% of its pixels while the 16-bit one
  is within a level. The healers treat coverage under half an 8-bit level as outside the spot (the rim it rounds away
  at 8 bits would otherwise move the membrane's ring: 7 levels before, 2 after).
- Brush parity: a `u16/` section of `brush_parity_baseline.txt` holds all 195 scenes painted on a 16-bit layer, each
  with its distance from the 8-bit render: MyPaint 0 levels (identical after reduction), the hard round tip, Leaf and
  Soft Ink 1, soft tips and tip brushes up to 4, a light flow with density by spacing at 2% spacing 5 (0.88%). The
  8-bit section is unchanged. Density by spacing keeps the interior alpha within 0.2% across 2–50% spacing at 16 bits
  (1.2% at 8).
- Still gated: the shape and text tools, Fill Path and Stroke Path, baking a clipping mask when its base is deleted,
  editing layer styles (their rendering stays `via8`), smart objects, vectors, Camera Raw, G'MIC, Remove Background,
  artboard and slice export, the timeline.
- For P5 (32-bit float): `BrushStroke` holds a second set of buffers behind `depth_`; a third depth argues for a
  templated raster (`StrokeRaster<S>`) rather than a third copy. `TiledSourceOf<Img>` and the `StampOf<T>` stamps are
  already templates. Painting in linear light changes how soft edges and gradients look (Photoshop's 32-bit mode does
  so too), so a U8 calibration will not hold there; gate it against a float reference instead. libmypaint's tiles are
  15-bit fixed point: a float layer would be clamped to 0..1 and quantised at the tile edge, so MyPaint needs either a
  float surface or a documented 15-bit round trip (and a choice of which transfer curve it paints in). Healing, the
  bucket and the wand decide on the 8-bit display rounding, which for float needs the display transform (exposure) to
  be fixed first. The toning curves assume display-referred 0..1 values.

## Review notes

- Mac project compatibility: since 2026-09-26 NekoPhoto no longer keeps Mac Compositor project-format parity, so
  "8-bit sRGB keeps writing version 7" is a convenience, not a requirement; a single version-8 writer is acceptable.
- The 0..32768 internal range is not lossless for arbitrary 16-bit files (65,536 input values onto 32,769 levels). The
  plan's `CarriedPlane` is therefore required, not optional, for byte-exact PSD round trips of unedited layers, and PNG
  and TIFF 16-bit exports of edited images lose the lowest bit. Alternative to decide in P2: full 0..65535 storage (exact
  I/O, slightly costlier premultiply) versus Photoshop's range (calibration parity).

## Decisions (2026-09-26)

Follow Photoshop's choices throughout:

- 16-bit uses Photoshop's 0..32768 range. `CarriedPlane` is mandatory: unedited 16-bit planes re-emit their original
  bytes, so PSD round trips stay byte-exact.
- CMYK and Lab modes and 32-bit float are all in scope; nothing from section 11 is cut up front.
- Channels come early (P6 can move up to follow P2, as noted there).
- Budgets become byte budgets (section 8).
- macOS is no longer a target, so the version-7 writer for 8-bit sRGB is optional.
