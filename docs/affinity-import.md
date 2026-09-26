# Affinity documents

File > Open reads Affinity documents: `.afphoto`, `.afdesign` and `.afpub` from Affinity 1 and 2, and the `.af`
files of the unified Affinity (3.x). Each opens in its own tab, like a PSD or a Clip Studio file, and a dialog
lists what could not be carried over. `document.open` does the same over the automation socket and answers
with `layers` and the `notes`. File > Place Embedded also accepts them, as the flattened document.

The reader is a port of Patchy's (MIT, `src/third_party/patchy_psd/README.md`): `src/core/src/affinity.cpp`
and `affinity_tree.{h,cpp}`, from Patchy's `src/formats/af_document_io.cpp` and `af_tree.{hpp,cpp}`. Patchy
worked the format out from documents authored in a licensed Affinity and the MIT-licensed afread project's
notes; the container walk, tile planes, blend table and placement rules are its findings, kept as it pinned
them. Most documents from Affinity 2 on compress their streams with zstd, so the build needs libzstd
(`libzstd-dev`, found through pkg-config; `-DCOMPOSITOR_WITH_ZSTD=OFF` leaves it out, and then such files are
refused with a message saying so).

## What is carried

| Affinity | Here |
|---|---|
| Pixel layers, 8 and 16 bits, gray, 32-bit float (linear, converted to sRGB) | pixel layers, 8 bits |
| CMYK bitmaps | through the bitmap's own ICC profile when it has one, else a plain ink formula (noted) |
| Lab bitmaps | through Little CMS's Lab profile to sRGB |
| Placed images (the original file kept in the document) | decoded (PNG in the core, JPEG and others through Qt) with their EXIF orientation; if the original cannot be decoded, Affinity's stored half-size reduction, noted |
| Scaled or rotated layers | resampled to an upright layer (bilinear, box-reduced first for strong reductions) |
| Groups | folders, pass-through unless the group has its own blend mode |
| Opacity, visibility, blend modes | the same; Average, Negation, Reflect, Glow and Pigment take Patchy's nearest match (noted); Contrast Negate becomes Normal (noted) |
| Fill opacity | multiplied into the layer's opacity (effects, where the two differ, are not carried) |
| Erase blend mode | a mask on a new isolated folder holding the layers beneath it, as in Patchy |
| Layer masks (raster) | layer masks, placed as Affinity places them |
| Vector masks (a shape or curve used as a mask) | drawn as a raster layer mask |
| Clipped layers (children of a pixel layer or shape) | clipped layers above their base |
| Vector curves and shapes (rectangles with every corner type, ellipses, polygons, stars, and the rest of Affinity's shape kinds Patchy reads) | drawn as pixel layers: solid fill, solid or dashed stroke with its alignment |
| Artboards | folders clipped to the artboard, the artboard's own fill as the bottom layer |
| Embedded Affinity documents | read and flattened into a pixel layer |
| Canvas size and resolution, the spread's background colour | the same; the background becomes a bottom layer |

## What is not

Listed in the notes, layer by layer:

- Text (artistic and frame text). The layer is left out.
- Adjustments and live filters, including those attached to a layer. Left out.
- Layer effects (shadows, glows, outlines, bevels, blur, gradient overlays). Left out.
- Gradient fills and strokes on shapes, circle-rounded stars, arrows with end styles other than plain.
- Pages and spreads after the first.
- Clipping a folder to a layer (NekoPhoto clips layers only): the folder shows unclipped.

When something visible was left out, the document gets one more layer on top, hidden: Affinity's own preview of
the whole document (at most 512 pixels, stretched over the canvas), to compare against. When nothing at all could
be read (a document that is only text, say), that preview, or an old document's saved full-size snapshot, is
imported as the one layer instead.

## How it was checked

`tests/affinity_tests.cpp` checks the stream predictors, the blend table and transform composition, and opens a
document written byte by byte in the test (container, stream table, doc.dat tree, tile streams). With
`COMPOSITOR_AFFINITY_FIXTURES` naming a folder of documents (Patchy's `test-fixtures/af`, 33 small files made in
Affinity 2.6 and 3.2), it opens each and prints how far the flattened import is from the preview Affinity
embedded, and requires the documents with nothing left out to come within 4/255 on average (they do: the Lab
document at 3.5, where the conversion differs slightly; the rest under 2, the vector, mask, artboard, group,
transform and shape files among them). `build/tests/affinity_check [--out DIR] file.af ...`
prints the layer tree and the notes, and with `--out` writes the import and the preview as PNGs side by side.
The core check tool has no JPEG decoder, so placed JPEG originals come in at half size there; the app decodes them.
