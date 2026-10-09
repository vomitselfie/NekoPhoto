# 16 and 32 bits per channel

**English** · [日本語](#日本語)

A document is 8, 16 or 32 bits per channel, as in Photoshop: every layer, mask and the selection have the same depth.
16 bits keep smooth gradients smooth through blending and masking, where 8 bits can band. 32 bits hold linear light
in floating point, brighter than white included (HDR). Image ▸ Mode ▸ 8 Bits/Channel, 16 Bits/Channel and
32 Bits/Channel switch a document between them, as one undo step; [32 bits per channel](#32-bits-per-channel) below says
what a 32-bit document can do so far.

This page says what works in a 16-bit document (everything NekoPhoto does) and how each part keeps its 16 bits. The
design is in [high-bit-depth-plan.md](high-bit-depth-plan.md).

## Getting a 16-bit document

- **Image ▸ Mode ▸ 16 Bits/Channel** converts the open document. Converting an 8-bit document is exact: back to
  8 bits, every pixel is what it was.
- **Opening a 16-bit file**: a 16-bit RGB or grayscale Photoshop file (PSD, PSB), a 16-bit PNG, or a 16-bit TIFF
  opens as a 16-bit document (a 32-bit Photoshop file opens at 32 bits, see below).
- **Projects** keep their depth: a 16-bit project stores its layers and masks as 16-bit PNGs (project format
  version 8, [project-format.md](project-format.md)). Older projects open as 8-bit.

Like Photoshop, NekoPhoto holds 16-bit samples in the range 0 to 32768, so blending is exact integer maths.
Files store 0 to 65535; the values are mapped when a file is read and written.

## What works at 16 bits now

- **Rendering**: every blend mode, opacity, layer and folder masks (at 16 bits), vector masks, clipping masks,
  folders (Pass Through and isolated, faded by their opacity), artboards, shape layers and fill layers, Dissolve,
  adjustment layers and layer styles. Vector masks and shapes are rasterised with 32,768 steps of edge coverage, and
  gradient fills are drawn from the gradient's exact colours, so a long ramp has no 8-bit steps.
- **Adjustments**, on pixels (Image ▸ Adjustments, Invert) and as adjustment layers: Levels, Curves, Hue/Saturation,
  Exposure, Gradient Map, Grain, Brightness/Contrast, Vibrance, Color Balance, Black & White, Photo Filter, Channel
  Mixer, Selective Color, Posterize, Threshold, Color Lookup and Invert. Each works on the exact colour, so a smooth
  16-bit gradient stays smooth through a strong Levels or Curves. Posterize and Threshold choose their steps as the
  8-bit ones do; the Levels histogram has 256 bins at either depth.
- **Filters**: every filter in the Filter menu: the blurs (Gaussian, Motion, Box, Radial, Surface), Add Noise (the same
  pattern for the same seed), Dust & Scratches, Median, Unsharp Mask, High Pass, Emboss, Mosaic, Find Edges, Clouds and
  Difference Clouds, the distortions (Twirl, Pinch, Spherize, Wave, Ripple, Polar Coordinates, ZigZag, Shear), Maximum,
  Minimum, Offset, Lens Correction and Mosh.
- **Camera Raw Filter**: every panel. Its kernels run on float colour with no rounding between the steps (the 8-bit
  filter rounds to a byte after each), and the result is rounded to 16 bits once, so a smooth 16-bit gradient keeps its
  steps through Exposure and the curves. Defringe decides which pixels are fringe on their colour rounded to 8 bits, so
  a colour at the edge of a hue window is treated as at 8 bits. On an 8-bit image converted to 16 bits, each panel lands
  within a level of the 8-bit filter; every panel at once lands two levels off on 0.19% of samples, the 8-bit
  filter's own rounding after each of its steps.
- **G'MIC**: G'MIC works in float on a 0..255 scale, so a 16-bit layer goes to it as float colour on that scale and
  comes back at 16 bits, in-process through libgmic or through a float file for the `gmic` program; nothing is
  reduced to 8 bits on the way. On an 8-bit image converted to 16 bits, blur, unsharp, sepia and sharpen land within
  a level of the 8-bit result; Solarize over half-transparent pixels lands six levels off on 0.9% of samples, because
  the 8-bit run hands G'MIC their colour rounded to whole levels and Solarize scales by the range of the whole image
  (none on an opaque image).
- **Remove Background**: the segmentation model takes 8-bit input, so the model and the refinement's guide see the
  layer reduced to 8 bits; the mask stays float through the refinement (Refine Edges, matting, cleanup, Shift Edge,
  Contrast) and is laid down as a 16-bit mask, and Clean edge colours works on the 16-bit pixels. On an 8-bit image the
  16-bit mask is within a level of the 8-bit one, with several times as many distinct soft values, and the cleaned
  colours are within a level.
- **Selections**: the Rectangular and Elliptical Marquee, Lasso and Polygonal Lasso, Magic Wand (with Refine Edge and
  the smart wand) and Quick Select tools; Select All, Deselect, Inverse, Expand, Contract, Feather, Smooth and Border;
  Load as Selection from a layer or its mask; Edit in Quick Mask Mode; moving the selection outline; Add Mask from
  Selection. The selection itself is 16-bit, so a feather has 32,768 steps instead of 256. The tools that read the
  image (Magic Wand, Quick Select, Select Subject) read it as the canvas shows it: Tolerance counts 8-bit levels, as
  in Photoshop.
- **Editing pixels**: Fill (with the foreground or background colour) and Clear through the selection; Cut, Copy,
  Copy Merged, Paste and Layer via Copy (other apps get 8 bits on the clipboard; pixels pasted from a document of the
  other depth are converted); Content-Aware Fill, Content-Aware Scale, and Content-Aware Move through automation
  (their patch search and seams are chosen on the pixels rounded to 8 bits, and the pixels they copy are 16-bit);
  Free Transform of selected pixels.
- **Painting and retouching**: the Brush in every engine (the round tip, imported tip brushes and the MyPaint
  presets) and the Eraser, on pixels, on layer masks and in Quick Mask; Clone Stamp and the Healing Brush (they copy
  16-bit pixels), Spot Healing and the Patch tool; Blur, Sharpen, Smudge and Liquify; Dodge, Burn and Sponge; the
  Gradient tool and the Paint Bucket; and moving or duplicating selected pixels with the Move tool. A stroke builds up
  at 16 bits, so a soft brush's edge or a long gradient has thousands of steps where 8 bits have a few dozen. The tools
  that choose what to change read the image as the canvas shows it: the Paint Bucket's Tolerance counts 8-bit levels,
  and Spot Healing and Patch pick the patch they copy on the pixels rounded to 8 bits, as Content-Aware Fill does.
- **Image Size** (each resampling method), **Crop**, the Crop tool, Crop to Selection and **Trim**; **Canvas Size**
  and Flip Canvas.
- **Layers**: new layers and folders, delete, duplicate, group, rename, reorder, move in and out of folders,
  visibility, opacity, blend mode, clipping, the resampling mode, and Merge Down (merged at 16 bits). Deleting a
  clipping base keeps the clipped layers' look, baked into their pixels at 16 bits.
- **Text**: the Type tool, Edit Text, text in several styles, Warp Text, Create Work Path and Convert to Shape. The
  text is painted at 16 bits per channel (Qt's 16-bit raster; its glyph antialiasing has 256 steps, as at 8 bits).
  A 16-bit PSD's type layers open as text and export as Photoshop type layers.
- **Shapes and paths**: the Shape tools (live rectangles and ellipses, polygons, lines, custom shapes) with solid,
  gradient and pattern fills and strokes, path operations, the Pen and Direct Selection tools, the Paths panel, vector
  masks on any layer, Fill Path and Stroke Path (at 16 bits), and Make Work Path from a selection.
- **Layer styles**: the Layer Style dialog and all ten effects (Drop Shadow, Inner Shadow, Outer and Inner Glow,
  Bevel & Emboss, Satin, Color, Gradient and Pattern Overlay, Stroke), on layers and folders, drawn at 16 bits;
  Copy, Paste and Clear Layer Style and style presets. At 1:1 they are within a level of their 8-bit render.
- **Transforming layers**: the Move tool, Free Transform (move, scale, rotate), Distort and Perspective, Edit ▸ Warp,
  Warp Cage, and Flip Layer. A warp bends the 16-bit pixels; the cage previews from an 8-bit copy while you drag.
- **Layer masks**: Reveal All, Hide All, enable and disable, link, invert, apply and delete, and painting them.
- **Smart objects**: Place Embedded, Convert to Smart Object, Edit Contents, Replace Contents, Rasterize, moving,
  scaling, Edit ▸ Warp and the Warp Cage on them. A smart object's contents keep their own depth: an 8-bit PNG placed
  in a 16-bit document stays an 8-bit source (its instances are drawn at 16 bits), and a 16-bit PNG, TIFF or PSB placed
  in an 8-bit document stays 16-bit, so converting the document back to 16 bits later loses nothing. Convert to Smart
  Object in a 16-bit document makes a 16-bit PSB; Edit Contents opens the contents at their own depth.
- **Smart Filters**: every Smart Filter NekoPhoto draws is drawn at 16 bits on the instance: Gaussian, Motion, Box,
  Surface and Radial Blur, High Pass, Median, Dust & Scratches, Unsharp Mask, Add Noise, Mosaic, Emboss and Plastic
  Wrap, with their opacity, blend mode and the shared filter mask. On an 8-bit image each is within a level of its
  8-bit result ([smart-objects.md](smart-objects.md) has the figures). Unsharp Mask needed its own calibration: the
  8-bit filter rounds its blurred copy to whole levels and the amount multiplies that rounding (two to four levels at
  usual amounts), so on colour that is 8-bit colour converted, the blurred copy is taken in 8-bit levels as the 8-bit
  filter takes it; on 16-bit colour proper it is exact, with no 8-bit steps for the amount to magnify.
- **Artboards and the timeline**: artboards with their backgrounds and clipping, the Artboard tool, and the
  timeline's frames, delays, looping and playback.
- **Importing** an image as a layer (it takes the document's depth).
- **Saving and exporting**: projects at 16 bits (a 16-bit smart object source is kept as a 16-bit PNG beside the
  project); Photoshop PSD at 16 bits, smart objects included; PNG at 16 bits; TIFF at 16 bits (through
  Qt's TIFF plugin, which writes 16 bits); JPEG, WebP, TGA, ICO and GIF are 8-bit formats, so they get the document
  dithered down to 8 bits, and the status bar says so. An animated GIF's frames are rendered at 16 bits and dithered
  down the same way. Export Artboards and Export Slices write 16-bit PNGs, or JPEGs dithered down (saying so). SVG
  export embeds the images it needs (pixels, text, styled layers, folder masks) as 16-bit PNGs, so it keeps the 16 bits.
- **Automation**: `image.mode` converts; `document.info` reports `bits`; the selection, `pixels.adjust`,
  `pixels.filter`, `pixels.fill`, `pixels.clear`, the content-aware methods, `image.resize`, `image.trim`,
  `canvas.crop`, `layers.warp`, `layers.setCage`, `brush.stroke` (every tool it takes), `gradient.draw`,
  `pixels.bucket`, `pixels.patch`, `layers.merge`, `text.*`, `shape.draw`, `shape.set`, `paths.*`, `vectorMask.*`,
  `layers.setStyle`, `layers.applyStyle`, every `smartObject.*` method, `pixels.cameraRaw`, `pixels.gmic`,
  `pixels.removeBackground`, `artboards.*`, `slices.*` and `timeline.*` work on a 16-bit document; the exports say
  in `bits` what depth they wrote and, when they dithered down to 8, `note` says so ([automation.md](automation.md)).

A 16-bit PSD that NekoPhoto opened and exports again as PSD keeps each unedited layer's channel data byte for byte,
so a round trip does not lose Photoshop's full 16 bits; a layer you edit (and a PSB) is written from its 0..32768
values, which drops the lowest of the file's 16 bits. PNG and TIFF are always written from 0..32768.

A 16-bit PSD does not get Photoshop's Smart Filter cache (it is 8-bit data; Photoshop rebuilds it). Opening a 16-bit
PSD that has none, NekoPhoto draws each Smart Filter stack over the whole canvas with its filter mask all white, so the
filters stay editable; a filter mask painted in NekoPhoto is not kept in a 16-bit PSD (it is in a project and an 8-bit
PSD).

## Nothing greyed out

Every menu item, tool and automation method works in a 16-bit document. What still reduces to 8 bits does so because
the format or the model takes 8 bits, and says so: the 8-bit export formats (dithered down), the clipboard for other
apps, and the input of the segmentation models (the masks they make are refined and kept at 16 bits).

Colour management works at both depths: a 16-bit document keeps its profile, converts with Convert to Profile at
16 bits, and is shown through the monitor profile in the same pass that reduces it to the screen's 8 bits
([color-management.md](color-management.md)).

CMYK and Lab documents work at 16 bits too ([color-modes.md](color-modes.md)).

## Memory

The size limits are memory limits, so a 16-bit document holds half the pixels of an 8-bit one: up to 50 megapixels
per image, layer or mask (100 at 8 bits), and 500 megapixels of layers in all (1,000 at 8 bits). Converting a document
that would not fit is refused, with the reason.

## 32 bits per channel

A 32-bit document holds premultiplied **linear light** in floating point, in its profile's primaries, as Photoshop's
32-bit mode does: colour may go above 1 (brighter than white), alpha, masks and the selection stay 0 to 1. Blending in
linear light is what light does, so soft edges, glows and semi-transparent layers look different from 8 and 16 bits
(Photoshop's 32-bit mode too).

The core, the files and the view came first (P5a in [high-bit-depth-plan.md](high-bit-depth-plan.md)); adjustments,
filters, selections and pixel edits followed (P5b), then painting and retouching (P5c). Which features work in which
depth and colour mode is in the [capability matrix](mode-matrix.md), generated from the code.

### Getting a 32-bit document

- **Image ▸ Mode ▸ 32 Bits/Channel** converts the open document, as one undo step (refused, with the reason, when it
  would not fit: see Memory below). Each layer's colour is linearised through its profile's tone curve (sRGB's curve for
  an untagged document); the profile becomes its linear version (same primaries and white point, a gamma 1.0 curve,
  "sRGB IEC61966-2.1 (Linear)"), and the profile it came from is remembered. As in Photoshop, 32 bits is RGB only:
  the entry is greyed in a CMYK or Lab document, and CMYK Color and Lab Color are greyed in a 32-bit one.
- **Opening a 32-bit file**: a 32-bit RGB or grayscale Photoshop file (PSD, PSB) opens as a 32-bit document, layers,
  masks, the merged image and alpha channels in float. Its colour profile (resource 1039) is taken as the space its
  linear values are in, as Photoshop does; an untagged file is linear sRGB.
- **Projects** keep 32 bits: layers, masks and channels are float sidecars (`.f32z`, [project-format.md](project-format.md)).
- Values that are not numbers (NaN) or infinite, from a file or elsewhere, are cleaned as they come in: NaN becomes 0,
  infinity the largest half-float value (65504), negative colour 0, alpha and masks are held to 0..1.

### Going back to 16 or 8 bits: HDR Toning

Image ▸ Mode ▸ 16 or 8 Bits/Channel on a 32-bit document opens **HDR Toning**, whose result the canvas shows while it is
open:

- **Exposure and Gamma**: the values multiplied by 2^exposure (stops, -20 to 20), then raised to 1 / gamma
  (0.1 to 9.99); what is above white is clipped.
- **Highlight Compression**: the brightest luminance is brought down to white along a smooth curve
  (L (1 + L / W²) / (1 + L), W the brightest), the rest little changed; an image with nothing above white is left as it
  is.

Then the remembered profile's curve encodes the values for 16 or 8 bits, and the document gets that profile back. Layers
are kept. At the defaults (Exposure and Gamma, exposure 0, gamma 1) a document that came from 8 or 16 bits comes back
exactly as it was, every pixel. (Photoshop's Local Adaptation and Equalize Histogram methods are not there yet.)

### The view

A 32-bit document's values are shown through a **view**, per tab, which changes what the canvas shows, never the pixels,
and is not an undo step:

- the **Exposure** slider in the status bar (32-bit documents only), in stops;
- **View ▸ 32-bit Preview Options**: Exposure and Gamma, or Highlight Compression (its white point the document's
  brightest area).

After the view, the canvas goes to the monitor through its profile (from the linear profile, in floating point), or,
with no monitor profile, is encoded with the document's own curve, so at exposure 0 an 8-bit document converted to
32 bits looks as it did (edges and transparency aside, which now blend in linear light).

### What works at 32 bits now

- **Rendering**: every blend mode, opacity, layer and folder masks (in float), vector masks, clipping masks, folders
  (Pass Through and isolated), artboards, shape and fill layers, Dissolve, layer styles (all ten effects, their colours
  linearised) and adjustment layers. Vector coverage and gradient and pattern fills come from their 16-bit forms (15 bits
  of coverage).
- **Blend modes**: the picker offers Photoshop's 32-bit set: Normal, Dissolve, Darken, Multiply, Lighten, Linear Dodge
  (Add), Difference, Subtract, Divide, Hue, Saturation, Color, Luminosity, Darker Color and Lighter Color; they work on
  the values as they are, above white included. The other modes are greyed ("Not available in 32-bit mode"); a layer
  that already has one (from a file, or converted) still draws, with its colours held to 0..1 inside the blend. The
  formulas are the W3C Compositing and Blending ones; Hue, Saturation, Color, Luminosity and Darker and Lighter Color
  take luminance from the profile's own primaries.
- **Adjustments**, on pixels (Image ▸ Adjustments) and as adjustment layers: Photoshop's 32-bit set, Levels, Curves,
  Exposure, Hue/Saturation, Color Balance, Black & White, Photo Filter, Channel Mixer, Vibrance, Gradient Map, Invert and
  Color Lookup. How they treat light above white is below.
- **Filters**: Gaussian Blur and Motion Blur (light averaged as it is, nothing rounded), Add Noise (the same pattern for
  a seed as at 8 and 16 bits), Lens Correction (exact bilinear or Catmull-Rom weights), the distortions (Twirl, Pinch,
  Spherize, Wave, Ripple, Polar Coordinates, ZigZag, Shear), Maximum, Minimum and Offset.
- **Selections**: the marquee and lasso tools, the Magic Wand and Quick Select, Select All, Deselect, Inverse, Select ▸
  Modify (Expand, Contract, Border, Smooth, Feather), Load as Selection, Quick Mask, Select Subject, Layer Mask ▸ From
  Selection. The selection is float coverage (0 to 1). The Magic Wand, Quick Select and Trim decide on the composite at
  exposure 0 through the document's curve (8-bit levels, as Tolerance counts them), never on the view: moving the
  exposure slider does not change what a click selects.
- **Channels**: alpha channels in float, Save Selection and Load Selection in every mode, a layer's transparency or mask
  and the composite or a colour channel as a selection (the colour encoded at exposure 0), single colour-channel editing
  of the float pixels, the Channels panel.
- **Pixel edits**: Fill and Clear through the selection (the colour picked is linearised through the document's curve),
  Cut, Copy, Copy Merged, Paste and Layer via Copy (inside NekoPhoto the float pixels, exactly; the system clipboard gets
  an 8-bit copy tone-mapped at exposure 0), Free Transform of selected pixels, Distort and Perspective, Edit ▸ Warp and
  Warp Cage on pixel layers, **Image Size** (the float resamplers: bilinear, Catmull-Rom and Lanczos with exact weights,
  light above 1 kept), **Crop**, the Crop tool, **Trim** and **Canvas Size**.
- **The layer structure**: new, duplicate, delete, reorder and group layers, names, visibility, opacity, blend mode,
  clipping; **masks** (add, enable, link, invert, delete); moving, scaling, rotating and flipping whole layers;
  **Flip Canvas**; importing images as layers (linearised through the document's curve).
- **Saving and exporting**: projects and 32-bit PSD and PSB files; PNG and TIFF at 16 bits and JPEG, WebP, TGA, ICO and
  GIF at 8, each tone-mapped at exposure 0 (values above white clip) and encoded with the document's curve, with a
  note saying so. The embedded profile is the one the values encode to.
- **Painting and retouching** ([below](#painting-at-32-bits)): the Brush and Eraser with the round tip and imported tip
  brushes, the MyPaint presets, on pixels, masks and the Quick Mask; the Gradient tool; Clone Stamp; Spot Healing and the
  Healing Brush; Blur, Sharpen, Smudge and Liquify; moving and duplicating selected pixels with the Move tool; the
  Eyedropper; Merge Down and Merge Layers; Layer Mask ▸ Apply.
- **Tools that do not touch pixels**: Move (layers), Hand, Zoom.

Everything else is greyed in a 32-bit document, and automation refuses it. Two wordings tell the reason apart:

- **"Not available in 32-bit mode"**: Photoshop itself has no such thing at 32 bits, so it stays greyed: Dodge, Burn
  and Sponge, the Paint Bucket, the Patch tool, the content-aware tools, the Brightness/Contrast, Posterize, Threshold, Selective Color
  and Grain adjustments (as adjustment layers they are kept but not drawn; converting says so), Mosh, G'MIC, and the blend
  modes outside the 32-bit set.
- **"Not available in 32-bit yet"**: not ported yet: the Camera Raw Filter, Box, Radial and Surface Blur, Dust &
  Scratches, Median, Unsharp Mask, High Pass, Emboss, Mosaic, Find Edges, Clouds and Difference Clouds, Remove Background, text, shape and path
  editing, layer style editing, smart objects and Smart Filters, deleting a clipping base (which bakes its clipped
  layers), artboard, slice and SVG export, the timeline, colour conversion (Assign and Convert to Profile).

### Painting at 32 bits

A stroke at 32 bits is the same stroke as at 8 and 16 bits (the dab spacing, the tip's profile, hard tips building up
to their opacity, soft ones screening) worked out in float, without rounding:

- **The colour** you pick is the colour the pickers show, encoded; it is linearised through the document's curve before
  it is painted, so a 50% grey paints 0.214 (sRGB), as in Photoshop. Soft edges, low opacity and the eraser blend in
  **linear light**, so they look different from 8 and 16 bits (as in Photoshop). Light above
  white under the brush is blended as it is: a half-covered pixel over 4.0 lands between the colour and 4.0; pixels a
  stroke does not touch keep their exact values.
- **Gradients** run between their stops in linear light (the stops linearised), as Photoshop's 32-bit gradients do; a
  mask's gradient is coverage and is not linearised.
- **Imported tip brushes** stamp their coverage at 15 bits (the same stamps as at 16 bits), then paint in float.
- **MyPaint presets**: libmypaint paints in 15-bit fixed point, so the layer goes to it encoded through the document's
  curve at 15 bits, a tile at a time as the brush reaches it, and comes back linearised. Only the samples a dab
  actually changed are rewritten; every other pixel, light above white included, keeps its exact float.
- **Spot Healing and the Healing Brush** work on the area around the spot encoded at 15 bits through the document's
  curve (what the patch search sees is the picture at exposure 0, as the Magic Wand decides), after dividing the area by
  its brightest straight colour when that is above 1, so a highlight heals as light and not as a white clipped at 1;
  the result is decoded, scaled back and blended in by the brush's coverage. Pixels the stroke does not cover keep their
  exact values.
- **Blur and Sharpen** paint a blurred or sharpened float copy of the layer through the tip (Sharpen on straight colour,
  not clamped above 1); **Smudge and Liquify** carry and resample float pixels.
- **The Eyedropper** reads the composite's linear value (`color.sample` answers it); the foreground colour it sets is
  that value encoded through the document's curve, so light above white shows as white in the pickers (an HDR colour
  picker is P5f).
- **Moving selected pixels**, **Merge Down** and **Layer Mask ▸ Apply** work on the float pixels exactly.

Checked by `paint_modes_tests`: a dab at every hardness, stamped and per pixel, painting and erasing, through a
selection over light up to 4, is within 1e-5 (absolute plus relative) of a double-precision reference
(`tests/float_reference.cpp`, worst 0.08 of the bound); a stroke's core is the linearised colour exactly; MyPaint leaves
every untouched float as it was; spot healing brings a dark dot in light at 2.5 back to 2.5 within 2%.

### Adjustments and filters at 32 bits

The pixels stay linear and nothing is clamped above 1. Each adjustment's settings (its sliders, points and histogram)
are the 8-bit ones, written for encoded values, so at 32 bits each pixel's straight colour is taken to the document's
encoding (its profile's curve; above 1 the curve's power law carried on, sRGB's own formula for sRGB), the adjustment's
function is applied there, and the result is linearised again. An 8- or 16-bit document converted to 32 bits therefore
adjusts as it did, and light above white goes on through the function:

- **Exposure** is the exception: an exact multiply by 2^exposure in linear light, the offset added there, the gamma a
  power, nothing clamped above.
- **Levels** loses its clamp at the white point: input above it goes on rising along the same formula (at 8 bits it is
  cut at white). The black point still cuts below.
- **Curves** is exactly the 8-bit curve from 0 to 255; above 255 it continues as a straight line along its end tangent
  (the last segment's slope when the last point is at 255, flat when the last point stops short of 255, as the curve is
  already flat there). Photoshop does not document how its 32-bit Curves and Levels carry on above 1; these are
  NekoPhoto's stated choices.
- **Hue/Saturation, Color Balance, Black & White, Photo Filter, Channel Mixer, Vibrance and Color Lookup** are defined on
  0 to 1: a colour brighter than white is adjusted as the same colour at white's brightness (its linear channels divided
  by the brightest) and scaled back up, so hue and saturation move and the light level stays.
- **Invert** is 1 minus the encoded value, so light above white inverts to black. **Gradient Map** maps the encoded
  luma, cut at white.
- **Add Noise** adds its noise to the encoded colour, so an amount looks as it does at 8 bits; light above white is not
  cut. The blurs and Lens Correction work on the linear values.

On an 8-bit picture converted to 32 bits, adjusted there and converted back at exposure 0, every kind is within one
level of the 8-bit adjustment on opaque pixels but Hue/Saturation, within three: at 8 bits it is Photoshop's byte
arithmetic (lightness, half-chroma and hue each rounded to a level, which a raised saturation multiplies), at 16 and 32
bits the same model unrounded. On half-transparent pixels Exposure, Levels, Curves, Invert, Black &
White, Channel Mixer and Gradient Map stay within a level; Hue/Saturation, Color Balance, Photo Filter and Vibrance
reach 2 to 5 levels on under 0.1% of samples, because the 8-bit kernels round the straight colour to a whole level
(value × 255 / alpha, cut) before their function, which at a low alpha is off by up to a level divided by the alpha.
Levels matches wherever the 8-bit Levels does not cut at its white point with an output white below 255: there 32 bits
carries on by design.

The **Camera Raw Filter** stays greyed ("yet") although Photoshop offers it at 32 bits: its sliders (Whites,
Highlights, the tone curve, Clarity's masks) are written for display-referred values from 0 to 1, and porting it means
a scene-referred pipeline with HDR Toning's Local Adaptation (P5f), not the 16-bit float kernels run on clipped values.
Unsharp Mask, destructive or as a Smart Filter, waits for the grid filters and smart objects at 32 bits (the Sharpen
tool works).

### Memory at 32 bits

A 32-bit sample takes four bytes, so a 32-bit document holds a quarter of the pixels of an 8-bit one: up to
25 megapixels per image, layer or mask, and 250 megapixels of layers in all.

### For developers

Pixels are `ImageF` / `GrayF` (`imaget.h`), premultiplied linear float. `depth.h` has the float primitives
(`lineariseImage`, `encodeImage8/16` with `TransferCurve::fromLinearExact`, the exact inverse that makes 8/16 → 32 → 8/16
lossless, halvings, samplers, `cleanFloat`); `colormgmt.h` has `linearProfile`, `gammaCounterpart`, `encodedProfileOf`,
`luminanceWeights`, `PixelFormat::RGBAFloat` and `toDisplayF`; `view32.h` has `View32` and `ToneMap`. The renderer is the
deep executor template (`render_exec_deep.inc`) over `DeepOps<F32>` (`render_deep_ops_f32.h`), unchanged from 16 bits;
`blend_f32.cpp` has the modes. `depth_float_tests` checks the renderer against a double-precision reference
(`tests/float_reference.cpp`) within 1e-5, absolute and relative.

Editing (P5b): `adjustments_f32.cpp` (the 32-bit set through `encodeExtended` / `decodeExtended`, depth.h), the colour
kinds' float frame `forEachEncodedColour`, `blur_f32.cpp`, `filters_f32.cpp`, `resample_f32.cpp`, the float warps
(`warp.h`, `warpmesh.h`), `resizeDocumentF`, `GrayF` selection, morphology and channel operations, and
`decisionImage()` (render.h) for the tools that decide on pixels. In the app the float counterparts of the 16-bit edit
paths are in `EditorSessionFloat.cpp`. `depth_float_edit_tests` checks every kernel against double-precision versions
in the same reference (1e-5 absolute plus relative, the recursive Gaussian above sigma 6 included), the extension
above 1, adjustment layers in the renderer, and the cross-depth parity above.

Painting (P5c): the brush stroke's raster is `StrokeRasterOf<StrokeOps<F32>>` (`stroke_raster.h`, `brush_f32.cpp`); the
`BrushStroke` constructor that takes a `Document` linearises the colour and picks the raster for the document's depth
and mode. `mypaint.cpp` has the 15-bit round trip, `StrokeRasterOf::healF` the healers' encoding, `warpstroke.cpp` and
`toning.cpp` (Sharpen) float paths, `pixels_any.cpp` merging and Apply Layer Mask at any layout.

---

## 日本語

[English](#16-and-32-bits-per-channel) · **日本語**

Photoshop と同じく、ドキュメントは 8、16、32 bit/チャンネルのいずれかです。レイヤー・マスク・選択範囲はすべて
同じビット数を持ちます。16 bit では描画モードやマスクを重ねてもグラデーションが滑らかなままです(8 bit では
階調の段差が出ることがあります)。32 bit はリニアな光を浮動小数点で持ち、白より明るい値(HDR)も扱えます。
イメージ ▸ モード ▸ 8 bit/チャンネル、16 bit/チャンネル、32 bit/チャンネル で切り替えます(取り消しは 1 回)。32 bit で
使えるものは後述の「32 bit/チャンネル」をご覧ください。

### 16 bit のドキュメントを作るには

- **イメージ ▸ モード ▸ 16 bit/チャンネル** で開いているドキュメントを変換します。8 bit からの変換は正確で、8 bit に
  戻せばすべてのピクセルが元どおりです。
- **16 bit のファイルを開く**:16 bit の RGB/グレースケールの Photoshop ファイル(PSD、PSB)、16 bit の PNG、16 bit の
  TIFF は 16 bit のドキュメントとして開きます(32 bit の Photoshop ファイルは 32 bit で開きます。後述)。
- **プロジェクト**はビット数を保ちます(レイヤーとマスクを 16 bit の PNG で保存、プロジェクト形式バージョン 8)。
  以前のプロジェクトは 8 bit で開きます。

Photoshop と同じく 16 bit の値は 0〜32768 で保持し、合成は正確な整数演算です。ファイルは 0〜65535 で保存するため、
読み書きのときに変換します。

### 16 bit で使えるもの

- **表示**:すべての描画モード、不透明度、レイヤーマスクとグループのマスク(16 bit)、ベクトルマスク、クリッピング
  マスク、グループ(通過と分離、不透明度)、アートボード、シェイプレイヤーと塗りつぶしレイヤー、ディザ合成、調整
  レイヤー、レイヤースタイル。ベクトルマスクとシェイプの縁は 32,768 段階で描き、グラデーションの塗りは正確な色から
  描くので、長いグラデーションにも 8 bit の段差が出ません。
- **色調補正**(ピクセルへの適用:イメージ ▸ 色調補正と階調の反転、および調整レイヤー):レベル補正、トーンカーブ、
  色相・彩度、露光量、グラデーションマップ、粒子、明るさ・コントラスト、自然な彩度、カラーバランス、白黒、
  レンズフィルター、チャンネルミキサー、特定色域の選択、ポスタリゼーション、2 階調化、カラールックアップ、
  階調の反転。どれも正確な色で計算するので、滑らかな 16 bit のグラデーションは強いレベル補正やトーンカーブでも
  滑らかなままです。ポスタリゼーションと 2 階調化は 8 bit と同じ段階を選びます。レベル補正のヒストグラムは
  どちらのビット数でも 256 段階です。
- **フィルター**:フィルターメニューのすべてのフィルター:ぼかし(ガウス・移動・ボックス・放射状・表面)、ノイズを加える
  (同じシードで同じパターン)、ダスト&スクラッチ、中間値、アンシャープマスク、ハイパス、エンボス、モザイク、輪郭検出、
  雲模様 1・2、変形(ツイスト、つまむ、球面、波形、波紋、極座標、ジグザグ、シアー)、明るさの最大値・最小値、オフセット、
  レンズ補正、Mosh。
- **Camera Raw フィルター**:すべてのパネル。処理は浮動小数点の色で行い、途中で丸めず(8 bit 版は各段階で 8 bit に
  丸めます)、最後に 1 回だけ 16 bit に丸めるので、滑らかな 16 bit のグラデーションは露光量やカーブを通しても滑らかな
  ままです。フリンジ除去はどのピクセルがフリンジかを 8 bit に丸めた色で決めるため、色相の範囲の境目にある色は 8 bit と
  同じ扱いになります。8 bit から変換した画像では各パネルとも 8 bit 版と 1 段階以内、すべてのパネルを同時に使うと
  0.19% のサンプルで 2 段階ずれます(8 bit 版が段階ごとに丸めるため)。
- **G'MIC**:G'MIC は 0〜255 の尺度の浮動小数点で処理するので、16 bit のレイヤーはその尺度の浮動小数点の色として渡し、
  16 bit で受け取ります(libgmic によるプロセス内の実行でも、`gmic` プログラムへの浮動小数点ファイル経由でも)。途中で
  8 bit には落としません。8 bit から変換した画像では、blur・unsharp・sepia・sharpen は 8 bit の結果と 1 段階以内です。
  半透明のピクセルにかけた Solarize は 0.9% のサンプルで 6 段階ずれます:8 bit では半透明のピクセルの色を 8 bit に
  丸めて渡し、Solarize は画像全体の範囲で尺度を決めるためです(不透明な画像ではずれません)。
- **背景を削除**:セグメンテーションのモデルは 8 bit の入力を取るため、モデルと調整のガイドは 8 bit に落とした
  レイヤーを見ます。マスクは調整(エッジを調整、マッティング、クリーンアップ、エッジをシフト、コントラスト)の間
  浮動小数点のまま保ち、16 bit のマスクとして作ります。エッジの色の除去は 16 bit のピクセルで行います。8 bit の画像
  では 16 bit のマスクは 8 bit のマスクと 1 段階以内(中間の値は何倍も細かく)、除去後の色も 1 段階以内です。
- **選択範囲**:長方形選択・楕円形選択、なげなわ・多角形選択、自動選択(エッジの調整とスマート自動選択を含む)、
  クイック選択の各ツール。すべてを選択、選択を解除、選択範囲を反転、拡張、縮小、境界をぼかす、滑らかに、
  境界線。レイヤーやマスクから選択範囲を読み込む、クイックマスクモードで編集、選択範囲の枠の移動、選択範囲から
  マスクを追加。選択範囲そのものが 16 bit なので、ぼかしの段階は 256 ではなく 32,768 です。画像を読むツール
  (自動選択、クイック選択、被写体を選択)はカンバスの表示どおりに読みます(許容値は Photoshop と同じく 8 bit の
  段階)。
- **ピクセルの編集**:選択範囲を通した塗りつぶし(描画色・背景色)と消去。カット、コピー、結合部分をコピー、
  ペースト、選択範囲をコピーしたレイヤー(他のアプリのクリップボードには 8 bit。別のビット数のドキュメントから
  ペーストしたピクセルは変換します)。コンテンツに応じた塗りつぶし、コンテンツに応じて拡大・縮小、コンテンツに
  応じた移動(自動化から。パッチの探索とシームは 8 bit に丸めた画像で決め、コピーするピクセルは 16 bit)。選択した
  ピクセルの自由変形。
- **ペイントとレタッチ**:すべてのエンジンのブラシ(円形の先端、読み込んだ先端ブラシ、MyPaint のプリセット)と
  消しゴム(ピクセル、レイヤーマスク、クイックマスク)。コピースタンプと修復ブラシ(16 bit のピクセルをコピー)、
  スポット修復ブラシとパッチツール。ぼかし・シャープ・指先・ゆがみ。覆い焼き・焼き込み・スポンジ。グラデーション
  ツールと塗りつぶしツール。移動ツールでの選択したピクセルの移動と複製。ストロークは 16 bit で重なるので、ソフト
  ブラシの縁や長いグラデーションの階調は 8 bit の数十段階ではなく数千段階になります。変更する場所を選ぶツールは
  カンバスの表示どおりに画像を読みます(塗りつぶしツールの許容値は 8 bit の段階、スポット修復ブラシとパッチは
  コンテンツに応じた塗りつぶしと同じく 8 bit に丸めた画像でコピー元を選びます)。
- **画像解像度**(各補間方法)、**切り抜き**、切り抜きツール、選択範囲で切り抜き、**トリミング**。**カンバス
  サイズ**、カンバスの反転。
- **レイヤー**:新規レイヤー・グループ、削除、複製、グループ化、名前の変更、重ね順、グループへの出し入れ、
  表示/非表示、不透明度、描画モード、クリッピング、補間方法、下のレイヤーと結合(16 bit で結合)。クリッピングの
  ベースを削除するときは、クリップされたレイヤーの見た目を 16 bit でピクセルに焼き込みます。
- **テキスト**:文字ツール、テキストを編集、スタイルの混在したテキスト、ワープテキスト、作業用パスを作成、シェイプに
  変換。テキストは 16 bit/チャンネルで描画します(Qt の 16 bit 描画。文字のアンチエイリアスは 8 bit と同じ 256 段階)。
  16 bit の PSD のテキストレイヤーはテキストとして開き、Photoshop のテキストレイヤーとして書き出します。
- **シェイプとパス**:シェイプツール(ライブの長方形・楕円、多角形、ライン、カスタムシェイプ)、単色・グラデーション・
  パターンの塗りと線、パスの演算、ペンツールとパス選択ツール、パスパネル、あらゆるレイヤーのベクトルマスク、パスの
  塗りつぶしと境界線(16 bit)、選択範囲から作業用パス。
- **レイヤースタイル**:レイヤースタイルダイアログと 10 種類の効果(ドロップシャドウ、シャドウ(内側)、光彩(外側・
  内側)、ベベルとエンボス、サテン、カラー・グラデーション・パターンオーバーレイ、境界線)をレイヤーとグループに
  16 bit で描画。レイヤースタイルのコピー・ペースト・消去とスタイルのプリセット。100% 表示では 8 bit の描画と
  1 段階以内です。
- **レイヤーの変形**:移動ツール、自由変形(移動・拡大縮小・回転)、自由な形に・遠近法、編集 ▸ ワープ、ワープ
  ケージ、レイヤーの反転。ワープは 16 bit のピクセルを変形します(ケージのドラッグ中は 8 bit のコピーで
  プレビュー)。
- **レイヤーマスク**:すべての領域を表示/隠す、有効/無効、リンク、反転、適用、削除、マスクへのペイント。
- **スマートオブジェクト**:埋め込みを配置、スマートオブジェクトに変換、コンテンツを編集、内容を置き換え、ラスタライズ、
  移動・拡大縮小、編集 ▸ ワープとワープケージ。スマートオブジェクトの内容は元のビット数のままです。16 bit の
  ドキュメントに配置した 8 bit の PNG は 8 bit のソースのまま(インスタンスは 16 bit で描画)、8 bit のドキュメントに
  配置した 16 bit の PNG・TIFF・PSB は 16 bit のままなので、後でドキュメントを 16 bit に戻しても失われません。16 bit
  のドキュメントでスマートオブジェクトに変換すると 16 bit の PSB になり、コンテンツを編集は内容をそのビット数で開きます。
- **スマートフィルター**:NekoPhoto が描画するスマートフィルターはすべてインスタンス上で 16 bit で描画します:ぼかし
  (ガウス)、ぼかし(移動)、ぼかし(ボックス)、ぼかし(表面)、ぼかし(放射状)、ハイパス、中間値、ダスト&スクラッチ、
  アンシャープマスク、ノイズを加える、モザイク、エンボス、ラップ。不透明度、描画モード、共有のフィルターマスクも
  含みます。8 bit の画像ではそれぞれ 8 bit の結果と 1 段階以内です(数値は [smart-objects.md](smart-objects.md))。
  アンシャープマスクには専用の調整が必要でした:8 bit 版はぼかしたコピーを 8 bit に丸め、量がその丸めを拡大する
  (通常の量で 2〜4 段階)ため、8 bit から変換した色ではぼかしたコピーを 8 bit 版と同じく 8 bit の段階で求めます。
  本来の 16 bit の色では正確に求めるので、量が拡大する 8 bit の段差はありません。
- **アートボードとタイムライン**:背景とクリッピングを含むアートボード、アートボードツール、タイムラインの
  フレーム・遅延時間・ループ・再生。
- **画像の読み込み**(ドキュメントのビット数に合わせます)。
- **保存と書き出し**:16 bit のプロジェクト(16 bit のスマートオブジェクトのソースは 16 bit の PNG として保存)、
  スマートオブジェクトを含む 16 bit の PSD、16 bit の PNG、16 bit の TIFF(Qt の TIFF プラグイン経由)。
  JPEG・WebP・TGA・ICO・GIF は 8 bit の形式なので、ディザをかけて 8 bit に変換し、ステータスバーでお知らせします。
  アニメーション GIF の各フレームも 16 bit で描画してから同じようにディザで 8 bit にします。アートボードとスライスの
  書き出しは 16 bit の PNG、または(お知らせのうえ)ディザで 8 bit にした JPEG です。SVG の書き出しは必要な画像
  (ピクセル、テキスト、スタイル付きのレイヤー、グループのマスク)を 16 bit の PNG として埋め込むので、16 bit のままです。
- **自動化**:`image.mode` で変換、`document.info` の `bits` でビット数がわかります。選択範囲、`pixels.adjust`、
  `pixels.filter`、`pixels.fill`、`pixels.clear`、コンテンツに応じた各メソッド、`image.resize`、`image.trim`、
  `canvas.crop`、`layers.warp`、`layers.setCage`、`brush.stroke`(指定できるすべてのツール)、`gradient.draw`、
  `pixels.bucket`、`pixels.patch`、`layers.merge`、すべての `smartObject.*` メソッド、`pixels.cameraRaw`、
  `pixels.gmic`、`pixels.removeBackground`、`artboards.*`、`slices.*`、`timeline.*` も 16 bit のドキュメントで
  使えます。書き出しは書いたビット数を `bits` で返し、ディザで 8 bit にしたときは `note` でお知らせします。

NekoPhoto で開いた 16 bit の PSD を PSD に書き出すと、編集していないレイヤーのチャンネルデータはバイト単位でそのまま
戻るので、Photoshop の 16 bit の値は失われません。編集したレイヤー(と PSB)は 0〜32768 の値から書き出すため、
ファイルの 16 bit のうち最下位の 1 bit が落ちます。PNG と TIFF は常に 0〜32768 の値から書き出します。

16 bit の PSD には Photoshop のスマートフィルターのキャッシュを書き出しません(8 bit のデータで、Photoshop が作り
直します)。キャッシュのない 16 bit の PSD を開くと、NekoPhoto は各スマートフィルターをカンバス全体に、フィルター
マスクをすべて白として描画するので、フィルターは編集できるままです。NekoPhoto で描いたフィルターマスクは 16 bit の
PSD には残りません(プロジェクトと 8 bit の PSD には残ります)。

### グレー表示になるものはありません

すべてのメニュー項目、ツール、自動化のメソッドが 16 bit のドキュメントで使えます。8 bit に落とすのは形式やモデルが
8 bit しか受け取らない場合だけで、そのときはお知らせします:8 bit の書き出し形式(ディザで変換)、他のアプリへの
クリップボード、セグメンテーションのモデルへの入力(モデルが作るマスクは 16 bit で調整・保持します)。

カラーマネジメントはどちらのビット数でも使えます。16 bit のドキュメントもプロファイルを持ち、プロファイル変換は 16 bit の
まま行い、画面の 8 bit への変換と同じ処理でモニタープロファイルを通して表示します([color-management.md](color-management.md))。

CMYK と Lab のドキュメントも 16 bit で使えます([color-modes.md](color-modes.md#日本語))。

### メモリ

サイズの上限はメモリの上限なので、16 bit のドキュメントが持てるピクセル数は 8 bit の半分です:画像・レイヤー・
マスク 1 枚あたり 5,000 万画素(8 bit は 1 億)、レイヤー合計 5 億画素(8 bit は 10 億)。収まらないドキュメントの
変換は理由を添えて中止します。

### 32 bit/チャンネル

32 bit のドキュメントは、Photoshop の 32 bit モードと同じく、プロファイルの原色での**リニアな光**を浮動小数点で持ちます
(乗算済み)。色は 1 を超えられ(白より明るい)、アルファ・マスク・選択範囲は 0〜1 です。リニアな光で合成するので、
柔らかい境界・光彩・半透明のレイヤーの見え方は 8 bit や 16 bit と変わります(Photoshop の 32 bit モードも同じです)。

中核・ファイル・表示が最初の段階(計画の P5a)、色調補正・フィルター・選択範囲・ピクセルの編集が次の段階(P5b)、
ペイントとレタッチがその次(P5c)です。どの機能がどのビット数とカラーモードで使えるかは、コードから生成した
[機能表](mode-matrix.md)にあります。

**32 bit のドキュメントを作るには**

- **イメージ ▸ モード ▸ 32 bit/チャンネル** で変換します(取り消しは 1 回。メモリに収まらないときは理由を添えて中止)。各レイヤーの
  色はプロファイルのトーンカーブでリニアにし(プロファイルなしは sRGB のカーブ)、プロファイルは同じ原色と白色点でガンマ 1.0 の
  リニア版に替わります。元のプロファイルは記憶しておきます。Photoshop と同じく 32 bit は RGB だけで、CMYK・Lab の
  ドキュメントではグレー表示、32 bit のドキュメントでは CMYK カラーと Lab カラーがグレー表示になります。
- 32 bit の RGB/グレースケールの **Photoshop ファイル(PSD、PSB)** は 32 bit で開きます。プロファイル(リソース 1039)は、
  Photoshop と同じくリニアな値の色空間として扱います。
- **プロジェクト**は 32 bit のまま保存します(浮動小数点のファイル `.f32z`)。
- 数でない値(NaN)や無限大は読み込むときに整えます。

**16 bit・8 bit に戻す:HDR トーン**

32 bit のドキュメントで イメージ ▸ モード ▸ 16 または 8 bit/チャンネル を選ぶと **HDR トーン** が開き、結果をカンバスに
表示します。**露光量とガンマ**(露光量は段数で -20〜20、ガンマは 0.1〜9.99。白を超える値はクリップ)と**ハイライト圧縮**
(一番明るい部分を白に収める)が選べます。レイヤーは保持されます。初期値(露光量 0、ガンマ 1)なら、8 bit や 16 bit から
変換したドキュメントは全ピクセルが元どおりに戻ります(ローカル露光量補正と平均化は未対応)。

**表示**

32 bit のドキュメントは**表示の設定**(タブごと)を通して表示します。ピクセルは変わらず、取り消しの対象にもなりません。
ステータスバーの**露光量**スライダー(32 bit のドキュメントのみ)と **表示 ▸ 32 bit プレビューオプション**(露光量とガンマ、
またはハイライト圧縮)で設定します。

**32 bit で使えるもの**

- **合成**:すべての描画モード、不透明度、レイヤーとグループのマスク、ベクトルマスク、クリッピングマスク、グループ、
  アートボード、シェイプと塗りつぶしレイヤー、ディザ合成、レイヤースタイル(10 種類の効果)、調整レイヤー。
- **描画モード**:Photoshop の 32 bit で使えるもの(通常、ディザ合成、比較(暗)、乗算、比較(明)、覆い焼き(リニア)- 加算、
  差の絶対値、減算、除算、色相、彩度、カラー、輝度、カラー比較(暗)、カラー比較(明))を選べます。それ以外はグレー表示
  (「32 bit/チャンネルモードでは使用できません」)で、すでに設定されているレイヤーは値を 0〜1 に収めて描画します。
- **色調補正**(イメージ ▸ 色調補正と調整レイヤー):Photoshop の 32 bit で使えるもの、レベル補正、トーンカーブ、露光量、
  色相・彩度、カラーバランス、白黒、フォトフィルター、チャンネルミキサー、自然な彩度、グラデーションマップ、階調の反転、
  カラールックアップ。白より明るい光の扱いは下の「32 bit での色調補正とフィルター」を参照してください。
- **フィルター**:ぼかし (ガウス) とぼかし (移動)(光をそのまま平均し、途中で丸めません)、ノイズを加える(シードが同じなら
  8 bit・16 bit と同じ模様)、レンズ補正(バイリニアまたは Catmull-Rom の正確な重み)、変形(ツイスト、つまむ、球面、波形、
  波紋、極座標、ジグザグ、シアー)、明るさの最大値・最小値、オフセット。
- **選択範囲**:長方形・楕円形選択ツールと投げ縄ツール、自動選択ツール、クイック選択ツール、すべてを選択、選択を解除、
  選択範囲を反転、選択範囲を変更(拡張、縮小、境界線、滑らかに、ぼかし)、レイヤーから選択範囲を読み込む、
  クイックマスク、被写体を選択、レイヤーマスク ▸ 選択範囲から。選択範囲は浮動小数点の範囲(0〜1)です。自動選択ツール、
  クイック選択ツール、トリミングは、露光量 0 でドキュメントのカーブを通した合成画像(許容値が数える 8 bit の段階)で判断し、
  表示の設定は使いません。ステータスバーの露光量を動かしても、クリックで選ばれる範囲は変わりません。
- **チャンネル**:浮動小数点のアルファチャンネル、すべての方法での選択範囲を保存・読み込む、レイヤーの透明部分やマスク、
  合成画像やカラーチャンネル(露光量 0 で書き出した値)からの選択範囲、浮動小数点のピクセルでの単独のカラーチャンネルの
  編集、チャンネルパネル。
- **ピクセルの編集**:選択範囲の塗りつぶしと消去(選んだ色はドキュメントのカーブでリニアにします)、カット、コピー、
  結合部分をコピー、ペースト、選択範囲をコピーしたレイヤー(NekoPhoto の中では浮動小数点のピクセルのまま正確に。
  システムのクリップボードには露光量 0 でトーンマッピングした 8 bit のコピー)、選択したピクセルの自由変形、
  ゆがみと遠近法、編集 ▸ ワープとワープケージ(ピクセルレイヤー)、**画像解像度**(浮動小数点の再サンプル:
  バイリニア、Catmull-Rom、Lanczos を正確な重みで。1 を超える光も保持)、**切り抜き**と切り抜きツール、**トリミング**、
  **カンバスサイズ**。
- **レイヤーの構成**、**マスク**、レイヤー全体の移動・拡大縮小・回転・反転、**カンバスの反転**、画像の読み込み。
- **保存と書き出し**:プロジェクト、32 bit の PSD・PSB。PNG と TIFF は 16 bit、JPEG・WebP・TGA・ICO・GIF は 8 bit で、
  露光量 0 でトーンマッピングし、その旨をお知らせします。
- **ペイントとレタッチ**(下の「32 bit でのペイント」):ブラシと消しゴム(円形ブラシ先端と読み込んだブラシ先端)、
  MyPaint のプリセット(ピクセル、マスク、クイックマスク)、グラデーションツール、コピースタンプ、スポット修復ブラシと
  修復ブラシ、ぼかし・シャープ・指先・ゆがみ、移動ツールでの選択ピクセルの移動と複製、スポイトツール、下のレイヤーと結合と
  レイヤーを結合、レイヤーマスク ▸ 適用。
- ピクセルに触れないツール(移動、手のひら、ズーム)。

それ以外はグレー表示になり、自動化でも使えません。理由は 2 通りの表示で区別します。

- 「**32 bit/チャンネルモードでは使用できません**」:Photoshop 自体が 32 bit で持たないもので、今後もグレー表示のままです。
  覆い焼き・焼き込み・スポンジ、塗りつぶしツール、パッチツール、コンテンツに応じた各機能、明るさ・コントラスト、ポスタリゼーション、
  2 階調化、特定色域の選択、粒子の色調補正(調整レイヤーは保持されますが 32 bit では描画しません。変換のときにお知らせします)、
  Mosh、G'MIC、32 bit で使えない描画モード。
- 「**32 bit/チャンネルではまだ使用できません**」:まだ移植していないもの。Camera Raw フィルター、ぼかし(ボックス・放射状・
  表面)、ダスト&スクラッチ、中間値、アンシャープマスク、ハイパス、エンボス、モザイク、輪郭検出、雲模様 1・2、背景を削除、テキスト・
  シェイプ・パスの編集、レイヤースタイルの編集、スマートオブジェクトとスマートフィルター、クリッピングの基点の削除
  (クリップされたレイヤーに焼き込む処理)、アートボード・スライス・SVG の書き出し、タイムライン、色の変換(プロファイルの
  指定とプロファイル変換)。

**32 bit でのペイント**

32 bit のストロークは 8 bit・16 bit と同じストローク(ブラシの間隔、ブラシ先端の形、硬いブラシは不透明度まで、柔らかい
ブラシはスクリーンで重なる)を、丸めずに浮動小数点で計算します。

- **色**:カラーピッカーに表示される色(書き出した値)を、ドキュメントのカーブでリニアにしてから塗ります。50% のグレーは
  0.214(sRGB)になり、Photoshop と同じです。柔らかい境界、低い不透明度、消しゴムは**リニアな光**で混ざります。ブラシの下の
  白より明るい光はそのまま混ざり(4.0 の上の半分覆われたピクセルは、色と 4.0 の間になります)、ストロークが触れない
  ピクセルは値がそのまま残ります。
- **グラデーション**は、Photoshop の 32 bit と同じく、リニアにした分岐点の間をリニアな光で補間します。マスクへの
  グラデーションは範囲なのでリニアにしません。
- **読み込んだブラシ先端**は範囲を 15 bit で押してから(16 bit と同じ)、浮動小数点で塗ります。
- **MyPaint のプリセット**:libmypaint は 15 bit の固定小数点で塗るので、レイヤーはドキュメントのカーブで 15 bit に
  書き出してから(ブラシが届いたタイルごと)渡し、リニアに戻します。書き換えるのはダブが実際に変えた値だけで、ほかの
  ピクセルは白より明るい光も含めて正確な値のまま残ります。
- **スポット修復ブラシと修復ブラシ**は、スポットの周りをドキュメントのカーブで 15 bit に書き出して処理します(パッチを
  探す基準は露光量 0 の画像で、自動選択ツールと同じです)。領域の一番明るい色が 1 を超えるときは先にその値で割るので、
  ハイライトは 1 で切れた白ではなく光として修復されます。結果はリニアに戻し、元の倍率に戻してブラシの範囲で合成します。
- **ぼかしとシャープ**は、ぼかした・シャープにした浮動小数点のコピーをブラシで塗ります(シャープは 1 を超えても切りません)。
  **指先とゆがみ**は浮動小数点のピクセルを運び、再サンプルします。
- **スポイトツール**は合成画像のリニアな値を読み(`color.sample` が返します)、描画色にはそれをドキュメントのカーブで
  書き出した色を設定します。白より明るい光はピッカーでは白になります(HDR カラーピッカーは P5f)。
- **選択ピクセルの移動**、**下のレイヤーと結合**、**レイヤーマスク ▸ 適用**は浮動小数点のピクセルのまま正確に処理します。

**32 bit での色調補正とフィルター**

ピクセルはリニアのままで、1 を超える値も切り捨てません。色調補正の設定(スライダー、ポイント、ヒストグラム)は 8 bit と
同じく書き出した値に対するものなので、32 bit では各ピクセルの色をドキュメントのエンコード(プロファイルのカーブ。1 を超える
部分はカーブのべき乗則をそのまま延長し、sRGB は sRGB 自身の式)に移し、そこで補正を適用してから、再びリニアに戻します。
そのため 8 bit・16 bit から変換したドキュメントは元と同じように補正され、白より明るい光もそのまま補正を通ります。

- **露光量**だけは例外で、リニアな光に 2^露光量 を正確に掛け、オフセットもそこで加え、ガンマはべき乗です。上限で切りません。
- **レベル補正**は白色点での切り捨てがなくなり、白色点を超える入力は同じ式のまま上がり続けます(8 bit では白で切れます)。
  黒色点より下は従来どおり切れます。
- **トーンカーブ**は 0〜255 では 8 bit のカーブそのもので、255 を超えると終点の接線に沿った直線で延長します(最後のポイントが
  255 にあればその区間の傾き、255 より手前で終わっていれば水平。カーブはそこですでに水平だからです)。Photoshop は 32 bit の
  トーンカーブとレベル補正が 1 を超えてどう続くかを公開していないので、これは NekoPhoto の決めごととして明記します。
- **色相・彩度、カラーバランス、白黒、フォトフィルター、チャンネルミキサー、自然な彩度、カラールックアップ**は 0〜1 で
  定義されているので、白より明るい色は、同じ色を白の明るさにしたもの(リニアのチャンネルを一番明るいチャンネルで割ったもの)
  で補正し、元の明るさに戻します。色相と彩度は動き、光の強さは保たれます。
- **階調の反転**は書き出した値を 1 から引くので、白より明るい光は黒になります。**グラデーションマップ**は書き出した値の
  輝度(白で切る)を使います。
- **ノイズを加える**はノイズを書き出した色に加えるので、同じ量なら 8 bit と同じように見えます。白より明るい光は切りません。
  ぼかしとレンズ補正はリニアな値で計算します。

8 bit の画像を 32 bit に変換して補正し、露光量 0 で戻すと、不透明なピクセルではどの補正も 8 bit での補正と 1 段階以内で
一致します。半透明のピクセルでは、露光量・レベル補正・トーンカーブ・階調の反転・白黒・チャンネルミキサー・
グラデーションマップは 1 段階以内、色相・彩度・カラーバランス・フォトフィルター・自然な彩度は 0.1% 未満のサンプルで 2〜5
段階の差になります。8 bit の処理が補正の前に色を整数の段階に丸める(値 × 255 ÷ アルファ、切り捨て)ためで、アルファが
小さいと最大で「1 段階 ÷ アルファ」ずれるからです。レベル補正は、8 bit のレベル補正が出力の白を 255 未満にして白色点で
切っている部分を除いて一致します(そこでは 32 bit は意図して値を延ばします)。

**Camera Raw フィルター**は、Photoshop では 32 bit でも使えますが、ここではグレー表示(「まだ」)のままです。スライダー
(白レベル、ハイライト、トーンカーブ、明瞭度のマスク)が 0〜1 の表示用の値を前提にしているため、移植には HDR トーンの
ローカル露光量補正(P5f)と同じシーンを基準にした処理が必要で、16 bit の浮動小数点の処理を切り詰めた値に適用するだけでは
足りないからです。アンシャープマスクは、フィルターとしてもスマートフィルターとしても 32 bit ではまだ使えません
(シャープツールは使えます)。

**メモリ**:32 bit の値は 4 バイトなので、持てるピクセル数は 8 bit の 4 分の 1 です(1 枚 2,500 万画素、レイヤー合計 2 億 5,000 万画素)。
