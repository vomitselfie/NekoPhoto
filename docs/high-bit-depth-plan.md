# High bit depth and colour management: design plan

Status: P1–P4 landed in 1.7 (16-bit RGB editing, painting and colour management); see "Status" below for each phase and what is still gated at 16 bits. P6 (channels) has landed too, and P5a (the 32-bit core); the rest of P5, P7 and P8 are planned. The design sections below are kept as written.

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
  Every edit brings buffers held at another depth to the document's (`conformToFormat`), so a document keeps one
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

**P5.0b landed (2026-09-28): `StrokeRaster<S>`, no behaviour change.**

- `src/core/src/stroke_raster.h`: the brush stroke's pixels as one template, `StrokeRaster<S>`, over a `StrokeOps<S>`
  policy. The raster owns the working pixels or mask, the coverage, the selection, the visible-mask copy, the dab
  table, the stamps (`StampOf<T>` moved here) and the tail's coverage backup, and does the dab (stamped and per
  pixel), the max and screen build-up, the recompose (paint, erase, mask, gray sample, clone and processed sources),
  lifting and moving pixels, gradients, Spot Healing and the Healing Brush, and the commit. `BrushStroke` keeps what
  is depth-free: the grid's placement, the curve and spacing, the provisional tail, the dirty and touched areas, and
  the clone settings. It holds the raster at its depth and hands each step to it; its public API is unchanged.
- `StrokeOps<U8>` and `StrokeOps<U16>` hold each depth's arithmetic with the exact expressions of the former
  `brush.cpp` and `brush_u16.cpp` (`mul`, `screen`, `toward`, `mix`, `quantise`, `store`, `nearest`, `solidCore`, the
  table interpolation), its buffer types, and which `AnyImage`, `CloneSource` and tiled source fields are its own.
  `brush.cpp` instantiates U8 and `brush_u16.cpp` U16. Two per-depth differences are kept on purpose: the 8-bit
  stroke paints a layer held at another depth as blank where the 16-bit one refuses it, and the 8-bit move samples
  with its old `sx - 0.5 + 0.5` centring.
