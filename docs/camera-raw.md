# Camera Raw Filter

Filter > Camera Raw Filter… (Shift+Ctrl+A) grades the active layer's pixels, inside the selection, as one
undo step. It is a port of upstream Compositor's Camera Raw Filter (MIT, `LICENSES/MIT-Compositor.txt`). It is
destructive: on a smart object it asks first, like the other pixel filters, and there is no Smart Filter version.

| Part | Upstream | Here |
|---|---|---|
| Settings, ranges, normalization, apply order | `Document/CameraRaw*.swift` | `src/core/include/compositor/cameraraw.h`, `src/core/src/cameraraw.cpp` |
| Pixel kernels (`adjust_camera_raw` and every `adjust_camera_raw_*`) | `Rendering/AdjustPixels.c` | `src/pixels/CameraRawPixels.c`, extracted unchanged apart from warning fixes |
| Panels | `UI/CameraRaw*.swift`, `UI/RawDevelopSheet.swift` | `src/app/CameraRawDialog.cpp` |
| Tests | `CompositorTests/CameraRawTests.swift` | `tests/cameraraw_tests.cpp` |
| Automation | none | `pixels.cameraRaw`, MCP `pixels_camera_raw` |

The grade runs in upstream's order: geometry, calibration, white balance and light, curve, colour mixer and
grading, effects and grain, optics, then detail. Alpha is kept. A reduced preview scales every radius, so it
matches the full-size result.

## Panels

