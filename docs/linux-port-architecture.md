# Linux port: architecture note

Answers to the investigation checklist in the port plan, gathered from the macOS
sources before any Linux code was written. Line references are to the Swift
sources under `Compositor/`.

## Where things live in the Mac app

| Concern | Files | Portable? |
|---|---|---|
| Document and layer state | `Document/EditorSession.swift` (`ImageLayer`, `CanvasDocument`), `Document/LayerTransform.swift`, `Document/LayerMask.swift`, `Document/LayerAppearance.swift`, `Document/LayerAdjustment.swift`, `Document/LayerGroups.swift`, `Document/LiveLayerMask.swift` | Logic yes, types are Swift value types over `CGImage` |
| Undo | `Document/DocumentHistory.swift` | Yes, pure logic |
| Selection | `Document/Selection.swift`, `SelectionEdits.swift`, `MagicWand.swift` | Uses `CGPath` boolean ops; needs a raster replacement |
| Brush | `Document/BrushStroke.swift`, `Rendering/MetalBrushCoverage.swift` | Algorithm yes; tiles and Metal are Apple-specific |
| Compositing | `Rendering/LayerRenderer.swift`, `LiveMaskRenderer.swift`, `TiledLayerRenderer.swift`, `SeparableBlend.swift`, `DownsampleCache.swift` | Semantics yes, implementation is Core Graphics |
| Pixel algorithms | `Rendering/*.c` (8 files, 887 lines) | Yes, plain C |
| Project format | `IO/ProjectStore.swift`, `docs/project-format.md` | Format yes; `FileWrapper`/`ImageIO` are Apple |
| Import/export | `IO/ImageImporter.swift`, `ImageExporter.swift` | Apple ImageIO |
| UI | `ContentView.swift`, `CompositorApp.swift`, `UI/*`, `Rendering/EditorCanvas.swift` (1814 lines, AppKit) | No |

Only the eight C files compile unchanged. Everything else is re-expressed in
C++ under `src/core`, following the Swift behaviour closely enough that the
same `.comp` file renders the same.

## Image buffers

- `CGImage`, sRGB, 8 bits per channel, **RGBA byte order, premultiplied alpha,
  rows top-down** (`BrushRaster.context`, `premultipliedLast | byteOrder32Big`).
- Masks are 8-bit grayscale without alpha, white reveals. A uniform mask may be
  1×1 and is stretched over the layer (`LayerMask.solid`).
