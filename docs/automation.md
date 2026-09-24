# Automation and MCP

compositor-linux can be driven by a program: an MCP bridge for agents such as
Claude Code, or any script that can talk JSON over a local socket. Everything
an agent does goes through the same editor session the person sees, lands in
the undo history, and shows on screen (or nowhere, in headless mode).

## Turning it on

- `compositor-linux --rpc` listens for one run.
- Edit > Preferences > Automation turns it on at every launch.
- `compositor-linux --headless` runs with no visible window (Qt's offscreen
  platform) and the socket on; for batch work and CI.

The socket is `$XDG_RUNTIME_DIR/compositor-linux.sock`, or the path given with
`--rpc-socket` or `$COMPOSITOR_RPC_SOCKET`. Only the same user can connect. The
status bar shows "Agent connected" while a client is attached.

## Using it from Claude Code

The bridge is a single Python file with inline dependencies; `uv` fetches the
MCP SDK on first run.

```bash
claude mcp add compositor -- uv run /path/to/compositor-linux/mcp/compositor_mcp.py
```

Inside this repository nothing needs adding: `.mcp.json` declares the server,
so Claude Code offers it when a session starts here.

Without `uv`: `pip install "mcp<2"` and run `python3 mcp/compositor_mcp.py` instead.
The bridge uses the 1.x MCP SDK API (2.x renamed its server class).

When nothing is listening the bridge launches `compositor-linux` (from `PATH`,
`$COMPOSITOR_BIN`, or `./build/src/app/`) with the socket on; with no display it
launches headless. When the editor is already open, that launch hands the
request to it instead and the running window starts listening on the socket,
so the agent works in the document the person is looking at. `COMPOSITOR_MCP_LAUNCH=0` disables launching and
`COMPOSITOR_MCP_LAUNCH=headless` forces the windowless kind.

The bridge exposes one MCP tool per common operation (`layers_list`, `render`,
`layers_set`, `pixels_filter`, `selection_rect`, ...) and a generic `rpc` tool
for the rest. Renders and screenshots come back as images, so the agent can
look at what it did.

A workable prompt for an agent: "Open photo.jpg, remove the background, put a
dark gradient layer behind it, and export result.png." It will call
`document_open`, `remove_background` (if the model is enabled in Preferences),
`layers_add`, `layers_move`, `render` to check, and `document_export`.

## From a shell

`compositor-linux --call <method> [--params '<json object>']` sends one request
to the running instance and prints the result (exit 1 on an error reply, 2 when
nothing is listening; `--rpc-socket` picks the socket). Useful from scripts and
from an agent's shell tool without any MCP setup:

```bash
compositor-linux --call layers.list
compositor-linux --call layers.set --params '{"id": "…", "opacity": 0.5}'
compositor-linux --call render --params '{"path": "/tmp/check.png", "maxSize": 800}'
```

`compositor-linux --headless --batch script.jsonl` (or `-` for stdin) runs a
file of requests, one JSON object per line (`#` comments allowed, ids
optional), in a fresh windowless instance with no socket, prints one response
per line and quits; the first error stops it unless
`COMPOSITOR_BATCH_CONTINUE=1`.

## The protocol

Newline-delimited JSON-RPC 2.0 over a Unix socket. One request object per line,
one response per line:

```
{"jsonrpc":"2.0","id":1,"method":"layers.list","params":{}}
{"jsonrpc":"2.0","id":1,"result":[{"id":"...","name":"Background",...}]}
```

Errors use the standard shape (`-32601` unknown method, `-32602` bad
parameters, `-32000` the editor refused: the message is what a dialog would
have said). `rpc.methods` lists every method. Coordinates are document pixels
with the origin top-left; layers are addressed by the UUIDs `layers.list`
reports; colours are CSS strings. Every method works on the current tab.

Python, without any library:

```python
import json, socket
s = socket.socket(socket.AF_UNIX); s.connect("/run/user/1000/compositor-linux.sock")
f = s.makefile("rw")
f.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "document.info", "params": {}}) + "\n"); f.flush()
print(json.loads(f.readline())["result"])
```

## Methods

