# Adjustment layers

NekoPhoto draws seventeen kinds of adjustment layer, as layers (Layer ▸ New Adjustment Layer) and destructively
(Image ▸ Adjustments): Levels, Curves, Hue/Saturation, Exposure, Gradient Map and Grain from the start, and ten of
Photoshop's others plus Color Lookup. Opened from a PSD they are drawn and editable; written to a PSD they are Photoshop's own
adjustment layers. An imported one whose settings have not changed goes back as the file's own block, byte for byte
(the carry keeps the settings as read); an edited or new one is written in Photoshop's layout.

Model and maths: `src/core/include/compositor/adjustments.h`, `src/core/src/adjustments_more.cpp`. Invert,
Brightness/Contrast and Posterize are per-channel tables, so a run of them with Levels, Curves and Exposure composes
into one pass.

| Kind | Block | How it is drawn | Checked against Photoshop |
|---|---|---|---|
| Invert | `nvrt` | 255 - v per channel | exact (Patchy's photoshop-invert.psd composite) |
| Brightness/Contrast | `brit`, `CgEd` | Patchy's closed forms for the modern and legacy modes (recovered from ~750 Photoshop captures) | modern exact, legacy within 1 level |
| Posterize | `post` | buckets floor(v * n / 256), each at step 255 / (n - 1) | exact; refitted here (rounding to the nearest step, as Patchy has it, misses a third of the samples) |
| Threshold | `thrs` | Photoshop's integer luminance (30 R + 59 G + 11 B) / 100, white at or above the level | exact |
| Color Balance | `blnc` | midtones: a gamma per channel, v^(2^(-amount / 100)); shadows and highlights move the channel's black and white points; Preserve Luminosity is Photoshop's Luminosity blend | midtones exact (0.2 levels); a file using all three ranges is ~10 levels off, the shadow and highlight curves not pinned down by its eight samples |
| Black & White | `blwh` | upstream Compositor's decomposition (grey + secondary + primary, each weighted by its slider); the tint is the tint colour at the grey's lightness | not yet (no Photoshop file) |
| Vibrance | `vibA` | Saturation scales every colour's distance from its luminance; Vibrance does so weighted towards muted colours | not yet |
| Photo Filter | `phfl` | each channel multiplied by the filter's, mixed in by the density; Preserve Luminosity keeps the luminance. Version 3 files store the colour as XYZ in an undocumented scale (read as 16.16, else hundredths); NekoPhoto writes version 2 (RGB) | not yet |
| Channel Mixer | `mixr` | each output a percent mix of the sources plus a constant; monochrome's grey row | not yet |
| Color Lookup | `clrL` | the embedded .cube / .3dl, or the ICC profile through lcms2; trilinear (see below) | not yet |
| Selective Color | `selc` | each colour's share of the nine ranges (hue ranges by their channel's lead, whites, neutrals, blacks) moves the cyan, magenta and yellow inks, black moving all three; relative scales by the ink there is | not yet |

**Color Lookup** (`clrL`, `src/core/src/colorlookup.cpp`): Photoshop embeds the LUT file itself in the layer's
descriptor (`LUT3DFileData`, with `LUTFormat`) or an ICC profile (`profile`). NekoPhoto reads a `.cube` (1D or 3D,
with `DOMAIN_MIN` / `DOMAIN_MAX` or an input range) or a `.3dl` (an input shaper line, integer triplets with blue
fastest, the bit depth taken from the largest value) and runs an abstract profile between sRGB and sRGB, or an RGB
device link, through lcms2 into a 33-point table; the table is applied with trilinear interpolation and cached by the
LUT's content. Layer ▸ New Adjustment Layer ▸ Color Lookup loads a LUT file; automation passes
`colorLookupSettings` (`name`, `format` cube, 3dl or icc, `data` the file's text or the profile in base64, `dither`).
Not checked against Photoshop yet (no Photoshop file with a Color Lookup layer); Photoshop's own `.look` format and
LUTs it only names without embedding are not read, and such a layer stays carried and undrawn. The layout it writes
for a new layer (Vrsn, lookupType, Nm, Dthr, LUTFormat, dataOrder, tableOrder, LUT3DFileData, LUT3DFileName) follows
Photoshop's documented keys and is unconfirmed.

The "Checked" column is against the merged composites Photoshop stored in Patchy's fixtures
(`test-fixtures/psd/photoshop-{invert,brightness-contrast-*,posterize,threshold,color-balance*}.psd`); the rest wait
for a Photoshop capture.
