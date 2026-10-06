# Actions

Window > Actions (Alt+F9) is Photoshop's Actions panel: record what you do to a document, then play it back on
another document, or on a whole folder with File > Automate > Batch.

## How it works

A step is an automation request: a method from [automation.md](automation.md) and its parameters, for example
`{"method": "pixels.filter", "params": {"kind": "Gaussian Blur", "radius": 4}}`. Playing an action sends its steps
to the automation engine in order, as an agent would, so a step does exactly what the socket does. This has three
results:

- Anything an agent can do can be a step, and steps can be edited as JSON (double-click a step in the panel, or
  `actions.save` over the socket).
- An agent recording an action gets its own requests as steps, with nothing to translate.
- The menu commands, dialogs and tools that record are the ones that know the request that repeats them. The list
  below is what records today; anything else you do while recording is not added (the action still plays, it just
  skips that edit). Almost every menu command in the table runs the request itself (the command path,
  CONTRIBUTING.md, "Commands"), so what records is exactly what plays back.

The library lives in `actions.json` in the app data folder (`~/.local/share/nekophoto/nekophoto/actions.json`), so it
outlasts the session. Import and Export (the panel's … menu, or `actions.import` / `actions.export`) read and write
the same format: `{"format": "nekophoto.actions", "version": 1, "actions": [{"name", "steps": [{"method", "params",
"enabled"}]}]}`.

## The panel

- **●** records into the selected action (or asks for a name); **■** stops. **+** makes a new action and starts
  recording it, as in Photoshop.
- **▶** plays the selected action on the current document. The steps it makes become one undo step named after the
  action, unless a step switches tabs or opens and closes documents. It stops at the first step that fails and says
  which.
- A step's checkbox switches it off without deleting it. Up and down reorder steps; the bin deletes the selected step
  or action.
- The … menu: rename the action, edit the selected step's parameters, import, export, and Batch.

## What records

| You do | Recorded as |
| --- | --- |
| Any editing request on the automation socket (from an agent, a script or `--batch`) | the request itself |
| Image > Mode (RGB, CMYK, Lab; 8, 16, 32 Bits, with HDR Toning's settings) | `image.mode` |
| Image > Canvas Size, Image Size, Trim, Crop to Selection, Flip Canvas | `canvas.resize`, `image.resize`, `image.trim`, `canvas.crop`, `canvas.flip` |
| Image > Adjustments (every dialog), Invert | `pixels.adjust` with the dialog's settings, `pixels.invert` |
| Filter > Gaussian Blur, Motion Blur, Add Noise (with its seed), Lens Correction | `pixels.filter` |
| Edit > Fill with Foreground / Background, Clear, Content-Aware Fill | `pixels.fill` with the colour, `pixels.clear` (or `layers.delete` without a selection), `pixels.contentAwareFill` (Auto and Wide Area sampling) |
| Edit > Assign Profile, Convert to Profile (a built-in profile or none) | `document.profile` |
| Edit > Cut, Copy, Copy Merged, Paste | `pixels.cut`, `pixels.copy`, `pixels.copyMerged`, `pixels.paste`; with layers selected and no selection, Copy is `layers.copy`, and Paste of copied layers `layers.paste` |
| Layer > New Layer, New Layer Below, New Folder, Layer via Copy, Group, Duplicate, Delete, Merge Down, New Adjustment Layer, Layer Mask (every item), Flip Layer | `layers.add`, `layers.viaCopy`, `layers.group`, `layers.duplicate`, `layers.delete`, `layers.merge`, `layers.mask`, `layers.flip` |
| Layer > Rename Layer, Create / Release Clipping Mask, Resampling | `layers.set` on the active layer (no id) |
| Layer > Bring Forward, Send Backward | `layers.reorder` |
| Layer > Smart Objects > Convert to Smart Object, Replace Contents, Rasterize | `smartObject.convert`, `smartObject.replace` with the file, `smartObject.rasterize` |
| Select > All, Deselect, Inverse, Reselect, Modify (Expand, Contract, Feather, Smooth, Border), Load as Selection (Layer Pixels, Layer Mask, Add, Subtract, Intersect) | `selection.all`, `selection.none`, `selection.invert`, `selection.reselect`, `selection.grow`, `selection.feather`, `selection.smooth`, `selection.border`, `selection.fromLayer` |
| The Layers panel: New layer, New folder, a new adjustment layer, Add layer mask, Delete, Duplicate, Merge Down and the mask items of a row's menu | `layers.add`, `layers.mask`, `layers.delete`, `layers.duplicate`, `layers.merge` |
| The Layers panel: opacity (a drag is one step, at its release), blend mode, an eye clicked, Alt-click to clip or release, Rename | `layers.set` on the active layer (no id); an eye of another layer names that layer by id |
| The Layers panel: a row dragged to another place | `layers.move` (by the layers' ids) |
| The Paths panel: Make Work Path, Fill, Stroke, Make Selection, Add to Selection, Make Shape Layer, Delete | `paths.fromSelection`, `paths.fill`, `paths.stroke`, `paths.toSelection`, `paths.toShape`, `paths.delete` (by the path's id: the Work Path's is the same in every document) |
| The Channels panel: Save selection as channel, New Channel | `channels.saveSelection`, `channels.new` |
| A rectangular or elliptical marquee | `selection.rect` with its box and mode |
| Free Transform of a layer, applied (Enter, Apply or a double-click) | `layers.setTransform` with the box's position, size, angle and flips |
| A guide dragged out of a ruler, moved, or dragged off; View > New Guide…, Clear Guides | `guides.add`, `guides.move`, `guides.delete` |
| A Brush or Eraser stroke (on pixels or a mask) | `brush.stroke` with its points (to a tenth of a pixel), size, hardness, opacity, colour, preset, and the pen's pressure when a tablet drew it |
| Swap or reset the colours, or pick one in the colour dialog | `colors.set` |

Steps act on the active layer, as the menu command did, so an action recorded on one document plays on another.
Requests an agent records may name layers by id (`layers.select`, `layers.set`), as do a dragged Layers panel row
and an eye clicked on a layer other than the active one; those ids belong to the document they were recorded on, so
edit or switch off such steps before playing the action elsewhere.

Not recorded (yet): the Move tool's drags and a distortion or a transform of several layers, a mask alone or selected pixels, the lasso and magic wand, Quick Select, the Gradient,
Shape, Text, Clone, Healing, Smudge and Dodge tools, an eye swipe across several layers, deleting several layers
from the Layers panel, Alt-dragging a layer or a mask there, G'MIC, Camera Raw and Remove Background dialogs, layer styles, smart objects' Edit Contents and filters,
vector masks, Type > Create Work Path and Convert to Shape (their methods name the layer), and the Channels panel's
other items (they name a channel). Each can
still be added by hand as the request that does it (`rpc.describe` lists every method's parameters). Looking
(`layers.list`, `render`, `document.info` and the like), tab switching, undo and redo are never recorded.

Photoshop `.atn` files are not read: their steps are Photoshop's own descriptors, most of which have no NekoPhoto
equivalent.

## Batch

File > Automate > Batch (or `actions.batch`) takes an action, a source folder and a destination folder. Every image,
PSD, project, Aseprite, Clip Studio or icon file in the source (not its subfolders) opens in a tab of its own, the
action plays, the result is exported to the destination under the same name with the chosen format (png, jpg, webp,
tif, psd, gif or tga) and the tab closes without saving. Files already in the destination are skipped unless
Replace is on. A file that fails to open, play or export is listed with the stage and the reason, and the batch goes
on to the next one. A step that saves or closes the document itself works, but is rarely what a batch wants.
