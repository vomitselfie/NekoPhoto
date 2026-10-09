# CMYK and Lab

**English** · [日本語](#日本語)

A document is in one colour mode, as in Photoshop: **RGB Color**, **CMYK Color** or **Lab Color**, at 8 or 16 bits
per channel (Photoshop has no 32-bit CMYK or Lab, and neither does NekoPhoto). The mode is what the pixels are: a
CMYK document holds cyan, magenta, yellow and black ink, a Lab document lightness and the a and b colour axes.
Profiles and the display are in [color-management.md](color-management.md); the design is in
[high-bit-depth-plan.md](high-bit-depth-plan.md), "P7 plan: CMYK and Lab". Which features work in which mode is in the
[capability matrix](mode-matrix.md), generated from the code.

## Image ▸ Mode

**Image ▸ Mode ▸ RGB Color, CMYK Color, Lab Color** convert the document, as one undo step:

- Every layer's pixels go from the document's profile to the new mode's: the **Working CMYK** (Edit ▸ Color
  Settings) for CMYK, **Lab D50** for Lab, the **working space** for RGB. The rendering intent and black point
  compensation are Color Settings' **Conversion Options** (Relative Colorimetric with black point compensation, as
  Photoshop's defaults are). Black generation (how much black ink replaces cyan, magenta and yellow) is the CMYK
  profile's.
- Colours the document keeps as values are converted too: text, shape fills and strokes, artboard backgrounds, layer
  style colours and gradients, Photo Filter and Gradient Map colours, and the **foreground and background colours**.
  They stay RGB values (sRGB in a CMYK or Lab document); converting to CMYK brings them into the press's gamut.
- **Levels and Curves lose their per-channel settings** (red, green and blue mean something else in CMYK and Lab);
  the composite settings stay.
- An **adjustment layer the new mode does not offer** (Vibrance and Black & White in CMYK and Lab; Hue/Saturation,
  Color Balance, Selective Color and Channel Mixer in Lab; Exposure in CMYK) is **kept, hidden and marked**; converting
  back to a mode that offers it shows it again as it was.
- Masks, alpha and spot channels and the selection are not colour and stay as they are.
- A document that would not fit the byte budget in the new mode (a CMYK layer holds four fifths the pixels of an RGB
  one) is refused with the reason; a 32-bit document converts to 8 or 16 bits first.

Undo returns the document exactly as it was. Converting RGB to CMYK and back does not return the same colours:
colours outside the press's gamut stay mapped into it, as in Photoshop. RGB to Lab and back at 16 bits returns the
values of an 8-bit source exactly; at 8 bits, 8-bit Lab's steps move some saturated colours near the edge of sRGB by
a few levels.

## What a CMYK or Lab document draws

- Pixel layers with opacity, pixel and vector masks, clipping masks, folders (Pass Through and isolated), artboards,
  text, shape layers and their strokes, gradient and pattern fill layers, layer styles, and hidden layers, at 8 and 16
  bits.