- Images are immutable and shared by reference; undo snapshots share them, so a
  layer edit costs no pixel copies (`DocumentHistory`, "Value snapshots share
  immutable CGImages").
- The C routines take `(rgba, width, height, stride)` on that same format.

The Linux core keeps exactly this contract: `Image` is RGBA8 premultiplied,
top-down, explicit stride, held in `std::shared_ptr<const Image>`; `GrayImage`
holds masks and coverage.

## Layer transform

`LayerTransform` (`Document/LayerTransform.swift`): `origin` and `size` are the
unrotated bounds in document pixels; `rotation` is degrees clockwise about the
center; `flipX`, `flipY`; `sampling` is `Nearest`, `Smooth` or `High quality`.
Pixel to document mapping is `BrushRaster.pixelToDocument`:

```
translate(center) · rotate(radians) · scale(size/pixels, flips) · translate(-pixels/2)
```

Pixels keep their full resolution; scaling is only a transform (README:
"non-destructive move, scale, rotate and flip").

## Compositing order

`ImageExporter.render` is the reference flatten path; the canvas uses the same
routines. For every visible non-group layer, bottom to top:

1. Folder masks: each enclosing group with an enabled mask clips the layer
   (`FolderMaskClip`). Groups are pass-through; they never composite as a unit.
2. Clipping masks (`maskSourceID`): a base followed by contiguous clipped
   siblings forms a stack (`LiveMaskRenderer.prepareStacks`). The base is drawn
   into a group buffer, its alpha extracted and the colour made opaque
   (`layer_extract_alpha`, `layer_unpremultiply_opaque`), the children drawn on
   top with their own blend modes, the alpha restored (`layer_restore_alpha`),
   and the group composited with the base's blend mode. A clipped layer whose
   source is not directly below is instead clipped by the source's coverage
   (its alpha with its own mask, ignoring visibility).
3. The layer itself: image resampled through its transform, own mask
   multiplied in (the mask covers the layer's pixel grid unless it has a
   `placement` of its own), opacity, then the blend mode against the backdrop.
4. Adjustment layers apply to the composite so far, clipped by folder masks
   and their own mask, mixed by opacity; with a non-normal blend mode the
   adjusted colour is blended at full coverage and the original alpha restored.

Blend modes (`LayerBlendMode`): Normal, Multiply, Screen, Overlay, Darken,
Lighten, Difference, Color Dodge, Color Burn, Hue, Saturation, Color,
Luminosity, with the PDF/Photoshop definitions (`SeparableBlend` exists
precisely because Core Graphics gets Dodge and Burn wrong; the Linux core
implements the spec directly, so no such workaround is needed).

Large reductions draw from sharp power-of-two halvings and let the final
resample do at most 2× (`DownsampleCache`, `LayerRenderer.reduced`). The Linux
renderer does the same: a per-image mip chain plus bilinear for the last step.

## Selections

`DocumentSelection` is a `CGPath` in document space with an antialias flag;
edits combine paths with `union`/`subtracting`/`intersection`. Selections are
session-only and never saved. On Linux the selection is a document-sized
8-bit coverage raster plus an outline traced with the existing `wand_trace`.
Boolean operations become per-pixel max/min. Outlines are rasterised with a
small scanline filler.

## Undo

`DocumentHistory`: an entry is a `before`/`after` snapshot of the whole
`CanvasDocument` value plus the active layer id. `begin`/`end` nest; a no-op
edit records nothing; entries are trimmed to 100 or 256 MiB retained beyond the
live document. Snapshots share image buffers, so this is cheap. Ported as is.

## `.comp` format

Documented in `docs/project-format.md` and validated in `ProjectStore.swift`.
A package directory:

```
Name.comp/
  manifest.json
  images/<LAYER-UUID>.png
  images/<LAYER-UUID>.mask.png
```

`manifest.json` is Swift `Codable` with `sortedKeys` and `prettyPrinted`.
Encoding details that matter for compatibility:

- `format` is `com.compositor.project`, `version` is 7 for new saves, 1–7 readable.
- UUIDs are uppercase strings.
- `CGPoint`/`CGSize` encode as two-element arrays: `"origin": [x, y]`,
  `"size": [w, h]`.
- Enums encode as their display strings: `"blendMode": "Color Dodge"`,
  `"sampling": "High quality"`, `"kind": "Gradient Map"`.
- Dictionaries keyed by an enum (`hsvSettings.adjustments`, `.bands`) encode as
  a flat array alternating key and value.
- Optional fields are omitted when nil, and old versions may not carry newer
  fields (see the validation rules in `ProjectStore.validate`).
- Limits: 30 000 px per side, 100 Mpx of images plus 100 Mpx of masks, 10 000
  layers, 4 MiB manifest, 512 MiB per asset.

Embedded PNGs hold straight (unpremultiplied) pixels as written by ImageIO;
they are premultiplied on load. The Linux core reads and writes the package
with libpng and keeps unknown JSON fields so a round trip through Linux does
not strip anything a newer Mac build wrote.

## Adjustment layers and filters

`LayerAdjustment` carries the settings for Hue/Saturation, Levels, Curves,
Exposure, Gradient Map and Grain. Gradient Map and Grain already run in C
(`AdjustPixels.c`); Levels has C helpers (`LevelsPixels.c`); Hue/Saturation,
Curves and Exposure are Swift/Core Image and are re-implemented in C++.
Destructive filters (Gaussian Blur, Motion Blur, Add Noise, Lens Correction,
Invert, Remove Background) act on layer pixels; Noise and Lens are in C.

Thumbnails are 96 px reductions of each asset, generated on import and after
each paint commit; the Linux app generates them the same way from `Image`.

## Apple dependencies of the C files

None. They include only `<stdint.h>`, `<stddef.h>`, `<stdlib.h>`, `<math.h>`
and `<string.h>` and use no Accelerate/vImage. They compile with GCC and Clang
with `-std=c11 -Wall -Wextra` unchanged.

## Decisions taken for the Linux port

- **Qt Widgets rather than QML.** The plan recommends Qt Quick for chrome. The
  UI here is a canvas, a tool rail, a layers tree and a few sheets; Qt Widgets
  gives all of that (dock widgets, tree views, tablet events, portal file
  dialogs) in less code and with no QML/C++ bridging. The canvas is a custom
  `QWidget` that paints the CPU composite. Nothing in the core depends on Qt,
  so a Quick front end remains possible.
- **CPU compositing at document resolution**, then displayed through the
  viewport transform. GPU work comes after parity, as the plan says.
- **Core is Qt-free C++20** with libpng for `.comp` assets and PNG export.
  Image import (PNG, JPEG, TIFF, WebP, HEIC where plugins exist) and JPEG
  export go through Qt's codecs in the app layer, as the plan suggests.
- **The C files stay where they are** under `Compositor/Rendering` and are
  referenced from CMake, so the Xcode project is untouched.
