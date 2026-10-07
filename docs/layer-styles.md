# Layer styles

A PSD's layer styles (the effects Photoshop keeps in a layer's 'lfx2' block, 'lmfx' for several of one kind,
'lfxs' on folders) are drawn around the layer, in the editor and in every export. They come from the file as it
was carried (psd-roundtrip.md) and go back unchanged on PSD export, and Layer ▸ Layer Style edits them.

- Model and parsing: `src/core/include/compositor/layerstyle.h`, `src/core/src/layerstyle.cpp` (with the global
  light from image resources 1037/1049, the effects scale, the 'fxrp' reference point and the file's patterns).
- Drawing: `src/core/src/layerstyle_render.cpp`, called by the renderer for a styled layer (`drawOwn`), a
  clipping base with a style, and a styled folder (exterior effects before its first child, the rest after its
  last, over the shape of its children drawn alone).
- Colour modes: at 8, 16 and 32 bits in RGB, and at 8 and 16 bits in CMYK and Lab, where the effects' colours go
  through the document's profile and they blend in its own channels (color-modes.md, "Text, shapes and layer
  styles").

- Editing: Layer ▸ Layer Style (and the Layers panel's menu) opens Photoshop's dialog: Blending Options and the
  ten effects, each with its switch; a page edits the effect's first instance, further ones are kept. Copy,
  Paste and Clear Layer Style are beside it; automation has `layers.style` and `layers.setStyle`. An edited
  style is written as a fresh 'lfx2' in Photoshop 2026's descriptor shapes (`src/core/src/layerstyle_write.cpp`,
  Patchy's authoring) into the layer's carry, so drawing, PSD export and the project package take it as they
  take a carried one; a style the dialog left as it was keeps the file's bytes. Effects switched off stay in
  the style with their settings, as in Photoshop. What the model does not hold is lost on an edit: a gradient's
  noise form, Satin's and the shadows' contours and noise, a pattern's name (its id is kept).
  `build/tests/restyle_check` rewrites every styled layer in a corpus and checks the render is unchanged and the
  new block reads back the same (Patchy's fixtures: 166 styles, all identical).

The renderer follows Patchy (MIT, `src/third_party/patchy_psd/README.md`), which calibrated each effect against
Photoshop 2026. Its mask machinery is ported: the tent blur, spread and choke as grayscale dilation, exact
Euclidean distance fields, the stroke band measured from the matte's subpixel half-coverage contour, the bevel
height field and Lambert split. So are its compositing rules: effects fold their alpha into the colour for the
burn and dodge modes; a layer and its shadow or glow add up against the backdrop (the shape knocks the effect
out, "Layer Knocks Out Drop Shadow" included); overlays stack pattern, gradient, colour, then satin, inner
glow, inner shadow, stroke, bevel; with Blend Interior Effects as Group off (the default) the layer's mode
blends its own pixels and the interior effects land on that, with it on they fold into the layer's colour; a
Stroke with Overprint off knocks the layer's content out of its band, even at 0% opacity; a clipped layer is
masked by the base's pixels, never its effects; with Fill below 100% the overlays are their own passes.

Bevel & Emboss goes past Patchy in three places, each fitted to Photoshop's merged image of a Patchy fixture
(`build/tests/psd_composite_oracle`):

- **Soften blurs the shading, not the surface** (Photoshop's own description: it "blurs the results of shading"):
  the signed light (highlight above zero, shadow below), after the Gloss Contour, goes through a tent of the soften's
  width and then a [1 2 1], and is split into highlight and shadow afterwards. A flat face's Gloss Contour wash goes
  into the blur too. `photoshop-bevel-gloss` (an Outer Bevel with Soften 7 and a cornered, non-monotone gloss)
  went from 2.8 levels off on average to 0.03, no pixel more than 2 off; blurring the height field instead (as
  before) left a shadow on the lit side and a saturated one on the other.
- **An anti-aliased Contour or Gloss Contour is supersampled**: the surface is shaded at nine points of each pixel,
  a third of a pixel apart starting at its corner (bilinear between pixels), and the nine signed lights averaged. A
  curve without Anti-aliased, and every linear curve, is shaded once per pixel as before. The anti-aliased Contour of
  `photoshop-bevel-subs` went from 5.2 levels off to 0.8 (centred sampling does not match: Photoshop's rings sit a
  third of a pixel up and left of them).
- **An Outer Bevel's texture comes out the other way up**: there, Invert with a negative depth raises the light
  texels (`photoshop-bevel-subs`); on an Inner Bevel the two cancel as before.

## Blend If

Blending Options' Blend If is drawn and editable (`src/core/include/compositor/blendif.h`). A layer, an adjustment
layer or a folder shows only where its own colours (This Layer) and the colours under it (Underlying Layer) fall
within a black and a white point per channel; Alt-drag splits a point in two, and between the halves the layer fades.
The channels are Photoshop's: Gray, Red, Green and Blue in RGB; Gray, Cyan, Magenta, Yellow and Black in CMYK;
Lightness, a and b in Lab.

- Drawing follows Photoshop 2026 as Patchy calibrated it: the channels' gates multiply, This Layer's multiplies
  Underlying Layer's; byte v in a split black range [a, b] keeps (v - a + 1) / (b - a + 1) for a <= v < b (the white
  side mirrors it; joined points cut hard); Gray is (299 R + 590 G + 111 B) / 1000, rounded; a transparent backdrop
  always passes. An adjustment layer reads This Layer on its adjusted colours and Underlying Layer on the colours
  before it. A folder with Blend If isolates (Pass Through too) and its result is gated against what is under it. A
  clipping base's ranges gate the clipped result, not the shape the clipped layers take. Only the layer's own pixels
  are gated, as in Photoshop: its exterior effects outside them are not, and its interior effects (overlays, satin,
  inner glow and shadow, stroke, bevel) land ungated on the gated pixels. With Blend Interior Effects as Group the
  interiors join the pixels and are gated with them, and a shape layer's own stroke keeps the gate over the whole
  draw (no Photoshop-saved file here combines Blend If with effects: unverified). `blendif_tests` pins it.
- Depths and modes: the gates read 8-bit levels at every depth (16-bit samples rounded to a level, 32-bit ones
  through the document's curve). CMYK reads the stored values (255 is no ink) and its Gray from the complements of C,
  M and Y darkened by K; Lab reads L, a and b as stored. Neither has a Photoshop-saved fixture: unverified.
  Converting the colour mode keeps the Gray range (Lab reads it on Lightness) and resets the per-channel ones.
- Editing: Layer ▸ Layer Style ▸ Blending Options (the channel menu and the two sliders), Copy and Paste Layer Style,
  and automation's `layers.set` `blendIf` (any layer kind, folders and adjustment layers included; `layers.get`
  reports it). The dialog does not open on adjustment layers yet. An edit patches the record's ranges (a transparency pair the
  file had is kept); ranges never edited go back to a PSD byte for byte; projects keep them.
- How close: Patchy's `photoshop-blend-if-4b` (a layer with Gray and per-channel split ranges on both sliders, a
  Levels adjustment layer and a folder) matches Photoshop's render within 2 levels, mean 0.52 per channel (it was
  106 levels off when the ranges were ignored); `build/tests/blendif_tests` checks it when Patchy is beside the
  checkout.