- **Blend modes.** CMYK offers every mode, as Photoshop does. The separable ones work on each ink as stored
  (inverted, so Multiply adds ink the way it darkens in RGB), with the same arithmetic as RGB's. Hue, Saturation,
  Color, Luminosity, Darker Color and Lighter Color are Photoshop's too, and do not pass through RGB: the PDF
  luminosity and saturation helpers run on the stored C, M and Y (the complements of the inks) and K is carried
  apart, the backdrop's for Hue, Saturation and Color and the source's for Luminosity; Darker and Lighter Color take
  one pixel whole, K included, weighing K in the comparison. Every CMYK mode matches Photoshop's own values for six
  colour pairs (measured with its sampler, from psd-tools' test suite) within two 8-bit steps, three for the
  separable modes at 16 bits, and a Photoshop-saved file with every mode over rubber ducks renders within the
  resampling noise of its smart objects. Checked against
  Photoshop: a Photoshop-saved 16-bit CMYK PSD (Japan Color 2001 Coated, Normal layers) renders as its stored composite
  converted through its own profile with Relative Colorimetric and black point compensation (Photoshop's default),
  2 levels off on average; the few pixels further off are cyans outside sRGB, where 16-bit inks clip red to 0.  A gradient
  fill whose stops are CMYK colours runs from ink to ink, as Photoshop's does, instead of through RGB; the gradient
  types other than Linear have no Photoshop render to check their geometry against yet. Lab offers every mode
  but Color Dodge, Color Burn, Darken, Lighten, Difference, Exclusion, Subtract and Divide (Adobe's list; they are greyed
  out, and a file that has one draws it as Normal). Normal and Dissolve are RGB's; Luminosity takes L from the layer
  and a and b from below, Color the other way round, Hue and Saturation work in L, chroma and hue; the separable modes
  apply to L, a and b as stored. Checked against Photoshop: two Photoshop-saved Lab PSDs render exactly as the composite
  Photoshop stored in them, one with a Color Fill layer in **Color** mode (no level off anywhere) and a monitor test
  chart with 1-level lightness step wedges (at most 1 level off). The other Lab modes have no Photoshop reference yet.
- **Adjustment layers**: every kind Photoshop offers in the mode draws on the document's own samples (see
  [Adjustments and filters](#adjustments-and-filters)). A Color Lookup layer holding a 3DLUT file is kept and written
  back to PSD but draws nothing, as 3DLUT files apply in RGB only.
- **Layer styles** draw in the document's channels: see [Text, shapes and layer styles](#text-shapes-and-layer-styles).
- The canvas always goes through a colour transform: the document's profile to the monitor profile, or to sRGB when
  none is known, in the same pass that reduces the frame to 8 bits. Layer thumbnails are drawn the same way.

## Adjustments and filters

**Image ▸ Adjustments**, **adjustment layers** and the **Filter** menu work in CMYK and Lab at 8 and 16 bits, on the
inks or on L, a and b as stored: nothing is converted to RGB and back. Each kind is offered where Photoshop offers it;
what Photoshop greys in a mode stays greyed for good ("Not available in CMYK mode").

| Kind | CMYK | Lab |
|---|---|---|
| Levels, Curves | composite and each ink (Cyan, Magenta, Yellow, Black) | Lightness, a, b (no composite; Lightness first) |
| Brightness/Contrast | every ink | Lightness |
| Invert, Posterize | every ink | L, a and b |
| Threshold, Gradient Map | on the colour's lightness | on L |
| Photo Filter | yes | yes |
| Exposure | greyed (Photoshop lacks) | on L |
| Hue/Saturation, Color Balance, Selective Color, Channel Mixer | yes | greyed (Photoshop lacks) |
| Vibrance, Black & White | greyed (Photoshop lacks) | greyed (Photoshop lacks) |
| Color Lookup | ICC abstract profiles, CMYK device links | ICC abstract profiles, Lab device links |

- **Levels and Curves** work on each channel as its histogram shows it: a CMYK plate is dark where the ink is, so
  moving Levels' black input point up adds ink, as in Photoshop. CMYK's composite applies to every ink after the ink's
  own setting. Lab has no composite channel: the channel menu starts at Lightness, and a and b take their own curves.
  The Levels histogram shows the layer's channels. Auto and the black, gray and white samplers are RGB only for now.
- **Invert** (Ctrl+I) inverts every ink, or L, a and b.
- **Threshold and Gradient Map** decide on lightness: L in Lab, and in CMYK the colour's L* read through the profile
  (for deciding only; the pixels are not converted). Threshold's black is the profile's black, as the brush paints it;
  a Gradient Map's colours are converted through the profile once and the map runs between them in the document's
  mode.
- **Hue/Saturation and Color Balance** in CMYK work on the stored cyan, magenta and yellow as the RGB kernels work on
  red, green and blue; the black plate is kept. **Selective Color** moves each ink by colour range, its Black slider
  the black plate. **Channel Mixer** in CMYK has four ink rows (cyan, magenta, yellow and black from the four inks and
  a constant); Monochrome makes the black plate alone.
- **Photo Filter** converts its colour through the profile: in CMYK it is laid over each ink, in Lab it moves a and b
  (and L too without Preserve Luminosity). **Exposure** in Lab changes L through relative luminance.
- **Color Lookup** takes an ICC **abstract profile** (run from the document's profile through it and back, so it works
  on the colours as they look) or a **device link** from the document's colour space to itself (a CMYK-to-CMYK link in
  a CMYK document, the usual way to apply one in Photoshop). **3DLUT files** (.cube, .3dl) work in RGB documents only:
  loading one in CMYK or Lab says so, and a 3DLUT layer brought in from RGB is kept and draws nothing.
- **Filters**: Gaussian Blur, Motion Blur and Lens Correction treat every sample alike, as in RGB. Add Noise puts its
  own noise on each ink, or on L, a and b; Monochromatic puts the same noise on every ink, or on L alone in Lab.
- These are the RGB kernels' formulas adapted to each mode and are not yet checked against Photoshop's own output.
  PSD Levels and Curves records map to the same channels (CMYK's black is the fifth), and a CMYK file's Channel Mixer
  reads its four ink rows.

Checked by `adjust_modes_tests` (Levels on CMYK moves ink, the black slot moves the black plate alone, Lab Curves on
Lightness leaves a and b, Invert inverts the inks, Selective Color, Threshold to the profile's black, a black-to-white
Gradient Map keeping L, the colour kinds, the filters over five samples, an adjustment layer drawing what the pixel
edit makes), CMYK and Lab adjustment and filter scenes in `render_hash_tests`, and `rpc_smoke.py`.

## Channels

The Channels panel lists the mode's colour channels: **CMYK, Cyan, Magenta, Yellow, Black** (Ctrl+2, then Ctrl+3 to
Ctrl+6; alpha channels from Ctrl+7) or **Lab, Lightness, a, b** (Ctrl+2 to Ctrl+5). One channel alone shows in gray
(a CMYK plate with its ink dark, as Photoshop shows it; a and b gray where neutral); several CMYK inks show as inks on
white; several Lab channels show their colour with the hidden ones neutral. Ctrl-click (or Alt+Ctrl+number) loads a
CMYK channel as a selection of its ink, a Lab channel as its value. With some colour channels the target, **Fill**
(Edit ▸ Fill, Alt+Backspace, Ctrl+Backspace) changes only them.

## Painting

The painting tools work on the document's own samples: a stroke in a CMYK document lays ink, one in a Lab document
paints L, a and b. Nothing is painted in RGB and converted back.

- **Brush and Eraser** (the round tip and imported tip brushes), on pixels, masks and the Quick Mask. The
  colour you pick (kept as sRGB in these modes, as the colour pickers show it) is converted through the document's
  profile first, with Color Settings' Conversion Options: in CMYK to the four inks the profile's black generation
  gives (black from the bundled ISO Coated v2 is a rich black, about 77/69/60/93%, not K alone, as in Photoshop), in
  Lab to its L, a and b. Soft edges, opacity and the eraser mix each ink, or L, a and b, as stored.
- **The Gradient tool**: each stop is converted, and the gradient runs between the stops in the document's mode (inks,
  or L a b), as Photoshop computes a gradient in the document's mode.
- **Clone Stamp** copies the document's own samples (the composite at its layout, or the layer alone), and **moving or
  duplicating selected pixels** with the Move tool carries every sample. **Merge Down**, **Merge Layers** and **Layer
  Mask ▸ Apply** work at the layout too.
- **Retouching**, on the document's own samples in both modes:
  - **Spot Healing, the Healing Brush and Patch**. In Lab they heal L, a and b as the RGB tools heal red, green and
    blue. In CMYK every one of the five samples is healed (the membrane that matches the tone to the edge runs on each
    ink, K included); where a spot heals from is chosen on the plates' look reduced to 8 bits (each of C, M and Y as
    stored times K), for deciding only. Content-Aware synthesis works on RGBA, so in CMYK the Content-Aware type copies
    the best-matching nearby patch, as Proximity Match does with a wider search.
  - **Blur, Sharpen, Smudge and Liquify** blur, sharpen, carry and resample every sample (five in CMYK).
  - **Dodge and Burn** move L along their range's curve in Lab, a and b kept (Lab holds colour apart from lightness,
    so Protect Tones changes nothing there). In CMYK they move each plate's brightness (the ink inverted) along the
    same curves, so Dodge removes ink and Burn adds it, black included. **Sponge** scales a and b in Lab (Saturate
    doubles the chroma, Desaturate takes it away) and in CMYK works on cyan, magenta and yellow as the RGB Sponge
    does on their complements, the black plate left alone.
  - **The Paint Bucket** chooses what to fill on the native samples (the canvas as shown, or the layer alone): every
    sample, alpha included, within Tolerance of the clicked pixel's, as Photoshop's bucket compares each channel. The
    colour goes in through the profile, as the brush's does.
