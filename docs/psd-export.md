# PSD export

File > Export as Photoshop Document writes a layered `.psd`; so does `document.export` with a `.psd` path
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
- Levels, Curves and Exposure adjustment layers, and Hue/Saturation when it uses Photoshop's saturation
  curve (the default for Hue/Saturation layers opened from a PSD).
- Empty layers and empty folders, so the structure survives.
- The merged image, from NekoPhoto's own renderer, matted against white where transparent as Photoshop
  stores it; the resolution.

Colour is written straight (not premultiplied), so soft edges keep their colour.

## What is written the way it looks

Each is listed before you export (the export dialog) or in the reply (`warnings`, `notes`):

- **Scaled, rotated or flipped layers** are resampled into place (a note; the look is the same).
- **Shape layers** are written as pixels; they stay editable in the NekoPhoto project (a note). **Text layers** are
  written as Photoshop type layers over the same pixels (see [psd-roundtrip.md](psd-roundtrip.md#text)); flipped text
  is written as pixels (a note).
- **Adjustments Photoshop has no equivalent for** (Grain; Gradient Map, whose ramp between its ends is
  NekoPhoto's own; Hue/Saturation with the plain scale) become a pixel layer holding the adjusted look of
  everything beneath, in the adjustment's place. The layers beneath stay in the file (a warning).
- **A layer clipped to one that is not right beneath it** is written unclipped, as it shows (a warning).
- **Folder opacity and folder blend modes** are written as set, but NekoPhoto does not apply them while
  Photoshop does, so such a folder looks different there (a warning).

## Limits

- PSD only; documents over 30,000 pixels a side need PSB, which is not written yet.
- 8 bits per channel, RGB.
- Not yet: editable Photoshop text, layer effects, smart objects, Gradient Map and other adjustments as
  Photoshop adjustment layers.

## How it is tested

- `tests/psd_writer_tests.cpp` builds documents (single layer, offsets and every blend mode, Unicode names,
  masks and clipping, nested folders with a folder mask and an empty folder, soft transparent edges,
  adjustment layers, the baked cases, the size limit), exports them, reads them back and compares the
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
