# High bit depth and colour management: design plan

Status: P1–P4 landed in 1.7 (16-bit RGB editing, painting and colour management); see "Status" below for each phase and what is still gated at 16 bits. P6 (channels) has landed too; P5, P7 and P8 are planned. The design sections below are kept as written.

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

**P4 landed (2026-09-27): colour management.** User-facing summary: [color-management.md](color-management.md).

- `ColorProfile` (`colorprofile.h`) on the `Document`: ICC bytes verbatim, description, model; empty is untagged,
  treated as sRGB. PSD resource 1039 reads into it and is written back byte for byte while unchanged (64 of 64 tagged RGB
  PSDs in the corpus); projects store it as `profile.icc` (format 8, `"colorSpace": "icc"`); PNG iCCP both ways.
- `colormgmt.h/.cpp` owns Little CMS for documents: built-in sRGB, Adobe RGB (1998), Display P3 and ProPhoto profiles
  generated from their published primaries (fixed header date, so the bytes are stable), a mutex-guarded LRU cache of 24
  transforms, `convertImage` at 8 and 16 bits on premultiplied pixels (made straight around the transform; 16-bit
  through the float pipeline, since Little CMS's 16-bit one precalculates a grid that was off by up to 2.4% near the
  gamut's edge), soft-proofing transforms with the gamut warning, `convertDocumentProfile`, and `TransferCurve` /
  `documentTransfer` (a profile's tone curve both ways) for the masking lane's linear-light code.
- The canvas transform rides in `RenderOptions::display`: applied to the 8-bit frame, fused with `toDisplay<U16>` for
  16-bit documents. Null (no monitor profile, the default, or the monitor's profile is the document's) leaves the
  render untouched. The monitor profile: X11 `_ICC_PROFILE` through libxcb loaded at run time, Windows
  `GetICMProfileW`, or a file in Preferences (Wayland).
- App: Edit > Color Settings (working space, policy, the two prompts off by default), Assign Profile and Convert to
  Profile (one undo step; perceptual or relative colorimetric, black point compensation), View > Proof Setup, Proof
  Colors, Gamut Warning; the open policy for PSD, PNG, JPEG and TIFF; imports converted to the document's profile;
  exports embed the profile, and the web formats offer Convert to sRGB (on for GIF). Automation `document.profile`
  and `color.settings`.
- Not colour-managed yet: pasting pixels from other applications and Place Embedded (both taken as the document's
  values), the Layers panel thumbnails and navigator-style previews (document values), the QColorDialog preview, TGA
  and ICO (no profile to carry), smart object sources (their placed pixels are converted, the source is not), layer
  patterns and blocks carried from a PSD that NekoPhoto does not model.
- Gates: 8-bit render hashes unchanged; `colormgmt_tests` against Little CMS and the published matrices (round trips
  through Adobe RGB, Display P3 and ProPhoto within Little CMS's own 8-bit loss and 16 of 32768 at 16 bits); PSD
  corpus plus K.psd identical (118 files, 3,975 carried blocks); GCC and Clang `-Werror`; rpc smoke with a colour
  section; `bench_core` 8-bit lines within ±2% of main.
- For P5: a 32-bit document's working space is linear; `TransferCurve` gives the curve to linearise with, and
  `PixelFormat` needs an `RGBAFloat` layout (Little CMS's float pipeline is already what the 16-bit path uses).
  CMYK and Lab (P7) need `ColorModel`-aware transforms and a proof default (a CMYK profile, which the built-ins lack).

**Text, vectors and layer styles at 16 bits (2026-09-27).** User-facing summary: [bit-depth.md](bit-depth.md).

- Text is painted into Qt's `Format_RGBA64_Premultiplied` (`renderTextLayer16`, `renderTextLayerAt`): colours and
  overlapping runs composite at 16 bits; the glyph coverage is Qt's, 8-bit. Rich text, Warp Text (the 16-bit
  `renderWarpedOverBox`), Create Work Path and Convert to Shape; 16-bit PSD type layers go out as `TySh` and back.
- Vectors: `rasterizeVectorMask16` / `rasterizeVectorStroke16` are the same float coverage quantised to 15 bits;
  `renderVectorPaint16` / `renderFillLayer16` draw gradients from `gradientColorExact` (the unrounded ramp) and widen
  pattern tiles; `applyMaskParameters(Gray16&)`. `setVectorShape` makes a 16-bit document's shape pixels at 16 bits.
  `RenderExec<U16>` uses them for vector masks, shape strokes, fill layers and mask density/feather. Fill Path and
  Stroke Path fill through 16-bit coverage.
- Layer styles: `drawStyledLayer` is one template over the sample type (`StyledDraw::drawSource16`, `coverage16`);
  the effects are float masks either way, only reading and writing pixels differ, so the 8-bit instantiation is the
  former code. `via8` is gone. At 16 bits a matte sample under half an 8-bit level is clear (as the healers treat it).
- Baking a clipping mask when its base is deleted renders the base chain at 16 bits and multiplies at 15 bits.
- Calibration (`depth_vector_tests`, `depth_text_tests`; 8-bit-sourced documents, 16-bit render reduced against the
  8-bit render): at 1:1 every style scene (each of the ten effects, bevel kinds and techniques, textures, a
  multiplied layer, opacity 0.6, folder styles pass-through and isolated) is within 1 level, except all ten effects
  together: 2 levels on 0.004% of samples (the 8-bit engine rounds to a byte after each effect). Shapes, strokes, vector
  masks and text: within 1 level; gradient fills and strokes 2 levels on 0.005% (shape) and 0.02% (fill layer at 70%),
  from the 8-bit ramp's three roundings (colour, premultiply, composite). Vector coverage: within 1 level. Reduced
  views (0.5): the layer is resampled at each depth (P2's resampling differs by up to 2 levels), so effects that
  decide on the matte (stroke contour, precise glow, chisel distance fields) move an edge pixel on up to 1.1% of
  samples (worst 152 levels on 0.49% for Stroke Emboss); blurred effects stay within 2 levels on under 0.3%.
- Gates: 8-bit render hashes and existing 16-bit hashes unchanged; 20 new U16 scenes (shapes, vector masks, a fill
  layer, each effect, all ten, a folder style); brush parity untouched; PSD corpus plus K.psd 118 files / 3,975
  blocks at 8 and 16 bits; rpc smoke's 16-bit section covers text, shapes, paths, vector masks and styles.
- Still gated: smart objects and Smart Filters (`SmartObjectSource::image` is 8-bit; placing and converting at 16
  bits needs the source at its own depth: an `Image16Ptr` beside it, 16-bit decoding in `contentsFromFile`, a 16-bit
  child document in Convert, 16-bit PNG in projects; Smart Filters have their own calibrated 8-bit kernels in
  `smartfilter_render.cpp`), Camera Raw, G'MIC, Remove Background, artboards, the timeline, SVG and slice export.

**Smart objects and Smart Filters at 16 bits (2026-09-28).** User-facing summary: [bit-depth.md](bit-depth.md);
details and the calibration table: [smart-objects.md](smart-objects.md#at-16-bits).

- `SmartObjectSource::image` is an `AnyImage` at the source's own depth; `smartObjectSourceImage` gives it at a
  document's depth (a converted copy made once per source, shared by its instances, `SmartObjectDepthCache`).
  Image ▸ Mode converts instances with their layers and leaves sources alone. The embedded-file decoders keep 16 bits
  (`decodeSmartObjectPng`, a nested PSD's `composite16`, Qt's 64-bit formats in the app's `contentsFromFile`).
- Convert to Smart Object builds the child at the document's depth (a 16-bit PSB); `smartObjectContentsDocument`
  opens contents at the source's depth; `encodeSmartObjectContents` writes a 16-bit PNG from a 16-bit child. Place,
  Replace, Rasterize, warps and the cage draw through `drawSmartObjectRaster` (either depth, filtered, warped or plain).
- Smart Filters at 16 bits: `smartfilter_render16.cpp`, the thirteen kernels on 15-bit straight colour, sharing the
  Gaussian line plans, the parameter checks and the noise hash with the 8-bit file (`smartfilter_kernels.h`; the 8-bit
  kernels are otherwise untouched). Twelve are offered; Unsharp Mask is gated at 16 bits with a message naming it (its
  8-bit reference amplifies its own byte-rounded low-pass: 2 levels apart at 50%, 4 at 400%). Every offered kernel is
  within one level of its 8-bit twin on 8-bit-sourced input; stacks with a blend or a gray mask differ by 2 levels on
  one or two isolated samples (the 8-bit engine's rounding between entries). The `FEid` record, the filter mask and
  the cache stay 8-bit, as Photoshop's 8-bit cache is.
- PSD: 16-bit documents write and read their smart objects (sources as they are in `lnk2`, untouched instances byte
  for byte). A 16-bit PSD gets no `FEid`, as before; opening one with no cache at all, supported stacks get a record
  made here (document canvas, white mask: `addDefaultSmartFilterCache`) and stay editable. Photoshop's own 16-bit
  `FEid` is still not read (no fixture has one), so such files keep filtered instances preview-locked.
- Projects: 16-bit sources are 16-bit PNG sidecars; `ProjectLoadLimits::smartObjectBytes` (was `smartObjectPixels`)
  counts decoded bytes at each source's depth.
- App: the filter-mask proxy layer is made at the document's depth and synced back to the 8-bit stack mask; the Smart
  Filter dialog previews at 16 bits.
- Gates: `edit.smartObject` in supports.cpp, the menu actions carry the feature, the automation `smartObject.*`
  methods are in `worksAtDepth`. 8-bit render hashes and existing 16-bit ones unchanged; 18 new U16 scenes; PSD corpus
  plus K.psd 118 files / 3,975 blocks at 8 and 16 bits; K.psd at 16 bits keeps its three editable and three linked
  smart objects, redraws the editable ones at 16 bits and keeps them editable through a 16-bit PSD; rpc smoke's
  16-bit section places, converts, edits contents, filters, paints the filter mask, warps, replaces and rasterizes.
- Still gated at 16 bits then: Unsharp Mask as a Smart Filter, Camera Raw, G'MIC, Remove Background, artboards, the
  timeline, SVG and slice export (all ported since: the next note).

**The last 16-bit gates (2026-09-28).** Nothing is greyed out in a 16-bit document any more. User-facing summary:
[bit-depth.md](bit-depth.md). Each item removed its gate (supports.cpp, the menu feature, the automation map) and has
U16-vs-U8 calibration tests and U16 render-hash scenes; existing hashes unchanged, 16 new scenes.

- Unsharp Mask as a Smart Filter (`smartfilter_render16.cpp`): the 8-bit kernel rounds its low-pass to whole levels
  after each pass and the amount multiplies that rounding, which was the 2 to 4 levels. On colour on the 8-bit grid
  (within half a 15-bit step over the alpha, so an unpremultiplied edge counts) the low-pass runs in 8-bit levels
  exactly as the 8-bit kernel's; off the grid it is exact. Threshold handling was not the cause. Within one level at
  50/1/0, 150/2/8, 175/2.5/7 and 400/3/2 (`smartfilter_tests`); a fine ramp stays within two 15-bit steps of itself at
  300%.
- Camera Raw (`src/pixels/CameraRawPixelsBody.inc`): the kernels compiled twice, over bytes (unchanged) and over
  premultiplied float on the 0..255 scale with no rounding between the steps; `applyCameraRaw(Image16&)` rounds once.
  Defringe decides its hue windows on the colour rounded to 8 bits (`CR_DECISION`), else a colour at a window's edge
  flipped (56 levels on 9 samples). Grain uses the Grain adjustment's 16-bit kernel. Every panel within one level on an
  8-bit picture with a half-transparent corner; all panels at once two levels on 0.19% of samples (`cameraraw_tests`).
- G'MIC (`app/Gmic.cpp`): 16-bit layers go in as straight float on G'MIC's 0..255 scale, in-process or through a float
  `.cimg` for the executable (G'MIC writes the narrowest exact type back; the reader takes them all), and come back at
  16 bits; a sample that is an 8-bit level widened goes in as that level. The executable's 8-bit PNG path now cuts to
  0..255 and rounds (an overshoot wrote a 16-bit PNG of raw values, read back scaled down; CImg truncates) and keeps the
  alpha of a gray or RGB result, as the in-process path does. blur, unsharp, sepia, sharpen within one level; Solarize
  over half-transparent pixels six levels on 0.9% (the 8-bit run's rounded straight colour through a range-wide
  scale; zero when opaque) (`gmic_catalogue_tests`, runs when `gmic` is installed).
- Remove Background: model and guide on the layer narrowed to 8 bits; `AlphaPlane` through refinement, committed as a
  16-bit mask (`applySubjectMask(Gray16, Image16)`); `estimateForeground(Image16, AlphaPlane)`. Mask within one level
  of the 8-bit mask with 687 distinct values on the test ramp; decontaminated colours within one level (`core_tests`).
- Artboards and the timeline: the 16-bit renderer already drew artboards; gates removed. GIF frames of a 16-bit
  document are rendered at 16 bits and dithered to 8 before the palette (`encodeDocumentGif`), with a note.
- Exports: artboards and slices as 16-bit PNG or dithered JPEG (`renderRect16`, replies carry `bits` and `note`); SVG
  renders a 16-bit document's images and folder masks at 16 bits and embeds 16-bit PNGs.
- Found on the way: Layer > Layer Mask > From Selection, the Layers panel's Edit Text, style and vector-mask items, the
  Eyedropper, and `brush.import`, `presets.import`, `presets.remove` were greyed or refused at 16 bits for want of a
  feature entry although they work; they have one now.
- Gates: GCC and Clang `-Werror` full builds, ctest 50/50, PSD corpus plus K.psd 118 files, 0 failed, 3,975 carried
  blocks at 8 and 16 bits, rpc smoke (with and without the background model), translations, `bench_core 9` 8-bit lines
  A/B against main within ±2%.

**P6 landed (2026-09-28): the Channels panel and alpha channels.** User-facing summary: [channels.md](channels.md).

- `Channel { id, name, image (AnyGray at the document's depth), kind Alpha|Spot, color, opacity, selectedAreas,
  psdCarry }` in `Document::channels` (section 7's design; the gray is the channel as Photoshop shows it, so Color
  Indicates Selected Areas stores the inverse and switching it inverts the gray). Channels are in undo snapshots,
  `retainedBytes`, the mask budget and depth conversion, and follow Crop, Canvas Size, Image Size and Flip Canvas.
  At most 53 (Photoshop's 56 with the colour channels).
- `channels.h`: Save Selection into a new or existing channel (replace, add, subtract, intersect), Load Selection
  from a channel, the composite's luminosity, a colour channel, a layer's transparency or mask (invert, four modes),
  `thumbnailClickMode` (Ctrl, Ctrl+Shift, Ctrl+Alt, Ctrl+Shift+Alt), `keepColorChannels` / `restrictToColorChannels`,
  and `applyChannelView` for the canvas.
- Colour channels are views: the session's `activeColors_` and `visibleColors_` bitsets. With some channels active,
  every pixel edit is limited to them when the outermost edit ends (the edit's alpha and grid are the layer's own
  again), and stroke and adjustment previews show it; edits of the canvas or the layer structure are left whole. With
  all three active nothing runs: 8-bit render hashes and brush parity are byte-identical. Paste into a single
  channel writes the clipboard's gray; Delete fills the channels with the background colour.
- An alpha channel made the target is painted through a temporary layer, as Quick Mask is (`paintsQuickMask` covers
  both), and written back into the channel inside each edit's undo step; choosing a channel is not an undo step.
- PSD: channels read from the merged image's extra planes with resources 1045/1006 (names), 1077/1007 (display) and
  1053 (identifiers), written after the transparency; untouched channels keep their DisplayInfo record, identifier
  and a 16-bit file's own samples (`PsdChannelCarry`). Of the corpus, `photoshop-saved-channels.psd` has two alpha
  channels and a spot channel (names, display and planes come back byte for byte, resources 1006, 1045, 1077, 1053
  identical); `arrows.psd` names its merged transparency in 1006/1045, which is recognised and not made a channel.
  Projects: a `channels` manifest array with `channels/<id>.png` at the document's depth (format 8; no bump).
  TIFF extra channels: not done (Qt's TIFF plugin does not expose them).
- App: Window ▸ Channels (tabbed between Layers and Paths), Select ▸ Save Selection and Load Selection, Channel
  Options, Ctrl+2..9 and Ctrl+Alt+2..9 (no clash with existing bindings). Automation `channels.*` (eight methods),
  the MCP tools, rpc smoke at 8 and 16 bits. `edit.channels` in supports.cpp (8 and 16 bits).
- Gates: `channels_tests` (model and undo, every save and load mode at both depths, click modifiers, canvas
  operations, single-channel painting and Invert at 8 and 16 bits, the view, PSD and project round trips, damaged
  channel resources); full ctest; 8-bit render hashes and brush parity unchanged; PSD corpus plus K.psd 118 files /
  0 failed / 3,975 carried blocks at 8 and 16 bits, 3 channels back; GCC and Clang `-Werror`; rpc and MCP smoke;
  translations_check. bench_core A/B (three alternating rounds of 9) was not conclusive: another lane loaded the machine and 8-bit lines moved from -23% to +54% in both directions; no benchmarked kernel (render, blend, brush, blur, adjustments) changed, and the all-channels path adds only a bitset test per edit.
- Not done: editing spot channels, Apply Image, Calculations, Split and Merge Channels, TIFF extra channels, CMYK and
  Lab colour channels (P7).

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