- **The Eyedropper** reads the composite's inks or L, a, b (`color.sample` answers them as percentages or values) and
  sets the foreground colour to that colour converted through the document's profile to sRGB.
- **Selections**: the Select menu, Quick Mask, loading a mask or channel and transforming the outline work in CMYK
  and Lab (the selection is coverage, not colour), and so does loading a layer's pixels as a selection (Ctrl-click the
  thumbnail; a CMYK layer's alpha is its fifth sample).
- **The Magic Wand and Quick Select** decide in L\*a\*b\*: a Lab document's own L, a and b, a CMYK document's composite
  (or the active layer) converted through its profile to Lab for deciding only; the pixels are never converted.
  Tolerance counts 8-bit levels of L, a and b, each compared on its own as Photoshop's wand compares each channel (L
  runs 0 to 255 over 0 to 100, a and b a level per unit). Quick Select's scribbles work on the same L, a and b; its
  click-to-select model, trained on sRGB, sees the composite converted to sRGB.
- Greyed for good: the **MyPaint** presets ("Not available in CMYK mode"): libmypaint mixes RGB and has no inks or Lab,
  so its strokes could only be painted in RGB and converted, which NekoPhoto does not do.

Checked by `paint_modes_tests`: black painted in CMYK at 8 and 16 bits is the inks Little CMS gives for black through
the profile, within half a level, K and C, M, Y all laid; a Lab stroke's L, a and b are Little CMS's for the colour
within a level; a CMYK gradient's ends and middle are the inks between the stops; moving and cloning carry all five
samples. `retouch_modes_tests` heals CMYK at 8 and 16 bits (every sample back to the surrounding inks), blurs a black
edge in CMYK with the other plates kept, dodges L in Lab with a and b kept, burns ink in, sponges a and b to neutral,
smudges and pushes black ink, and checks the bucket's choice on K and on Lab's a and b. `rpc_smoke.py` paints, erases,
clones, heals, patches, blurs, sharpens, smudges, dodges, burns, sponges, fills with the bucket, merges and applies a
mask in each mode.

## Text, shapes and layer styles

Text, shape layers, paths, vector masks and layer styles work in CMYK and Lab at 8 and 16 bits, as in RGB: the Type
tool and Edit Text, the Shape, Pen and Direct Selection tools, live shapes, Fill Path and Stroke Path, Layer ▸ Vector
Mask, the Layer Style dialog, copying, pasting and clearing a style, style presets, and their automation methods. Each
edit is one undo step.

- **Layer styles**: every effect (drop and inner shadow, outer and inner glow, bevel and emboss, satin, colour,
  gradient and pattern overlay, stroke), on layers and folders. The shapes the effects take (blurs, spread and choke,
  distance fields, the bevel's height) are the same as in RGB; what changes is colour. Each effect colour, gradient ramp
  and pattern tile goes through the document's profile once (Color Settings' Conversion Options), and the effects blend
  in the document's own channels with the mode's blend modes: per ink in CMYK (Multiply adds ink), on L, a and b as
  stored in Lab, Hue, Saturation, Color, Luminosity, Darker and Lighter Color as the mode's layers compute them. A Lab
  document offers no Color Dodge, Color Burn, Darken, Lighten, Difference, Exclusion, Subtract or Divide for effects
  either: the dialog greys them, and an effect a file sets to one draws as Normal. A colour a CMYK file gives as inks
  (Photoshop's 'CMYC') draws as exactly those inks and is written back as inks until it is changed; a gradient whose
  stops are all inks runs from ink to ink.
- **Text** is typed on the canvas or set in the Text dialog as in RGB. Qt paints the glyphs in sRGB at 16 bits; each
  colour then goes through the profile once and the glyphs' coverage is kept, so the text lands in the document's
  channels (a run whose colour a CMYK file gave as inks keeps those inks). Photoshop type layers in CMYK files, whose
  colours are inks, open as editable text and are written back as type with their inks.
- **Shape layers** fill in the document's channels: a solid colour through the profile (a CMYK file's ink colour as
  its inks), a gradient of inks from ink to ink, RGB gradients and patterns drawn in sRGB and converted. PSD writes them
  as shape layers, and a shape layer stays one through Image ▸ Mode and Convert to Profile. **Fill Path** and **Stroke
  Path** paint the foreground colour through the profile, as the brush does.
- Checked: a document drawn in CMYK or Lab against the same document drawn in RGB and converted, every effect alone
  and together, at 8 and 16 bits (`style_modes_tests`): on average at most 1.7 levels apart in CMYK and 1.1 in Lab (sRGB
  levels), the largest gaps where a soft edge mixes two far-apart colours, which blending in inks or in L, a and b
  mixes differently from RGB. Patchy's `photoshop-cmyk-style-colors.psd` (Photoshop-saved CMYK) draws its colour
  overlay as the file's inks exactly (42/45/67/13%) and its red type (0/100/100/0%) as inks, and its type layer opens
  as editable text. That file stores no composite, so no Photoshop render of effects in CMYK or Lab was available to
  compare against; how Photoshop blends a and b for the separable modes in Lab (as stored, as NekoPhoto does) is
  unverified, as it is for Lab's layers.

## Photoshop files

- CMYK (mode 4) and Lab (mode 9) PSDs **open in their own mode** at 8 and 16 bits: the layers' planes as stored, the
  CMYK profile (resource 1039) as the document's, alpha and spot channels. Duotone, Multichannel, Indexed, Bitmap and
  Grayscale files are converted to RGB, as before.
