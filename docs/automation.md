# Automation and MCP

NekoPhoto can be driven by a program: an MCP bridge for agents such as
Claude Code, or any script that can talk JSON over a local socket. Everything
an agent does goes through the same editor session the person sees, lands in
the undo history, and shows on screen (or nowhere, in headless mode).

## Turning it on

- `nekophoto --rpc` listens for one run.
- Edit > Preferences > Automation turns it on at every launch.
- `nekophoto --headless` runs with no visible window (Qt's offscreen
  platform) and the socket on; for batch work and CI.

The socket is `$XDG_RUNTIME_DIR/nekophoto.sock`, or the path given with
`--rpc-socket` or `$COMPOSITOR_RPC_SOCKET`. Only the same user can connect. The
status bar shows "Agent connected" while a client is attached.

## Using it from Claude Code

The bridge is a single Python file with inline dependencies; `uv` fetches the
MCP SDK on first run.

```bash
claude mcp add nekophoto -- uv run /path/to/nekophoto/mcp/nekophoto_mcp.py
```

Without `uv`: `pip install "mcp<2"` and run `python3 mcp/nekophoto_mcp.py` instead.
The bridge uses the 1.x MCP SDK API (2.x renamed its server class).

When nothing is listening the bridge launches `nekophoto` (from `PATH`,
`$COMPOSITOR_BIN`, or `./build/src/app/`) with the socket on; with no display it
launches headless. When the editor is already open, that launch hands the
request to it instead and the running window starts listening on the socket,
so the agent works in the document the person is looking at. `COMPOSITOR_MCP_LAUNCH=0` disables launching and
`COMPOSITOR_MCP_LAUNCH=headless` forces the windowless kind.

The bridge exposes one MCP tool per common operation (`document_overview`, `render`,
`layers_set`, `pixels_filter`, `selection_rect`, ...), `describe_method` and a generic `rpc` tool
for the rest. Renders and screenshots come back as images, so the agent can
look at what it did. Every tool has a title and says whether it only reads, edits (one undo step) or
reaches past undo (saving, exporting, closing, `rpc`), so clients can skip confirmations on the safe
ones. The `edit_photo` prompt carries the working loop and recipes. `tools/mcp_smoke.py` drives the
bridge with the MCP client in CI.

A workable prompt for an agent: "Open photo.jpg, remove the background, put a
dark gradient layer behind it, and export result.png." It will call
`document_open`, `remove_background` (if the model is enabled in Preferences),
`layers_add`, `layers_move`, `render` to check, and `document_export`.

## From a shell

`nekophoto --call <method> [--params '<json object>']` sends one request
to the running instance and prints the result (exit 1 on an error reply, 2 when
nothing is listening; `--rpc-socket` picks the socket). Useful from scripts and
from an agent's shell tool without any MCP setup:

```bash
nekophoto --call layers.list
nekophoto --call layers.set --params '{"id": "…", "opacity": 0.5}'
nekophoto --call render --params '{"path": "/tmp/check.png", "maxSize": 800}'
```

`nekophoto --headless --batch script.jsonl` (or `-` for stdin) runs a
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
have said, with the next step where there is one). `rpc.methods` lists every
method; `rpc.describe {"method": "layers.set"}` gives one method's parameters
with types, defaults and valid values (blend modes, tools, filter kinds), and
without a method a one-line summary of each. A request with a key the method
does not take, or without a required one, is refused with the keys it does
take, rather than the key being ignored. `app.info` reports `protocolVersion`
(2 since these checks). Coordinates are document pixels
with the origin top-left; layers are addressed by the UUIDs `layers.list`
reports; colours are CSS strings. Every method works on the current tab.

Python, without any library:

```python
import json, socket
s = socket.socket(socket.AF_UNIX); s.connect("/run/user/1000/nekophoto.sock")
f = s.makefile("rw")
f.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "document.info", "params": {}}) + "\n"); f.flush()
print(json.loads(f.readline())["result"])
```

