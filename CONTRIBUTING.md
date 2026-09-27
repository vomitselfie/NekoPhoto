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
- **Pixels** are premultiplied RGBA8, top-down, with an explicit stride (`compositor::Image`). Masks are 8-bit.
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
- **Every visible string goes through `tr()`** and gets its Japanese translation in the same change; the
  `translations_check` test fails otherwise. How to update and translate: [docs/translating.md](docs/translating.md).
- **No warnings.** CI builds with `-Werror` on GCC and Clang.
- **Render hashes don't change by accident.** `render_hash_tests` hashes 133 scenes against
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
| Menus and shortcuts | `src/app/MainWindowMenus.cpp` |
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
