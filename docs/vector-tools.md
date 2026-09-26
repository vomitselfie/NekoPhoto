# Vector tools

Shape layers, paths, the Pen and Direct Selection, as Photoshop has them.

## Vector shape layers

A shape layer is a path filled with a colour and optionally stroked. It is stored the way Photoshop stores a shape
layer: the path as a `vsms` block, the stroke as `vstk`, the fill as `SoCo`, in the layer's PSD carry
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
- Automation: `shape.draw`, `shape.get`, `shape.set`.

## Paths

The document's paths are Photoshop's: the Work Path (image resource 1025) and saved paths (2000 and up, named),
kept in the document's PSD carry, so they go into PSD files and projects as Photoshop keeps them.

- The Paths panel (tabbed with Layers) lists them. Double-click the Work Path to save it, a saved path to rename
  it. Its buttons and context menu fill a path (foreground colour, the brush's opacity), stroke it (the brush's size;
  Photoshop strokes with a painting tool's own tip, here a round solid stroke), load it as a selection, make a shape
  layer from it, make the Work Path from the selection (its traced outline, simplified), and delete it.
- The Pen (P): click for corner points, drag for smooth ones; click the first point to close, Enter leaves the path
  open, Esc cancels. In Shape mode the result is a new shape layer (or, with "Add to active shape", a new subpath of
  the active shape layer); in Path mode it goes into the chosen path, or a new Work Path.
- Auto Add/Delete (the Pen's option, on by default, as in Photoshop): with no path being drawn, a click on the target
  path's outline adds an anchor there (the curve split so its shape stays) and it can be dragged at once; a click on an
  anchor deletes it. Automation: `paths.addAnchor`, `paths.deleteAnchor`.
- Direct Selection (A) edits the target path: the path chosen in the Paths panel, or else the active shape layer's.
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

## Not yet

Path operations between subpaths other than add in the tools (the blocks carry them), gradient and pattern fills
for shapes, custom shapes from .csh files, live shape properties (Photoshop's `vogk`: a rectangle stays a rectangle
with editable corner radii), converting text to a path.
