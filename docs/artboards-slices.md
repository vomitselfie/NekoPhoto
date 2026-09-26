# Artboards and slices

## Artboards

An artboard is a folder with a rectangle and a background, as in Photoshop. Its background fills the rectangle
under its layers, and its layers are clipped to the rectangle; what lies outside every artboard shows the canvas.

- **Artboard tool** (Shift+V, next to Move): drag on the canvas to add an artboard (it goes to the top of the
  layer stack; move layers into it in the Layers panel). Drag inside an artboard to move it together with its
  layers, drag an edge or a corner to resize it (its layers stay put). Ctrl turns snapping off. The options bar
  sets the active artboard's background: White, Black, Transparent or Other (a colour).
- **File > Export Artboards to Files**: every visible artboard as its own PNG or JPEG (over white), named after the
  artboard with an optional prefix, into a folder you choose.
- **Undo**: adding, moving, resizing and recolouring are one step each.

### In PSD

Photoshop keeps an artboard as a folder whose opening record carries an `artb` tagged block (older files `artd`,
some `abdd`): a version-16 descriptor of class `artboard` with `artboardRect` (`classFloatRect`: `Top `, `Left`,
`Btom`, `Rght`), `guideIndeces`, `artboardPresetName`, `Clr ` (an `RGBC` colour) and `artboardBackgroundType`
(1 white, 2 black, 3 transparent, 4 other). NekoPhoto reads any of the three keys into the folder's artboard. On
export the file's own block goes back byte for byte while it still describes the artboard; once the artboard was
moved, resized or recoloured, a new `artb` block is written in Photoshop's layout. Descriptors are read and written
with Patchy's `psd_descriptor` (MIT, Seth A. Robinson; see THIRD-PARTY-NOTICES.md).

### In the project

The folder's manifest record has an `artboard` object: `x`, `y`, `width`, `height`, `background` (1-4 as above),
`red`, `green`, `blue` (0..1) and `preset`. A project with artboards is written as format version 8, which
Compositor for macOS does not open (it has no artboards); without them the Mac's version 7 stays.

## Slices

Slices are named rectangles for export (Photoshop's Save for Web slices), kept on the document.

- **Slice tool** (Shift+C, next to Crop): drag to add a slice, drag inside one to move it, an edge or corner to
  resize it. The slices and their numbers show while the tool is chosen; the options bar can delete them all.
- **File > Export Slices**: each slice as its own PNG or JPEG, named after the slice.

### In PSD

Image resource 1050. NekoPhoto reads version 6 (binary records) and versions 7 and 8 (a descriptor with a `slices`
list); the user and layer slices become the document's slices (Photoshop's automatic ones, which fill the rest of
the canvas, follow from them and are left out). On export the file's own resource goes back unchanged while the
slices are the ones it holds and the canvas has kept its size; otherwise a version 6 resource is written: the
automatic whole-canvas slice, then each slice as a user slice with its name, rectangle, URL, target, message and
alt text.

### In the project

The manifest's `slices` array: `id`, `name`, `x`, `y`, `width`, `height`, `url`, `target`, `message`, `altTag`.
Slices also make the project version 8.

## Automation

`artboards.list`, `artboards.add`, `artboards.set`, `artboards.delete`, `artboards.export`, `slices.list`,
`slices.add`, `slices.set`, `slices.delete`, `slices.export` (see automation.md), with MCP tools of the same names.

## Not verified

No Photoshop was at hand: the written `artb` block and version 6 slices follow the published format and the
layouts psd-tools and ag-psd read, and our reader takes them back, but opening the files in Photoshop has not been
tried. No Photoshop-saved file with artboards or slices was in the test corpus.
