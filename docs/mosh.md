# Mosh

Filter > Mosh holds glitch, distortion and retro effects from [OpenMosh](https://github.com/vomitselfie/openmosh), a
photo-effects app whose effects are WGSL shaders. NekoPhoto runs them on the CPU: each shader is ported line for line
to C++ (`src/core/src/mosh_effects.cpp`) over a small runtime that behaves like the GPU it was written for
(`mosh_runtime.h`). OpenMosh is MIT-licensed, by the same author; the notice is in
[THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md) and [LICENSES/OpenMosh-MIT.txt](../LICENSES/OpenMosh-MIT.txt).

Each effect opens a dialog whose controls come from its parameter list, previews on the canvas, and applies to the
active layer's pixels inside the selection as one undo step named after the effect. The seeded effects have a **Seed**
and a **Reroll** button: the same seed always draws the same pattern. The effects work at 8 and 16 bits per channel.

Automation: `pixels.mosh` with `effect` (the id below), `params` (by key; switches as booleans, a choice as its index
or its option's name) and `seed` (0 to 100). The MCP tool is `pixels_mosh`.

## The effects

Ranges and defaults are OpenMosh's, so a value from an OpenMosh preset means the same here. Angles are in radians.
Sizes in pixels are the layer's pixels.

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
| Kaleidoscope (`kaleidoscope`): mirrored wedges about the centre | `segments` 2-24, 6; `rotation` 0-6.28, 0 | |

### Retro

| Effect (id) | Parameters | Seeded |
|---|---|---|
| Pixelate (`pixelate`) | `size` (Block Size) 1-128, 12 | |
| Scan Lines (`scanlines`) | `density` 10-800, 250; `opacity` 0-1, 0.5 | |
| VHS (`vhs`): tracking jitter, colour bleed, tape noise | `tracking` 0-1, 0.4; `bleed` (Color Bleed) 0-1, 0.4; `noise` 0-1, 0.3 | yes |
| 8-Bit CGA (`cga-8bit`): pixelated to a four-colour palette | `size` (Pixel Size) 1-64, 6; `palette` Cyan/Magenta, Green/Red or Grayscale | |
| CRT (`crt`): barrel curvature, scanlines, an aperture mask | `curvature` 0-1, 0.3; `scan` (Scanlines) 0-1, 0.5; `mask` (Aperture Mask) 0-1, 0.3 | |
| Dither (`dither`): Bayer 4x4 ordered dither | `scale` 1-16, 2; `levels` 2-8, 2 | |
| Dot Screen (`dot-screen`): black dots on white, sized by darkness | `scale` 2-64, 8; `angle` 0-3.14, 0.4 | |
| Halftone (`halftone`): colour dots on black, sized by brightness | `scale` 2-64, 10; `angle` 0-3.14, 0.4 | |

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

The rest of OpenMosh's catalogue (the Stylize, Color and Composite effects, and the remaining Distort and Retro ones)
is to come.

## How close to OpenMosh

Each port was checked against OpenMosh's own render (its headless command line, on an NVIDIA GPU through Vulkan) on
three images (two photos and a synthetic gradient with shapes), at the defaults and at one or two other settings.
Differences are in 8-bit levels, over every channel:

| Effect | Mean difference | Largest | Pixels off by more than 2 levels |
|---|---|---|---|
| Soft Glitch, Jitter, Slices, Shake, Wave, Kaleidoscope, Scan Lines, CRT, Dither, Dot Screen, Pixelate, Decimate, Data-Mosh | 0.02-0.34 | 1 | none |
| Pixel Sort, Strobe, 8-Bit CGA | 0 | 0 | none |
| Halftone | 0.015 | 21 | 0.001% (single pixels on dot edges) |
| Hard Glitch | 0.15 | 208 | 0.25% (block edges) |
| Splitter | 0.22 | 188 | 0.26% (one row, at defaults on one image) |
| VHS without noise | 0.02 | 1 | none |
| VHS with noise | 4-7 | 253 | the grain itself (see below) |

What remains is the GPU's own arithmetic:

- A GPU interpolates each pixel's coordinates to within a few units in the last place, where NekoPhoto uses the exact
  pixel centre. Where a coordinate lands exactly on a block or strip boundary (a row whose centre is exactly a quarter
  of the height, say), the GPU puts it on one side and NekoPhoto on the other: a one-pixel line of the neighbouring
  block in Hard Glitch and Splitter.
- VHS's tape noise hashes those coordinates' bits, so its grain is a different draw of the same noise; with Noise at 0
  VHS matches to a level.
- The GPU fuses a multiply and an add feeding the seeded hashes of Decimate and Data-Mosh, and divides by multiplying
  with the reciprocal in Hard Glitch; the ports do the same, so those patterns match. Its bilinear filter weighs in
  steps of 1/256 of a pixel, which the port's sampler also does.

The ports compute sin, cos and atan2 from their own polynomials and are built without fused multiply-adds, so the
same settings give the same pixels on every platform and any number of threads (`tests/mosh_tests.cpp` holds
fingerprints of each effect).

## Smart Filters

Mosh effects are not yet available as Smart Filters. NekoPhoto keeps a smart object's filter stack as Photoshop's own
`filterFX` descriptor with Photoshop's filter ids, so the stack survives a PSD round trip; a Mosh entry would need a
filter class Photoshop does not know, and how such an entry is kept (and what Photoshop does with a file that has one)
is still to be decided. On a smart object, Filter > Mosh asks to rasterize first, like Camera Raw and G'MIC.
