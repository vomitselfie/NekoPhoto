# Layer styles

A PSD's layer styles (the effects Photoshop keeps in a layer's 'lfx2' block, 'lmfx' for several of one kind,
'lfxs' on folders) are drawn around the layer, in the editor and in every export. They come from the file as it
was carried (psd-roundtrip.md) and go back unchanged on PSD export, and Layer ▸ Layer Style edits them.

- Model and parsing: `src/core/include/compositor/layerstyle.h`, `src/core/src/layerstyle.cpp` (with the global
  light from image resources 1037/1049, the effects scale, the 'fxrp' reference point and the file's patterns).
- Drawing: `src/core/src/layerstyle_render.cpp`, called by the renderer for a styled layer (`drawOwn`), a
  clipping base with a style, and a styled folder (exterior effects before its first child, the rest after its
  last, over the shape of its children drawn alone).

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

## How close

Against Photoshop's own renders of Patchy's fixtures (`../Patchy/test-fixtures/psd/*.bmp`), mean difference per
pixel on a 0-255 scale:

| Effect | Mean |
|---|---:|
| Inner shadow, interior/exterior blending, pattern overlay (anchor, transparency) | 0.00-0.02 |
| Shadow knockout, inner glow range, outer glow range, gradient overlay geometry, smooth bevel, stroke overprint | 0.02-0.08 |
| Pillow emboss, emboss styles | 0.08-0.13 |
| Inner glow, bevel texture, Shape Burst stroke, gloss contour, stroke on antialiased edges | 0.3-0.9 |
| Outer glow, overlay z-order, pattern scale | 1.1-1.7 |
| Styled folders | 0.00-2.6 |
| Bevel with a non-monotone contour | 6.1 |

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
