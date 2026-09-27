# Mosh

Filter > Mosh holds the effects of [OpenMosh](https://github.com/vomitselfie/openmosh), a photo-effects app whose
effects are WGSL shaders: glitch, distortion, retro, stylize, colour and composite effects, 54 in all. NekoPhoto runs
them on the CPU: each shader is ported line for line to C++ (`src/core/src/mosh_effects.cpp`) over a small runtime that
behaves like the GPU it was written for (`mosh_runtime.h`). OpenMosh is MIT-licensed, by the same author; the notice is
in [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md) and [LICENSES/OpenMosh-MIT.txt](../LICENSES/OpenMosh-MIT.txt).

Each effect opens a dialog whose controls come from its parameter list, previews on the canvas, and applies to the
active layer's pixels inside the selection as one undo step named after the effect. The seeded effects have a **Seed**
and a **Reroll** button: the same seed always draws the same pattern. The effects work at 8 and 16 bits per channel.

Automation: `pixels.mosh` with `effect` (the id below), `params` (by key; switches as booleans, a choice as its index
or its option's name), `seed` (0 to 100), and for the Composite effects `layer` (Overlay and Mask: the id of the layer
they read) or `text` (Caption). The MCP tool is `pixels_mosh`, with `layer` and `caption`.

## The effects

Ranges and defaults are OpenMosh's, so a value from an OpenMosh preset means the same here. Angles are in radians.
Sizes in pixels are the layer's pixels; positions and centres are fractions of the layer's width and height.

### Glitch

| Effect (id) | Parameters: key, range, default | Seeded |
|---|---|---|
| Soft Glitch (`soft-glitch`): chromatic aberration | `amount` 0-1, 0.3; `angle` 0-6.28, 0 | |
| Hard Glitch (`hard-glitch`): blocks displaced at three scales, a channel shift | `amount` 0-1, 0.4; `blocks` 2-64, 12; `shift` (Color Shift) 0-1, 0.5 | yes |
| Decimate (`decimate`): random blocks collapse to one colour | `amount` 0-1, 0.5; `size` (Block Size) 2-128, 24 | yes |
| Data-Mosh (`data-mosh`): stuck blocks copied from a drifted place, smeared rows | `size` (Block Size) 4-128, 32; `drift` 0-1, 0.4; `stick` (Stuck Blocks) 0-1, 0.5 | yes |
| Splitter (`splitter`): alternate strips shifted each way | `strips` 1-64, 8; `offset` 0-1, 0.2; `vertical` off | |
| Jitter (`jitter`): each band shifted sideways | `amount` 0-1, 0.3; `size` (Band Height) 1-100, 20 | yes |
| Slices (`slices`): slabs shifted by random amounts | `count` 1-40, 10; `offset` 0-1, 0.15; `vertical` off | yes |
| Shake (`shake`): the whole image moved by a random offset | `amount` 0-0.5, 0.1 | yes |
| Pixel Sort (`pixel-sort`): runs of pixels sorted by brightness | `low` (Threshold Low) 0-1, 0.25; `high` (Threshold High) 0-1, 0.85; `reverse` off; `vertical` off | yes |
| Strobe (`strobe`): a flash frame | `phase` 0-1, 0; `rate` 0-30, 10; `mode` Blackout, Whiteout or Invert | |

Pixel Sort works along each row (or column) in segments of 256 pixels, the segment grid shifted by a seeded amount per
line so the seams do not line up. In each segment the runs of pixels whose luma is between the thresholds are sorted
by luma, darkest first (brightest first with Reverse); pixels outside the thresholds stay where they are and end the
runs. This is OpenMosh's compute shader, whose odd-even sort is stable: equal pixels keep their order, here as there.

Strobe is made for video: on a still image the flash shows when Phase is 0.5 or more, and Rate does nothing.

### Distort

| Effect (id) | Parameters | Seeded |
|---|---|---|
| Wave (`wave`): sinusoidal displacement | `amplitude` 0-0.5, 0.05; `frequency` 0-50, 8; `phase` 0-6.28, 0; `vertical` off | |
| Bulge (`bulge`): a lens bulge (or pinch, below 0) | `strength` -1-1, 0.5; `radius` 0.05-1.5, 0.5; `cx` (Center X) 0-1, 0.5; `cy` (Center Y) 0-1, 0.5 | |
| Stretch (`stretch`): a band about a line pulled wider | `center` 0-1, 0.5; `width` 0.02-1, 0.25; `amount` 0-4, 1.5; `vertical` off | |
| Push (`push`): the image moved, wrapping round | `dx` (Push X) -1-1, 0; `dy` (Push Y) -1-1, 0; `wrap` on | |
| Luma-Mesh (`luma-mesh`): each pixel moved along a direction by its brightness | `amount` 0-0.3, 0.08; `angle` 0-6.28, 1.57 | |
| 3D Transform (`transform-3d`): a card tilted in perspective, scaled, turned and moved | `scale` 0.1-4, 1; `rotation` -3.14-3.14, 0; `x` (Offset X) -1-1, 0; `y` (Offset Y) -1-1, 0; `tilt_x` -1-1, 0; `tilt_y` -1-1, 0 | |
| Tile (`tile`): a grid of copies, alternate ones mirrored | `cols` (Columns) 1-16, 3; `rows` 1-16, 3; `mirror` on | |
| Kaleidoscope (`kaleidoscope`): mirrored wedges about the centre | `segments` 2-24, 6; `rotation` 0-6.28, 0 | |
| Mirror (`mirror`): one half reflected onto the other | `mode` Left → Right, Right → Left, Top → Bottom or Bottom → Top | |
| Wobble (`wobble`): a sinusoidal warp on both axes | `amount` 0-0.2, 0.04; `frequency` 0-40, 10; `phase` 0-6.28, 0 | |
| Smear (`smear`): a directional blur | `distance` 0-0.3, 0.08; `angle` 0-6.28, 0 | |
| Twirl (`twirl`): a swirl about the centre | `angle` -10-10, 3; `radius` 0.05-1.5, 0.5 | |
| Optical-Flow (`optical-flow`): a liquid warp along (or across) the brightness gradient | `amount` 0-0.2, 0.05; `scale` 1-20, 4 (pixels between gradient samples); `swirl` 0-1, 0.5 | |

### Retro

| Effect (id) | Parameters | Seeded |
|---|---|---|
| Pixelate (`pixelate`) | `size` (Block Size) 1-128, 12 | |
| Scan Lines (`scanlines`) | `density` 10-800, 250; `opacity` 0-1, 0.5 | |
| VHS (`vhs`): tracking jitter, colour bleed, tape noise | `tracking` 0-1, 0.4; `bleed` (Color Bleed) 0-1, 0.4; `noise` 0-1, 0.3 | yes |
| Super 8 (`super8`): film grain, a vignette and a warm cast | `grain` 0-1, 0.4; `vignette` 0-1, 0.5; `warmth` 0-1, 0.4 | yes |
| 8-Bit CGA (`cga-8bit`): pixelated to a four-colour palette | `size` (Pixel Size) 1-64, 6; `palette` Cyan/Magenta, Green/Red or Grayscale | |
| CRT (`crt`): barrel curvature, scanlines, an aperture mask | `curvature` 0-1, 0.3; `scan` (Scanlines) 0-1, 0.5; `mask` (Aperture Mask) 0-1, 0.3 | |
| Dither (`dither`): Bayer 4x4 ordered dither | `scale` 1-16, 2; `levels` 2-8, 2 | |
| Bad TV (`bad-tv`): a rolled, wavy picture with static | `distortion` 0-1, 0.3; `roll` 0-1, 0; `noise` 0-1, 0.3 | yes |
| Dot Screen (`dot-screen`): black dots on white, sized by darkness | `scale` 2-64, 8; `angle` 0-3.14, 0.4 | |
| Halftone (`halftone`): colour dots on black, sized by brightness | `scale` 2-64, 10; `angle` 0-3.14, 0.4 | |
| Ascii (`ascii`): an 8x8 character per cell, from a density ramp | `size` (Cell Size) 4-32, 10; `mode` (Color) Terminal Green, White or Original; `invert` off | |

### Stylize

| Effect (id) | Parameters | Seeded |
|---|---|---|
| Bleach (`bleach`): bleach bypass | `amount` 0-1, 0.6 | |
| Edges (`edges`): the Sobel edges of the brightness | `amount` 0-4, 1.5; `invert` off | |
| Emboss (`emboss`): a relief of the brightness | `strength` 0-8, 2; `angle` 0-6.28, 0.8 | |
| Vignette (`vignette`): darkened corners | `amount` 0-1, 0.6; `radius` 0-1.5, 0.7; `softness` 0.01-1, 0.4 | |
| Noise Displace (`noise-displace`): a smooth-noise warp | `amount` 0-0.3, 0.06; `scale` 1-60, 12 | yes |
| Watercolor (`watercolor`): a Kuwahara filter | `radius` 1-8, 4 | |
| Zoom Blur (`zoom-blur`): streaks toward a centre | `strength` 0-0.5, 0.15; `cx` (Center X) 0-1, 0.5; `cy` (Center Y) 0-1, 0.5 | |
| Glow (`glow`): the bright parts blurred and added | `threshold` 0-1, 0.6; `intensity` 0-2, 0.8; `radius` 0-16, 6 | |
| Light Streak (`light-streak`): the bright parts drawn out into streaks | `threshold` 0-1, 0.6; `length` 0-1, 0.5; `angle` 0-6.28, 0; `intensity` 0-2, 1 | |
| Feedback (`feedback`): video-feedback trails, ten iterations | `zoom` -0.2-0.2, 0.05; `rotation` -0.5-0.5, 0.05; `decay` 0-1, 0.85 | |

Glow, Light Streak and Feedback run several passes (four, five and ten), as OpenMosh does; the frames between passes
are kept at 16 bits a channel.

### Color

| Effect (id) | Parameters | Seeded |
|---|---|---|
| Color Correction (`color-correction`) | `brightness` -1-1, 0; `contrast` -1-1, 0; `saturation` -1-1, 0; `gamma` 0.2-3, 1 | |
| Duotone (`duotone`): brightness mapped between two hues | `shadow` (Shadow Hue) 0-1, 0.66; `highlight` (Highlight Hue) 0-1, 0.12; `mix` 0-1, 1 | |
| Solarize (`solarize`): channels above a level inverted | `center` 0-1, 0.5; `amount` 0-1, 1 | |
| Chromatic Warp (`chromatic-warp`): red and blue scaled apart about a centre | `amount` 0-0.2, 0.05; `cx` (Center X) 0-1, 0.5; `cy` (Center Y) 0-1, 0.5 | |
| Sepia (`sepia`) | `amount` 0-1, 0.8 | |

### Composite

| Effect (id) | Parameters | Seeded |
|---|---|---|
| Overlay (`overlay`): another layer blended over | `blend` Normal, Multiply, Screen, Lighten or Darken; `opacity` 0-1, 1 | |
| Mask (`mask`): the layer kept where another layer is bright, cut away where it is dark | `low` 0-1, 0.3; `high` 0-1, 0.7; `invert` off | |
| Mask Blocks (`mask-blocks`): random blocks cut away | `size` (Block Size) 2-128, 32; `amount` 0-1, 0.5 | yes |
| ChromaKey (`chroma-key`): one hue made transparent | `hue` (Key Hue) 0-1, 0.33; `tolerance` 0-1, 0.3; `softness` 0-1, 0.1 | |
| Caption (`caption`): a line of text stamped on | `x` (Position X) 0-1, 0.5; `y` (Position Y) 0-1, 0.85; `scale` 1-16, 4; `hue` 0-1, 0; `saturation` 0-1, 0 | |

In OpenMosh an effect is one step of a chain over a photo, and the Composite effects read three pictures: the chain so
far, an image file (Overlay's and Mask's) and the photo as it was before the chain (the "original" Mask and Mask
Blocks reveal). NekoPhoto has layers instead of a chain:

- **The other picture is a layer.** Overlay and Mask have a **Layer** menu listing the document's other visible layers
  with pixels (`layer`, a layer id, in automation). That layer is read where it lies over the one being changed in the
  document, pixel for pixel, whatever either one's position, size or transform: Overlay blends what lies above each
  pixel, Mask reads its brightness there. Its own pixels are read, without its mask, opacity or effects; where it does
  not reach it counts as transparent (Overlay leaves those pixels alone; Mask counts them as black).
- **The original is what is beneath the layer.** Where Mask and Mask Blocks would reveal the original, the layer becomes
  transparent, so the layers below show through. To mosh a picture and reveal the untouched one, duplicate the layer,
  mosh the copy and run Mask or Mask Blocks on it: the original shows through from the layer below. (The core takes an
  explicit original too, which the checks against OpenMosh use.)
- **ChromaKey writes real transparency**: keyed pixels become transparent in the layer.
- **Caption's text is drawn with the interface font** (bold, one line), where OpenMosh draws an 8x8 bitmap font.
  Scale keeps its meaning: a line of text is 8 pixels times Scale high, centred on Position X and Y. The colour is
  Hue and Saturation at full brightness (white at the defaults). In automation the text is `text`.

## OpenMosh effects that NekoPhoto already has

These are not in the Mosh menu; the existing tool does the same job, with Photoshop's controls.

| OpenMosh | In NekoPhoto |
|---|---|
| Blur | Filter > Gaussian Blur |
| Invert | Image > Adjustments > Invert |
| Posterize | Image > Adjustments > Posterize (also an adjustment layer) |
| Hue/Saturation | Image > Adjustments > Hue/Saturation |
| Brightness/Contrast | Image > Adjustments > Brightness/Contrast |
| Threshold | Image > Adjustments > Threshold |
| Gradient Map | Image > Adjustments > Gradient Map |
| Sharpen | Filter > Camera Raw Filter > Detail, the Unsharp Mask Smart Filter, or the Sharpen tool |
| Grain | Image > Adjustments > Grain, or Filter > Add Noise |

OpenMosh's one planned effect, Audio Visualizer, has nothing to port.

## How close to OpenMosh

Each port was checked against OpenMosh's own render (its headless command line, on an NVIDIA GPU through Vulkan) on
three images (two photos and a synthetic gradient with shapes), at the defaults and at one to three other settings.
Overlay read another of the images as its aux file; Mask and Mask Blocks ran after Pixelate in OpenMosh's chain, so that
their original differs from their input; Caption's text was OpenMosh's 8x8 font for both. Differences are in 8-bit
levels, over every channel (colour premultiplied, so ChromaKey's transparency counts):

| Effect | Mean difference | Largest | Pixels off by more than 2 levels |
|---|---|---|---|
| Soft Glitch, Jitter, Slices, Shake, Wave, Kaleidoscope, Scan Lines, CRT, Dither, Dot Screen, Pixelate, Decimate, Data-Mosh | 0.02-0.34 | 1 | none |
| Bulge, Stretch, Push, Luma-Mesh, Tile, Wobble, Optical-Flow, Bleach, Edges, Emboss, Vignette, Color Correction, Duotone, Solarize, Chromatic Warp, Sepia, Overlay, Mask, ChromaKey | 0.003-0.35 | 1 | none |
| 3D Transform, Twirl, Noise Displace, Light Streak | 0.02-0.33 | 2 | none |
| Feedback | 0.19 | 3 | none |
| Pixel Sort, Strobe, 8-Bit CGA, Mirror, Mask Blocks | 0 | 0 | none |
| Halftone | 0.015 | 21 | 0.001% (single pixels on dot edges) |
| Hard Glitch | 0.15 | 208 | 0.25% (block edges) |
| Splitter | 0.22 | 188 | 0.26% (one row, at defaults on one image) |
| Caption | 0.011 | 230 | 0.013% (one column at the text's left edge, on one image) |
| Glow | 0.43 | 6 | up to 12% at Intensity 1.8, none at the defaults |
| Watercolor | 0.18 | 68 | up to 2.5% (flat areas, on the synthetic image) |
| Ascii | 0.02-4.7 | 255 | 0.01-13% (whole glyph rows and columns, see below) |
| VHS without noise | 0.02 | 1 | none |
| VHS with noise, Super 8, Bad TV | 2.3-12.9 | 72 | the grain and static themselves (see below) |
| Smear, Zoom Blur | 0.3-5.5 | 109 | the jitter of their taps (see below) |

What remains is the GPU's own arithmetic:

- A GPU interpolates each pixel's coordinates to within a few units in the last place, where NekoPhoto uses the exact
  pixel centre. Where a coordinate lands exactly on a block or strip boundary (a row whose centre is exactly a quarter
  of the height, say), the GPU puts it on one side and NekoPhoto on the other: a one-pixel line of the neighbouring
  block in Hard Glitch and Splitter, and Caption's first column. Ascii meets this everywhere: with a cell of 10 pixels
  every fifth row and column of pixels lies exactly on a boundary between two rows of a glyph (a third of them with a
  cell of 6), so a glyph's row or column can show one pixel off.
- VHS's tape noise, Super 8's grain, Bad TV's static and the per-pixel jitter of Smear's and Zoom Blur's taps hash those
  coordinates' bits, so their grain is a different draw of the same noise; with the noise off VHS and Bad TV match to
  a level, and Super 8 does without grain. The interpolation was probed: every row of the GPU's coordinates lies within
  two units in the last place of the exact one, but no single-precision formula (the plane equation from any vertex,
  fused or not) reproduces all of them.
- The GPU fuses a multiply and an add feeding the seeded hashes of Decimate, Data-Mosh and Mask Blocks, and divides by
  multiplying with the reciprocal in Hard Glitch; the ports do the same, so those patterns match (unfused, Mask Blocks
  puts 9% of its blocks elsewhere). Watercolor's choice of quadrant compares variances that are equal on flat areas;
  the port computes brightness with the GPU's fused multiply-adds, which settles most of those ties OpenMosh's way.
  Its bilinear filter weighs in steps of 1/256 of a pixel, which the port's sampler also does.
- OpenMosh keeps the frames between Glow's passes at 8 bits; NekoPhoto at 16. The blurred glow is smoother, and at a
  high Intensity (the glow is added up to four times over) a pixel can differ by a few levels.

The ports compute sin, cos, atan2, exp and log from their own polynomials and are built without fused multiply-adds, so
the same settings give the same pixels on every platform and any number of threads (`tests/mosh_tests.cpp` holds
fingerprints of each effect).

## Smart Filters

Mosh effects are not yet available as Smart Filters. NekoPhoto keeps a smart object's filter stack as Photoshop's own
`filterFX` descriptor with Photoshop's filter ids, so the stack survives a PSD round trip; a Mosh entry would need a
filter class Photoshop does not know, and how such an entry is kept (and what Photoshop does with a file that has one)
is still to be decided. On a smart object, Filter > Mosh asks to rasterize first, like Camera Raw and G'MIC.