Observe: `app.info`, `tabs.list`, `document.info`, `layers.list`, `layers.get`,
`adjustments.get`, `adjustments.defaults`, `selection.info`, `history.info`,
`render` (composite, or a `region`, longest side `maxSize`; `path` writes a file
instead of returning base64), `layers.render` (one layer's pixels),
`screenshot` (the canvas as shown, or the `window`).

Documents: `tabs.select`, `tabs.new`, `tabs.close`, `document.new`,
`document.open` (.comp, a Photoshop .psd/.psb or a Clip Studio .clip, which answer with `layers`
and the import `notes`, or an image), `document.import` (an image as a layer),
`document.save` (answers `macCompatible`: false past the 100 megapixels of layers Compositor for
macOS opens; projects here hold up to a gigapixel), `document.export` (.png, .jpg, .webp or .tif; `quality` for JPEG and WebP, where 100 is lossless; `background` behind a JPEG), `document.close`.

Layers: `layers.select`, `layers.set` (name, visible, opacity, blend, sampling,
clipping), `layers.add` (pixels, group, adjustment, or text with `text`, `x`, `y`, `font`, `size`, `bold`,
`italic`, `color`, `align`; `below: true` puts it under the active layer), `text.set` (a text layer's
content and style, same keys plus `lineSpacing` and `letterSpacing`), `layers.delete`,
`layers.duplicate`, `layers.move`, `layers.reorder`, `layers.setTransform`,
`layers.flip`, `layers.mask` (add, addFromSelection, delete, toggle, invert,
apply, link), `layers.merge`, `layers.group`, `adjustments.set`.
Hue/Saturation settings accept `"saturationCurve": "photoshop"` (+100 saturates fully,
-100 greys out, lightness kept) beside the default `"scale"`.

Pixels of the active layer, inside the selection: `pixels.adjust`,
`pixels.filter` (`kind` and its settings; Lens Correction takes `bicubic: true` for a sharper resample), `pixels.invert`, `pixels.fill`, `pixels.clear`,
`pixels.contentAwareFill`, `pixels.removeBackground` (`refine`, and then `refineEdges`,
`contrast`, `shiftEdge`, `matting`: the band width in pixels in which hair opacity is solved,
`cleanup`: half-transparent specks touching no edge go, `decontaminate`: the edge pixels take the
subject's own colour; both default true; `detail`: the model runs again on full-resolution windows
along the edge of a large photo, `detailWindows` at most, default 12; `flip`: average the mask with the
mirrored image's, defaulting to the preference), `pixels.gmic` (`command`, a G'MIC
command line made of catalogue filters or common built-ins, each followed only by numbers: G'MIC can
run shell commands and read or write files, so strings, paths, substitutions and other commands are
refused unless `COMPOSITOR_GMIC_UNRESTRICTED=1` is set; the G'MIC dialog is not restricted;
`gmic.filters` lists the catalogue with parameters and defaults, leaving out the filters that do not
work here unless `all: true`, when they carry an `unsupported` reason).

Selection: `selection.all`, `selection.none`, `selection.invert`,
`selection.rect` (or `ellipse: true`), `selection.polygon`, `selection.wand`,
`selection.scribble` (`foreground` and `background`: lists of strokes, each a list of `[x, y]`
points; `size`, `refine` 0..40, `clear`), `selection.subject` (click to select with the EfficientSAM
model once downloaded: `foreground` and `background` points as `[x, y]` lists, an optional `box`
`[x0, y0, x1, y1]`, `refine`, `clear`; `app.info` reports `clickSelect` when it can run) (all take
`mode` replace, add, subtract or intersect),
`selection.fromLayer`, `selection.grow`, `selection.feather` (`radius`), `selection.smooth`
(`radius`), `selection.border` (`width`).

Canvas and history: `canvas.resize`, `canvas.crop`, `canvas.flip`,
`image.resize`, `history.undo`, `history.redo`.

Painting by coordinates: `brush.stroke` (`points` as `[x, y]` pairs; `tool` brush,
eraser, healing, clone with `source`, smudge, blur or liquify; `size`,
`hardness`, `opacity` 0..1, `color`, `mask: true` paints the active layer's
mask; with brush or eraser, `preset` paints with a MyPaint brush from
`brush.presets`, or `"round"` for the plain tip, or an imported tip brush, at its own size unless `size`
is given, and `pressure` 0..1 or `pressures`, one per point, drive it the way
a pen would), `brush.presets` (the MyPaint presets: `id`, `name`, `group`,
`size`, `eraser`; `group` filters), `brush.import` (`path` or `paths`: Photoshop
`.abr`, Procreate `.brushset`/`.brush`, Clip Studio `.sut` or images as tips;
answers the new preset ids and notes on what was approximated), `gradient.draw` (`x0, y0, x1, y1`, `shape` linear or radial, `style`
foreground-to-transparent or foreground-to-background, `reversed`, `opacity`,
`foreground`, `background`), `shape.draw` (a new shape layer: `kind` rectangle
or ellipse, `x, y, width, height`, `cornerRadius`, `color`). The person's tool,
brush settings and colours are restored afterwards.

View: `tool.select` (`name`: move, marquee, lasso, wand, quickselect, crop, brush, healing, clone,
smudge, gradient, shape, text, eyedropper, hand or zoom), `colors.set`, `view.zoom`.

Events: `events.subscribe` (`kinds`: document, layers, selection, history,
tool, view, tabs; default all) makes the server push
`{"jsonrpc":"2.0","method":"event","params":{"kind":"layers","tab":0}}` lines on
that connection, one per kind per event-loop turn, in between replies;
`events.unsubscribe` stops them. Clients must skip event lines while waiting
for a reply.

`layers.list {"thumbnails": true}` adds each pixel layer's 96 px thumbnail (and
its mask's) as base64 PNG; `selection.render` returns the selection as a mask
image; `history.list` names every recorded edit.

`tools/rpc_smoke.py` exercises a representative set and is what CI runs against
a headless instance. `docs/agent-guide.md` has recipes and habits that work
well for agents.

## Not there yet

Text layers (the editor has none) and a remote transport (the socket is local
only, by design) are the open items. Adding a method is one `add("name", handler)` in
`src/app/Automation.cpp`; the bridge's generic `rpc` tool reaches it without a
Python change.
