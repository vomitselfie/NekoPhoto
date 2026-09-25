# Vector masks and shapes

A PSD layer's vector mask ('vmsk' / 'vsms': a shape layer's shape, or a path mask on any layer) is drawn from its
path, and follows the layer: moving, scaling or rotating the layer moves the path, on screen and in the exported
PSD (the path records are rewritten; the rest of the block stays as it was). `src/core/src/vectormask.cpp`; the
record layout and combine rules follow Patchy (MIT, `src/third_party/patchy_psd/README.md`).

- **Paths**: 26-byte records, knots as (in, anchor, out) of y, x in 8.24 fixed point of the canvas. Subpaths of one
  shape group fill even-odd together; groups combine in order (add, subtract, intersect, exclude, with Patchy's
  coverage formulas; a first subtract starts from a full canvas). Antialiased: four sample rows per pixel, exact
  horizontal coverage, at any view scale. Inverted and disabled masks are honoured.
- **Shapes**: a solid shape is its fill (the layer's full-canvas pixels) cut by the path. Gradient and pattern fill
  layers ('GdFl', 'PtFl'), which store no pixels, draw their fill: the gradient over the shape's bounds with
  Photoshop's fill-layer geometry (the centre chord, unsnapped, Classic easing even on two stops; Patchy's
  calibration), the pattern from the file's patterns.
- **Strokes** ('vstk'): a band along the path, inside, centred or outside, in the stroke's colour and opacity over
  the fill, in the layer's mode; with the fill switched off, the stroke alone. It is an outline like Patchy's stroker:
  a quad per segment, mitred (within the miter limit), round or bevelled joins, butt, round or square caps, and
  dashes (lengths and offset in stroke widths), filled together under the nonzero rule; inside and outside strokes
  are a centred band of twice the width kept to one side.
- **Mask parameters** (the mask section's parameter form): vector density and feather apply to the path's coverage
  (feather a gaussian of sigma = feather, not clamped at the canvas, the stroke feathered too); the pixel mask's
  density and feather likewise, its feather clamped at the canvas edge.
- **Photoshop's derived plane** (a mask plane "rendered from other data", flag bit 3): the path baked unfeathered.
  It is not imported as a pixel mask (that masked twice); the section goes back to Photoshop as it was.

## How close

Against Photoshop's renders of Patchy's fixtures (mean difference per pixel, 0-255): shape booleans 0.00, first
combine ops 0.00, both masks 0.05 (and with parameters 0.05), vector mask on a pixel layer 0.05, live rectangle
0.09, pattern shape 0.11, solid shape 0.14, vector mask feather 0.59, gradient shape 1.27, strokes 0.25, pixel mask
parameters 4.58, shape feather 5.38. Before, these ran from 17 to 156.

## Not yet

Editing paths (a pen tool), gradient and pattern strokes, the last of shape feather (5.4: Photoshop feathers the
shape as one render), live-shape origination ('vogk', left out once a shape moves).
