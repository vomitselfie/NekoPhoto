# Vector tools

Shape layers, paths, the Pen and Direct Selection, as Photoshop has them.

## Vector shape layers

A shape layer is a path filled with a colour, a gradient or a pattern and optionally stroked. It is stored the way
Photoshop stores a shape layer: the path as a `vsms` block, the stroke as `vstk`, the fill as `SoCo` (a gradient as
`GdFl`, a pattern as `PtFl`), its live properties as `vogk`, in the layer's PSD carry
(`src/core/include/compositor/vectorlayer.h`). The renderer draws it from those blocks (the same code that draws
shape layers opened from a PSD), PSD export writes them back so Photoshop opens a live shape layer, and the project
package keeps them. The layer's pixels are the fill colour over the path's bounds; the path cuts them.

- The Shape tool (U; Shift-U steps through the kinds) draws Rectangle (corner radius), Ellipse, Polygon (sides, and
  a star inset), Line (weight; Shift snaps to 45°) and Custom Shape (Heart, Star, Arrow, Speech Bubble, Check Mark,
  Lightning), filled with the foreground colour.
- Its options bar has Fill and Stroke (colour, width, Inside / Center / Outside, Solid / Dashed / Dotted). With a
  shape layer active they edit that layer, one undo step per change, as Photoshop's bar does.
- Moving, scaling or rotating a shape layer redraws it on its moved path (`refreshVectorShapes`, run after every
  edit), so it scales as a path. Painting on it makes it pixels cut by a vector mask (it no longer counts as a shape;
  PSD export then writes pixels and the vector mask).
- Fill and stroke paints: the bar's Color / Gradient / Pattern choices fill (or stroke) with a colour, a gradient preset
  (imported `.grd` presets, or foreground to background; linear at 90 degrees, as Photoshop starts one) or one of the
  document's patterns. The blocks are Patchy's authoring (`fill_content_object`, MIT): `GdFl` / `PtFl` for the fill, a
  `gradientLayer` / `patternLayer` content in the stroke. A pattern the document does not have falls back to the colour.
  A shape with a gradient or pattern goes into SVG exports as an image.
- Path operations: the Shape and Pen bars' menu picks what the next outline does: New Layer (a shape layer of its own),
  or Combine Shapes, Subtract Front Shape, Intersect Shape Areas, Exclude Overlapping Shapes, which add it to the active
  shape layer as a new component (shape group) combined that way with the ones before it. With Direct Selection the
  menu shows and changes the component of the subpath last clicked. Merge Shape Components (both bars) flattens the
  target path into add-only outlines: drawn at up to 8x and traced, so curves come back as corner points within about
  a tenth of a pixel.