## Methods

Observe: `app.info`, `rpc.describe`, `tabs.list`, `document.overview` (the document, selection, undo and
layer tree as text, one line per layer with its id: where an agent starts), `document.info`, `layers.list`, `layers.get` (a smart object is kind `smartObject`, with its source and whether it is preview-locked),
`adjustments.get`, `adjustments.defaults`, `selection.info`, `history.info`,
`render` (composite, or a `region`, longest side `maxSize`; `zoom` 2..32 enlarges a region with square
pixels to judge an edge exactly; `path` writes a file instead of returning base64), `layers.render` (one
layer alone; a masked layer as it shows, `masked: false` for its raw pixels),
`screenshot` (the canvas as shown, or the `window`).

Documents: `tabs.select`, `tabs.new`, `tabs.close`, `document.new`,
`document.open` (.comp, a Photoshop .psd/.psb, a Clip Studio .clip, an Affinity .afphoto, .afdesign, .afpub or .af, an Aseprite .ase/.aseprite (a layer per cel, its frames on the timeline), an
icon .ico/.cur (a layer per size, the largest visible), an SVG (.svg/.svgz: shapes as vector
shape layers), a PDF page (`page`, 1-based, and `resolution` in pixels per inch, default 150; when `app.info` reports
`pdf`) or an animated GIF (a layer per frame, "Frame N (D ms)", frame 1
at the bottom and the only one visible, with the frames, delays and loop count on the timeline), which open in a tab of their own and answer with `layers` and the import
`notes`; or an image, .tga included), `document.import` (an image as a layer),
`document.save` (answers `macCompatible`: false past the 100 megapixels of layers Compositor for
macOS opens; projects here hold up to a gigapixel), `document.export` (.psd, layered, text layers as Photoshop text (`texts` counts them), answering with the counts and any `warnings` and `notes` about what Photoshop cannot carry; or .svg, answering with the `shapes`, `images` and `groups` written and `notes` on what became images (docs/svg-pdf.md); an animated .gif of the timeline's frames (the composite when there are none; `frames` counts them); or the composite as .png, .jpg, .webp or .tif; `quality` for JPEG and WebP, where 100 is lossless; `background` behind a JPEG; PNG, JPEG, WebP, TIFF and PSD carry the
document's colour profile, `embedProfile: false` leaves it out, and `convertToSrgb` converts to sRGB first, the default
for GIF), `document.close`. `document.info` reports the document's `profile`.

Artboards and slices (docs/artboards-slices.md): `artboards.list`, `artboards.add` (`x`, `y`, `width`, `height`, `name`,
`background`: white, black, transparent or a CSS colour), `artboards.set` (the same by `id`; a move takes its layers along
unless `moveContents` is false), `artboards.delete` (a plain folder again, or with `contents` everything in it), `artboards.export`
(`directory`, `format` png or jpeg, `prefix`, `quality`; answers the `files` written and their `bits`: a 16-bit document
writes 16-bit PNGs, and JPEGs dithered down to 8 bits with a `note`); `slices.list`, `slices.add`, `slices.set`
(by numeric `id`; `name`, `url`, `target`, `altTag`), `slices.delete` and `slices.export` (as `artboards.export`). `layers.get`
shows a folder's `artboard`; `tool.select` takes `artboard` and `slice`.

