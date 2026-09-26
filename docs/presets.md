# Photoshop presets

File ▸ Import Presets… (and Layer ▸ Layer Style ▸ Import Styles…) reads Photoshop's preset libraries into
NekoPhoto's own preset library:

| File | What it holds | Where it shows up |
| --- | --- | --- |
| `.asl` | Layer styles, and the patterns they use | Layer ▸ Layer Style ▸ Apply Style |
| `.pat` | Patterns | Added to the open document; the Layer Style dialog's Pattern Overlay list (as "name (imported)") |
| `.grd` | Gradients | The Gradient tool's preset list; the Layer Style dialog's gradient Preset list |

The library lives in the app data folder (`~/.local/share/nekophoto/nekophoto/presets` on Linux):
`styles.json` (each style as the JSON `layers.style` shows), `patterns.pat` and `gradients.grd` (written back in
Photoshop's own formats). Importing a style or gradient whose name is already there replaces it; a pattern whose id
is already there is kept.

## Styles

Applying a style replaces the layer's effects, as one undo step ("Apply Style"). A style that uses patterns (a
pattern overlay, a bevel texture) gives the document those patterns first, in its `Patt` block, so they draw, save
in the project and export to PSD. Choosing an imported pattern in the Layer Style dialog does the same.

What is left out: a style's blending options (opacity, fill opacity, blend mode, Blend If), since NekoPhoto's layer
styles hold effects only; the import notes say so. Effects that are off in the preset stay off, and Use Global Light
follows the document's light.

## Patterns

Records are 8-bit RGB, grayscale, indexed or CMYK patterns up to 4096 px on a side, raw or PackBits; other ones
(16-bit, larger) are left out with a note. A record without an id gets one, re-encoded as RGB.

## Gradients

A `.grd` (version 5, Photoshop 6 and later) gives named solid gradients: colour stops (a stop can stand for the
foreground or background colour, filled in when the gradient is drawn), opacity stops and midpoints. Noise gradients
and version 3 files (Photoshop 5) are not read.

The Gradient tool draws them with every stop: each run between two stops blends linearly after its midpoint remap,
colours are held beyond the first and last stops, and on a mask each stop counts by its lightness. Photoshop's
Smoothness easing is not applied to the tool's fills (it is kept in the preset and used by layer styles).

## Automation

`presets.import`, `presets.list`, `presets.remove`, `layers.applyStyle` and `gradient.draw`'s `preset`
([automation.md](automation.md)).

## Code

`src/core/src/presets.cpp` reads and writes the three formats; the envelopes and record layouts are ported from Patchy
(MIT, `src/third_party/patchy_psd/README.md`: `src/psd/asl_io`, `pat_reader`, `grd_io`, `psd_patterns`). An `.asl`'s
`Lefx` object is the same descriptor as a layer's `lfx2` block and goes through the same parser (`layerstyle.cpp`); a
`.pat` record is a `Patt` record with a slightly different header and is stored in that shape. `src/app/PresetLibrary`
keeps the library; `tests/presets_tests.cpp` covers multi-stop fills and each format's round trip.