- Live shape properties (Photoshop's `vogk`): a rectangle keeps its box and a radius per corner, an ellipse its box. The
  Shape bar's Properties fields (W, H, X, Y and the four radii) show and change the active shape's (the component
  Direct Selection picked, else the first), drawing its outline anew. They last while the component is still what they
  make: dragging one of its anchors, merging, rotating or scaling it unevenly ends them, as editing the path directly
  does in Photoshop; moving it or scaling it evenly carries them along. Lines and custom shapes are not live here.
- Automation: `shape.draw`, `shape.get`, `shape.set` (paints, `op`, `live`), `paths.setOperation`, `paths.mergeComponents`.

## Vector masks on layers

Any layer but a folder or an adjustment layer can have a vector mask of its own (`vmsk`, written into the carry as
Photoshop keeps it, and moved with its layer). Layer ▸ Vector Mask ▸ Reveal All (an empty path: everything shows),
Hide All (an empty path, inverted), Current Path (the path chosen in the Paths panel), Edit and Delete; the Layers
panel's context menu has the same. Its thumbnail sits beside the layer's (and its pixel mask's): a click makes it the
target of the Pen and Direct Selection, which then draw into and edit it (the path operation picks how a new outline
combines; drawn into an empty Reveal All mask, the outline becomes the mask). Choosing a path in the Paths panel lets
it go. Automation: `vectorMask.get`, `vectorMask.set`, `vectorMask.delete`, `vectorMask.target`.

## Text to paths

Type ▸ Create Work Path makes the Work Path from the active text layer's glyph outlines (each glyph its own component,
so overlapping letters add and a letter's counters stay holes); Type ▸ Convert to Shape turns the text layer into a
shape layer of those outlines in the text's colour, keeping its name, opacity, blending and style. The outlines are
the text as laid out upright (Qt's shaping of the same layout the layer draws); warped text is refused (set its warp
to None first), and underline and strikethrough are not outlined. Automation: `text.toPath`, `text.toShape`.

## Paths

The document's paths are Photoshop's: the Work Path (image resource 1025) and saved paths (2000 and up, named),
kept in the document's PSD carry, so they go into PSD files and projects as Photoshop keeps them.

- The Paths panel (tabbed with Layers) lists them. Double-click the Work Path to save it, a saved path to rename
  it. Its buttons and context menu fill a path (foreground colour, the brush's opacity), stroke it (the brush's size;
  Photoshop strokes with a painting tool's own tip, here a round solid stroke), load it as a selection, make a shape
  layer from it, make the Work Path from the selection (its traced outline, simplified), and delete it.
- The Pen (P): click for corner points, drag for smooth ones; click the first point to close, Enter leaves the path
  open, Esc cancels. In Shape mode the result is a new shape layer (or, with a path operation other than New Layer, a
  new component of the active shape layer); in Path mode it goes into the chosen path, or a new Work Path, as a
  component combined by the path operation.
- Auto Add/Delete (the Pen's option, on by default, as in Photoshop): with no path being drawn, a click on the target
  path's outline adds an anchor there (the curve split so its shape stays) and it can be dragged at once; a click on an
  anchor deletes it. Automation: `paths.addAnchor`, `paths.deleteAnchor`.
- Direct Selection (A) edits the target path: the path chosen in the Paths panel, else the active layer's vector mask
  when its thumbnail was clicked, else the active shape layer's.
  Drag a point, a handle (a smooth point's other handle turns with it; Alt moves just the one), or a whole subpath
  (Shift constrains to an axis); Alt-click a point to turn it from smooth to corner or back; Delete removes the
  chosen point. A drag is one undo step.
- Automation: `paths.list`, `paths.set`, `paths.select`, `paths.delete`, `paths.fill`, `paths.stroke`,
  `paths.toSelection`, `paths.toShape`, `paths.fromSelection`.

## Checks

`tests/vectorlayer_tests.cpp` writes paths, strokes and fills and reads them back through the parsers the renderer
and the PSD round trip use, draws a shape layer, moves and scales one, and keeps paths as resources. A document of
every shape kind exported to PSD and opened again gives the same shapes (`build/tests/psd_roundtrip` on it: every
carried block back). Photoshop's own reading of these files is not checked here (no Photoshop); the block layouts
are Patchy's, which were pinned against Photoshop 2026.

`tests/vectorlayer_tests.cpp` also combines components by each operation and merges them, round-trips gradient and
pattern fills and strokes and live shape properties through a PSD, gives a pixel layer a vector mask and moves it,
and checks that an empty vector mask reveals all (and inverted hides all). `tools/rpc_smoke.py` drives each through
automation, including text to a path and a shape.

Shapes, paths and vector masks work the same in CMYK and Lab documents at 8 and 16 bits: a shape's fill is in the
document's channels (its colour through the profile, a CMYK file's ink colour as its inks), it is written to PSD as a
shape layer, and it stays one through Image ▸ Mode (color-modes.md, "Text, shapes and layer styles").

## Not yet

Custom shapes from .csh files; live properties of lines and polygons (Photoshop 2026's `vogk` line entries are read
past, not kept editable); a live shape turned by a transform (its `Trnf`) is read as not live; Merge Shape Components
keeps no curves; text on a path, and warped text to a path.
