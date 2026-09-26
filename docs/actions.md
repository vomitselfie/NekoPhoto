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
  skips that edit).

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
| Image > Canvas Size, Image Size, Trim, Crop to Selection, Flip Canvas | `canvas.resize`, `image.resize`, `image.trim`, `canvas.crop`, `canvas.flip` |
| Image > Adjustments (every dialog), Invert | `pixels.adjust` with the dialog's settings, `pixels.invert` |
| Filter > Gaussian Blur, Motion Blur, Add Noise (with its seed), Lens Correction | `pixels.filter` |
| Edit > Fill with Foreground / Background, Clear, Content-Aware Fill | `pixels.fill` with the colour, `pixels.clear` (or `layers.delete` without a selection), `pixels.contentAwareFill` |
| Layer > New Layer, New Layer Below, New Folder, Group, Duplicate, Delete, Merge Down, New Adjustment Layer, Layer Mask (every item), Flip Layer | `layers.add`, `layers.group`, `layers.duplicate`, `layers.delete`, `layers.merge`, `layers.mask`, `layers.flip` |
| Select > All, Deselect, Inverse, Modify (Expand, Contract, Feather, Smooth, Border), Load Layer Pixels / Mask | `selection.all`, `selection.none`, `selection.invert`, `selection.grow`, `selection.feather`, `selection.smooth`, `selection.border`, `selection.fromLayer` |
| A rectangular or elliptical marquee | `selection.rect` with its box and mode |
| A Brush or Eraser stroke (on pixels or a mask) | `brush.stroke` with its points (to a tenth of a pixel), size, hardness, opacity, colour, preset, and the pen's pressure when a tablet drew it |
| Swap or reset the colours, or pick one in the colour dialog | `colors.set` |

Steps act on the active layer, as the menu command did, so an action recorded on one document plays on another.
Requests an agent records may name layers by id (`layers.select`, `layers.set`); those ids belong to the document
they were recorded on, so edit or switch off such steps before playing the action elsewhere.

Not recorded (yet): the Move tool and Free Transform drags, the lasso and magic wand, Quick Select, the Gradient,
Shape, Text, Clone, Healing, Smudge and Dodge tools, the Layers panel's own buttons, eyes, opacity and blend
controls, the Paths panel, G'MIC, Camera Raw and Remove Background dialogs, layer styles and smart objects. Each can
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
