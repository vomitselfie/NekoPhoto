# PSD round trip

Opening a PSD and exporting it as PSD again gives back what NekoPhoto does not model, as long as it is
still true of the layer. The model is `src/core/include/compositor/psd_carry.h`; the importer
(`psd.cpp`) fills it, the writer (`psd_writer.cpp`) writes it back, and the project package keeps it
(`images/<layer id>.psdcarry`, `images/document.psdcarry`), so a PSD can be saved as a project,
reopened and still exported with everything carried.

## What is carried

Per layer and folder, as the file's bytes:

| What | Written back |
|---|---|
| Layer styles (`lfx2`, `lmfx`, `lfxs`, `lrFX`; drawn here too: [layer-styles.md](layer-styles.md)), Blend If ranges, knockout, blend-interior options, colour label, locks, metadata, and every other tagged block we do not read | always |
| The blend mode's own key (Linear Burn, Vivid Light, Dissolve ...), and an isolated (Normal) folder | while the layer keeps the mode it was read as |
| Opacity and Fill as two values | while the combined opacity is unchanged; otherwise our opacity, Fill 100% |
| Vector masks (`vmsk`, `vsms`) | always, the path moved with the layer ([vector-masks.md](vector-masks.md)) |
| A shape's stroke and fill content (`vstk`, `vscg`) | always |
| A shape's live-shape origination (`vogk`, `vowv`) | while the layer has not moved or changed size |
| Editable text (`TySh`), fill layers (`SoCo`, `GdFl`, `PtFl`), the adjustment layers we have no counterpart for (Brightness/Contrast, Color Balance, Selective Color ...) | while the pixels are the ones imported, and in place |
| The mask section and mask channels as stored (vector-mask coverage, mask density and feather, both masks) | while the mask and the layer's place are unchanged (8-bit PSD sources) |
| Photoshop's layer id (`lyid`), and the folder's closed state and end-marker blocks | always (ids stay unique: duplicates get new ones) |
| Smart objects (`SoLd`, `SoLE`, `PlLd`) | as long as the layer still places its source, the placement patched to where it is ([smart-objects.md](smart-objects.md)) |

Per document: the image resources (colour profile, XMP, EXIF, captions, print settings, layer comps,
guides, slices, paths, plug-in resources) and the global blocks (linked smart-object data, patterns,
text engine data, filter masks).

What is not carried: the resolution (ours), thumbnails, what indexes layers or alpha channels by
position, the ID seed; guides, slices and paths once the canvas size changes; the colour profile of a
CMYK, Lab or grayscale file (the export is RGB); and 16-bit or PSB smart-filter caches (`FEid`, `FXid`,
which Photoshop rebuilds).

Adjustment and fill layers we have no counterpart for now open as empty layers that carry them, instead of
being dropped; painting on one turns it into a pixel layer on export (a warning says so).

"Unchanged pixels" is a fingerprint of the layer's pixels (`psdContentHash`), stable across a project
save and reopen. When a carried block is left out, the export summary says which and why.

## Text

