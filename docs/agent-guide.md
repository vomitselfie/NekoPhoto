# Working with NekoPhoto as an agent

How to get good results driving the editor through MCP, `--call`, `--batch`
or the socket. `docs/automation.md` has the method reference; this is the
practice.

## The loop

1. Look before touching: `document.overview` (the layer tree with ids, the
   selection and the undo step in one call; the MCP tool adds a render with
   `render: true`), then `render` at a modest `maxSize` (512 is plenty for
   orientation). `rpc.describe` says what any method takes.
2. Act in small steps. Every method is one undo step; if a render shows the
   wrong thing, `history.undo` and try again. `history.list` shows what you did.
   Wrap a change of many steps in `history.beginGroup {"name": "Retouch by agent"}`
   ... `history.endGroup` so the person can take it back in one Undo, and send
   calls that need no look in between as one `rpc.batch` (named: all or nothing).
3. Verify with `render` after each meaningful change, cropping with `region`
   at full resolution for details (edges of a mask, text, a blemish).
4. Save or export at the end: `document.save` for the editable project,
   `document.export` for a PNG or JPEG.

Coordinates are document pixels, origin top-left, y down. Layer transforms
give the layer's box in those units; `pixelSize` is the raster underneath it.

## Recipes

Cut out a subject and put it on a new background:

```
document.open {"path": "photo.jpg"}
pixels.removeBackground {}                       # needs the model enabled in Preferences
layers.add {"kind": "pixels", "name": "Backdrop", "below": true}
gradient.draw {"x0": 0, "y0": 0, "x1": 0, "y1": <height>, "foreground": "#202030", "background": "#000000", "style": "foreground-to-background"}
render {"maxSize": 800}
document.export {"path": "result.png"}
```

Non-destructive colour grade (keeps the original pixels):

```
layers.add {"kind": "adjustment", "adjustmentKind": "Levels", "settings": {...}}     # adjustments.defaults shows the shape
layers.add {"kind": "adjustment", "adjustmentKind": "Hue/Saturation", "settings": {"saturation": -10}}
adjustments.set {"id": "<levels id>", "settings": {"gamma": 1.1}}                    # tweak after a render
```

Clean up a spot:

```
render {"region": {"x": 400, "y": 300, "width": 200, "height": 200}, "maxSize": 0}   # look closely
brush.stroke {"tool": "healing", "size": 40, "points": [[480, 380], [500, 390]]}
```

Select one object by clicking it (the click model must be downloaded; `app.info` says `clickSelect`):

```
selection.subject {"foreground": [[520, 300]]}                       # one point on the object usually takes all of it
selection.subject {"foreground": [[520, 300]], "background": [[800, 500]]}   # a point on a separate thing it grabbed too
selection.subject {"box": [260, 190, 470, 400]}                      # a part of an object: box it (a negative point does not split one object)
layers.mask {"action": "addFromSelection"}                          # or pixels.clear, or a copy to a new layer
```

Replace a flat background colour on one layer:

```
layers.select {"id": "<layer>"}
selection.wand {"x": 5, "y": 5, "tolerance": 24}           # reads the active layer only
selection.grow {"amount": 2}
pixels.clear {}
selection.none {}
```

Drawings and line art (learned on an anime character sheet, September 2026):

```
render {"region": {"x": 0, "y": 0, "width": 60, "height": 60}, "maxSize": 0}   # is the "transparency" a baked-in checkerboard?
selection.wand {"x": 3, "y": 3, "tolerance": 80}                                # from a corner; black outlines stop it
selection.wand {"x": <each other background pocket>, "tolerance": 80, "mode": "add"}
selection.grow {"amount": 1}
pixels.clear {}
selection.polygon {"points": [...]}                                            # to keep one figure: trace the gap
render {"region": {...}, "zoom": 4}                                             # check the gap at the seam first
```

- Remove Background and click select are trained on photos. On flat line art they bleed across
  neighbouring figures and into a baked-in checkerboard; the magic wand is exact there.
- Where figures touch, trace a polygon through the gap between their outlines, checking the tight spots
  with `render` and `zoom`, then invert and clear.
- To make a pasted photo object sit in a drawing, give it the drawing's outline:
  `selection.fromLayer {"id": <object>, "mask": true}`, `selection.grow {"amount": 4}`, a new layer below it,
  `pixels.fill {"color": "#111111"}`.
- `layers.render` shows a masked layer as it looks; `masked: false` for the raw pixels.

Batch export a folder of projects:

```
for f in *.comp; do
  printf '%s\n' "{\"method\":\"document.open\",\"params\":{\"path\":\"$f\"}}" \
                "{\"method\":\"document.export\",\"params\":{\"path\":\"${f%.comp}.png\"}}" \
  | nekophoto --headless --batch -
done
```

## Things to know

- An open project follows its package on disk: an agent that writes the `.comp` folder (its `manifest.json` and
  `images/`, see project-format.md) sees the open tab reload in place about half a second later, as long as the
  person has no unsaved changes there (they are asked otherwise). The socket is still the richer way in; writing
  files suits agents that cannot reach it.

- `layers.set` opacity is 0..1; blend names are the ones `layers.list` reports, in any case or
  spacing ("color-dodge"). Folders have no blend mode.
- A misspelt parameter is refused with the list of the ones the method takes.
- Filters and pixel adjustments apply to the active layer inside the selection;
  select the layer first (`layers.select`) and clear the selection if you mean
  the whole layer.
- `brush.stroke` and friends restore the person's tool and colours afterwards,
  so scripting never leaves the UI in a strange state.
- Subscribe to events (`events.subscribe`) when the person is editing at the
  same time and you want to react; otherwise poll `document.info`.
- Headless runs never show dialogs: an operation that would have asked a
  question fails with that question as the error message.
