# Camera Raw Filter

Filter > Camera Raw Filter… (Shift+Ctrl+A) grades the active layer's pixels, inside the selection, as one
undo step. It is a port of upstream Compositor's Camera Raw Filter (MIT, `LICENSES/MIT-Compositor.txt`). It is
destructive: on a smart object it asks first, like the other pixel filters, and there is no Smart Filter version.

| Part | Upstream | Here |
|---|---|---|
| Settings, ranges, normalization, apply order | `Document/CameraRaw*.swift` | `src/core/include/compositor/cameraraw.h`, `src/core/src/cameraraw.cpp` |
| Pixel kernels (`adjust_camera_raw_*`: texture, glow, grain, defringe, profile corrections, detail, point colours, the clipping views) | `Rendering/AdjustPixels.c` | `src/pixels/CameraRawPixels.c`, extracted unchanged apart from warning fixes |
| Tone and colour responses fitted to Camera Raw ([Fitted responses](#fitted-responses)) | none | `src/core/src/rawtone.cpp`, tables from `tools/camera_raw_fit.py` |
| Panels | `UI/CameraRaw*.swift`, `UI/RawDevelopSheet.swift` | `src/app/CameraRawDialog.cpp` |
| Tests | upstream `CompositorTests/CameraRawTests.swift` | `tests/cameraraw_tests.cpp`, `tests/camera_raw_parity.cpp` |
| Automation | none | `pixels.cameraRaw`, MCP `pixels_camera_raw` |

The grade runs in this order: manual Distortion and the Transform sliders; then, per pixel, the relative white balance,
Camera Calibration, Exposure, the Basic panel's tone sliders, the Tone Curve, Vibrance and Saturation, the colour mixer
and Colour Grading; point colours; Clarity, Texture and Glow, the post-crop vignette and Grain; lens vignetting and the
lens profile and defringe; then detail. Alpha is kept. A reduced preview scales every radius, so it matches the
full-size result.

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
  file, as Photoshop remembers its workflow options. The colour space is sRGB.
- **Open** develops the whole file (off the UI thread) and opens it as a new document with one layer.
- **Open Object** (Photoshop's Shift+Open) opens a new document whose one layer is a smart object. Its source is the
  RAW file itself, byte for byte, with the settings; the contents are the developed image at 16 bits.
  Double-clicking the smart object's badge (Layer ▸ Smart Objects ▸ Edit Contents) reopens the RAW file in Camera
  Raw with those settings; OK develops it again and every layer placing it updates, as one undo step.
- **Cancel** (or Escape) stops a decode that is running and opens nothing.

The develop is `compositor/raw.h`: LibRaw demosaics with the white balance's multipliers (the camera's own for As
Shot) and the camera's colour matrix into linear ProPhoto RGB at 16 bits, the raw white point at 1; a DNG's
BaselineExposure scales it; the [default tone curve](#the-default-render) takes it to display light and sRGB. Then
`applyCameraRaw` grades it with the settings, the same code as the filter (so the dialog's result equals Open with no
settings followed by Filter ▸ Camera Raw Filter with them, which `raw_tests` checks).

### The default render

Camera Raw renders a RAW file with a tone curve of its own, and so does NekoPhoto. Ours is fitted to Camera Raw 18.7's
render of the corpus chart's grey ramp ([Camera Raw parity](#camera-raw-parity)): 24 steps from −8 to +3.5 EV, read
against the scene light LibRaw decodes for each, give the curve's knots (`tools/camera_raw_fit.py` writes them into
`src/core/src/rawtone_tables.inc`; `src/core/src/rawtone.cpp` interpolates them with a monotone cubic). Below the
darkest step display light is proportional to scene light; the raw white point renders white, as blown highlights do in
Camera Raw. The curve is applied to the brightest and the darkest channel of the colour in ProPhoto RGB, and the
middle channel keeps its place between them, so it keeps hues and only lightens and darkens; the colour then goes to
sRGB. On the chart the default render is 0.28 mean ΔE00 from Camera Raw's (95th percentile 0.87); LibRaw's own
rendering, with its BT.709 gamma and automatic brightening, was 14.9.

What the fit cannot know:

- Camera Raw brightens most cameras by an exposure offset of its own (their Adobe profiles' baseline). A DNG says
  what it wants in BaselineExposure, which NekoPhoto applies; other formats develop without an offset, so they can
  look darker than in Camera Raw.
- Camera Raw rebuilds clipped channels from the ones that are not; LibRaw clips them. Pulling Exposure down shows the
  difference in blown highlights.
- Colours follow the camera's colour matrix (LibRaw's, or the DNG's ColorMatrix): a DNG's forward matrices and a
  camera's Adobe profile (its hue and saturation tables) are not used. The chart's embedded profile is matrices only,
  and its colours come out within the default render's 0.28 of Camera Raw's; with a camera's Adobe Standard profile
  Camera Raw's colours differ by what the profile's tables do.

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

## Camera Raw parity

`tests/camera_raw_parity` measures how far a RAW develop is from Photoshop's Camera Raw. It uses the public colour corpus
of [RAWmakase](https://github.com/pch/rawmakase) (MIT; `tests/fixtures/camera-raw-parity`, see THIRD-PARTY-NOTICES.md):

- `synthetic-d65.dng`, a 970 × 742 chart from an invented camera under daylight with its own colour matrices: a 24-step
  grey ramp from −8 to +3.5 EV, 216 hue patches (24 hues × 3 lightnesses × 3 chromas), a wide-gamut row, a ColorChecker,
  skin tones, near-neutrals, two sweeps and colours on black and white surrounds (`layout.json`);
- `cases.json`, the settings cases as Camera Raw XMP attributes (sliders one at a time and at their extremes, pairs, the
  colour mixer bands, curves, effects and geometry);
- `camera-raw-synthetic-d65.json`, Camera Raw 18.7's render of each case: the mean encoded sRGB (16-bit) of every patch.

The test develops the chart as File ▸ Open does (`developRaw`), once per case with its attributes mapped onto
`CameraRawSettings`, measures the same patches and scores each case by CIEDE2000: the mean and the 95th percentile over
the patches, the wide-gamut row left out. Cases for controls NekoPhoto has no counterpart for (Convert to Black & White
and its mix, look profiles, Point Color) are listed and skipped. Every case is held to
`tests/camera_raw_parity_baseline.txt`: one that gets worse by more than 0.1 on its mean or 0.3 on its 95th percentile
fails. After an intended change, `COMPOSITOR_BLESS_CAMERA_RAW_PARITY=1 build/tests/camera_raw_parity` rewrites the
baseline (commit its diff with the change); `COMPOSITOR_CAMERA_RAW_PARITY_DUMP=<file>` writes every case's patch values
as JSON, for fitting; `COMPOSITOR_CAMERA_RAW_PARITY_ONLY=<text>` runs the cases whose name contains it and
`COMPOSITOR_CAMERA_RAW_PARITY_TRY="<case>=<settings JSON>"` patches one case's settings, to try a mapping by hand. The
test prints every case and a summary per control. Where Geometry or Distortion uncovers the layer (transparent here),
the patches are measured over white, which is what Camera Raw renders there.

## Fitted responses

The grade's tone and colour controls follow Camera Raw 18.7 as measured on the corpus chart. `tools/camera_raw_fit.py`
fits them from the committed renders (it needs only numpy) and writes `src/core/src/rawtone_tables.inc`;
`src/core/src/rawtone.cpp` applies them. Most of the fits start from Camera Raw's own default render of the chart, so a
control's fit does not carry the default render's error. The same maths runs in the Camera Raw Filter on any layer, which
treats its pixels as a RAW develop (Open with settings equals Open, then the filter with them).

The tone controls work on ProPhoto RGB: Exposure in scene light (each colour taken back through the default tone curve),
the rest on the values with the sRGB transfer, each curve on the brightest and darkest channel with the middle one kept
in its place, which keeps hues, unless the fit says otherwise.

- **Exposure**: a scale of scene light; raising it also rounds off the highlights with x / (1 + x^p)^(1/p) (p fitted to
  Exposure +1, +2 and +3), blended in over the first stop.
- **Whites, Blacks, Highlights, Shadows, Dehaze, Contrast**: each is the curve that takes the chart's grey ramp from the
  default render to the slider's render, sampled at 33 encoded levels for each amount Camera Raw rendered (Contrast
  ±50/±100, Highlights and Shadows ±60/±100, Whites −100/−50/+25/+50/+100, Blacks and Dehaze ±50/±100), mixed linearly
  between amounts. They apply in the order Whites, Blacks, Highlights, Shadows, Dehaze, Contrast, which is the order that
  fits the chart's pairs. Lowering Whites and Highlights works on the colour's luminance (its channels scaled together),
  raising Blacks and either way Shadows half on the luminance and half on the channels: whichever fits the colours.
- **Parametric curve**: the four regions the same way at ±50 and ±100; moved splits warp each region's input so its range
  follows them. Shadows then Highlights, then Darks and Lights.
- **Point curves**: the RGB curve on the brightest and darkest channel; Refine Saturation is Lightroom's (0–200, 100 the
  curve as drawn) less 100: at −100 a colour keeps its channel differences from before the curve and takes its brightness
  (Rec. 601 luma) from it, and values in between blend. The red, green and blue curves run on each channel of ProPhoto RGB.
- **Vibrance and Saturation**: Saturation scales a colour's distance from its luminance in linear ProPhoto RGB by
  1 + amount / 100 (which matches the chart from −100 to +100). Vibrance scales colourfulness by
  1 + amount · gain · (1 − S)^power, S the colour's (max − min) / max; raising it keeps the brightest channel and spares
  skin hues (around 15°) by half, lowering it pulls the channels toward their mean (gain and power fitted per sign).
- **Colour mixer**: the HSL of ProPhoto RGB with the sRGB transfer, each band's share of a hue linear between the
  neighbouring band centres, and a gain per band and direction for Hue (degrees at ±100), Saturation and Luminance, fitted
  to the 96 single-band cases. Luminance moves with the square root of the colour's chroma, so greys stay.
- **Colour Grading**: each wheel pushes the colour (on ProPhoto RGB with the sRGB transfer) along its hue, the fully
  saturated colour of that hue less its mean, by its saturation and the weight its region has at the colour's encoded
  luminance, read off the grey ramp per region. Blending and Balance move the shadow and highlight weights toward those
  read off Blending 100 and Balance +50 (linearly; other values are extrapolated). The Luminance sliders add grey by their
  own ramp tables.
- **Camera Calibration**: a matrix on scene light that keeps greys: each primary's Hue moves it into the other two channels
  in opposite directions and its Saturation out of both (about 0.33 and 0.40 per 100, fitted per slider). Shadow Tint
  pushes along its measured green–magenta direction by a ramp table. Older process versions scale the sliders down as
  before.
- **Post-crop vignette**: an elliptical mask (a circle in pixels at Roundness +100, shrinking below 0) whose start and
  width follow Midpoint and Feather, fitted to every darkening case at once. Paint Overlay scales the encoded value;
  Highlight and Color Priority darken in scene light (about 3.2 stops at the corners at −100), sparing bright colours by
  Highlights, and all styles lighten toward white.
- **Lens vignetting**: a gain in display light of 2^(amount · strength · r^power), r 1 at the corners, the power following
  Midpoint.
- **Distortion and Transform**: manual Distortion samples an output radius r at r · (1 + k (1 − r²)) (corners fixed,
  k 0.4 per 100 raising and 0.5 lowering), before the Transform. Vertical and Horizontal are a perspective (each corner
  divided by 1 − 0.95 (h x − v y) in long-edge units, slightly enlarged); Rotate turns clockwise; Aspect narrows by
  2^(−0.14) per 100; X and Y Offset move by 0.75 of the image per 100.
- **Clarity**: a local contrast of log luminance against a blur of 2% of the long edge (strength fitted per sign).

Mean ΔE00 to Camera Raw per control on the chart (the mean of its cases' means; the default render first). "Before" is
upstream's maths on the fitted default render (Distortion and Transform then measured over black); LibRaw's own
rendering, before both, was 13–26 on every control:

| Control | Cases | Before | Now |
|---|---:|---:|---:|
| Default render | 1 | 0.28 | 0.28 |
| Exposure | 5 | 6.26 | 0.59 |
| Contrast | 4 | 10.98 | 0.46 |
| Highlights | 4 | 10.45 | 3.33 |
| Shadows | 4 | 7.15 | 1.09 |
| Whites | 5 | 4.02 | 0.82 |
| Blacks | 4 | 3.74 | 0.54 |
| Clarity | 4 | 3.46 | 2.93 |
| Texture | 4 | 1.58 | 1.58 |
| Dehaze | 4 | 11.96 | 3.16 |
| Vibrance | 4 | 4.31 | 1.91 |
| Saturation | 4 | 2.40 | 0.96 |
| Parametric curve | 24 | 4.75 | 0.72 |
| Point curve | 7 | 3.01 | 0.84 |
| Colour mixer | 105 | 1.53 | 0.91 |
| Colour grading | 20 | 5.85 | 1.73 |
| Camera Calibration | 9 | 2.77 | 0.38 |
| Post-crop vignette | 16 | 4.45 | 1.46 |
| Lens vignetting | 3 | 9.54 | 0.72 |
| Distortion | 2 | 19.95 | 0.76 |
| Transform | 5 | 17.28 | 1.03 |
| White balance | 9 | 0.94 | 0.94 |
| Grain | 1 | 1.17 | 1.17 |
| Sharpening, noise reduction | 4 | 0.28 | 0.28 |
| Pairs and the combined look | 24 | 7.04 | 2.39 |

What does not match yet:

- **Highlights, Shadows, Dehaze and Clarity are local operators in Camera Raw**: they adapt to the picture around each
  pixel. The fitted curves are their response on this chart, so on a photo with a different key they can be stronger or
  weaker than Camera Raw's (on the chart, Highlights −100 darkens mid grey by about a third). Positive Clarity on the chart
  is still 3.5–6 ΔE00 off.
- **Positive Whites follows the photo's highlights** in Camera Raw; here it is the chart's curve, so Whites +100 after
  Exposure −1.5 is far off (`pair-exposure-1.5-whites+100`, 14.8).
- **Texture** works on edges and fine detail, which the chart's flat patches hardly show; it keeps upstream's operator.
- **White balance** at 2500 and 3200 K is 2–3.3 ΔE00 off (the other temperatures and tints 0.3–0.9).
- Colour mixer bands combine by adding their changes, so alternating luminance (`mixer-alternate-luminance`, 3.8) is off.

## XMP presets

**Load Preset…** (beside Reset All, in the filter and in the dialog a RAW file opens in) applies a Camera Raw or Lightroom
preset, or a sidecar, as Photoshop's Camera Raw loads `.xmp` presets: the `crs:` settings it names replace the current
ones and the rest stay (`compositor/camerarawxmp.h`; the parity test maps its cases through the same code). The preset's
settings are read as attributes or elements, the point curves as sequences.

- Read: Exposure2012, Contrast2012, Highlights2012, Shadows2012, Whites2012, Blacks2012, Texture, Clarity2012, Dehaze,
  Vibrance, Saturation; the Parametric* regions and splits; ToneCurvePV2012 (and Red, Green, Blue) and
  CurveRefineSaturation (stored less 100); Hue/Saturation/LuminanceAdjustment for the eight bands; SplitToning* and
  ColorGrade* (wheels, Luminance, Blending, Balance); ShadowTint and the Red/Green/Blue Hue and Saturation; PostCropVignette*
  (style codes 1 Highlight Priority, 2 Color Priority, 3 Paint Overlay); Grain*; VignetteAmount and VignetteMidpoint;
  LensManualDistortionAmount, LensProfileEnable with its distortion and vignetting scales, AutoLateralCA and the Defringe*
  settings; Perspective* (Scale less 100); Sharpness, SharpenRadius (0.5–3 px onto the slider), SharpenDetail,
  SharpenEdgeMasking and the noise reduction settings; the Enable* panel switches (off resets that panel).
- White balance: on a RAW file Temperature and Tint in kelvin, and WhiteBalance As Shot, Auto or a preset the file
  records (from the file); on pixels IncrementalTemperature and IncrementalTint, the relative ones the Camera Raw Filter
  saves.
- Descriptive attributes (name, group, UUID, version, the Supports* flags, lens profile names) are skipped.
- Everything else is listed when the preset is loaded and left out: a camera profile or look, Convert to Black & White
  and its mix, Point Color, a crop, Upright analysis, the 2010 process's sliders, masks and retouching, and any setting at
  a value this engine has no control for. A setting at the value that changes nothing is not listed.

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
