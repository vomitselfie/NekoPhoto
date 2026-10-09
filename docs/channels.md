# Channels

Window ▸ Channels is Photoshop's Channels panel: the colour channels of the image (RGB, Red, Green, Blue) as views,
then the document's alpha and spot channels, then Quick Mask while it is on. Alpha channels are saved selections;
spot channels hold a spot colour's ink and are kept from PSD files, shown, and written back. Everything here works
at 8, 16 and 32 bits ([bit-depth.md](bit-depth.md)) and in CMYK and Lab documents (below). The code: `src/core/include/compositor/channels.h` (the model, Save and Load Selection, the canvas
view, single-channel editing), `src/core/src/psd_channels.cpp` (PSD), `src/app/EditorSessionChannels.cpp`,
`ChannelsPanel.cpp`, `ChannelDialogs.cpp`.

## The panel

| Row | Click | Shortcut |
|---|---|---|
| RGB | all three colour channels are the target and shown (the usual state) | Ctrl+2 |
| Red, Green, Blue | that channel alone is the target, shown in gray; Shift-click adds another (shown in colour) | Ctrl+3, Ctrl+4, Ctrl+5 |
| An alpha channel | it becomes the target and shows over the image in its colour | Ctrl+6 to Ctrl+9 for the first four |
| A spot channel | shown over the image in its ink (not edited in this version) | as above |
| Quick Mask | the Quick Mask's layer is painted | |

- The eye shows or hides a channel. With no colour channel shown, the target alpha channel (or the first one shown)
  is drawn alone in gray; hiding the target's eye stops editing it.