- Saving writes the document's mode, depth and profile. A layer that has not changed goes back **byte for byte**
  (its channels as they were stored), and so do the adjustment blocks and everything else the file held that
  NekoPhoto does not model.
- Checked against: Patchy's `photoshop-cmyk-style-colors.psd` (the one Photoshop-saved CMYK file available) opens as
  CMYK and comes back byte for byte; constructed CMYK and Lab files at 8 and 16 bits (layers, a mask, an adjustment
  layer, a spot channel; `tests/psd_modes_tests.cpp`) are read back by an independent decoder. Constructed files are a
  weaker check than Photoshop's own; Photoshop-saved CMYK and Lab files are welcome.

## Transforms, the clipboard and Place

- **Image Size, Crop, Trim, the Crop tool, Crop to Selection, Canvas Size and Flip Canvas** work at 8 and 16 bits in
  both modes. Resampling (Image Size, Distort, Perspective, **Edit ▸ Warp** and the **warp cage**, and **Free Transform
  of selected pixels**) weighs every sample as the RGB samplers weigh a channel: all five in CMYK (alpha included), L,
  a and b in Lab (a and b come out as the mean of their neighbours', never pulled toward neutral). Nothing is resampled
  in RGB. A flat ink stays exactly that ink.
- **Cut, Copy, Copy Merged, Paste and Layer via Copy** keep the document's own samples within a document and between
  documents of the same mode and profile. Between modes the pixels are converted through the profiles, with Color
  Settings' Conversion Options, as Photoshop converts a paste: RGB into CMYK (into the press gamut), CMYK or Lab into
  RGB, Lab and CMYK into each other; copied layers (Edit ▸ Copy with no selection) are converted as Image ▸ Mode
  converts a document. Other apps get an 8-bit sRGB copy; their pixels come in as sRGB. Pasting into one colour channel
  writes the pixels' gray (read in sRGB) into it.
- **File ▸ Import** converts the file once, from its embedded profile (sRGB when it has none) through the document's.
- Paste Into and Layer via Cut are greyed: NekoPhoto has them in no mode yet.

## Exports

PNG, JPEG, WebP, TIFF, TGA, ICO and GIF exports of a CMYK or Lab document are the composite converted through the
document's profile to sRGB, as Photoshop's Export As does: the same conversion the canvas makes without a monitor
profile, written untagged (sRGB). A 16-bit document exports 16-bit PNG and TIFF, and is dithered to 8 bits for the
other formats. PSD keeps the document's own mode. **Export Artboards**, **Export Slices** (PNG, JPEG, WebP, TIFF) and
**SVG** are drawn the same way: each area or image through the profile to sRGB, and SVG's shape colours as the
document shows them (a CMYK fill's inks, or an RGB colour as it lands in the press gamut), in sRGB.

Checked by `transform_modes_tests` (Image Size keeps the inks at 8 and 16 bits with each sampling mode, Lab keeps a and
b, the CMYK warp equals the RGB warp channel by channel, Warp and the warp cage on five samples, the flat export equals
the native composite through the profile within a level, sRGB red into CMYK through the profile) and `rpc_smoke.py`
(Image Size, Crop, Warp, the cage, Trim, Import, every flat export and layers copied into and out of RGB, in each mode
and depth).

## Smart objects

Smart objects work in CMYK and Lab documents at 8 and 16 bits, as in Photoshop:

- **Place Embedded** and **Replace Contents** with an RGB file (PNG, JPEG, an RGB PSD, a camera RAW file placed with
  Open Object): the contents are converted once from their own profile (sRGB when they have none) into the document's
  mode, and every instance shares the converted pixels. Moving or scaling an instance resamples those, never a copy
  of a copy.
- **Convert to Smart Object** makes a PSB of the document's mode and profile: the layers look exactly as they did,
  and **Edit Contents** opens them in a tab of that mode. Saving the tab puts them back as a PSB of that mode.
- CMYK or Lab contents placed in a document of the same mode and profile keep their own inks (or L, a and b); in a
  document of another mode or profile they are converted through the profiles.
- **Warp**, the warp cage and **Free Transform** draw the contents through the mesh on every sample.
- **Smart Filters** run on the document's samples: Lab's L, a and b as they are, CMYK's four inks with black treated
  like the others. Every Smart Filter but **Plastic Wrap** is offered: Plastic Wrap is a Filter Gallery filter, which
  Photoshop offers in RGB only. The filter mask and Photoshop's filter cache in the PSD hold the document's channels.
- **Rasterize** keeps what the layer shows.
- PSD and projects keep the instances live; a CMYK or Lab PSB's own samples are read again from the file when a
  project opens.

## Not yet, and not here

Remove Background, Content-Aware Fill, Move and Scale, and Clouds and Difference Clouds (which paint sRGB colours)
stay greyed in CMYK and Lab ("Not available in CMYK mode yet"); CMYK JPEG and TIFF (a CMYK document exports them in sRGB). Camera Raw, G'MIC and the MyPaint brushes stay
RGB only ("Not available in CMYK mode", for good), and so does **Mosh**: it is a glitch effect on RGB colour that
Photoshop has no counterpart for, and running it through RGB and back is not done here.

## Automation

`image.mode` takes `colorMode` (`rgb`, `cmyk`, `lab`) with or without `bits`; `document.info` reports `colorMode`;
`color.settings` reads and sets the Conversion Options (`intent`, `blackPointCompensation`); `brush.stroke` and
`gradient.draw` paint in the document's model; `color.sample` is the Eyedropper (inks as percentages, or L, a, b);
`channels.*` name the
colour channels `cyan` ... `black` and `lightness`, `a`, `b`, and the composite `cmyk` or `lab`
([automation.md](automation.md)).

## For developers

- `convertDocumentMode` and `convertModeColor` (`colormgmt.h`, `colormode_convert.cpp`); `adjustmentOfferedInMode`,
  `isDormantAdjustment` (the marker is a `dormantInMode` key in the adjustment's settings); `modeThumbnail`.
- Rendering: `render_exec_deep.inc` is written once over a policy with a channel count; `ModeOps<S, M>`
  (`render_modes.h`) is the CMYK and Lab policy, instantiated in `render_exec_c8.cpp` (8-bit CMYK), `render_exec_lab8.cpp`
  and `render_exec_modes16.cpp`; the RGB executors are untouched. The blend kernels are `blend_c8.cpp` and
  `blend_modes16.cpp` over `blend_modes.inc`; `blendModeFor` holds the calibration hook for CMYK's non-separable modes.
  `renderNative` renders at the document's layout.
- PSD: `assembleMode8/16` in `psd.cpp`, `setPixelsMode` and `writeMergedMode` in `psd_writer.cpp`; the plane carry is
  `PsdLayerCarry::planes`.
- Painting: `CmykStrokeOps<S>` (`stroke_raster.h`, `brush_modes.cpp`) paints five samples; Lab uses the RGB rasters of its
  depth; `BrushStroke(layer, mask, settings, document, options)` converts the colour through the profile
  (`RGBFloat` to `CMYKFloat` / `LabFloat`); `tiledProcessedNative` renders a layer at its layout for Blur and Sharpen;
  `trimToPixelsAny` and `applyMaskAny` (`pixels_any.cpp`) merge and apply masks at any layout.
- Transforms: `modetransform.h` (`transform_modes.cpp`) runs `warpImage`, `warpImageTrimmed`, `resampleLayer`,
  `renderWarpedImage` and `renderWarpedOverBox` over any layout: CMYK as (C, M, Y, alpha) and (K, K, K, alpha),
  joined back. The session's CMYK and Lab paths (clipboard, Distort, merging a floating selection, conversions between
  modes) are in `EditorSessionModes.cpp`.
- Text, shapes and styles: `ModeOps` draws styles through `drawStyledLayer` (`layerstyle_render.cpp`, templated on the
  mode: `NativeColours` converts the colours, ramps and tiles, `blendEffect` blends in the mode's channels);
  `textRasterInMode` (`colormode_convert.cpp`) takes Qt's 16-bit text raster into the document's mode;
  `setVectorShape` fills in the document's channels and `keepVectorShapeBlocks` keeps a shape through conversions.
  Inks read from CMYK files live on `StyleColor::ink`, `TextRun::ink` / `LayerText::ink` and `VectorShape::ink`,
  used while their RGB still matches (`inkMatches`, `textInkMatches`).
- Tests: `colormodes_render_tests`, `psd_modes_tests`, `style_modes_tests`, `paint_modes_tests`, `transform_modes_tests`, `select_modes_tests` (the wand, Quick Select and loading a CMYK layer as a selection), CMYK and Lab scenes in `render_hash_tests`, and
  `psd_roundtrip` (CMYK and Lab files must reopen in their mode with every layer channel byte for byte).

## 日本語

ドキュメントのカラーモードは Photoshop と同じく 1 つです:**RGB カラー**、**CMYK カラー**、**Lab カラー**。それぞれ
8 bit または 16 bit/チャンネルです(Photoshop と同じく 32 bit の CMYK・Lab はありません)。

### イメージ ▸ モード

**イメージ ▸ モード ▸ RGB カラー、CMYK カラー、Lab カラー** はドキュメントを変換します(1 回の取り消しで元に戻ります)。

- すべてのレイヤーのピクセルを、CMYK は**作業用 CMYK**、Lab は **Lab D50**、RGB は**作業用スペース**に変換します。
  マッチング方法と黒点の補正は、カラー設定の**変換オプション**(既定は Photoshop と同じ「相対的な色域を維持」と黒点の
  補正)を使います。墨版の生成は CMYK プロファイルに従います。
- 値として持つ色(テキスト、シェイプの塗りと線、アートボードの背景、レイヤースタイルの色とグラデーション、レンズ
  フィルターとグラデーションマップの色、**描画色と背景色**)も変換します。CMYK への変換では印刷の色域に収まります。
- **レベル補正とトーンカーブはチャンネルごとの設定がリセット**されます(複合チャンネルの設定は残ります)。
- **新しいモードにない調整レイヤー**(CMYK と Lab の自然な彩度・白黒、Lab の色相・彩度・カラーバランス・特定色域の
  選択・チャンネルミキサー、CMYK の露光量)は**非表示にして印を付けて残し**、そのレイヤーがあるモードに戻すと元どおり表示されます。
- マスク、アルファチャンネル、スポットカラーチャンネル、選択範囲は変わりません。

### CMYK・Lab ドキュメントの表示

- ピクセルレイヤー、不透明度、ピクセルマスクとベクトルマスク、クリッピングマスク、グループ、アートボード、
  テキスト、シェイプレイヤーとその線、グラデーション・パターンの塗りつぶしレイヤー、レイヤースタイルを 8/16 bit で
  合成します。
- **描画モード**:CMYK ではすべてのモードを使えます。分離可能なモードは各インキに RGB と同じ計算で適用します。
  色相・彩度・カラー・輝度・カラー比較(暗)・カラー比較(明)も Photoshop と同じ計算で、RGB を経由しません
  (Photoshop の値と 8 bit の 2 階調以内で一致)。Lab では覆い焼きカラー・焼き込みカラー・比較(暗)・比較(明)・
  差の絶対値・除外・減算・除算は使えません(Adobe の説明のとおり)。
- 調整レイヤーは、そのモードで Photoshop にある種類をすべてドキュメント自身の値で描画します(下の「色調補正と
  フィルター」)。3DLUT ファイルのカラールックアップは保持して PSD に書き戻しますが、3DLUT は RGB 専用のため描画しません。レイヤースタイルは
  ドキュメントのチャンネルで描画します(下の「テキスト、シェイプ、レイヤースタイル」)。
- カンバスは常にドキュメントのプロファイルからモニタープロファイル(不明なら sRGB)へ変換して表示します。

### 色調補正とフィルター

**イメージ ▸ 色調補正**、**調整レイヤー**、**フィルター**メニューは CMYK と Lab(8/16 bit)でも使え、インキまたは
L・a・b をそのまま変更します。RGB に変換して戻すことはありません。各種類は Photoshop がそのモードで提供するものだけで、
Photoshop にないものは今後も使えません(「CMYK モードでは使用できません」)。

| 種類 | CMYK | Lab |
|---|---|---|
| レベル補正、トーンカーブ | 複合チャンネルと各インキ(シアン・マゼンタ・イエロー・ブラック) | 明度・a・b(複合チャンネルなし) |
| 明るさ・コントラスト | 各インキ | 明度 |
| 階調の反転、ポスタリゼーション | 各インキ | L・a・b |
| 2 階調化、グラデーションマップ | 色の明度で判定 | L で判定 |
| レンズフィルター | 可 | 可 |
| 露光量 | 使用不可(Photoshop にない) | L に適用 |
| 色相・彩度、カラーバランス、特定色域の選択、チャンネルミキサー | 可 | 使用不可(Photoshop にない) |
| 自然な彩度、白黒 | 使用不可(Photoshop にない) | 使用不可(Photoshop にない) |
| カラールックアップ | ICC 抽象プロファイル、CMYK のデバイスリンク | ICC 抽象プロファイル、Lab のデバイスリンク |

- **レベル補正とトーンカーブ**はヒストグラムに表示されるとおりの各チャンネルに適用します。CMYK の版はインキのある所が
  暗いので、Photoshop と同じくレベル補正の入力の黒を上げるとインキが増えます。CMYK の複合チャンネルは各インキの設定の
  後にすべてのインキに適用します。Lab には複合チャンネルがなく、チャンネルは明度から始まり、a と b にはそれぞれの
  カーブがあります。自動補正とスポイトは今のところ RGB のみです。
- **階調の反転**(Ctrl+I)はすべてのインキ、または L・a・b を反転します。
- **2 階調化とグラデーションマップ**は明度で判定します(Lab は L、CMYK はプロファイルを通して読んだ L*。判定のみで、
  ピクセルは変換しません)。2 階調化の黒はブラシと同じプロファイルの黒です。グラデーションマップの色はプロファイルで
  一度変換し、ドキュメントのモードで補間します。
- CMYK の**色相・彩度とカラーバランス**は、保存されたシアン・マゼンタ・イエローを RGB の赤・緑・青と同じように扱い、
  ブラックの版はそのままです。**特定色域の選択**は色域ごとに各インキを動かし、ブラックはブラックの版を動かします。
  CMYK の**チャンネルミキサー**は 4 つのインキの出力(4 インキと定数から)を持ち、モノクロはブラックの版だけを作ります。
- **レンズフィルター**の色はプロファイルで変換し、CMYK では各インキに重ね、Lab では a と b(輝度を保持しないときは L も)
  を動かします。Lab の**露光量**は相対輝度を通して L を変えます。
- **カラールックアップ**は ICC の**抽象プロファイル**(ドキュメントのプロファイルから通して戻すので、見た目の色に
  作用します)か、ドキュメントのカラースペースどうしの**デバイスリンク**(CMYK ドキュメントでは CMYK→CMYK のリンク。
  Photoshop でデバイスリンクを適用する一般的な方法です)を使えます。**3DLUT ファイル**(.cube、.3dl)は RGB
  ドキュメント専用です。CMYK・Lab で読み込むとその旨を表示し、RGB から持ち込んだ 3DLUT のレイヤーは保持しますが
  描画しません。
- **フィルター**:ぼかし(ガウス)、ぼかし(移動)、レンズ補正はすべての値を RGB と同じように扱います。ノイズを加えるは
  各インキ、または L・a・b にノイズを加え、グレースケールノイズはすべてのインキに同じノイズ(Lab では L のみ)を加えます。
- 各モードに合わせた RGB の計算式で、Photoshop の出力との照合はまだです。PSD のレベル補正とトーンカーブは同じ
  チャンネルに対応し(CMYK のブラックは 5 番目)、CMYK ファイルのチャンネルミキサーは 4 つのインキの行を読み込みます。

### チャンネル

チャンネルパネルには **CMYK、シアン、マゼンタ、イエロー、ブラック**(Ctrl+2、Ctrl+3〜6。アルファチャンネルは Ctrl+7 から)
または **Lab、明度、a、b**(Ctrl+2〜5)が並びます。1 つだけ表示するとグレー(CMYK の版はインキを暗く、a と b は中間でグレー)、
複数の CMYK のインキは白地にインキの色で表示します。Ctrl+クリック(または Alt+Ctrl+数字)で、CMYK のチャンネルはそのインキを、
Lab のチャンネルはその値を選択範囲として読み込みます。一部のカラーチャンネルを選んでいるとき、
**塗りつぶし**はそのチャンネルだけに適用されます。

### ペイント

ペイントツールはドキュメント自身の値に塗ります。CMYK のドキュメントではインキを、Lab のドキュメントでは L・a・b を塗り、
RGB で塗ってから変換することはありません。

- **ブラシと消しゴム**(円形ブラシ先端と読み込んだブラシ先端)。ピクセル、マスク、クイックマスクに使えます。選んだ色
  (このモードでは sRGB で持ちます)は、カラー設定の変換オプションでドキュメントのプロファイルを通して先に変換します。
  CMYK ではプロファイルの墨版生成による 4 色のインキ(同梱の ISO Coated v2 の黒は K だけでなく約 77/69/60/93% の
  リッチブラック。Photoshop と同じ)、Lab では L・a・b になります。
- **グラデーションツール**:各分岐点を変換し、分岐点の間はドキュメントのモード(インキ、または L・a・b)で補間します。
- **コピースタンプ**はドキュメント自身の値をコピーし、移動ツールでの**選択ピクセルの移動と複製**もすべての値を運びます。
  **下のレイヤーと結合**、**レイヤーを結合**、**レイヤーマスク ▸ 適用**も同じです。
- **レタッチ**(どちらのモードでもドキュメント自身の値に適用):
  - **スポット修復ブラシ、修復ブラシ、パッチ**:Lab では RGB の赤・緑・青と同じように L・a・b を修復します。CMYK では
    5 つの値すべて(K を含む各インキ)を修復します。修復元の選択は、版の見た目を 8 bit にしたもの(C・M・Y それぞれの
    値に K を掛けたもの)で判断するだけで、ピクセルは変換しません。コンテンツに応じた合成は RGBA で動くため、CMYK の
    「コンテンツに応じる」は近傍で最もよく合うパッチを(近似色に合わせるより広く探して)コピーします。
  - **ぼかし・シャープ・指先・ゆがみ**はすべての値(CMYK では 5 つ)をぼかし、シャープにし、運び、再サンプルします。
  - **覆い焼き・焼き込み**:Lab では範囲のカーブに沿って L を動かし、a・b は変えません(保護トーンは Lab では影響
    しません)。CMYK では各版の明るさ(インキの反転)を同じカーブで動かすため、覆い焼きはインキを減らし、焼き込みは
    ブラックを含めてインキを増やします。**スポンジ**は Lab では a・b を拡大縮小し(彩度を上げると 2 倍、下げると 0)、
    CMYK では RGB のスポンジが補色に行うのと同じようにシアン・マゼンタ・イエローに適用し、ブラックの版は変えません。
  - **塗りつぶしツール**は塗る範囲をドキュメント自身の値(表示されている画像、またはレイヤーのみ)で決めます。
    Photoshop と同じく、アルファを含むすべての値がクリックしたピクセルから許容値以内のピクセルを塗ります。色は
    ブラシと同じくプロファイルを通して変換します。
- **スポイトツール**は合成画像のインキまたは L・a・b を読み(`color.sample` が返します)、描画色にはそれをプロファイルで
  sRGB に変換した色を設定します。
- **選択範囲**:選択範囲メニュー、クイックマスク、マスクやチャンネルの読み込み、境界線の変形、レイヤーのピクセルからの
  選択範囲の読み込み(サムネールを Ctrl+クリック。CMYK レイヤーのアルファは 5 番目の値)が CMYK と Lab でも使えます
  (選択範囲は色ではなく範囲です)。
- **自動選択ツールとクイック選択ツール**は L\*a\*b\* で判定します。Lab ドキュメントはその L・a・b を、CMYK ドキュメントは
  合成画像(または作業中のレイヤー)をプロファイルで Lab に変換したものを判定にだけ使い、ピクセルは変換しません。
  許容値は L・a・b それぞれの 8 bit の階調で数え、Photoshop の自動選択と同じくチャンネルごとに比べます。クイック選択の
  ストロークも同じ L・a・b を使い、クリックで選択するモデル(sRGB で学習)には合成画像を sRGB に変換して渡します。
- **MyPaint** のプリセットは今後も使えません(「CMYK モードでは使用できません」)。libmypaint は RGB で混色し、インキや
  Lab を持たないため、RGB で塗って変換するしかなく、NekoPhoto はそうしないからです。

### テキスト、シェイプ、レイヤースタイル

テキスト、シェイプレイヤー、パス、ベクトルマスク、レイヤースタイルは CMYK と Lab(8/16 bit)でも RGB と同じように
使えます:横書き文字ツールとテキストの編集、シェイプ・ペン・パス選択ツール、ライブシェイプ、パスの塗りつぶしと
境界線、レイヤー ▸ ベクトルマスク、レイヤースタイルダイアログ、スタイルのコピー・ペースト・消去、スタイルの
プリセット、それぞれの自動化メソッド。どの編集も 1 回の取り消しで戻ります。

- **レイヤースタイル**:すべての効果(ドロップシャドウ、シャドウ(内側)、光彩(外側)・(内側)、ベベルとエンボス、
  サテン、カラー・グラデーション・パターンオーバーレイ、境界線)を、レイヤーにもグループにも描画します。効果の形
  (ぼかし、スプレッドとチョーク、距離、ベベルの高さ)は RGB と同じで、変わるのは色です。効果の色、グラデーション、
  パターンはドキュメントのプロファイルで一度だけ変換し(カラー設定の変換オプション)、ドキュメント自身のチャンネルで
  そのモードの描画モードで合成します:CMYK はインキごと(乗算でインキが増えます)、Lab は L・a・b をそのまま、
  色相・彩度・カラー・輝度・カラー比較(暗・明)はそのモードのレイヤーと同じ計算です。Lab では効果でも覆い焼き
  カラー・焼き込みカラー・比較(暗)・比較(明)・差の絶対値・除外・減算・除算は使えず、ダイアログでは選べず、
  ファイルで指定されていれば通常として描画します。CMYK ファイルでインキとして指定された色('CMYC')はそのインキの
  まま描画し、変更するまでインキのまま書き戻します。分岐点がすべてインキのグラデーションはインキからインキへ
  補間します。
- **テキスト**は RGB と同じくカンバスまたはテキストダイアログで入力します。Qt がグリフを sRGB の 16 bit で描き、
  それぞれの色をプロファイルで一度変換し、グリフの被覆はそのまま残すので、テキストはドキュメントのチャンネルに
  なります(CMYK ファイルでインキとして指定された色はそのインキのまま)。色がインキの CMYK ファイルの Photoshop
  テキストレイヤーは編集可能なテキストとして開き、インキのままテキストとして書き戻します。
- **シェイプレイヤー**はドキュメントのチャンネルで塗ります:単色はプロファイルを通した色(CMYK ファイルのインキ色は
  そのインキ)、インキのグラデーションはインキからインキへ、RGB のグラデーションとパターンは sRGB で描いて変換します。
  PSD にはシェイプレイヤーとして書き出し、イメージ ▸ モードやプロファイル変換の後もシェイプレイヤーのままです。
  **パスの塗りつぶし**と**パスの境界線**は描画色をブラシと同じくプロファイルで変換して塗ります。
- 確認:CMYK・Lab で描いたドキュメントと、同じドキュメントを RGB で描いて変換したものを、各効果単独とすべての組み合わせで
  8/16 bit で比べました(`style_modes_tests`)。平均の差は CMYK で最大 1.7、Lab で最大 1.1(sRGB の階調)、最大の差は
  離れた 2 色が柔らかい境界で混ざる所で、インキや L・a・b での合成は RGB とは混ざり方が違います。Patchy の
  `photoshop-cmyk-style-colors.psd`(Photoshop で保存した CMYK)はカラーオーバーレイをファイルのインキどおり
  (42/45/67/13%)、赤い文字(0/100/100/0%)もインキどおりに描画し、テキストレイヤーは編集可能なテキストとして
  開きます。このファイルには合成画像がないため、CMYK・Lab の効果を Photoshop の描画と比べることはできていません。
  Lab の分離可能な描画モードで a と b を Photoshop がどう合成するか(NekoPhoto は値のまま)は、Lab のレイヤーと
  同じく未確認です。

### Photoshop ファイル

CMYK(モード 4)と Lab(モード 9)の PSD は 8/16 bit のまま**そのモードで開き**、CMYK のプロファイル(リソース 1039)も
読み込みます。保存するとドキュメントのモード・ビット数・プロファイルで書き出し、変更していないレイヤーは**バイト単位で
そのまま**戻ります。ダブルトーン、マルチチャンネル、インデックスカラー、モノクロ 2 階調、グレースケールは従来どおり
RGB に変換します。Photoshop で保存された CMYK ファイル(Patchy の `photoshop-cmyk-style-colors.psd`)はバイト単位で
そのまま戻り、自作した 8/16 bit の CMYK・Lab ファイル(`tests/psd_modes_tests.cpp`)は別の実装で読み直して確認しています。

### 変形、クリップボード、読み込み

- **画像解像度、切り抜き、トリミング、切り抜きツール、選択範囲で切り抜き、カンバスサイズ、カンバスの反転**は両方の
  モードの 8/16 bit で使えます。再サンプル(画像解像度、自由な形に、遠近法、**編集 ▸ ワープ**と**ワープケージ**、
  **選択ピクセルの自由変形**)は RGB のサンプラーがチャンネルを扱うのと同じようにすべての値を扱います。CMYK では
  アルファを含む 5 つ、Lab では L・a・b(a と b は近傍の平均になり、中間に寄りません)。RGB で再サンプルすることは
  ありません。単色のインキはそのままのインキです。
- **カット、コピー、結合部分をコピー、ペースト、コピーしたレイヤー**は、同じドキュメントや同じモード・プロファイルの
  ドキュメントの間ではドキュメント自身の値のままです。モードが違うときは Photoshop のペーストと同じく、カラー設定の
  変換オプションでプロファイルを通して変換します(RGB から CMYK は印刷の色域へ、CMYK や Lab から RGB、Lab と CMYK の
  相互)。コピーしたレイヤー(選択範囲なしのコピー)はイメージ ▸ モードと同じように変換します。ほかのアプリには
  8 bit の sRGB を渡し、ほかのアプリからのピクセルは sRGB として受け取ります。
- **ファイル ▸ 読み込み**は埋め込みプロファイル(なければ sRGB)からドキュメントのプロファイルへ一度だけ変換します。
- 「ペースト(選択範囲内)」と「カットしたレイヤー」はどのモードにもまだありません。

### 書き出し

CMYK・Lab ドキュメントの PNG、JPEG、WebP、TIFF、TGA、ICO、GIF への書き出しは、Photoshop の「書き出し形式」と同じく
合成画像をドキュメントのプロファイルで sRGB に変換したもの(モニタープロファイルなしのカンバス表示と同じ変換)で、
プロファイルなし(sRGB)で書き出します。16 bit のドキュメントは PNG と TIFF を 16 bit で、ほかの形式はディザーで
8 bit にして書き出します。PSD はドキュメントのモードのままです。**アートボードの書き出し**、**スライスの書き出し**
(PNG、JPEG、WebP、TIFF)と **SVG** も同じく、各領域や画像をプロファイルで sRGB に変換します。SVG のシェイプの色は
ドキュメントでの見た目(CMYK の塗りはそのインキ、RGB の色は印刷の色域に入った色)を sRGB で書きます。

### スマートオブジェクト

CMYK・Lab ドキュメントでも、8 bit・16 bit ともに Photoshop と同じくスマートオブジェクトを使えます。

- RGB のファイル(PNG、JPEG、RGB の PSD、Camera Raw の「オブジェクトとして開く」で開いた RAW)の**埋め込みで配置**と
  **コンテンツを置換**:内容をそれ自身のプロファイル(なければ sRGB)からドキュメントのモードへ一度だけ変換し、
  すべてのインスタンスが変換後のピクセルを共有します。移動や拡大・縮小はそれを再サンプリングします。
- **スマートオブジェクトに変換**はドキュメントと同じモード・プロファイルの PSB を作ります。レイヤーの見た目は
  そのままで、**コンテンツを編集**はそのモードのタブで開き、保存するとそのモードの PSB として戻します。
- 同じモード・プロファイルのドキュメントに置いた CMYK・Lab の内容はインキ(または L・a・b)をそのまま使い、
  別のモードやプロファイルのドキュメントではプロファイルを通して変換します。
- **ワープ**、ワープのケージ、**自由変形**はすべての値をメッシュに通して描画します。
- **スマートフィルター**はドキュメントの値で動きます(Lab は L・a・b をそのまま、CMYK は 4 つのインキでブラックも
  ほかと同じように扱います)。**ラップ**以外のすべてのスマートフィルターを使えます。ラップはフィルターギャラリーの
  フィルターで、Photoshop でも RGB 専用です。フィルターマスクと PSD 内のフィルターキャッシュはドキュメントの
  チャンネルを持ちます。
- **ラスタライズ**は表示どおりのピクセルを残します。
- PSD とプロジェクトはインスタンスを編集可能なまま保ちます。プロジェクトを開くと、CMYK・Lab の PSB の値は
  ファイルから読み直します。

### 未対応・対象外

背景を削除、コンテンツに応じた塗りつぶし・移動・拡大・縮小、雲模様 1・2(sRGB の色で描くため)は CMYK・Lab では使えないままです(「CMYK モードでは
まだ使用できません」と表示)。CMYK の JPEG と TIFF(CMYK ドキュメントは sRGB で書き出します)もまだです。Camera Raw、
G'MIC、MyPaint ブラシは RGB 専用で(「CMYK モードでは使用できません」)、**Mosh** も同じです。Mosh は RGB の色に
かけるグリッチ効果で Photoshop に対応する機能がなく、RGB を経由して戻すことはしません。
