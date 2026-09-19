# Working with compositor-linux as an agent

How to get good results driving the editor through MCP, `--call`, `--batch`
or the socket. `docs/automation.md` has the method reference; this is the
practice.

## The loop

1. Look before touching: `document.info`, then `layers.list` (add
   `thumbnails: true` for a visual index) and, when it matters, `render` at a
   modest `maxSize` (512 is plenty for orientation).
2. Act in small steps. Every method is one undo step; if a render shows the
   wrong thing, `history.undo` and try again. `history.list` shows what you did.
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

Batch export a folder of projects:

```
for f in *.comp; do
  printf '%s\n' "{\"method\":\"document.open\",\"params\":{\"path\":\"$f\"}}" \
                "{\"method\":\"document.export\",\"params\":{\"path\":\"${f%.comp}.png\"}}" \
  | compositor-linux --headless --batch -
done
```

## Things to know

- `layers.set` opacity is 0..1; blend names are the ones `layers.list` reports.
- Filters and pixel adjustments apply to the active layer inside the selection;
  select the layer first (`layers.select`) and clear the selection if you mean
  the whole layer.
- `brush.stroke` and friends restore the person's tool and colours afterwards,
  so scripting never leaves the UI in a strange state.
- Subscribe to events (`events.subscribe`) when the person is editing at the
  same time and you want to react; otherwise poll `document.info`.
- Headless runs never show dialogs: an operation that would have asked a
  question fails with that question as the error message.