## Gradient methods

Gradients follow Photoshop's Method (gradient overlays, layer-style strokes, gradient fill layers and shape strokes,
the Gradient tool and Gradient Map; the Layer Style dialog, the Gradient tool's options bar and the Gradient Map
editor have the menu): Classic interpolates the stored sRGB values, Linear interpolates
in linear light, and Perceptual in Oklab, each with the gradient's smoothness applied in its own space (between two
stops too; Classic smooths only past two stops, as before). Colour stops, midpoints and opacity stops work as in
Classic; opacity interpolates linearly in every method. The model follows PhotoCraft's (THIRD-PARTY-NOTICES.md),
fitted to Photoshop's composites, and is checked against two Photoshop-saved files of ag-psd's tests (MIT), whose
merged images it now matches (mean / max level difference): a Perceptual gradient overlay 0.57 / 4 (8.1 / 20 drawn
as Classic), a Linear one 0.08 / 1 (2.2 / 12 before). `build/tests/gradient_method_tests` re-measures them with
`AGPSD_FIXTURES` set to ag-psd's `test` folder, and runs every method through a PSD at 8 and 16 bits in RGB, CMYK
and Lab. The method is written as Photoshop writes it: the `gradientInterpolationMethodType` enum `Gcls`, `Perc` or
`Lnr ` (under `gs99` in a gradient overlay, `gradientsInterpolationMethod` in a stroke and a gradient fill), as in
those files; the lower-case `perceptual` and `linear` some other writers use are read too.

