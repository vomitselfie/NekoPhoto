# PSD export

File > Export > Export as Photoshop Document writes a layered `.psd`; so does `document.export` with a `.psd` path
over automation. The writer is `src/core/src/psd_writer.cpp`, the reader it is tested against
`src/core/src/psd.cpp`.

## What is carried

- Pixel layers with their names (Unicode, in `luni`, beside an ASCII legacy name), positions (also past the
  canvas edge), visibility, opacity and blend modes. All thirteen of NekoPhoto's blend modes have Photoshop
  keys.
- Folders, nested, as Photoshop's section records; folders are written pass-through, which is how NekoPhoto
  draws them (a folder opened from a PSD keeps its own mode).
- For a document opened from a PSD, what NekoPhoto does not model: layer styles, editable text, smart
  objects, vector masks, Fill, Blend If, the file's resources, while each is still true of its layer
  ([psd-roundtrip.md](psd-roundtrip.md)).
- Layer masks and folder masks, including disabled ones.
- Clipping, where the clipped layers sit directly above their base, as PSD requires.
- Levels, Curves and Exposure adjustment layers, and Hue/Saturation: master and the six colour ranges (reds,
  yellows, greens, cyans, blues, magentas), each with its four range points (the begin and end falloffs, linear as in
  Photoshop; untouched ranges keep Photoshop's defaults), and Colorize. Saturation is exact on Photoshop's
  saturation curve (the default for Hue/Saturation layers opened from a PSD); a layer on the plain scale is written
  live as long as no range moves its saturation, since hue and lightness draw the same on either.
- Empty layers and empty folders, so the structure survives.
- The merged image, from NekoPhoto's own renderer, matted against white where transparent as Photoshop
  stores it; the resolution.

Colour is written straight (not premultiplied), so soft edges keep their colour.

## What is written the way it looks

Each is listed before you export (the export dialog) or in the reply (`warnings`, `notes`):

- **Scaled, rotated or flipped layers** are resampled into place (a note; the look is the same).
- **Shape layers** are written as Photoshop live shape layers (see [vector-tools.md](vector-tools.md)). **Text layers** are
  written as Photoshop type layers over the same pixels (see [psd-roundtrip.md](psd-roundtrip.md#text)); flipped text
  is written as pixels (a note).
- **Adjustments Photoshop has no equivalent for** (Grain; a Gradient Map made or edited in NekoPhoto, whose ramp
  between its ends is NekoPhoto's own (one read from a PSD and left alone goes back as it was); Hue/Saturation that moves saturation on the plain scale, or with Invert Range on) become a pixel layer holding the adjusted look of
  everything beneath, in the adjustment's place. The layers beneath stay in the file (a warning).
- **A layer clipped to one that is not right beneath it** is written unclipped, as it shows (a warning).
- **Folder opacity and folder blend modes** are written as set, but NekoPhoto does not apply them while
  Photoshop does, so such a folder looks different there (a warning).

## Limits

- PSD, or PSB (Photoshop's large format: export to a `.psb` path). Every merged image is written with even-length
  compressed rows, which Photoshop requires of a smart object's embedded file when the document keeps Smart Filter
  caches.
- 8 bits per channel, RGB.
- Text, layer styles ([layer-styles.md](layer-styles.md)), shape layers and smart objects
  ([smart-objects.md](smart-objects.md)) are written as Photoshop's own; adjustments Photoshop has no equivalent for
  are baked as described above.

## How it is tested

- `tests/psd_writer_tests.cpp` builds documents (single layer, offsets and every blend mode, Unicode names,
  masks and clipping, nested folders with a folder mask and an empty folder, soft transparent edges,
  adjustment layers, Hue/Saturation by range (also from a 16-bit document), the baked cases, the size limit), exports them, reads them back and compares the
  structure and the render, which must agree to within one level (two where Levels' gamma is rounded to
  hundredths). Writing premultiplied colour fails four of its checks.
- Every layered PSD at hand, 13 files from Photoshop and Clip Studio (up to 54 layers in 16 folders at
  4096 × 4096, one with 24 clipped layers), exports, reopens in a fresh process and renders with a maximum
  difference of zero. A 54-layer 4096 × 4096 file exports in 0.34 s, at the size Photoshop's own file has.
- An independent reader (psd-tools) opens every export with its layers, folders, masks, clipping and blend
  modes; its own compositing of the layers agrees with our merged image as closely as it agrees with
  Photoshop's merged image in the original files.

Still to do by hand: open the exports in Photoshop itself (and Photopea and Krita) and compare, clipping
over soft-edged bases in particular, where psd-tools' compositing is approximate and so cannot settle it.
