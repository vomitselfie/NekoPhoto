# Smart objects

A smart object is a source (an embedded file, or a linked one) that layers place without owning it; each
instance has its own placement. The model is NekoPhoto's (`src/core/include/compositor/smartobject.h`), not a
wrapper around Photoshop's blocks: PSD is one mapping of it. It follows `NekoPhoto_Smart_Objects_Roadmap.md`
(phases 1 and the start of 2) and the architecture notes beside it (sources shared by identity, nesting bounded).

- **Sources** live on the document (`Document::smartObjects`, by id): the embedded file's bytes (shared by
  every copy and every undo step), its name and type, a linked file's path, and the contents as an image.
- **Instances** live on layers (`Layer::smartObject`): the source id, Photoshop's placement quad (four corners in
  document pixels), a lock state, the instance id, and the layer's Photoshop blocks for writing back.

An **editable** instance's pixels are the source's image, placed by an ordinary `LayerTransform` derived from the
quad: moving, scaling, rotating or flipping it resamples the full-resolution source every time, never the last
result, and Image Size scales the placement instead of resampling pixels. Painting on it (or anything else that
replaces its pixels) makes it a plain pixel layer, as with text.

A **warped** or **filtered** instance is editable too, but its pixels are not the source under a transform: they
are the source drawn through its warp and its Smart Filters onto the document's pixel grid (see below), redrawn
whenever its contents change. Moved, scaled or rotated, it previews by resampling those pixels and is drawn again
from its contents when the edit ends (`refreshSmartObjectRasters`, from `EditorSession::endEdit`), so it stays sharp
and the filter mask stays where it is on the canvas; export compares the quad with the one the file holds.

A **preview-locked** instance shows the preview the file carried, because NekoPhoto cannot yet redraw it itself:
a warp it does not draw (a quilt warp, a mesh with distortion), placed in perspective or skewed, a Smart Filter
stack with any filter outside the thirteen below, a source it cannot read (a vector `.ai`, a file type the app
cannot decode) or a linked file, or only Photoshop's old `PlLd` form. It can still be moved, scaled
and rotated (its quad follows), and its Photoshop data goes back untouched but for the placement. The import
notes list each kind.

## From Photoshop

The importer reads the global linked-file blocks (`lnk2`, `lnkD`, `lnk3`, `lnkE`; embedded `liFD`, external
`liFE`) into sources and decodes each embedded file: a PSD/PSB through this same importer (Photoshop's own merged
image when the file says it is real, resource 1057, else our render of its layers), PNG directly, anything else
through the app's decoder (JPEG, TIFF, ...). Nested files are read at most `psdSmartObjectDepthLimit` (4) deep.
Each layer's `SoLd` (or `SoLE`, or `PlLd`) gives the instance: source id, `placed` id, `Trnf` quad,
`nonAffineTransform`, warp and `filterFX`. Layouts follow Patchy (MIT, `src/third_party/patchy_psd/README.md`),
which pinned them against Photoshop 2026.

## To Photoshop

A live instance writes its blocks back verbatim while the layer sits where it was placed; moved, scaled or
rotated, the quad is patched in (`Trnf`, and `nonAffineTransform` by the same per-corner delta), every other
descriptor field untouched (the vendored descriptor writer keeps key order and id forms). A duplicate gets a new
`placed` id (Photoshop aliases layers that share one). A drawn Smart Filter instance that moved, or whose contents
changed, gets its record in the document's `FEid` cache written anew (below); a copy of one, or a preview-locked
one that moved, is written as pixels with a warning: its document-space filter cache would no longer match. The sources go back in
the file's own linked-file blocks, byte for byte. The layer's pixels are our render of the placement.

## In a project