Smart objects: `smartObject.convert` (the selection or `ids`), `smartObject.place` (`path`), `smartObject.replace`
(`path`), `smartObject.rasterize`, `smartObject.editContents` (opens a tab) and `smartObject.commit` (in that tab).
`smartObject.addFilter` adds a Smart Filter (any of the thirteen drawn here, with its settings, opacity and blend) on
top of a smart object's stack. `smartObject.filters` lists the stack (entries by `index` in running order, 0 applied
first; each with its `kind`, `settings`, `enabled`, `opacity`, `blend`, and `drawn` false for one NekoPhoto does not draw,
which makes the stack read-only), `smartObject.setFilter` changes an entry's settings, switch, opacity or blend (no
`index`: `enabled` switches the whole stack), `smartObject.moveFilter` (`index`, `to`), `smartObject.removeFilter`
(`index`, or `all` to clear them), and `smartObject.filterMask` (`action`: enable, disable, invert, delete, or select
to paint it with `brush.stroke` `mask` and the other mask tools, `show` to see it; deselect, or selecting a layer, ends that). `layers.cage` and `layers.setCage` read and apply a free warp cage (16 control points; a smart object keeps it as its
own Custom warp). `layers.warp` bends a layer with one of Photoshop's fifteen presets (`style`, `bend`, `horizontal`, `vertical`,
`orientation`): Warp Text on text (`none` removes it), a mesh baked into a smart object's placement, bent pixels
otherwise.
Layers: `layers.select`, `layers.set` (name, visible, opacity, blend (a folder also takes "Pass Through"), sampling,
clipping), `layers.add` (pixels, group, adjustment, or text with `text`, `x`, `y`, `font`, `size`, `bold`,
`italic`, `color`, `align`; `below: true` puts it under the active layer), `text.set` (a text layer's
content and style, same keys plus `lineSpacing` and `letterSpacing`; on text in several styles, which
`layers.get` lists as `text.runs`, the change carries into every run and a new size scales each), `text.styleRange` (some letters' style, as
Photoshop's Character panel on a selection: `start` and `length` in UTF-16 units of the text, default all of it, and
any of `font`, `size`, `bold`, `weight`, `italic`, `color`, `letterSpacing`, `baselineShift`, `leading`, `caps`
(normal, small, all), `underline`, `strikethrough`; the runs split and merge as needed), `layers.delete`,
`layers.duplicate`, `layers.move`, `layers.reorder`, `layers.setTransform`,
`layers.flip`, `layers.mask` (add, addFromSelection, delete, toggle, invert,
apply, link), `layers.merge`, `layers.group`, `adjustments.set`.
`layers.style` gives a layer's effects (Photoshop's layer style) as JSON, every kind a list with switched-off
effects kept (`enabled` false); `layers.setStyle` replaces them with an object of that shape (settings left out take
Photoshop's defaults, `{}` clears the style). The style is written into the PSD as Photoshop's own `lfx2`.
`layers.applyStyle` (`id`, `style`: an imported style preset's name) gives a layer a style preset, with the patterns it
uses added to the document. `presets.import` (`path` or `paths`: Photoshop `.asl` styles, `.pat` patterns, `.grd`
gradients) fills the preset library and answers the names imported, `patternsAddedToDocument` (a `.pat`'s patterns join
the open document) and notes; `presets.list` (`kind` styles, gradients or patterns) lists it, gradients with their
stops; `presets.remove` (`kind` style, gradient or pattern, `name`) takes one out ([presets.md](presets.md)).
Hue/Saturation settings accept `"saturationCurve": "photoshop"` (+100 saturates fully,
-100 greys out, lightness kept) beside the default `"scale"`.

Pixels of the active layer, inside the selection: `pixels.adjust`,
`pixels.filter` (`kind` and its settings; Lens Correction takes `bicubic: true` for a sharper resample),
`pixels.mosh` (Filter > Mosh: `effect`, an OpenMosh id such as `pixel-sort` or `vhs`, `params` by OpenMosh's keys, `seed`
0..100 for the seeded effects, `layer` (the id of the layer `overlay` and `mask` read) and `text` (what `caption` stamps);
replies with the settings applied; [mosh.md](mosh.md) lists every effect and parameter),
`pixels.cameraRaw` (Filter > Camera Raw Filter: `settings` with the model's keys, nested `curve`, `mixer`, `grading`,
`detail`, `optics`, `geometry` and `calibration` objects, unknown keys refused, `whiteBalance: "Auto"` balances the layer;
`rpc.describe` lists every key and range and [camera-raw.md](camera-raw.md) what each does), `pixels.invert`, `pixels.fill`, `pixels.clear`,
`pixels.contentAwareFill` (`sampling`: `auto` around the selection, `all` the whole layer, or `custom`: the
`include` rectangles, the whole canvas when none, less the `exclude` rectangles; `output`: `current` or `new`, only
the filled pixels on a new layer, whose id comes back as `layer`), `pixels.contentAwareMove` (Content-Aware Move:
the selection's pixels move `dx`, `dy`, the hole is filled and the patch blended in; `mode` `move` or `extend`,
`adaptation` 0 very strict to 4 very loose; the selection follows), `pixels.contentAwareScale` (Edit > Content-Aware Scale by seam carving: `width`/`height` in pixels or `widthPercent`/`heightPercent`, `protectSelection` keeps the selected pixels; see [content-aware-scale.md](content-aware-scale.md)), `pixels.removeBackground` (`refine`, and then `refineEdges`,
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
`selection.rect` (or `ellipse: true`), `selection.polygon`, `selection.wand` (`edgeAware`, on by default: see docs/smart-wand.md),
`selection.scribble` (`foreground` and `background`: lists of strokes, each a list of `[x, y]`
points; `size`, `refine` 0..40, `clear`), `selection.subject` (click to select with the EfficientSAM
model once downloaded: `foreground` and `background` points as `[x, y]` lists, an optional `box`
`[x0, y0, x1, y1]`, `refine`, `clear`; `app.info` reports `clickSelect` when it can run) (all take
`mode` replace, add, subtract or intersect),
`selection.fromLayer`, `selection.grow`, `selection.feather` (`radius`), `selection.smooth`
(`radius`), `selection.border` (`width`).

Channels ([channels.md](channels.md)): `channels.list` (the colour channels edits write to and the canvas shows, the
target alpha channel, every alpha and spot channel), `channels.new` (`name`, `fromSelection`), `channels.duplicate`,
`channels.delete`, `channels.select` (`channel` rgb, red, green, blue or an alpha channel's id; `extend` as a Shift-click:
with one colour channel the target, brushes, fills, adjustments, filters and paste change only it; an alpha channel is
painted with `brush.stroke` and `mask: true`), `channels.set` (Channel Options: `name`, `color`, `opacity`,
`colorIndicates` masked or selected, `index`, `visible`), `channels.saveSelection` (Select > Save Selection: a new
channel, or `id` with `mode`), `channels.loadSelection` (Select > Load Selection: `channel`, or `layer` with `mask`;
`invert`, `mode`, or a thumbnail Ctrl-click's `shift` and `alt`).

Canvas and history: `canvas.resize`, `canvas.crop`, `canvas.flip`,
`image.resize`, `image.trim` (Photoshop's Trim: `basedOn` transparent, topLeft or bottomRight, the sides, `tolerance`),
`image.mode` (Image > Mode: `bits` 8, 16 or 32, one undo step, refused with the reason when the document would not fit
its byte budget, half the pixels of an 8-bit one at 16 bits, a quarter at 32; from 32 bits HDR Toning's `method`
exposure-gamma or highlight-compression, `exposure`, `gamma`, whose defaults give an 8- or 16-bit-sourced document back
exactly; `colorMode` rgb, cmyk or lab, RGB Color, CMYK Color or Lab Color, one undo step, with Color Settings' Working
CMYK, `intent` and `blackPointCompensation`; with both, the colour mode first, except when leaving 32 bits; 32 bits is
RGB only; see docs/color-modes.md), `document.profile` (colour management, docs/color-management.md:
`action` get, `assign` (Edit > Assign Profile, the tag only) or `convert` (Edit > Convert to Profile: every layer's pixels
and the stored colours); `profile` srgb, adobe-rgb, display-p3, prophoto, working, working-cmyk (CMYK documents), none or
an ICC file's path of the document's mode; `intent`
perceptual or relative, `blackPointCompensation`; one undo step), `history.undo`, `history.redo`, `history.beginGroup` (`name`) and
`history.endGroup`: the steps one connection records in between become one undo step with that name.
Each call still records its own step while the group is open, so the person's Undo keeps working; the
merge happens at the end, and only when no one else recorded a step in between (the reply says why
not). A connection that closes with a group open has it closed.

16-bit documents (docs/bit-depth.md): `document.info` reports `bits` (8 or 16). Every method works on a 16-bit
document, as on an 8-bit one. `document.export` writes a 16-bit PNG (and TIFF, when the Qt TIFF plugin writes
16 bits) from a 16-bit document; the 8-bit formats (an animated GIF's frames too) get it dithered down, and the reply
says so in `note`; SVG embeds its images as 16-bit PNGs (`bits` 16).

Batches: `rpc.batch` (`calls`: a list of `{"method", "params"}`; `name`) runs the calls in order in one
request and stops at the first error, answering the results so far and the error's index. With a
`name` the calls are one undo step and all or nothing: an error takes back what the earlier calls did
(`rolledBack`). Nothing else runs between the calls of a batch.

Actions ([actions.md](actions.md)): `actions.list` (each action's steps: `method`, `params`, `enabled`, a `label`;
whether one is recording), `actions.record` (`action` start with a `name`, or stop: while it records, every editing
request on any connection and the person's recordable menu commands, dialogs and strokes become steps; looks such as
`layers.list` or `render` are not recorded), `actions.play` (`name`, `times`; stops at the first failing step and
answers its `index`, `method` and `message`; steps that stay in one document merge into one undo step named after the
action), `actions.batch` (`name`, `input` and `output` folders, `format` png, jpg, webp, tif, psd, gif or tga,
`overwrite`: File > Automate > Batch), `actions.save` (`name`, `steps`: create or replace an action, which is how an
agent edits, reorders or switches off steps), `actions.delete`, `actions.import` and `actions.export` (JSON files).

Frame animation ([animation.md](animation.md)): `timeline.info` (frames with their `delay` in milliseconds and
`visibleLayers`, `current`, `loopCount`, 0 for forever), `timeline.frame` (`action` create, fromLayers, duplicate,
select, delete, move with `index` and `to`, or clear; one undo step each except select), `timeline.set` (`delay` for the frame at
`index`, default the current one, -1 for all; `loopCount`). While a frame is selected, `layers.set` visibility,
opacity and moves go into that frame.

Vector shapes and paths: `shape.draw` makes a vector shape layer (rectangle, ellipse, polygon, star, line or a
custom shape, with fill and stroke), `shape.get` and `shape.set` read and change its path, fill and stroke;
`paths.list`, `paths.set`, `paths.select`, `paths.delete`, `paths.fill`, `paths.stroke`, `paths.toSelection`,
`paths.toShape`, `paths.fromSelection`, `paths.addAnchor` and `paths.deleteAnchor` work the Paths panel (docs/vector-tools.md).
`shape.draw` and `shape.set` also take gradient and pattern paints (`fillType`, `gradient`, `gradientType`, `gradientAngle`,
`pattern`; `strokeType`, `strokeGradient`, `strokePattern`), `op` (combine, subtract, intersect, exclude: a component of
the active shape layer instead of a new layer) and, on `shape.set`, `live` (a live rectangle's or ellipse's box and corner
radii, which `shape.get` lists); `paths.setOperation` and `paths.mergeComponents` work the target path's components;
`vectorMask.get`, `vectorMask.set` (Reveal All, Hide All, the chosen path or a path), `vectorMask.delete` and
`vectorMask.target` (the Pen's and Direct Selection's target) handle a layer's own vector mask; `text.toPath` and
`text.toShape` are Type > Create Work Path and Convert to Shape. Quick Mask: `selection.quickMask` (`on` true enters, false turns the mask back into the selection; while on,
paint the mask with `brush.stroke` and `mask: true`, white selecting). Painting by coordinates: `pixels.patch` (the selection repaired from `dx`, `dy` away), `pixels.bucket` (Paint Bucket at `x`, `y` with `color`, `opacity`, `tolerance`,
`contiguous`, `antialias`, `allLayers`), `brush.stroke` (`points` as `[x, y]` pairs; `tool` brush,
eraser, healing, healingbrush or clone with `source`, smudge, blur, sharpen or liquify, or dodge and burn (with `range`
shadows, midtones or highlights and `protectTones`) and sponge (`saturate: true` to saturate), whose `opacity` is
the Exposure or Flow; `size`,
`hardness`, `opacity` 0..1, `color`, `mask: true` paints the active layer's
mask; with brush or eraser, `preset` paints with a MyPaint brush from
`brush.presets`, or `"round"` for the plain tip, or an imported tip brush, at its own size unless `size`
is given, and `pressure` 0..1 or `pressures`, one per point, drive it the way
a pen would, as do `tilts` (`[tiltX, tiltY]` degrees per point), `twists` (degrees per point) and
`times` (seconds per point, 8 ms apart by default); `seed` repeats a tip brush's jitter, and `viewScale` (default 1)
is the zoom the stroke is taken as drawn at, 2 for 200%, which speed-on-screen dynamics read, so a recorded stroke
replays exactly; with brush or eraser, `smoothing` 0..100 is the stabiliser (the options bar's Smoothing) with
`pulledString`, `strokeCatchUp` (default true), `catchUpOnEnd` and `adjustForZoom` (default true), and `inputSmoothing`
and `pressureSmoothing` 0..100 the other two filters; none applies unless given, and the answer says `smoothed`), `brush.presets` (the MyPaint presets: `id`, `name`, `group`,
`size`, `eraser`; `group` filters), `brush.import` (`path` or `paths`: Photoshop
`.abr`, Procreate `.brushset`/`.brush`, Clip Studio `.sut` or images as tips;
answers the new preset ids and notes on what was approximated), `gradient.draw` (`x0, y0, x1, y1`, `shape` linear or radial, `style`
foreground-to-transparent or foreground-to-background, `reversed`, `opacity`,
`foreground`, `background`, `preset`: an imported gradient's name, used instead of `style`), `shape.draw` (a new shape layer: `kind` rectangle
or ellipse, `x, y, width, height`, `cornerRadius`, `color`). The person's tool,
brush settings and colours are restored afterwards.

View: `tool.select` (`name`: move, marquee, lasso, wand, quickselect, crop, brush, healing, clone,
smudge, gradient, shape, text, eyedropper, hand or zoom), `colors.set`, `view.zoom`, `view.exposure` (a 32-bit
document's view, not the pixels and not an undo step: `exposure` in stops, `gamma`, `method` exposure-gamma or
highlight-compression; with no keys it reads it), `color.settings` (Edit > Color
Settings: `workingSpace`, `workingCmyk` (default, the bundled ISO Coated v2 300%, or a CMYK ICC file), the Conversion Options
`intent` (perceptual, relative, saturation, absolute) and `blackPointCompensation` Image > Mode uses, `policy` preserve,
convert or off, `askMissing`, `askMismatch`; the monitor profile: `monitorProfile` (an ICC file, "" for the system's),
`useSystemMonitor`; View > Proof Setup: `proofProfile` (working-cmyk, the default, a working space or an RGB or CMYK ICC file),
`proofIntent`, `proofBlackPoint`, `proofColors`, `gamutWarning`, `gamutColor`; with no keys it only reads them).

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
the `src/app/Automation*.cpp` file for its area and its entry in `AutomationDescriptions.cpp` (the smoke
test fails without one); the bridge's generic `rpc` tool reaches it without a Python change.
