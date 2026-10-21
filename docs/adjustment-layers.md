# Adjustment layers

NekoPhoto draws seventeen kinds of adjustment layer, as layers (Layer ▸ New Adjustment Layer) and destructively
(Image ▸ Adjustments): Levels, Curves, Hue/Saturation, Exposure, Gradient Map and Grain from the start, and ten of
Photoshop's others plus Color Lookup. Opened from a PSD they are drawn and editable; written to a PSD they are Photoshop's own
adjustment layers (except Grain, which Photoshop lacks, and a Gradient Map made or edited here: those are written as a
pixel layer of their result, [psd-export.md](psd-export.md)). An imported one whose settings have not changed goes back as the file's own block, byte for byte
(the carry keeps the settings as read); an edited or new one is written in Photoshop's layout.

Model and maths: `src/core/include/nekophoto/adjustments.h`, `src/core/src/adjustments_more.cpp`. Invert,
Brightness/Contrast and Posterize are per-channel tables, so a run of them with Levels, Curves and Exposure composes
into one pass.

**Exposure** (`expA`) works in linear light as Photoshop does at 8 and 16 bits: the value is linearised through a 2.2
power (not the sRGB curve), multiplied by 2^exposure, the offset added, raised to 1 / gamma and encoded again; an Exposure
layer read from a grayscale PSD keeps Photoshop's grey power of 1.732 (the file opens as RGB). Exported as PSD (RGB),
such a layer is drawn through 2.2 by Photoshop and by NekoPhoto on reopening, as in Photoshop after Image ▸ Mode ▸ RGB
Color; the export warns. At 32 bits the samples are
linear already and are taken as stored. photocraft-corpus `exposure.psd` in rgb8/16/32 and gray8/16/32 and psd-tools'
`exposure_rgb` and `exposure_grayscale` are within a level or so of Photoshop (they were up to 34 levels off through the
sRGB curve). Camera Raw's exposure is its own.

**Curves** (`curv`) is drawn as Photoshop draws it: the natural cubic spline through the points (no bend at the end
points), flat beyond the first and last point. Every `curves.psd` in photocraft-corpus (RGB, grayscale, CMYK and Lab, 8
and 16 bits) renders within a level of Photoshop's merged image (max 0 at 8 bits); a monotone cubic, as before, was up
to 4 levels off. Camera Raw's point curve is its own and keeps its monotone cubic.

**Gradient Map** (`grdm`) reads every colour stop with its location and midpoint, the Method, Reverse and the
Smoothness. The stops are stored in the document's colour space (a CMYK file's in CMYK, a Lab file's in Lab, a Grayscale
file's as grey ink); a CMYK or Lab document maps to those values as the file holds them, so a CMYK stop keeps its own
black. The map reads Photoshop's luminosity 0.30 R + 0.59 G + 0.11 B (the blend modes' Lum, not Rec. 709), L in Lab, and
in CMYK the luminosity of the inks' plain complement (stored cyan times stored black, and so on) whatever the profile; a
Classic map at Smoothness 100% eases each run towards the Catmull-Rom through its neighbours, kept between the run's own
stops, as the layer styles' Classic gradients do. Every `gradient-map.psd` in photocraft-corpus (RGB, grayscale, CMYK
and Lab at 8 and 16 bits) is within a level of Photoshop's merged image. The shadows and highlights colours edit the end
stops; the stops between are kept as read. Dither is read and written back but not drawn (its pattern is not known; it
moves a colour by under a level). A map made here is the plain two-stop blend.