- **Ctrl-click a thumbnail** loads the channel as the selection: Ctrl replaces, Ctrl+Shift adds, Ctrl+Alt subtracts,
  Ctrl+Shift+Alt intersects (`thumbnailClickMode`). The composite loads its luminosity (0.30 R + 0.59 G + 0.11 B),
  a colour channel its values; both are read over white, as Photoshop's channels show transparency.
  Ctrl+Alt+2 to Ctrl+Alt+9 load the same channels without the panel (Photoshop CC's keys).
- The footer: Load channel as selection, Save selection as channel, Create new channel, Delete current channel.
  The context menu adds Duplicate, Rename, Channel Options and Load as Selection; double-clicking an alpha channel
  opens Channel Options; alpha channels are dragged to reorder them.
- **Channel Options**: the name, Color Indicates Masked Areas (Photoshop's default: the colour covers what is not
  selected) or Selected Areas, the overlay colour and its opacity (a spot channel's solidity). Switching Color
  Indicates inverts the channel's gray, as Photoshop does, so the channel still selects what it did.

### Keyboard shortcuts

Photoshop's defaults: Ctrl+2 composite, Ctrl+3/4/5 red, green, blue, Ctrl+6 and up the alpha channels, and with
Alt the channel as a selection. None of them was bound before (Ctrl+0 and Ctrl+1 are Fit on Screen and Actual
Pixels; the plain digits set the brush opacity and are left alone), so there is no clash. Photoshop's older layout
(Ctrl+~ for the composite, Ctrl+1 for red) is not offered.

## Selections and channels

- **Select ▸ Save Selection**: into a new channel (with a name), or into an alpha channel: replace it, add to it,
  subtract from it or intersect with it. The channel keeps its own sense: saving into a Selected Areas channel
  stores the inverse gray.
- **Select ▸ Load Selection**: from an alpha channel, a layer's transparency or a layer's mask (Photoshop's list),
  optionally inverted, as a new selection or added to, subtracted from or intersected with the current one. A
  result that selects nothing leaves no selection. The layers that stand in for Quick Mask and a channel being
  painted are left out of the composite a colour channel is read from.

Both are one undo step, and both are at the document's depth.

## Editing an alpha channel

A target alpha channel is painted as Quick Mask is: a temporary layer at the top holds the channel's colour at its
opacity, its mask the inverse of the channel's gray, and it is the active layer with its mask selected. Every mask
tool works on it (brushes in every engine, the eraser, gradients, fills, filters, Invert), painting white adds to
what the channel selects, and each edit's change is written into the channel in the same undo step. The layer is
hidden from the Layers panel and never written; saving, exporting, choosing a layer or targeting another channel
takes it away. Choosing the channel is not an undo step (it is not one in Photoshop); undoing past that point ends
the editing. The Layers panel's opacity, while the layer is active, is the channel's overlay opacity.

## Single-channel editing

With one or two colour channels the target, an edit of a layer's pixels changes only them: the brush and eraser,
fills and Delete (which fills the channels with the background colour, as in Photoshop), the Paint Bucket, gradients,
the retouching tools, adjustments and filters on pixels, and moved selected pixels. The edit is made as usual, then
limited when it ends (`restrictToColorChannels`): each changed layer keeps its other channels and its alpha, and its
old grid, so painting never grows a layer or changes its transparency, and a blank layer stays blank. Where the edit kept a pixel's alpha, the channel's new value is
exactly the edit's; elsewhere the edit's straight colour is put back at the old alpha. Brush strokes, gradients and
adjustment and filter previews show on the canvas as they will land. Paste writes the clipboard's gray into the
active channels of the active layer (or into the target alpha channel), as Photoshop pastes into a channel.

Edits of the canvas or of the layer structure (Image Size, Crop, merges, masks added or applied, free transforms)
work on whole layers, as in Photoshop. With all three colour channels active, which is the usual case, none of
this runs and every edit takes its usual path, so rendering and brush parity are unchanged.

The canvas shows one colour channel alone in gray, several in their colours, and any alpha or spot channel shown
as its colour over its dark areas (`applyChannelView`, after the render; the default view leaves the frame alone).

## CMYK and Lab

A CMYK document's colour channels are **CMYK, Cyan, Magenta, Yellow and Black** (Ctrl+2 to Ctrl+6, the alpha channels
from Ctrl+7), a Lab document's **Lab, Lightness, a and b** (Ctrl+2 to Ctrl+5). The session's colour channel bits have
one bit per channel of the mode (`colorChannelsAllFor(mode)`: 15 for CMYK, 7 for RGB and Lab). The view reads them from
the frame at the document's layout (`ChannelView::native`, `renderNative`): one channel alone in gray, a CMYK plate
with its ink dark as Photoshop shows it, a and b gray where neutral; several CMYK inks as inks on white paper; several
Lab channels as their colour with the hidden ones neutral. Loading a CMYK channel as a selection selects its ink (paper
where the composite is transparent), a Lab channel its value. Single-channel editing (`keepColorChannels`,
`restrictToColorChannels`) works on 4 or 5 samples a pixel, so any pixel edit (painting, retouching, Fill,
adjustments and filters) changes only the target channels, as in RGB. See [color-modes.md](color-modes.md).

## Files

- **PSD**: the merged image's planes after the colour (and its transparency) are the alpha and spot channels,
  named by resource 1045 (Unicode; 1006, Pascal, when it is missing), shown as resource 1077 (the older 1007) says
  (colour space and colour, opacity, and kind: 0 selected areas, 1 masked areas, 2 spot), identified by 1053.
  Some writers name the merged transparency as a channel too (Patchy's `arrows.psd`): a list one entry longer than
  the channels skips its first entry. Writing, NekoPhoto adds its channels after the transparency and writes 1006,
  1045, 1077 and 1053; an unchanged channel keeps its stored DisplayInfo record and identifier, and a 16-bit file's
  channel keeps its own 0..65535 samples while its gray is the one read from them (the carry, `PsdChannelCarry`),
  so an untouched channel round-trips byte for byte. A document without channels writes what it always did.
  Spot colours in a colour book or custom space show in gray; their record goes back as it was.
- **Projects**: a `channels` manifest array (format 8): id, name, kind, colour, opacity, colorIndicates, and
  `file`: `channels/<id>.png`, a gray PNG at the document's depth, with `channels/<id>.psdcarry` for a channel read
  from a PSD. Older projects have no channels and load as before; a document without channels writes no key.
- TIFF extra alpha channels are not read or written (TIFF goes through Qt's plugin, which has no access to them).

Channels count toward the document's mask budget and the undo history's memory, and follow Crop, Canvas Size
(black where the canvas grows), Image Size (resampled with the chosen method) and Flip Canvas. A document holds at
most 53 alpha and spot channels (Photoshop's 56 with the colour channels).

## Automation

`channels.list`, `channels.new`, `channels.duplicate`, `channels.delete`, `channels.select`, `channels.set`,
`channels.saveSelection` and `channels.loadSelection` ([automation.md](automation.md)); the MCP bridge's
`channels_*` tools.

## Not yet

- Editing spot channels (New Spot Channel, painting ink, Merge Spot Channel), and spot channels in the view with
  their ink's real colour when it is a colour book's.
- Apply Image and Calculations.
- Split Channels and Merge Channels, and Multichannel mode.
- TIFF extra channels.