The Gradient tool and Gradient Map draw Perceptual and Linear from the same model, the ramp sampled at 257 even
stops (within a level of the overlay's colours; Classic draws exactly as before, and stays the default, while
Photoshop's tool now defaults to Perceptual). Gradient Map reads its method from a Photoshop file's 'grdm' (version 3
holds the enum's code after Dither: ag-psd's Photoshop-saved Perceptual map is read as Perceptual). NekoPhoto's Gradient
Map is a two-colour ramp (a file's first and last stops), so an unedited map goes back to PSD as its own 'grdm', byte for
byte, and an edited one is written as pixels as before; projects keep the method. Automation:
`gradient.draw` `interpolation`, and `interpolation` in a Gradient Map's `gradientMapSettings`.

Unverified: a CMYK fill whose stops are inks interpolates the inks in every method (no Photoshop-saved CMYK file
with Linear or Perceptual exists here); no Photoshop render of the Gradient tool or of a two-stop Gradient Map in
Perceptual or Linear is here to compare with (they share the overlay's measured model).

## How close

Against Photoshop's own renders of Patchy's fixtures (`../Patchy/test-fixtures/psd/*.bmp`), mean difference per
pixel on a 0-255 scale (the merged image where the file has one: an Outer Bevel with Soften and an anti-aliased
gloss, `photoshop-bevel-gloss`, is 0.03):

| Effect | Mean |
|---|---:|
| Inner shadow, interior/exterior blending, pattern overlay (anchor, transparency) | 0.00-0.02 |
| Shadow knockout, inner glow range, outer glow range, gradient overlay geometry, smooth bevel, stroke overprint | 0.02-0.08 |
| Pillow emboss, emboss styles | 0.08-0.13 |
| Inner glow, bevel texture, Shape Burst stroke, gloss contour, stroke on antialiased edges | 0.3-0.9 |
| Bevel with an anti-aliased non-monotone contour | 0.8 |
| Outer glow, overlay z-order, pattern scale | 1.1-1.7 |
| Styled folders | 0.00-2.6 |
| Outer bevel with a texture | 5.2 |

A designer's client PSD (three smart objects, eight styled layers, a gold Multiply colour overlay carrying the
whole design) renders within 1.7 levels of Photoshop's own composite, no pixel off by more than 8.

Not drawn yet: contours on shadows and glows (they draw as Linear), noise and jitter, Dissolve (drawn as
Normal), "Layer Mask Hides Effects". Vector masks and shape layers are drawn too (vector-masks.md).

Folders follow Photoshop: a folder in any mode but Pass Through isolates its children and composites their
result in its mode and opacity; a Pass Through folder below full opacity fades its result back toward what was
under it. With that, the styled-folder fixtures match Photoshop at 0.00-0.02 and the folder opacity fixture at 2.1
(it was 14.9).

## Speed

Each styled layer works only within its bounds plus the effects' reach: the 1500 x 1500 client file renders in
60 ms, a 1745 x 1158 label sheet with five styled layers in 11 ms (156 ms before effects were bounded to their layer). A styled layer above the one being
edited is drawn each frame (effects blend in their own modes, so it cannot join the flattened layers above).
