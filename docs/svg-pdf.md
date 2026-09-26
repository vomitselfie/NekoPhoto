# SVG and PDF

NekoPhoto opens SVG files as layered documents of editable vector shape layers, writes documents out as SVG,
and opens a PDF page as pixels. The SVG reader and writer are ported from Patchy (MIT,
`src/third_party/patchy_psd/README.md`): `formats/svg_xml`, `svg_document_read`, `svg_document_write`,
`vector_fill_rule` and `vector_export_plan`, adapted to the shape model in `docs/vector-tools.md`.

## Opening SVG

File > Open (or `document.open`) on a `.svg` or `.svgz` opens it in its own tab. The core
(`src/core/src/svg.cpp`, Qt-free) reads the XML (`svg_xml.cpp`: entities, CDATA, namespaces, UTF-16 and
Latin-1), then walks the tree in paint order, bottom to top:

- The canvas is the root's `width` and `height` (CSS pixels; `in`, `cm`, `mm`, `pt` and `pc` at 96 to the
  inch, which also sets the document to 96 ppi), else the `viewBox` size, else 300 x 150. `viewBox` with
  `preserveAspectRatio` (meet, slice, none, the nine alignments) maps onto it. Canvases past 30,000 pixels a
  side or 100 megapixels are scaled down, with a note.
- `path`, `rect` (with `rx`/`ry` corners, elliptical ones as arcs), `circle`, `ellipse`, `line`,
  `polyline` and `polygon` with solid paint become one vector shape layer each (`setVectorShape`): the
  path in document pixels with every transform applied, the fill colour, and the stroke's colour, width
  (scaled by the transform's area scale), caps, joins, miter limit and dashes. Path data takes the whole
  grammar (M L H V C S Q T A Z, relative forms, implicit repeats); quadratics become cubics and arcs become
  cubics of 90 degrees or less. The layer's name is the element's `id`, else its `<title>`, else
  "Rectangle 1", "Shape 2" and so on.
- Colours: hex (3, 4, 6, 8 digits), `rgb()`/`rgba()`, `hsl()`/`hsla()`, the CSS named colours,
  `currentColor`, `transparent`. Style comes from presentation attributes, `<style>` rules (type, `.class`
  and `#id` selectors) and `style=""`, in CSS order.
- Fill rules: `evenodd` puts every subpath in one shape group, which the model fills even-odd. `nonzero`
  gives each subpath its own group, largest first, adding when the winding just inside it is non-zero and
  subtracting otherwise, so holes wound against their outline and islands inside holes come out right.
  Outlines that cross one another are unioned (exact non-zero for crossing outlines is not in the model).
- Opacity: `opacity` times `fill-opacity` times the colour's alpha is the layer's opacity; the stroke's own
  opacity is divided by that, so a stroke more opaque than its fill is held to the fill's (noted).
- `g` and nested `svg` become folders with their opacity, visibility and `mix-blend-mode`; a folder with
  opacity or a blend mode is isolated (SVG isolates it), a plain one passes through. `a` and `switch` are
  transparent; `use` is expanded in place (nesting, cycles and total expansions are bounded).
- `display:none` and `visibility:hidden` shapes are kept as hidden layers.

What the shape model cannot hold is drawn as pixels, element by element, keeping its place in the stack:
gradient and pattern paint, `text`, `image`, anything with `filter`, `clip-path`, `mask` or markers, and
unknown drawable elements. The core hands each run of such neighbours to the app as a standalone SVG (the
file's definitions and stylesheets, the element under a wrapper carrying its transform and inherited
style); `src/app/VectorFiles.cpp` renders it with Qt SVG at the document's size and crops it into a pixel
layer. Qt 6.4 draws no filters, masks or clip paths (6.7 and later draw some); an element that draws
nothing is left out with a note. The notes after opening list what became pixels. A file the reader
refuses (not SVG, broken XML, more than 4,000 drawable elements) is drawn whole as one pixel layer.

A 2-point line imports as an open stroked path, not Photoshop's filled Line quad. The `data-nekophoto-stroke-*`
(and Patchy's `data-patchy-stroke-*`) hints restore inside and outside strokes from our own exports.

## Exporting SVG

File > Export SVG (or `document.export` to a `.svg`) writes `src/core/src/svg_write.cpp`'s output:

- Vector shape layers become `<path>` elements (`fill-rule="evenodd"`) with the fill colour and the stroke
  attributes; the layer's opacity and blend mode become CSS `opacity` and `mix-blend-mode`. A centre
  stroke maps directly. An inside stroke is written at twice its width, clipped to the shape's own outline;
  an outside one at twice its width under an opaque fill (`paint-order="stroke"`), or, with no fill,
  masked off the shape's inside. Both carry `data-nekophoto-stroke-align` and `-width` hints so NekoPhoto
  reads the true stroke back.
- Combined paths: one group, or holes inside separate outlines, are one even-odd path; overlapping added
  outlines are one path each under a `<g>` with the stroke drawn once over them all. Intersect and exclude
  combinations, inverted paths, layer styles, pixel masks and clipped layers make the layer an image.
- Folders become `<g>` with opacity, blend mode, `isolation:isolate` unless pass-through, a pixel mask as a
  luminance `<mask>` and a vector mask as a `<clipPath>`; a folder with a layer style is an image.
- Every other visible layer (pixels, text, smart objects, fill layers, styled or masked shapes) is rendered
  by the compositor on its own and embedded as a cropped PNG `<image>`; clipping runs are one image.
- Adjustment layers and blend modes CSS lacks (Dissolve, Linear Burn, Vivid Light and the rest) need what
  is below them: everything in that folder up to the last such layer is merged into one image.
- Hidden layers are left out. The reply (and the status bar) counts shapes, images and groups; `notes`
  says what became an image.

## Opening PDF

PDF pages are rendered by Qt PDF (`QPdfDocument`, PDFium), an optional build dependency
(`-DCOMPOSITOR_WITH_QTPDF`, found as its own CMake package so a missing module never fails the Qt lookup;
Arch `qt6-webengine`, Ubuntu `qt6-pdf-dev`, Homebrew `qt`, and the release builds' aqt module `qtpdf`).
Without it `.pdf` is not offered and opening one says why; `app.info` reports `pdf`.

A page opens as one pixel layer named "Page N" on a transparent canvas the page's size at 150 pixels per
inch (`document.open` takes `page`, 1-based, and `resolution`, 18 to 1200; a multi-page file opened from
the window asks which page). Pages past the canvas limits render at a lower resolution, with a note.
Annotations are drawn; password-protected files are refused.

Patchy's editable PDF reader (its own content-stream interpreter, fonts, shadings and image decoders,
about 8,000 lines) was not ported: PDF text and vector art arrive as pixels. Porting it onto the vector
shape model is the path to editable PDF import.

## Tests

`tests/svg_tests.cpp` covers the path grammar, shapes with colours, strokes, dashes and opacity, transforms
with a `viewBox`, both fill rules (holes and islands), CSS classes and `currentColor`, raster parts, and an
export read back and drawn the same. `tools/rpc_smoke.py` exports the demo as SVG, reopens it, and opens a
hand-written two-page PDF at page 2 when the build has Qt PDF.
