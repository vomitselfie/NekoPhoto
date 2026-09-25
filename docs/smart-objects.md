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

A **preview-locked** instance shows the preview the file carried, because NekoPhoto cannot yet redraw it itself:
warped, placed in perspective or skewed, carrying Smart Filters, a source it cannot read (a vector `.ai`, a file
type the app cannot decode) or a linked file, or only Photoshop's old `PlLd` form. It can still be moved, scaled
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
`placed` id (Photoshop aliases layers that share one). A Smart Filter instance that moved, or a copy of one, is
written as pixels with a warning: its document-space filter cache would no longer match. The sources go back in
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

Contents that NekoPhoto cannot redraw in some instance (a preview-locked one) cannot be edited or replaced; the
message says which layer and why. New sources are written into the PSD's `lnk2` as version-7 `liFD` elements
beside the file's own untouched ones, and new placements as Photoshop 2026's `SoLd` (Patchy's authoring shape;
Photoshop reads SoLd-only files).

## Not yet

Warps, Smart Filters, relinking linked files, PSB writing (converted contents are PSD, so 30,000 pixels a side),
and a prompt before painting turns a smart object into pixels.