- Gates: brush_parity (8-bit, u16, synthetic, and the local section against a NEOMAWZ brushset), brush_grain_tests,
  brush_smoothing_tests, depth_paint_tests and render_hash_tests pass; the layers every depth_paint_tests case
  compares, plus mask gray samples, clones without a source at their depth, tiled clones, colour fills and a
  fractional move on a scaled layer (170 buffers), hash the same before and after at both depths. The 8-bit stamp
  merge still vectorises (GCC's report lists the same loops). GCC and Clang `-Werror`, full ctest, rpc smoke.
- For P5c, `StrokeOps<F32>`: the comment at the top of `stroke_raster.h` lists what it needs (float samples, a float
  table lerp, `screen` as `old + t × (1 − old)`, stores without rounding, colour unclamped but coverage clamped, the
  brush colour linearised first, float overloads of the crop, bounds, gradient, MipCache and Asset calls, and healing
  on the `decisionImage`).

**P5a landed (2026-09-28): the 32-bit core.** User-facing summary: [bit-depth.md](bit-depth.md#32-bits-per-channel).

- Float primitives (`depth_f32.cpp`): `lineariseImage` / `encodeImage8` / `encodeImage16` through the document's
  `TransferCurve`, with `fromLinearExact` (a table curve's own interpolation inverted), so 8/16 → 32 → 8/16 gives every
  sample back at every working space and every 15-bit level; coverage widened and narrowed by scale only; premultiply,
  halvings, bilinear and Catmull-Rom samplers (unquantised), crops, bounds, fingerprints, sRGB-encoded thumbnails,
  `imageAtDepth(image, type, curve)`, float resampling, MipCache float levels, `cleanFloat` (NaN 0, infinity 65504,
  negative colour 0, coverage 0..1) at the file entry points, and PSD's byte-planar predictor.
- Profiles: `linearProfile` (the profile with gamma 1.0 curves, LUT tags dropped, "<name> (Linear)", fixed date),
  `gammaCounterpart` (the working space it came from, by bytes or primaries, else sRGB's curve), `Document::encodedProfile`
  (`std::optional`: the profile to go back to, empty for untagged), `encodedProfileOf`, `luminanceWeights`,
  `PixelFormat::RGBAFloat` (display input) and `toDisplayF`.
- `view32.h`: `View32 { exposure, gamma, method }` and `ToneMap`. Highlight Compression is extended Reinhard on the
  profile's luminance with the peak as white (identity when nothing exceeds 1); Photoshop's exact curve is unknown.
- `blend_f32.cpp`: the W3C formulas for all 27 modes on premultiplied float. Photoshop's 32-bit set (Normal, Dissolve,
  Darken, Multiply, Lighten, Linear Dodge, Difference, Subtract, Divide, Hue, Saturation, Color, Luminosity, Darker and
  Lighter Color; `blendModeAt32`) blends unbounded values; the others clamp cb and cs to 0..1 inside B only, so the
  source and backdrop terms keep their range. ClipColor keeps only its lower bound (light above 1 is not clipped).
- Rendering: `DeepOps<F32>` (`render_deep_ops_f32.h`), `render_exec_f32.cpp`, `render_f32.cpp`, instantiating
  `render_exec_deep.inc` and `render_deep.inc` unchanged. The policy's functions are static, so what depends on the
  document (the curve 8-bit colours are linearised with, the luminance weights, the view) comes from a
  `FloatRenderScope` the entry points set; one 32-bit render runs at a time. Vector coverage, gradient and pattern fills
  and mask density and feather come from the 16-bit rasterisers (15 bits), adjustment layers are not drawn yet
  (`adjust` returns false; converting says so). `drawStyledLayer` has an F32 instance (colours linearised, nothing
  rounded or clamped above 1). `render()` dispatches F32 through `renderForDisplayF` (the view, then the display transform
  or the curve).
- Mode conversion: `convertSampleType(document, type, error, toning)`; to 32 bits one undo step with the byte budget
  (a quarter of the 8-bit pixels); from 32 bits HDR Toning (Exposure and Gamma, Highlight Compression) with a live
  preview through the view. `conformToFormat` linearises 8/16-bit buffers reaching a 32-bit document through its
  encoding curve.
- `supports()`: a 32-bit column (rendering, Image ▸ Mode, saving and the export formats, the layer structure, masks,
  whole-layer transforms, canvas size and flip, import, the non-pixel tools, the view), `photoshopLacksAt32` and
  `notAvailableAtDepth`: "Not available in 32-bit mode" for Photoshop's own gaps (Dodge/Burn/Sponge, the Paint Bucket,
  the content-aware tools, Brightness/Contrast, Posterize, Threshold, Selective Color, Grain, Mosh, G'MIC, blend modes
  outside the set), "Not available in 32-bit yet" for the rest. Patch shares `tool.spotHealing` and so says "yet".
- Formats: PSD/PSB 32 read (`Lr32`, raw/RLE/zip with prediction, float masks, merged image and channels; 1039 taken as the
  space of the linear values) and written (`Lr32` with zip and prediction, float merged image, 1039 the encoding
  profile); unedited layers carry their planes byte for byte, also through projects. Projects: `sampleType "f32"`,
  `.f32z` sidecars, `encoded.icc`. Exports to 8/16-bit formats are tone-mapped at exposure 0 with a note.
- App: Image ▸ Mode ▸ 32 Bits/Channel, HDR Toning, View ▸ 32-bit Preview Options, the status bar's exposure, the blend
  picker greying, per-feature tooltips; automation `image.mode` (32, `method`, `exposure`, `gamma`) and `view.exposure`,
  the MCP bridge, rpc smoke's 32-bit section, Japanese strings.
- Gates: `depth_float_tests` (every mode against the double reference `tests/float_reference.cpp` within 1e-5 absolute
  plus relative, with values up to 6, masks, opacity and an isolated folder; exact round trips; opaque Normal stacks
  identical at 8 bits), `depth_float_format_tests` (a constructed 32-bit PSD, as no Photoshop-saved one exists in
  Patchy's fixtures: opens as float, channels and 1039 byte for byte through PSD and projects; PSD, raw PSD and PSB of a
  converted document back to its 8-bit pixels exactly; `.f32z` projects), 72 `f32/` render-hash scenes with every 8- and
  16-bit hash unchanged, brush parity and the depth tests unchanged, the PSD corpus plus K.psd 118 files / 0 failed /
  3,975 carried blocks at 8, 16 and (new, `PSD_ROUNDTRIP_32`) 32 bits, full ctest, GCC and Clang `-Werror`, rpc smoke,
  translations. `bench_core`: instruction counts (`perf stat -e instructions:u`, core 0) of the 8/16-bit lines against
  the start within ±1%.
- For P5b (adjustments and filters, selections, pixel edits): adjustment layers need `DeepOps<F32>::adjust`; the float
  kernels follow `adjustments_u16.cpp`, in linear light (Photoshop's 32-bit Levels, Curves, Exposure, Hue/Saturation,
  Photo Filter, Channel Mixer, Vibrance...); blur in float; selections as `GrayF` with the wand deciding on a fixed
  exposure-0 `decisionImage()`; Image Size and the other resamplers.
- For P5c: `StrokeOps<F32>` (stroke_raster.h lists it); MyPaint's documented 15-bit round trip; healing on the
  `decisionImage`.
- For P5d: EXR (optional system library), Radiance .hdr, 32-bit TIFF through libtiff, RAW to linear float. For P5e:
  exact float vector coverage and gradient ramps (today via 16 bits), text, shape and style editing, smart objects at
  F32 (their sources are converted with sRGB's curve today). For P5f: Local Adaptation, Equalize, the HDR colour picker.
  Also open: Assign and Convert to Profile at 32 bits, 32-bit channels in the Channels panel's editing, and a concurrency
  cost: 32-bit renders serialise on one lock while a scope is active.

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

## P5 plan: 32-bit float (decided 2026-09-28)

**Semantics.**
- A 32-bit document holds premultiplied linear light in its profile's primaries. The profile is the linear version of the gamma profile: `linearProfile()` keeps the primaries and white point, uses a gamma 1.0 curve, and gets a fixed header date. `gammaCounterpart()` goes back, and `Document::encodedProfile` remembers the curve. Untagged documents become linear sRGB.
- Colour samples may exceed 1.0. NaN and Inf are cleaned at every entry point.
- Alpha, masks and selections are coverage: clamped to 0..1 and never linearised.

**Storage and budgets.**
- 16 bytes per pixel, so a quarter of the 8-bit pixel budget.
- **Masks, selections and channels are `GrayF`** (one depth per document).
- **Region undo (`RegionPatch`) lands first:** a stroke keeps only the changed region's before-copy, at every depth.

**Rendering.**
- **One deep executor:** `render_exec_u16.cpp` becomes a template shared by U16 and F32, `RenderExecDeep<S>` with a `DeepOps<S>` policy. The 16-bit hashes must stay identical after the move. The 8-bit executor is untouched.
- **The brush stroke raster becomes `StrokeRaster<S>`.**
- **`blend_f32`** uses the W3C/PDF formulas on premultiplied float spans.
  - The UI offers Photoshop's 32-bit subset: Normal, Dissolve, Darken, Multiply, Lighten, Linear Dodge, Difference, Subtract, Divide, Hue, Saturation, Color, Luminosity, Darker/Lighter Color (verify).
  - The other modes render with their inputs clamped, so imported and converted files still draw, but they are greyed in the picker.
  - Non-separable modes use the profile's linear Y.
- **Layer styles:** an F32 instance of the existing template, with effect colours linearised at draw time.

**Display.**
- A per-view `View32 { exposure, gamma, method }`: an exposure slider in the status bar, View ▸ 32-bit Preview Options, and `view.exposure`.
- The display runs exposure, then gamma (or highlight compression), then Little CMS float to the monitor. That needs `PixelFormat::RGBAFloat` and `toDisplayF`.

**Mode conversion.**
- **8/16 → 32:** linearise through the document's transfer curve. One undo step.
- **32 → 16/8:** a dialog with Exposure and Gamma or Highlight Compression, and a live preview. Layers are kept. At the defaults it is an exact round trip for 8/16-sourced documents.
- Local Adaptation and Equalize come later (P5f).

**Features at 32 bits follow Photoshop.** What Photoshop greys at 32 bits is greyed here with "Not available in 32-bit mode" (no "yet"): Dodge/Burn/Sponge, the Paint Bucket, Patch, the content-aware tools, Brightness/Contrast, Posterize, Threshold, Selective Color, Grain, Mosh and G'MIC (at first). Remove Background is greyed at first.

**Hard problems.**
- **MyPaint:** a documented 15-bit round trip. Only samples a dab changes are rewritten; untouched pixels keep their exact floats.
- **Dodge/Burn:** greyed, as in Photoshop.
- **Decisions on the display:** the wand, Quick Select and healing decide on a fixed exposure-0, 8-bit `decisionImage()`, so results don't change with the view.
- **Tests** run against a double-precision `float_reference`, not an 8-bit one.

**Formats.**
- **PSD/PSB 32:** read `Lr32` planes as float; write `Lr32` with zip and prediction.
- **OpenEXR:** an optional system library (BSD-3) through `find_package`, with a vendored static build only if the Windows packages are missing.
- **Radiance .hdr:** our own reader and writer.
- **32-bit TIFF:** through libtiff directly.
- **RAW to linear float:** linear sRGB by default.
- **Projects:** `.f32z` sidecars (zlib over byte-planar delta rows, the PSD predictor), so no extra dependency.

**Phasing.** Each step keeps the 8/16-bit hashes, brush parity and PSD corpus identical, builds clean with -Werror, passes smoke, and holds benches within ±2%.

| Step | Content |
|---|---|
| P5.0a | The deep executor template, 16-bit hashes identical |
| P5.0b | `StrokeRaster<S>` |
| P5.0c | `RegionPatch` undo |
| P5a | Float primitives, `blend_f32`, `RenderExec<F32>`, display and View32, mode conversion, supports wording, project and PSD 32 |
| P5b | The adjustments and filters subset, selections, pixel edits |
| P5c | Painting on `StrokeRaster<F32>`, MyPaint round trip, `decisionImage` |
| P5d | EXR, HDR, TIFF float, RAW float |
| P5e | Text, shapes, styles, smart objects at F32 |
| P5f | HDR Toning Local Adaptation, and the HDR colour picker (optional) |

## P7 plan: CMYK and Lab (decided 2026-09-28)

**Storage.**
- **CMYK:** 5 samples (C, M, Y, K, alpha) on **inverted ink** (PSD's convention), premultiplied. The RGB separable kernels then apply per ink, and PSD planes carry over untransformed.
- **Lab:** 4 samples with offset a/b (128 at 8 bits, 16384 at 16), premultiplied. Modes that read the a/b sign unpremultiply first. The offset is hidden behind accessors in `colormodes.h`.
- **8 and 16 bits only.** Photoshop has no 32-bit CMYK or Lab, so P5 and P7 are independent.
- **8-bit CMYK is a new `ImageC8` (`ImageT<U8>`, 5 channels), a fourth `AnyOf` alternative** with `.c8()`. `.u8()` stays null, so RGB-only paths gate themselves.
- 8-bit Lab uses `Image`; 16-bit uses `Image16` with 4 or 5 channels. `Document::colorMode` is a document property.
- Budgets are bytes: sample size × channels.

**Rendering.**
- New `render_exec_c8.cpp` and `blend_c8.cpp`. The 16-bit executor gets a channel-count parameter, with the RGB16 instance unchanged. The display transform to the monitor is fused into the reduction and is never null for CMYK/Lab.
- **CMYK:** all 27 modes. The non-separable ones (Hue, Saturation, Color, Luminosity, Darker/Lighter Color) ship only if they can be calibrated against Photoshop renders; otherwise they render as Normal with a notice.
- **Lab:** the modes Photoshop shows (verify; Adobe lists Color Dodge/Burn, Darken/Lighten, Difference, Exclusion, Subtract and Divide as unavailable).
- **Soft proof:** a CMYK target for RGB documents, using the bundled working CMYK.

**Default CMYK profile.** Bundle **basICColor `ISOcoated_v2_300_bas.ICC` (FOGRA39, zlib/libpng licence)**; check the licence text in the source archive before shipping. Adobe's SWOP and the ECI profiles can't be bundled, but Color Settings ▸ Working CMYK accepts any installed ICC file. Lab uses the built-in D50.

**Mode conversion.**
- Image ▸ Mode ▸ RGB/CMYK/Lab (`convertDocumentMode`) uses Color Settings' intent and black-point compensation; black generation comes from the profile. One undo step.
- It converts every raster, fill, style and adjustment colour, and the foreground/background.
- **Adjustment layers with no counterpart in the new mode are kept, inactive and marked**, so converting back restores them. Per-channel curves reset.

**Channels.** `colorChannelsAll(mode)`: C, M, Y, K or L, a, b in the panel, with single-channel editing generalised to N channels. Loading a CMYK channel selects its ink.

**Features follow Photoshop per mode.** `supports()` gains a mode axis: "Not available in CMYK mode" / "Lab mode". **MyPaint brushes are greyed in CMYK and Lab.** Camera Raw and G'MIC are off in both.

**Formats.**
- **PSD modes 4 and 9 open natively** at 8 and 16 bits, replacing today's conversion to RGB; untouched layers carry byte for byte.
- **CMYK JPEG** (APP14, inverted) through libjpeg-turbo directly. CMYK TIFF comes later, through libtiff.
- **Projects:** a `colorMode` key, with CMYK layers as raw compressed planes.
- **RGB exports convert with a note.**
- **Fixtures:** the owner is looking for Photoshop-saved CMYK and Lab PSDs. Until they arrive, format tests use constructed files, marked as weaker.

**Phasing.**

| Step | Content |
|---|---|
| A | Types, model, budgets, the mode axis in `supports()`, the manifest |
| B | colormgmt CMYK/Lab formats, the bundled profile, Working CMYK, the CMYK soft proof for RGB documents |
| C | Executors and blends, display, channel view |
| D | Native PSD 4/9 and Image ▸ Mode |
| E | Editing ports (fill, adjustments, blurs, transforms, then the brush) |
| F | Channels panel and pickers per mode |
| G | CMYK JPEG, and non-separable calibration if fixtures allow |

Minimum shippable subset: A–D and F plus the cheap part of E.

**Status (2026-09-28): steps A and B landed.**

- A: `ColorMode` on `Document` (manifest `colorMode`, project version 9; RGB writes nothing new), `colormodes.h`
  (inverted-ink and Lab offset accessors), `ImageC8` as the fourth `AnyImage` alternative (`.c8()`; `.u8()` null),
  `conformToFormat`, byte budgets by channel count (`imagePixelBudget(type, mode)`, `formatBudgetProblem`), the mode
  axis in `supports()` ("Not available in CMYK mode"), CMYK layers as `images/<id>.cmyk` planes (zlib, zstd read),
  Lab as 4-sample PNGs, `ProjectLoadLimits::layerBytes`. Tests: `colormodes_tests`.
- B: `PixelFormat` CMYKA8/16 and LabA8/16 (staged, straight around Little CMS), `labProfile()` (Lab D50, fixed
  bytes), the bundled Working CMYK (basICColor ISO Coated v2 300%, zlib licence verified in Debian's
  `icc-profiles-free` 2.4 source), Color Settings > Working CMYK, `convertImage` between any layouts, fused
  `convertImageTo8`, Proof Setup > Working CMYK (the default proof) with the gamut warning, `color.settings
  workingCmyk`, `document.profile working-cmyk`. Tests: `colormgmt_cmyk_tests` against Little CMS.
- Gates: full ctest (53), render hashes and brush parity unchanged, PSD corpus plus K.psd 118 files / 0 failed / 3,975
  blocks at 8 and 16 bits, GCC and Clang `-Werror`, headless rpc smoke, translations_check. bench_core A/B was not
  conclusive: other lanes loaded the machine (lines moved from -46% to +460% in both directions, including layer
  bookkeeping this work does not touch); the only change on benchmarked paths is one branch in `ColorTransform::apply`.
- For step C: thumbnails for CMYK layers (`Asset::make(ImageC8Ptr)` has none, 5-channel `Image16` none either), Lab
  thumbnails read as RGB; the display path is `transformBetween(document profile, monitor, pixelFormatFor(depth, mode),
  RGBA8)` plus `convertImageTo8`, never null for CMYK/Lab; `conformToFormat` leaves buffers of another channel count
  alone, so paste/place into CMYK needs `convertImage(..., RGB, from, CMYK, to)`; stored colours (text, shapes, styles)
  are still RGB values in every mode.