Sources are `smartobjects/<n>.source` (the record and the file's bytes) with `<n>.png` (the contents), instances
`images/<layer id>.smartobject`; the manifest does not change, so the Mac app still opens the project (as pixels).

## Checked

`smartobject_tests`: quads and transforms agree (upright, rotated, flipped; skew and perspective refused), a
moved layer moves its quad, placement blocks parse and patch (and an unchanged patch is byte-identical), linked
blocks parse (damage gives nothing), an instance draws its source and survives a project, export writes the
placement where the layer is, painting makes pixels, Image Size scales the placement. On Photoshop's own file
placing a PNG, the source opens editable on the exact quad Photoshop wrote; moved by (10, 5) and scaled x2 it
exports, reopens editable on the moved quad, and survives a project save. A designer's client file opens with
three embedded PSB sources decoded (one 20 MB) and three linked `.ai` sources, its instances locked for their
real reasons (warp, Smart Filters, linked), and round-trips byte for byte.

## Making and changing them

In the Layer ▸ Smart Objects menu and over automation (`smartObject.*`, the MCP `smart_object_*` tools), all in
`src/core/src/smartobject_edit.cpp`, each one undo step:

- **Convert to Smart Object** puts the selected layers (and everything in selected folders) into a PSD whose canvas
  is their bounds; a layer placing it takes the topmost one's place and name, looking the same. Text stays text
  inside (the app's font metrics write it as Photoshop type), and smart objects among them nest.
- **Place Embedded** (File menu) places a PSD, PSB or any image Qt reads, 1:1 in the middle, scaled down to fit.
- **Edit Contents** (the ◆ badge on the layer's row, or the menu) opens the contents in a tab of their own; Save in
  that tab puts them back (in the source's own format: PSD through our writer, PNG, or what Qt writes, else PNG),
  and every layer placing them updates, as one undo step in the document. Save As saves the contents as a project
  of their own instead. The source gets a fresh id, as Photoshop gives it.
- **Replace Contents** swaps in a file's contents; every instance keeps its centre and scale ("A copy" becomes "B
  copy", as Photoshop renames).
- **Rasterize** keeps what the layer shows as plain pixels.
- **Painting or filtering** a smart object asks first, as Photoshop does: Edit Contents (when it can be edited),
  Rasterize, or Cancel. Automation refuses with the same two ways forward.

Converted contents are a PSB, as Photoshop stores them (`8BPB`, "<name>.psb"); PSB contents edited go back as PSB.

Contents that NekoPhoto cannot redraw in some instance (a preview-locked one) cannot be edited or replaced; the
message says which layer and why. New sources are written into the PSD's `lnk2` as version-7 `liFD` elements
beside the file's own untouched ones, and new placements as Photoshop 2026's `SoLd` (Patchy's authoring shape;
Photoshop reads SoLd-only files).

Contents come in as NekoPhoto reads them: a CMYK PSD (common for print illustration) converted to sRGB through its
own colour profile. Edited and put back, such contents are written as an RGB PSD.

## Warps

Photoshop keeps a smart object's warp in the `SoLd`'s `warp` descriptor: a Bezier patch of 2 to 4 by 2 to 4 control
points (`customEnvelopeWarp` `meshPoints`, with `uOrder` / `vOrder`) in the contents' own space, or, as interactive
Photoshop writes it, only a preset style and bend (`warpArc`, `warpFlag`, ... with `warpValue`, and the Warp Text
distortion sliders `warpPerspective` / `warpPerspectiveOther`). `src/core/src/warpmesh.cpp` is a port of Patchy's
warp mesh (MIT): the patch, the fifteen preset constructions Patchy pinned point for point against Photoshop 2026's
own bakes, the distortion, and the resampler. The rules it follows, all from Patchy's captures:

- The placement quad (`Trnf`, and `nonAffineTransform`, the same) is the mesh's **control-point hull** placed in the
  document, not the contents' rectangle; the warp bounds only anchor the contents in mesh space.
- The contents map linearly onto the patch's (u, v); the surface is evaluated forward on a lattice (cells about two
  pixels across) and each output pixel found by inverting its cell's bilinear map; where the surface folds, the first
  cell wins. Contents much larger than their warped size are halved first so the bilinear samples do not alias.
- Moving the layer moves only the quad: the mesh bytes stay as they were. Replace or Edit Contents keeps the cage and
  draws the new contents through the same surface; the mesh is scaled onto the new contents' bounds so Photoshop
  reads the same shape.
- A mesh that leaves every point where it was is no warp (K.psd's ellipse), a mesh with distortion still on it, a
  quilt warp, or a style Patchy did not pin stays preview-locked.

`warpPsdPlacement` writes a mesh into a placement (Custom style, value 0, as Photoshop's own bakes are) for a future
Warp tool and the tests.

## Smart Filters

A stack (`SoLd` `filterFX`: `filterFXList` in the order the filters run, each with its blend options and settings)
is drawn when every entry is one of the thirteen filters Patchy calibrated against Photoshop 2026 (Gaussian Blur,
High Pass, Median, Dust & Scratches, Surface Blur, Unsharp Mask, Motion Blur, Plastic Wrap, Mosaic, Emboss, Box Blur,
Radial Blur's Spin, Add Noise), each within Photoshop's own dialog ranges, in a blend mode NekoPhoto has, the filter
mask not linked, and the instance has exactly one readable record in the document's `FEid` / `FXid` cache. The
record gives the filter canvas (the rect the blurs may grow into) and the shared filter mask (document space).
`src/core/src/smartfilter.cpp` reads and writes these; `smartfilter_render.cpp` is the port of Patchy's kernels.

Drawing: the contents placed on the quad (through the warp, if any) on the document's pixel grid, then each enabled
filter over the result so far with its opacity and blend (Normal at 100% replaces), then the mask between the
unfiltered and the filtered pixels.

Writing: while an instance and its contents are as they were read, its blocks and the cache go back byte for byte.
Moved, or with new contents, its cache record is written anew in Photoshop 2026's shape (Patchy's authoring shape:
record version 1, the unfiltered instance over the whole canvas as PackBits RGB and alpha, the mask as an explicit
plane), the other records untouched, and its placement patched. Plastic Wrap and Add Noise are Patchy's own
compatible renders rather than Photoshop's pixels; Photoshop redraws them from the settings.

Where Photoshop's own previews in Patchy's fixtures disagreed with Patchy's documented rules, the previews won:

- Median and Dust & Scratches see the canvas past the layer's edge (transparent), not the layer's edge repeated, so
  a rectangle filling its layer loses its corners (Median then Gaussian: 4.2 levels off, now 0.35).
- Unsharp Mask sharpens transparency too, its low-pass seeing the transparent canvas past the layer (a
  half-transparent band beside an opaque one comes out opaque: 2.8 levels off, now 0.34).
- Emboss works per channel: half the height either side along the angle, the side toward the light lit, the
  difference times the amount about middle grey (Patchy's grey relief was 46 levels off; now 0.29).

Every other drawn filter matches Photoshop's preview exactly or within half a level, except two Gaussian layers with
a five-tone filter mask, whose previews ignore the mask entirely (probably stale; a hard mask on the same stack
matches exactly), Radial Blur (2.3) and the two above.

## Warping and adding Smart Filters

**Edit ▸ Warp…** (and `layers.warp`) bends the active layer with one of Photoshop's fifteen presets, with bend and
horizontal and vertical distortion (`warpLayer` in `smartobject_edit.cpp`):

- a smart object has the preset baked into its placement as a Custom mesh over its contents (value 0, the mesh's
  hull as the quad, as Photoshop's own bakes are) and is drawn again from them, so it stays a smart object;
- text gets Warp Text (see psd-roundtrip.md), "None" removing it;
- pixels are bent over their own rectangle, for good.

A smart object already warped or filtered is refused (rasterize it to warp it again).

**Smart Filters** (`addSmartFilter` in `smartfilter.cpp`, `smartObject.addFilter`): Filter ▸ Gaussian Blur, Motion
Blur and Add Noise on a smart object add a Smart Filter, as in Photoshop, instead of asking to rasterize;
automation adds any of the thirteen, with opacity and blend. The placement gets its `filterFX` in Photoshop 2026's
shape (Patchy's authoring: `filterFXStyle`, each entry with its name, blend options, colours, `Fltr` and
`filterID`, before the trailing `comp`); the document's `FEid` block gets the instance's record (a new block when
the file had none), the unfiltered contents over the canvas and the mask kept or all white; the layer is drawn
through the stack. A stack with a filter not drawn here cannot be added to. Adding goes through `setSmartFilters`
(below).

## Editing Smart Filters

The Layers panel shows a smart object's stack under it, as Photoshop does: a **Smart Filters** row with the shared
filter mask's thumbnail and an eye that turns the whole stack off, then each entry (last applied at the top) with its
own eye, its name and a Blending Options button (opacity and mode). Double-click an entry to change its settings (a
dialog per filter over all thirteen, with the canvas previewing the change); the context menu edits, disables, moves
up or down, deletes one, or clears them all (the layer's own menu has Clear Smart Filters too). An entry NekoPhoto
does not draw is shown greyed with a tooltip, and its stack is read-only, since `filterFX` could not be written
back without it; so is a preview-locked instance's.

The filter mask: click its thumbnail to paint it (brushes, fills, gradients, filters and Invert act on it as on a
layer mask), Alt-click to show it on the canvas, Shift-click to turn it off or on; its menu enables, inverts or
deletes it (all white). Painting it works the way Quick Mask does: a temporary top layer, hidden from the panel, holds
the mask as its layer mask, active with the mask selected; each edit writes the mask into the stack in the same undo
step. Selecting a layer, saving or exporting takes the temporary layer away.

Each change is one undo step through `setSmartFilters` (`smartfilter.cpp`): the stack is written into the
placement's `filterFX` and the instance's `FEid` record anew (the mask over the document), and the instance drawn
again; an empty stack removes `filterFX` and the record (a block left with none goes). Settings are held to the
ranges a Photoshop file may carry. `smartFilterStackOf` reads a stack with its mask. Automation: `smartObject.filters`,
`smartObject.setFilter`, `smartObject.moveFilter`, `smartObject.removeFilter`, `smartObject.filterMask`
(docs/automation.md). `tests/smartfilter_tests.cpp` covers reordering, editing, the mask and clearing through PSD.

## Not yet

Dragging Smart Filters to reorder them (the context menu moves them), editing a stack with a filter not drawn
here, a linked filter mask, relinking linked files.

## Warp Cage

Edit ▸ Warp Cage shows a 4 x 4 Bezier mesh over the active layer (`layerWarpCage` in `smartobject_edit.h`): flat over
a pixel layer's placed rectangle, or a smart object's own warp carried from contents space onto its placement quad.
Dragging its points previews through a reduced copy (`previewWarpCage`); Enter applies (`warpLayerToCage`), Esc
cancels. A smart object's cage is written into its placement as Photoshop's Custom warp: the mesh scaled into
contents space, its hull placed on the cage's own bounding box (an axis-aligned quad, so the control points carry
exactly), and the instance is drawn again from its contents; opening the cage again gives back the same points,
through PSD too. Pixels are bent for good. A vector shape layer bends as a path (its anchors and handles carried
through the mesh) and previews exactly. Automation: `layers.cage`, `layers.setCage`.
