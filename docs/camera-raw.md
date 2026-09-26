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
  (relative, not kelvin), Exposure in stops, Contrast, Highlights, Shadows, Whites, Blacks, Texture, Clarity,
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

- **Camera RAW file decoding** (`RawImporter.swift`) is out of scope. The filter works on rendered pixels.
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