- **Basic**: White Balance (Custom, or Auto, the gray-world balance of the layer), Temperature and Tint
  (relative, −100…100, as Photoshop's Camera Raw Filter has them for pixels; a RAW file has kelvin, see
  [White balance](#white-balance)), Exposure in stops, Contrast, Highlights, Shadows, Whites, Blacks, Texture, Clarity,
  Dehaze, Vibrance (protects skin tones) and Saturation. Two check boxes paint clipped shadows blue and
  clipped highlights red in the preview only.
- **Curve**: the parametric curve (Highlights, Lights, Darks, Shadows and the three splits), an RGB point
  curve preset (Linear, Medium or Strong Contrast) and Refine Saturation.
- **Detail**: sharpening (Amount, Radius, Detail, Masking, with a preview of the mask), luminance and colour
  noise reduction.
- **Color**: the colour mixer (hue, saturation and luminance of eight colour families) and colour grading
  (shadow, midtone, highlight and global wheels as hue, saturation and luminance, with Blending and Balance).
- **Optics**: chromatic aberration, generic profile corrections, distortion, lens vignetting, and purple and
  green defringe with their hue ranges.
- **Geometry**: Perspective or Rectilinear projection, Vertical, Horizontal, Rotate, Aspect, Scale, Offset X
  and Y, and Constrain Crop.
- **Effects**: Glow (Diffusion, Bloom, Halation), post-crop Vignette (three styles) and Grain.
- **Calibration**: process version 1–6 (older versions apply the sliders more weakly), shadow tint and the
  red, green and blue primaries.

The grade from the last OK comes back on the next open. Reset All returns every panel to its defaults.

## Opening camera RAW files

File ▸ Open (or a drop, or a double-click in the file manager) of a camera RAW file (CR2, CR3, NEF, ARW, RAF, ORF,
RW2, DNG and the rest LibRaw reads) opens it in the Camera Raw dialog, as Photoshop does, never as a layer of the
open document.

- The dialog appears at once: a half-size decode (one pixel per Bayer quad) runs off the UI thread and fills the
  preview when it is ready, reduced to 1,600 pixels on the long side. The panels are the filter's, with White
  Balance as Camera Raw shows it for a RAW file: Temperature in kelvin and Tint (see [White balance](#white-balance)).
- **Depth** is Camera Raw's workflow option: 16 Bits/Channel (the default) or 8. The choice is remembered for the next
  file, as Photoshop remembers its workflow options. The colour space is sRGB, which is what LibRaw develops into.
- **Open** develops the whole file (off the UI thread) and opens it as a new document with one layer.
- **Open Object** (Photoshop's Shift+Open) opens a new document whose one layer is a smart object. Its source is the
  RAW file itself, byte for byte, with the settings; the contents are the developed image at 16 bits.
  Double-clicking the smart object's badge (Layer ▸ Smart Objects ▸ Edit Contents) reopens the RAW file in Camera
  Raw with those settings; OK develops it again and every layer placing it updates, as one undo step.
- **Cancel** (or Escape) stops a decode that is running and opens nothing.

The develop is `compositor/raw.h`: LibRaw demosaics with the white balance's multipliers (the camera's own for As
Shot) into sRGB at 16 bits, then `applyCameraRaw` grades it with the settings, the same code as the filter (so the dialog's result equals Open with
no settings followed by Filter ▸ Camera Raw Filter with them, which `raw_tests` checks).

Where it is kept:

- In a project, the smart object's source record (`smartobjects/<n>.source`, record version 3) holds the RAW file's
  bytes and the settings as JSON (`CameraRawSettings::toJson`); `<n>.png` holds the developed contents at 16 bits.
  A project without RAW sources still writes version 2.
- In a PSD, the RAW file goes into the document's linked-file block as the smart object's embedded file (`liFD`),
  and the layer's pixels are the developed image, so the PSD looks right everywhere. NekoPhoto's develop settings
  are not written in Photoshop's form (Photoshop keeps its own Camera Raw settings, which this engine does not
  reproduce), so Photoshop develops the embedded file with its own defaults when its contents are edited there.
  Opened in NekoPhoto again, such a source shows the pixels the file carries and cannot be developed again: use the
  project to keep the settings.

Automation opens a RAW file without the dialog: `document.open` develops it as shot, or with `settings` (the same
object as `pixels.cameraRaw`, with white balance as below), at `bitsPerChannel` 16 or 8, and `asSmartObject: true` makes the Open Object smart
object. `smartObject.editContents` on such a smart object develops it again with `settings` (or its own). A run
without a window on screen (`--headless`) does the same.

`tools/make_test_dng.py` writes the synthetic DNGs in `tests/fixtures/raw` (a grey ramp and six patches under a
coloured light, with the as-shot neutral that corrects it; `synthetic-dual.dng` carries a second colour matrix for
Standard light A) that the tests and `rpc_smoke` open.

## White balance

For a RAW file White Balance reads as Camera Raw's does: **As Shot**, **Auto**, the presets the file records
(**Daylight**, **Cloudy**, **Shade**, **Tungsten**, **Fluorescent**, **Flash**; only those the camera wrote into the
file, none made up), and **Custom**, with **Temperature** in kelvin (2000–50000 K, the slider even in reciprocal
temperature) and **Tint** (−150…150, positive toward magenta). Moving either slider makes it Custom.

- **As Shot** develops with the camera's own multipliers exactly (LibRaw's `cam_mul`, which carries a DNG's
  AsShotNeutral), and shows the white point they balance for. Choosing it again restores them, whatever the sliders did.
- **Presets** develop with the multipliers the camera recorded for that light (LibRaw's `WB_Coeffs`) and show their
  white point.
- **Auto** is the gray-world white point of the quick decode, in kelvin.
- **Custom** turns Temperature and Tint into a white point, the white point into the camera's neutral through the
  camera's colour matrix, and the neutral into multipliers for the develop.

The conversions (`compositor/whitebalance.h`):

- Camera neutral ↔ white point: through the camera's XYZ → camera matrix, as the DNG specification describes. A DNG
  gives ColorMatrix1/2 (with CameraCalibration1/2 when present) for its two calibration illuminants, interpolated
  linearly in reciprocal temperature (1/T) between them and solved together with the temperature; other formats use
  LibRaw's matrix for the camera (Adobe's D65 matrix). A four-colour camera, or one LibRaw has no matrix for, keeps
  the older relative Temperature and Tint (−100…100 around As Shot).
- White point ↔ Temperature: Robertson's isotemperature-line method (1968) over the table Wyszecki & Stiles
  publish, 0–600 mired.
- **Tint** is the white point's distance from the Planckian locus along the isotemperature line, in CIE 1960 uv,
  times 3000: one unit of tint is 1/3000 of a uv unit. Positive is a white point on the green side of the locus,
  which the correction answers with magenta, as Camera Raw's positive Tint does. The readout and the develop use the
  same conversion both ways, so a white point read and set again returns the same multipliers (to rounding).

Temperature and Tint are an interpretation of the camera's matrix, so they can differ from Photoshop's by some
kelvin and a few tint units for cameras whose Adobe profile has a second (Standard light A) matrix that LibRaw does
not carry; the develop uses the multipliers, and As Shot is exact either way.

Settings keep the white balance as `rawTemperature` and `rawTint` beside `whiteBalance` (`As Shot`, `Auto`,
`Custom` or a preset's name). Projects saved before kelvin white balance hold relative `temperature` and `tint`
and no `rawTemperature`: they develop exactly as before (as shot, then the relative grade). Edit Contents opens such
a smart object with its relative values turned into the white point they neutralize (0, 0 is As Shot), and OK stores
kelvin from then on.

Automation takes the same settings on `document.open` and `smartObject.editContents`:

- `temperature` above 100 is kelvin (2000–50000) and `tint` then Camera Raw's tint (−150…150, the as-shot tint when
  left out); `rawTemperature` and `rawTint` say the same.
- `whiteBalance` `As Shot` or a preset's name fills in the file's values; a preset the file does not record is
  refused. `Auto` without numbers solves the white point.
- Settings that say nothing about white balance open As Shot.
- A `temperature` within −100…100 is the older relative form and stays relative (as do `tint` values without a
  kelvin temperature).
- The reply's `settings` carry `whiteBalance`, `rawTemperature` and `rawTint`.

```json
{"method": "document.open", "params": {"path": "IMG_0001.CR3", "settings": {"temperature": 5200, "tint": 8, "exposure": 0.3}}}
```

`pixels.cameraRaw` (the filter on pixels) keeps relative Temperature and Tint, as Photoshop's Camera Raw Filter does,
and refuses the RAW-only keys.

## Automation

`pixels.cameraRaw` takes `settings`, an object with the model's keys. Nested objects hold `curve`, `mixer`,
`grading`, `detail`, `optics`, `geometry` and `calibration`. Keys that are left out keep their defaults, which
change nothing. Unknown keys and wrong types are refused. The reply carries the normalized settings that were
applied. `rpc.describe {"method": "pixels.cameraRaw"}` lists every key and range.

```json
{"method": "pixels.cameraRaw", "params": {"settings": {
  "whiteBalance": "Auto", "exposure": 0.4, "shadows": 30, "clarity": 15,
  "grading": {"shadows": {"hue": 220, "saturation": 20}},
  "detail": {"sharpenAmount": 40}, "curve": {"rgb": [[0, 0], [0.25, 0.18], [0.75, 0.82], [1, 1]]}}}}
```

## Left out, and why

- **Geometry's warp** is Core Image's `CIPerspectiveTransform` upstream. Here the same corner maths drives a
  homography with bilinear resampling, so edge pixels can differ by a level from the Mac's. Constrain Crop
  follows upstream: it scales the covered bounding box back up, so a rotation (whose box still spans the frame)
  keeps its empty corners.
- **Canvas interaction** is not in the dialog: the white-balance and defringe eyedroppers, the targeted
  adjustment drag, drawing Upright > Guided lines and picking point colours. The maths behind them is
  ported (`CameraRawSettings::neutralize`, guided corrections, point colours in `mixer.points`), so automation
  can set all of it. White Balance > Auto works in the dialog.
- **Point-curve editing** for the RGB, red, green and blue channels is not in the dialog: it offers the RGB
  presets only. Any curve can be set through `curve.rgb`, `curve.red`, `curve.green` and `curve.blue`.
- **The histogram and vectorscope** (`CameraRawScope`) and the RGB readout under the pointer are display
  panels. They are not ported.
- **The per-panel eye buttons** are not in the dialog. The model has them (`CameraRawSettings::applying`),
  and the tests use it.
- **The Option-drag clipping view** (`CameraRawClipping`) is in the core and the tests. The dialog shows the
  clipping indicators instead.
