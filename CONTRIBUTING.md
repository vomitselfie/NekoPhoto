# Contributing to NekoPhoto

Thanks for helping. This page is what you need to build NekoPhoto, find your way around the code and get a pull
request through CI. Security problems go through [SECURITY.md](SECURITY.md), not the issue tracker.

## Build and test

Linux (the packages for Arch and Ubuntu are in the [README](README.md#build-from-source)):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # once
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/src/app/nekophoto
```

Configure with `-DCOMPOSITOR_WARNINGS_AS_ERRORS=ON` to build as CI does. The other options (building without
OpenCV, MyPaint, SQLite or Qt PDF) are in [docs/linux-port.md](docs/linux-port.md#building).

CI's toolchain is older than a rolling desktop's (Ubuntu 24.04: GCC 13, Clang 18, Qt 6.4; releases build on
22.04 with GCC 11). To catch what only the older compilers reject, run CI's build in Docker:

```bash
tools/ci-in-docker.sh ubuntu:24.04 gcc
tools/ci-in-docker.sh ubuntu:24.04 clang
```

OpenCV is vendored: CI builds `tools/build-opencv.sh` (a pinned 4.x, static, three modules) and configures with
`-DOpenCV_DIR=<prefix>/lib/cmake/opencv4`. A local build takes the system OpenCV unless you pass that too.

**Windows** builds with MinGW-w64 in MSYS2's UCRT64 shell (MSVC is not supported); the packages and steps are in
[docs/linux-port.md](docs/linux-port.md#windows). From Linux, `tools/windows-cross.sh` cross-compiles the same
tree in a Fedora container and runs the tests, the offscreen smoke test and an automation call under Wine,
headless. It is for iterating on Windows-only code; platform code lives in `src/app/Platform.{h,cpp}`.

### Running it without a screen

```bash
QT_QPA_PLATFORM=offscreen ./build/src/app/nekophoto --demo --tool brush --dialog levels --screenshot out.png
./build/src/app/nekophoto --headless --rpc-socket /tmp/np.sock --demo &
python3 tools/rpc_smoke.py /tmp/np.sock
```

`COMPOSITOR_WINDOW_SIZE=1000x700` forces the window size and `QT_SCALE_FACTOR=2` a scale, for checking layouts.
`--call <method> --params '{...}' --rpc-socket <path>` sends one automation request from the shell;
`--headless --batch script.jsonl` runs a file of them. On Linux the socket path must stay under 107 bytes.

## Architecture

```
src/pixels/   the original C pixel routines
src/core/     compositor_core: the editor core in portable C++20, no Qt
              (document, layers, render, blend, brushes, selections, filters, file formats: PSD, CLIP, ABR, SUT, ...)
src/app/      the Qt 6 Widgets application: windows, panels, dialogs, canvas, EditorSession, automation
mcp/          nekophoto_mcp.py, the MCP server that bridges AI agents to the automation socket
tools/        packaging, CI helpers, smoke tests, the translation check
tests/        CTest suites, golden PNGs, render hashes, fuzz targets (tests/fuzz)
translations/ Qt Linguist files (Japanese is complete)
docs/         design notes per feature; docs/linux-port-architecture.md maps the code
```

- **The core is Qt-free.** `src/core` builds and tests without Qt (`-DCOMPOSITOR_BUILD_APP=OFF`). Keep Qt types
  out of it; the app converts at the boundary (`src/app/ImageConvert.h`, `QtGeometry.h`).
- **Pixels** are premultiplied, top-down, with an explicit stride, at the document's depth: `AnyImage` / `AnyGray`
  over 8-bit `Image`, 16-bit `ImageT<U16>` (0..32768) or 32-bit float, in RGB, CMYK or Lab. `.u8()` is null on a
  deeper buffer, so gate a feature with `supports()` (`compositor/supports.h`; [docs/bit-depth.md](docs/bit-depth.md)).
- **`EditorSession`** (`src/app/EditorSession*.cpp`) owns the open document and its undo history; the widgets and
  the automation handlers both go through it.
- **`Compositor/`, `Compositor.xcodeproj` and the other Xcode folders** are the original macOS app's Swift
  sources, kept as a reference for behaviour. Don't edit them; the port lives in `src/`.

## Rules

- **One edit, one undo step.** Every user-visible change is wrapped in `beginEdit(name)` / `endEdit()` on
  `EditorSession`, so Ctrl+Z undoes it in one go and automation clients see one history entry.
- **An automation method is five pieces**, all in the same pull request:
  1. a handler, `add("name", handler)`, in the `src/app/Automation*.cpp` file for its area (document, layers,
     pixels, selection, paint; app, tabs, history and view in `Automation.cpp`);
  2. its parameters in `src/app/AutomationDescriptions.cpp` (requests with other keys are refused);
  3. a matching tool in `mcp/nekophoto_mcp.py`, marked `@look`, `@edit` or `@outside`;
  4. a line in `docs/automation.md`;
  5. coverage in `tools/rpc_smoke.py` (and `tools/mcp_smoke.py` when the bridge does something special).
- **Commands have one implementation.** An edit that automation can make is made by its automation method, and a
  menu item, shortcut, dialog OK or canvas gesture that makes the same edit runs that method rather than calling
  `EditorSession` itself. Every menu item is a command in the command registry. See [Commands](#commands) below.
- **Every visible string goes through `tr()`** and gets its Japanese translation in the same change; the
  `translations_check` test fails otherwise. How to update and translate: [docs/translating.md](docs/translating.md).
- **No warnings.** CI builds with `-Werror` on GCC and Clang.
- **Render hashes don't change by accident.** `render_hash_tests` hashes 711 scenes (at every depth and colour mode) against
  `tests/render_hashes.txt` and `golden_tests` compares against `tests/golden/*.png`. When a pull request changes
  rendering on purpose, regenerate and say why in the description:
  ```bash
  COMPOSITOR_UPDATE_RENDER_HASHES=1 build/tests/render_hash_tests
  COMPOSITOR_UPDATE_GOLDEN=1 build/tests/golden_tests
  ```
- **The PSD round trip stays clean.** With a checkout of [Patchy](https://github.com/SethRobinson/Patchy) beside
  this one, `build/tests/psd_roundtrip ../Patchy/test-fixtures/psd` must report `0 failed`; run it for any change
  to `psd*.cpp`, layer styles, vector masks, smart objects or text. See [docs/compatibility.md](docs/compatibility.md).
- **Keyboard shortcuts follow Photoshop's defaults.** People bring their muscle memory; don't invent new
  bindings where Photoshop has one ([docs/linux-port.md](docs/linux-port.md#keyboard-shortcuts)).
- **Hostile files**: parsers must survive any input. A crash found by fuzzing gets a regression test in
  `tests/hostile_input_tests.cpp` ([docs/fuzzing.md](docs/fuzzing.md)).
- **Licences**: new dependencies must be GPL-3.0 compatible and listed in
  [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Test files and sample art must be clearly licensed for
  redistribution.

## Commands

Two tables meet here. The **command registry** (`src/app/CommandRegistry.{h,cpp}`) is every menu command: a stable id,
its label, its menu path, its default keys, what it needs and why it is greyed, and what it runs. The **automation
registry** (`add("name", handler)` in `src/app/Automation*.cpp`, with each method's parameters in
`AutomationDescriptions.cpp`) is the command layer: the socket, `--call`, `--batch`, MCP and Actions playback all run
those handlers. A menu item converted to it runs the method too, so the edit has one implementation, one history
entry and one Actions step, wherever it starts:

```cpp
// MainWindowMenus.cpp: the menu item (and its shortcut) is a command; it sends the request with typed parameters.
add(layer, Spec("layer.new", tr("&New Layer"), {QKeySequence("Ctrl+Shift+N")}).document("layers.structure")
               .request("layers.add", [] { return QJsonObject{}; }, tr("New Layer")));
```

`MainWindow::runCommand(method, params, title)` sends the request through `AutomationServer::handle`, exactly as a
socket request: the parameters are checked against the description, the depth gate applies, an action that is
recording gets the step (once), and an error is shown in a dialog titled `title`. Code that has a session but not the
window (dialogs, the canvas) calls `EditorSession::runCommand(method, params)`, which `MainWindow` routes to the
same place for the tab on screen; `commandsRouted()` says whether it will, and when it will not (a dialog left open on
another tab) the caller makes the edit directly as before.

### The command registry

`MainWindow::buildMenus` (`MainWindowMenus.cpp`) adds the commands to the menus in order; nothing else makes menu
items. A command is one row, written with `Spec`:

- **`Spec(id, label, keys)`**: the id is stable across languages and releases, the area and then the menu's own words
  in camel case (`edit.undo`, `file.saveAs`, `layer.mask.revealAll`, `filter.blur.gaussianBlur`; `commandSlug` makes
  the last part from English words). The label is translated, with its accelerator. The keys are Photoshop's defaults;
  no two actions may share one.
- **`.document(feature)`**: it needs a document, and the `supports()` feature it is (`compositor/supports.h`): the
  depth and colour mode grey it, saying why. Without a feature it is 8-bit RGB only.
- **`.request(method, params, title)`**: it is that automation request; `params` returns the parameters, or nothing when
  there is nothing to do now (no active layer, nothing to merge). An error is shown in a dialog titled `title`.
- **`.runs(function, method)`**: it opens a dialog or asks something first; the function ends in `runCommand`, a
  dialog's `commitAsCommand` or the session's `runCommand`, and `method` names the method for Edit > Search and the docs.
- **`.when(reason)`**: why it cannot run now, or an empty string when it can (`tr("Nothing to undo")`). The registry
  greys the item while there is a reason, and Edit > Search shows it beside the greyed command.

An action made elsewhere (a panel's show/hide, a submenu, a view switch, a tool on the rail and its Shift + letter
switch) joins with `CommandRegistry::adopt`. The keys in a `Spec` are the defaults: Edit > Keyboard Shortcuts
(`KeyboardShortcuts.{h,cpp}`) lets the person change any command's, kept as `shortcuts/<id>` in the settings, so a
command's id is also its name in a person's settings and `.nekokeys` file (renaming one drops their key for it), and
what a key runs now is the action's `shortcuts()`, not the `Spec`'s. A
submenu that lists files or presets as it opens (Open Recent, Apply Style) is marked `commandsDynamic`; its entries
are not commands. Edit > Search's entries carry the registry id (`PaletteEntry::commandId`), and Recently Used keeps it.

To add a menu command:

1. Find (or add, with the five pieces above) the automation method that makes the edit.
2. Add a `Spec` row where it belongs in `buildMenus`, with a new id, Photoshop's key if it has one, the feature, and
   `.request` (or `.runs` for a dialog, whose OK runs the method) and `.when` for a reason it is greyed.
3. Translate the label and any reason (`translations_check`).
4. Cover it in `command_path_selftest` (below). Its registry check fails on a menu item that is not a command, an id
   that is malformed or used twice, and two actions sharing a key.

To convert a menu item:

1. Find (or add, with the five pieces above) the method whose handler makes the same edit. Its result must be the
   same document and the same history name as the menu's direct call; if the handler differs, make the handler
   right, not the menu.
2. Replace the lambda's session call and its `recordAction(...)` with `runCommand("method", params)`. Do not keep
   the `recordAction`: `handle` records the step.
3. A dialog's OK builds the same parameters it used to record and calls `commitAsCommand(method, params)`
   (`PixelDialog`), keeping its own commit only as the fallback.
4. Interactive commands keep their stages in the interface and converge on the command at the commit. Free
   Transform: Ctrl+T invokes it (`transformCommand`), the canvas updates it (`previewTransform`), Enter, Apply or a
   double-click commits it through `commitTransformCommand`, which runs `layers.setTransform` with the box's final
   values, and Esc cancels it (`cancelTransform`). What the method cannot express (a distortion, several layers, a
   mask alone, selected pixels) still commits directly.
5. Add the item to `tests`' `command_path_selftest` (`src/app/SelfTest.cpp`): the menu path and the automation path
   must give the same document and history names, and the same document as the direct call did before.

A panel that has only the session uses `EditorSession::runCommandOr(method, params, direct)`: the method when the
session is routed, `direct` (the old call) when it is not. The canvas context menu (`MainWindowCanvasMenu.cpp`)
reuses the menu bar's `QAction`s (by the keys `nameAction` gives them), so it follows whatever path each item takes.

### Converted

`command_path_selftest` checks every item below that is marked *checked* (the test prints how many it checked; G'MIC's and Select ▸ Subject's only run where G'MIC and the click-to-select model are installed):
the menu path (or dialog, key or panel control), the requests the recording holds, and the session calls the item made
before, compared as documents, selections, paths, channels, colours, vector masks, layer styles and history names, with
each recorded once; File > New, Open, Import File, Save As and Edit Contents through their file dialogs. When two paths
part it names the first step where they do.

| Where | Items | Method |
|---|---|---|
| File | Export > Export As… and Quick Export, Layer > Export As… and Quick Export (the dialog's choices as parameters; `export_as_selftest` checks them) | `document.export` |
| File | Revert *(checked)*: the file the document was opened from or last saved to, read again as one undo step | `document.revert` |
| File | New…, Open…, Import File…, Save, Save As… *(checked)*: the dialogs choose, the method opens or saves (Open's image as a document of its own: `asDocument`; a PSD too large for its layers asks first and sends `mergedOnly`); Save on a smart object's contents puts them back | `document.new`, `document.open`, `document.import`, `document.save`, `smartObject.commit` |
| Edit | Undo, Redo *(checked)*; Zoom In, Zoom Out, Fit on Screen, Actual Pixels *(checked)* (not recorded: history and the view) | `history.undo`, `history.redo`, `view.zoom` |
| Edit | Warp…'s OK, the Warp Cage's Enter, Content-Aware Scale's OK *(checked)* | `layers.warp`, `layers.setCage`, `pixels.contentAwareScale` |
| Tools | Swap and Default colours (X, D), the colour picker *(checked)*: a 16-bit pick as `#rrrrggggbbbb` | `colors.set` |
| Edit | Fill with Foreground / Background *(checked, a 16-bit colour too)*; Clear *(checked)*: pixels with a selection, else the selected layers, asking first whether to bake layers clipped to them | `pixels.fill`, `pixels.clear`, `layers.delete` |
| Edit | Cut, Copy, Copy Merged, Paste *(checked)*: Copy with layers selected and no selection copies the layers, Paste pastes copied layers | `pixels.cut`, `pixels.copy`, `pixels.copyMerged`, `pixels.paste`, `layers.copy`, `layers.paste` |
| Edit | Free Transform's commit *(checked)* | `layers.setTransform` |
| Edit | Assign Profile…, Convert to Profile… for a built-in profile or none | `document.profile` |
| Edit | Content-Aware Fill's OK, Auto and All sampling | `pixels.contentAwareFill` |
| Image | Mode: RGB, CMYK, Lab, 8, 16, 32 Bits *(checked: 16, 8, Lab, RGB)*; Canvas Size, Image Size, Trim, Crop to Selection, Flip Canvas Horizontal / Vertical, Adjustments > Invert *(checked)* | `image.mode`, `canvas.resize`, `image.resize`, `image.trim`, `canvas.crop` + `selection.none`, `canvas.flip`, `pixels.invert` |
| Image | Adjustments: every dialog's OK *(checked: Levels, Curves, Brightness/Contrast, Posterize)* | `pixels.adjust` |
| Filter | Every filter's OK in the Blur, Distort, Noise, Pixelate, Render, Sharpen, Stylize and Other submenus, and Lens Correction *(checked: Gaussian Blur, Motion Blur, Add Noise, Lens Correction)* | `pixels.filter` |
| Layer | New Layer, New Layer Below, New Folder, New Adjustment Layer, Layer via Copy, Duplicate, Delete, Merge Down, Merge Visible, Rename, Group, Bring Forward, Send Backward, Flip Layer Horizontal / Vertical, Resampling, Create / Release Clipping Mask *(checked)* | `layers.add`, `layers.viaCopy`, `layers.duplicate`, `layers.delete`, `layers.merge`, `layers.set`, `layers.group`, `layers.reorder`, `layers.flip` |
| Layer | Layer Mask: Reveal All, Hide All, From Selection (Reveal / Hide), Enable / Disable, Invert, Apply, Delete *(checked but From Selection)* | `layers.mask` |
| Layer | Delete Layer with several layers selected *(checked)*, Move Out of Folder *(checked)* | `layers.delete` with `ids` and `bakeClipping`, `layers.move` |
| Layer | Layer Style: the dialog's OK, Copy, Paste and Clear Layer Style, Apply Style *(checked)*; the Layers panel row menu's Copy, Paste and Clear | `layers.setStyle`, `layers.style`, `layers.applyStyle` |
| Layer | Vector Mask: Reveal All, Hide All, Current Path, Edit, Delete *(checked)*; the Layers panel's vector mask items and thumbnail | `vectorMask.set`, `vectorMask.target`, `vectorMask.delete` |
| Layer | Smart Objects > Edit Contents *(checked)* (a camera RAW smart object reopens in Camera Raw, as before) | `smartObject.editContents` |
| Type | Create Work Path, Convert to Shape *(checked)* | `text.toPath`, `text.toShape` |
| Layer | Smart Objects: Convert *(checked)*, Rasterize *(checked)*, Replace Contents…; File > Place Embedded… | `smartObject.convert`, `smartObject.rasterize`, `smartObject.replace`, `smartObject.place` |
| Select | All, Deselect, Inverse, Reselect, Modify (Expand, Contract, Feather, Smooth, Border), Load as Selection (Layer Pixels, Layer Mask, Add, Subtract, Intersect) *(checked)* | `selection.*` |
| Select | Subject (checked where the click-to-select model is downloaded; without it, that it is greyed and says why) | `selection.subject` |
| Select | Edit in Quick Mask Mode, Load Selection…, Save Selection…, the channel keys Ctrl+2 to 9 and Ctrl+Alt+2 to 9 *(checked)* | `selection.quickMask`, `channels.loadSelection`, `channels.saveSelection`, `channels.select` |
| Filter | Camera Raw Filter, Mosh, G'MIC (where installed), Remove Background: the dialogs' OK *(checked but Remove Background, which needs the model)*; Remove Background reuses the mask the dialog showed | `pixels.cameraRaw`, `pixels.mosh`, `pixels.gmic`, `pixels.removeBackground` |
| Layers panel | New layer (Ctrl-click: below), New folder, New adjustment layer's menu, Add layer mask, Delete (several layers too), the opacity slider (one step per drag, shown as it goes) and field, the blend mode, an eye clicked, Alt-click to clip or release, Rename, a row dragged to another place *(checked)*; the row menu's Duplicate, Delete, Create / Release Clipping Mask, Merge Down and mask items | `layers.add`, `layers.mask`, `layers.delete`, `layers.set`, `layers.move`, `layers.duplicate`, `layers.merge` |
| View | New Guide…, Clear Guides; the rulers' and Move tool's guides | `guides.*` |
| Paths panel | Make Work Path, Fill, Stroke, Make Selection, Make Shape Layer, Delete *(checked)*; Add to Selection | `paths.*` |
| Channels panel | Save selection as channel, Create new channel *(checked)*; New Channel… | `channels.saveSelection`, `channels.new` |

Where the method cannot express what the item does, the item keeps its direct call (and its old `recordAction`):
a colour neither `#rrggbb` nor `#rrrrggggbbbb` says exactly (a 32-bit pick), an ICC profile file in Assign / Convert
to Profile, Content-Aware Fill's painted Custom area, Cut on a smart object (it copies, then asks to rasterize), a
G'MIC command typed in the dialog that `pixels.gmic` does not allow, an eye swipe across several layers (one undo
step, which a `layers.set` per layer would split), and a dialog left open on another tab. An eye clicked on another layer than the active one, and a dragged row, record the layers'
ids (`layers.set` with `id`, `layers.move`), as an agent's requests do.

### Not converted, and why

| Items | Why |
|---|---|
| File: Open… or Import File… of a camera RAW file or a PDF | They open through their own dialogs (Camera Raw's develop, the PDF's page and resolution), which choose what the request would carry; `document.open` takes `settings` and `page` for scripts |
| File: the PSD, SVG, ICO, animated GIF, artboard and slice exports, Batch, Open Project Folder, tabs, Quit | Export dialogs of their own and the window's tabs; `document.export`, `artboards.export`, `slices.export`, `actions.batch` and `tabs.*` are there for scripts |
| Edit: Toggle Last State, Color Settings…, Keyboard Shortcuts…, Preferences…, Search… | History has no toggle method; the others are application settings and the interface |
| Layer: Edit Text… | It opens the text editor; `text.set` is the edit |
| Channels panel: Duplicate, Delete, Rename, reorder, eyes, Load as selection | The methods name channels by id; the eyes are view state |
| Filter: Smart Filters | A smart object's filter stack, edited in its own rows |
| Layers panel: Alt-drag a row (a copy placed there), Alt-drag a mask onto another layer, a folder's fold, the Smart Filter rows | No method copies a layer or a mask to a place; folding is view state; the rest name a filter by id |
| View (rulers, guides' switches, proofing, panels), Window, Help | Interface only |

## Where to start

| You want to ... | Look at |
|---|---|
| Fix how a PSD opens | `src/core/src/psd.cpp` (reader), `psd_text.cpp` (type), `psd_carry.cpp`; tests in `tests/psd_tests.cpp` |
| Fix how a PSD is written | `src/core/src/psd_writer.cpp`; `tests/psd_writer_tests.cpp`, `tests/psd_roundtrip.cpp`; [docs/psd-export.md](docs/psd-export.md) |
| Layer styles | `src/core/src/layerstyle*.cpp`, `src/app/LayerStyleDialog.cpp`; [docs/layer-styles.md](docs/layer-styles.md) |
| Vector masks and shapes | `src/core/src/vectormask.cpp`, `vectorlayer.cpp`, `shape.cpp`; [docs/vector-tools.md](docs/vector-tools.md) |
| Smart objects and Smart Filters | `src/core/src/smartobject*.cpp`, `smartfilter*.cpp`, `src/app/EditorSessionSmartObjects.cpp` |
| Clip Studio `.clip` files | `src/core/src/clip.cpp`, `csp_clip_import.c`; `tests/clip_tests.cpp` |
| Brush import (ABR, SUT, Procreate) | `src/core/src/abr.cpp`, `sut.cpp`, `procreate.cpp`, `brushimport.cpp`; `src/app/BrushImporter.cpp` |
| The brush engines | `src/core/src/brush.cpp`, `mypaint.cpp`, `tipbrush.cpp`; [docs/brush-latency.md](docs/brush-latency.md) |
| Rendering and blend modes | `src/core/src/render.cpp`, `blend.cpp` |
| Filters and adjustments | `src/core/src/filters.cpp`, `adjustments*.cpp`, `src/app/FilterDialog.cpp`, `AdjustmentEditor.cpp` |
| Selections, wand, Quick Select | `src/core/src/selection.cpp`, `smartwand.cpp`, `scribble.cpp`, `src/app/EditorSessionSelection.cpp` |
| A tool's canvas behaviour | `src/app/CanvasWidget*.cpp`, `ToolOptionsBar.cpp` |
| Menus and shortcuts | `src/app/MainWindowMenus.cpp` (the commands, added to the menus in order), `CommandRegistry.{h,cpp}`; the canvas's context menu in `MainWindowCanvasMenu.cpp` and `CanvasWidgetMenu.cpp` |
| Rulers, guides and snapping | `src/app/Ruler.h`, `CanvasWidgetGuides.cpp`, `CanvasWidgetPointer.cpp` (`guideTargets`), `ViewOptions.h`; `src/core/src/guides.cpp` |
| The Layers panel | `src/app/LayersPanel.cpp` |
| Other file formats | `src/core/src/{tga,ico,gif,aseprite,svg,affinity,raw}.cpp`, `src/app/MainWindowFiles.cpp` |
| An automation method | `src/app/Automation*.cpp`, `AutomationDescriptions.cpp`, `mcp/nekophoto_mcp.py`, [docs/automation.md](docs/automation.md) |
| Translations | `translations/nekophoto_ja.ts`, [docs/translating.md](docs/translating.md) |
| Packaging | `tools/package-appimage.sh`, `tools/package-windows.sh`, `.github/workflows/` |

The design notes in `docs/` explain the choices behind most features; read the one for your area first.

## Submitting a pull request

1. Branch from `main` and keep the change focused; one topic per pull request.
2. Build with `-DCOMPOSITOR_WARNINGS_AS_ERRORS=ON` and run `ctest --test-dir build --output-on-failure`.
   For automation changes, also run `tools/rpc_smoke.py` against a headless instance.
3. Add or update tests, the doc for the feature, and the Japanese translation of any new text.
4. In the description, say what changed and how you checked it; attach a screenshot for UI changes, and say why
   when render hashes or golden images change.

CI then runs, and must pass: the GCC and Clang builds on Ubuntu 24.04 with `-Werror`, every CTest suite (unit,
golden, render hashes, hostile input, translations), the offscreen smoke test, the automation socket and MCP bridge
smoke tests, and the Windows MinGW build with its tests and pipe smoke test.

By contributing you agree that your contribution is licensed under GPL-3.0-or-later, the project's licence.