| Kind | Block | How it is drawn | Checked against Photoshop |
|---|---|---|---|
| Invert | `nvrt` | 255 - v per channel | exact (Patchy's photoshop-invert.psd composite) |
| Brightness/Contrast | `brit`, `CgEd` | Patchy's closed forms for the modern and legacy modes (recovered from ~750 Photoshop captures) | modern exact, legacy within 1 level |
| Posterize | `post` | buckets floor(v * n / 256), each at step 255 / (n - 1) | exact; refitted here (rounding to the nearest step, as Patchy has it, misses a third of the samples) |
| Threshold | `thrs` | Photoshop's integer luminance (30 R + 59 G + 11 B) / 100, white at or above the level | exact |
| Color Balance | `blnc` | a Levels per channel, ((v - black) / (white - black))^gamma. Preserve Luminosity on: the shadows raise each channel's black point by how far its slider sits below the highest of the three (in levels), the highlights lower the white point by how far it sits above the lowest, the midtones give the gamma 2^-((v - (max + min) / 2) / 100). Off: each slider moves its own channel only (a negative shadow raises the black point, a positive highlight lowers the white point; the midtones bend the gamma by 2^(-v / 100), shadows and highlights by 2^(-v / 200)) | within a level on both files (mean 0.2 and 0.3) |
| Black & White | `blwh` | upstream Compositor's decomposition (grey + secondary + primary, each weighted by its slider); the tint is the tint colour at the grey's lightness | not yet (no Photoshop file) |
| Vibrance | `vibA` | Saturation scales every colour's distance from its luminance; Vibrance does so weighted towards muted colours | not yet |
| Photo Filter | `phfl` | Photoshop's lens (after PhotoCraft's `photo_filter_matrix`): the colour's D50 XYZ in linear light multiplied by the filter's, relative to white and mixed in by the density, encoded again; Preserve Luminosity puts back the luminosity 0.30 R + 0.59 G + 0.11 B as the Luminosity blend mode does. At 32 bits the linear samples go through the same map with the luminance Y kept in it, unclipped; in Lab, X, Y and Z are scaled and L kept. The colour is read from any colour record (Photoshop 27 saves it in Lab); version 3 files store XYZ in an undocumented scale (read as 16.16, else hundredths); NekoPhoto writes version 2 (RGB) | photocraft-corpus rgb16, lab8 and lab16 within a level; rgb8 mean 0.2 but up to 5 levels where the colour clips (2% of pixels; 8-bit Photoshop clips differently from 16-bit there, rule unknown); rgb32 mean 1.3, max 9 (its 32-bit map is a linear matrix, but not this one); CMYK mean 3.2, max 30 (the RGB kernel on the stored cyan, magenta and yellow: Photoshop keeps the black plate and the stored colour's luminosity, as this does, but moves the colour otherwise) |
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
for a Photoshop capture. `tests/photoshop_adjustment_tests.cpp` holds Hue/Saturation and Color Balance to them.

**Hue/Saturation** (`hue2`, `src/core/src/adjustments.cpp`) is Photoshop's model as Patchy calibrated it (MIT,
`docs/adjustments-calibration.md` there): the Lightness slider blends each channel towards white or black first (its
percent quantised to a byte step); the colour is then read as a lightness (max + min) / 2, a half-chroma and a position
on a 1530-step hue wheel; Saturation multiplies the half-chroma by a measured table (+100 is 128 times, so any colour
saturates fully, neutrals never tint), the Hue slider turns the wheel by whole steps (4.25 a degree), and the colour is
rebuilt rounding towards its brightest channel and truncating towards its darkest, so sliders at zero give every colour
back exactly. A range (Reds ... Magentas) is chosen by the colour's own hue through its four stops; its lightness
collapses the chroma towards the brightest (positive) or darkest (negative) channel before the master, its saturation
adds `weight * (table - 1)` to the master's factor and its hue adds `weight * degrees` in steps. Colorize keeps the
lightness and rebuilds from measured per-percent saturation and per-degree hue tables. On bytes this is Photoshop's
integer arithmetic; at 16 and 32 bits and in CMYK the same model runs unrounded on each colour. Without the Photoshop
saturation curve (`"saturationCurve": "scale"`, the default for a layer made here) Saturation scales the half-chroma by
1 + percent / 100 instead. Against Photoshop's flatten of Patchy's probes (`photoshop-hue-saturation-{master,bands}.bmp`;
the merged image stored in those two PSDs is the probe before the settings were written into them, so
`psd_composite_oracle` cannot judge them): master within a level (mean 0.07); ranges within two levels but on their
feathered edges, where Photoshop weighs a colour as if its hue were up to three-quarters of a degree lower (7 levels at
worst, mean 0.13, 2% of pixels beyond two levels; Patchy found no rule either).