**Export.** A NekoPhoto text layer is written as a Photoshop type layer ('TySh', `src/core/src/psd_text.cpp`) over the
pixels we drew: point text, one style run per run (below), the face's PostScript name (read from the font's `name` table), its size in
pixels (Photoshop's engine units are document pixels), colour, alignment, fixed leading equal to our line height,
letter spacing as tracking, and bold or italic as faux styles where Qt synthesised them. It is anchored at the first
baseline (on the left edge, the centre or the right edge of the block by alignment) through a matrix carrying the
layer's scale and rotation, so Photoshop's own layout lands on our lines. The layout follows Patchy's writer and the
rules it pinned: fixed leading needs `/AutoLeading false`, tracking is an integer, numbers stay short, and the block
stays even without a pad after its end-anchored tail. The metrics come from the app (`app::psdTextMetrics`); the core
has no font engine, and without them text is written as pixels. A flipped layer is written as pixels.

**Import.** A Photoshop type layer opens as NekoPhoto text when our model can hold it: horizontal point text, one
alignment, RGB fill, no warp, skew, mirroring, horizontal or vertical scale, superscript or subscript. Turned text
opens as text too: drawn upright at its scale, the layer turned by Photoshop's angle about the anchor on its first
redraw; later redraws keep a turned layer's top-left corner where it is (a turned layer turns about its centre, so a
new size would otherwise slide it). Glyphs are drawn unhinted, as Photoshop draws them. Paragraph (box)
text keeps its frame (`boxWidth` / `boxHeight`): lines wrap at its width, a line that does not fit its height is
hidden, and the first baseline sits the first line's cap height below the frame's top, the rules Patchy pinned
against Photoshop; it is anchored at the frame's top-left and written back as `ShapeType 1` with `/BoxBounds [0 0 w
h]` (a frame moved down would move Photoshop's text by twice as much). A type layer with nothing typed opens as
empty text, a transparent pixel where Photoshop anchored it. A scale a hair
uneven (under 1.5%, a transform nudged by hand) is read as its vertical scale. It keeps Photoshop's
pixels until it is edited; the first redraw puts our first baseline where Photoshop anchored its own. The face is
found among the installed families by its PostScript name ("ArialMT" is Arial); one that is not installed is spelled
out for fontconfig ("TimesNewRomanPSMT" asks for Times New Roman, which gets its metric twin) and noted. Photoshop's
leading, fixed or automatic, becomes our line spacing. Everything else (box text, vertical, warped)
shows as Photoshop's pixels, and its own 'TySh' comes back on export while those pixels are unchanged; so does the
original block of a layer opened as text and not edited, since it says more than ours.

**Several styles.** Text whose runs differ (runs are compared over the normal style sheet, as Photoshop leaves out what
equals it) keeps them: `LayerText::runs`, each with its face, size, weight, bold and italic, colour, tracking,
baseline shift, leading, caps (small or all) and underline or strikethrough, in UTF-16 units of the text. The face's
weight comes from its name, in words ("Medium", "Light") or Linotype's abbreviations (`HelveticaNeueLTStd-Th`, `-Lt`,
`-Md`, `-BdCn`). The app draws each line through QTextLayout with a format per run; a line sits the largest leading
among its runs below the one before (Photoshop's rule), tabs stop every 36 pixels (Photoshop's half inch), and export
writes one style run per run with its own face in the font set. Editing through the text dialog or `text.set`
carries into the runs: typing extends the run it starts in, a new size scales every run (and its leading), and any
other field changed applies to all of them; a field the dialog merely rounded (a whole-pixel size, a stand-in for a
missing font) is not a change. Projects keep the runs in the manifest's text object (the Mac app ignores them).

**Missing faces.** A face that is not installed is asked for by its family name; for faces sold with Adobe's apps
the app asks fontconfig for a free twin with the same metrics (Helvetica and Helvetica Neue: TeX Gyre Heros, Nimbus
Sans, Liberation Sans; Myriad: Source Sans; Verdana: DejaVu Sans), since fontconfig aliases "Helvetica" but not
"Helvetica Neue LT Std". A medium or heavier weight the stand-in lacks is drawn in its bold (Qt would fall back to
regular). On Manjaro, `tex-gyre-fonts` and `gsfonts` give the closest Helvetica widths; no free font has Helvetica
Neue's thin, light and medium weights.

On the 2014 styleguide (a text-heavy corporate file), all 22 of its type layers now open as text: 14 in several
styles, one box of paragraph text, five empty. After a redraw their lines, tabs, indents and frame land where
Photoshop's do, Liberation Sans standing in for Helvetica Neue.

Checked on Patchy's Photoshop text fixtures: after an edit, our lines land within a pixel of Photoshop's (automatic
and fixed leading, tracking, centred and right-aligned anchors), with Liberation Sans standing in for Arial.

## Masks

Photoshop stores a layer that has a vector mask and a painted mask with the painted one in channel -3
(the "real" mask, with its own rectangle) and the vector mask's coverage in -2. The importer now takes the
real mask as the layer's mask; before, it took -2 and lost the painted mask. A layer with only a vector
mask shows its coverage as its mask here, and exports with the mask section as Photoshop wrote it.

## Write rules adopted from Patchy

Patchy (MIT, `src/third_party/patchy_psd/README.md`) pinned these against Photoshop 2026, and the writer
follows them: layer record flags bit 3 is set on every layer; tagged blocks declare an even length with
the pad byte inside; global blocks are padded to four bytes outside their length; no private per-layer
tags; a pass-through folder says `norm` in its record and `pass` in `lsct`; every layer gets a unique
`lyid` (Photoshop requires one on smart-object layers with filter caches).

## Checking

`build/tests/psd_roundtrip DIR` opens every PSD in a folder, exports it and compares the two files record
by record: every carried block, Blend If, the mask section, blend key, flags, folder state, opacity and
clipping, the resources and the global blocks, byte for byte; then reopens our file. Over Patchy's 117
Photoshop-saved fixtures (text, smart objects and smart filters, shapes, styles, masks, Blend If, 16-bit
and PSB): all pass, 3,675 blocks back unchanged. `psd_writer_tests` covers the edit cases (painted, moved,
opacity changed, a project save, a canvas change).

Not verified: that Photoshop opens our files without a warning, or re-lays our type layers exactly where we drew
them. The structure matches what Photoshop
wrote, but the files have not been opened in Photoshop; that check is still to do.

## Colour

A CMYK file (or a CMYK smart object's contents) converts to sRGB through the file's own ICC profile with the
vendored Little CMS (`src/core/src/colour.cpp`; relative colorimetric, black point compensation, no dither, as
Patchy calibrated against Photoshop's conversion); only a file without a profile falls back to the plain formula.
On a CMYK-illustrated styleguide this brought the smart objects within 0.3 levels of Photoshop's own render.
