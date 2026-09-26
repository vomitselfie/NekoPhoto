# NekoPhoto

NekoPhoto is a Qt 6 front end over a portable C++ core that re-implements
the Mac app's document model, compositor, brush, history and `.comp` project
format. The existing C pixel routines under `Compositor/Rendering` are compiled
unchanged. The macOS application and its Xcode project are untouched; see
`docs/linux-port-architecture.md` for how the two relate.

Until 1.0 the project was called compositor-linux. The rename covers the
program (`nekophoto`, `NekoPhoto.app` on the Mac), the desktop entry and icon,
the settings and data folders (`~/.config/nekophoto/`, `~/.local/share/nekophoto/nekophoto/`),
the automation socket (`nekophoto.sock`) and the MCP bridge (`mcp/nekophoto_mcp.py`,
server name `nekophoto`). On the first launch after the rename the old settings
file and data folder (model, imported brushes, G'MIC catalogue, recovery files)
move to the new places, unless something is there already. Unchanged on
purpose: the `.comp` format and its MIME type, which keep Mac projects opening;
the `COMPOSITOR_*` environment variables and build options; and the C++
`compositor` namespace. `tools/integrate-appimage.sh` removes a launcher that
an older compositor-linux AppImage installed.

## Building

Requirements: CMake 3.22+, Ninja (or Make), GCC 12+ or Clang 15+, Qt 6.4+
(Core, Gui, Widgets, Network, Svg, plus the Wayland platform plugin), libpng;
OpenCV for Remove Background; libmypaint 1.5 or newer for the MyPaint brushes; LibRaw for camera RAW files;
SQLite for importing Clip Studio brushes; libzstd for Affinity documents; Qt PDF (Arch: qt6-webengine, Ubuntu: qt6-pdf-dev, Homebrew: part of qt) for opening PDF files.

Arch / Manjaro:

```bash
sudo pacman -S cmake ninja qt6-base qt6-svg qt6-wayland qt6-imageformats qt6-webengine libpng libmypaint libraw zstd opencv
```

Ubuntu 24.04:

```bash
sudo apt install cmake ninja-build qt6-base-dev qt6-svg-dev qt6-pdf-dev qt6-wayland qt6-image-formats-plugins libpng-dev libmypaint-dev libraw-dev libzstd-dev libsqlite3-dev libgl1-mesa-dev libopencv-dev
```

Then:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/src/app/nekophoto      # or: ./build/src/app/nekophoto Photo.comp
```

macOS (Homebrew) builds an app bundle; the G'MIC filters appear once
`brew install gmic` has put `gmic` on the path:

```bash
brew install cmake ninja qt libpng libmypaint libraw zstd opencv pkg-config
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build -j && open build/src/app/NekoPhoto.app
```

Under a Wayland session Qt picks the Wayland platform on its own; force it with
`QT_QPA_PLATFORM=wayland` if needed. `QT_QPA_PLATFORM=xcb` runs under X11 or
XWayland.

Options: `-DCOMPOSITOR_BUILD_APP=OFF` builds only the core and tests;
`-DCOMPOSITOR_WITH_OPENCV=OFF` leaves out the Remove Background model;
`-DCOMPOSITOR_WITH_MYPAINT=OFF` leaves out the MyPaint brushes (the round brush stays);
`-DCOMPOSITOR_WITH_SQLITE=OFF` leaves out Clip Studio brush import;
`-DCOMPOSITOR_WITH_QTPDF=OFF` leaves out PDF import;
`-DCOMPOSITOR_WARNINGS_AS_ERRORS=ON` is what CI uses.
`-DOpenCV_DIR=<prefix>/lib/cmake/opencv4` builds against the OpenCV that
`tools/build-opencv.sh <prefix>` makes: a pinned 4.x, static, with only the
three modules the model needs and every optional dependency off. CI, the
release jobs and the Mac bundle use that build (cached, so it is compiled once
per runner image), which is how every platform ends up running the model on
the same OpenCV; a local build takes the system OpenCV unless told otherwise.
`-DCOMPOSITOR_QT_TOOL_DIR=<dir>` points the build at copies of `moc`, `uic`
and `rcc` for shells that cannot execute binaries under `/usr/lib`.

`nekophoto --download-model isnet` fetches a model without the GUI (into
`COMPOSITOR_MODEL_DIR` when set); `--preferences` opens the Preferences dialog;
One editor per user: a launch that carries only file names (a double-click in
the file manager, `nekophoto photo.psd`) hands them to the running
editor, which opens them as tabs and raises its window, and quits (with
`--rpc` the running editor also starts its automation socket for the caller);
`--new-window` keeps a separate process, as any of the options below does.
`--tool brush` (or any tool name from `--help`) selects a tool, and `--dialog new`
(or canvas-size, image-size, jpeg, levels, curves, hue, exposure, gradient-map,
grain, blur, motion-blur, noise, lens, gmic, background, text, fonts, brushes) opens that
dialog; with `--screenshot` the dialog is what gets grabbed.
`nekophoto --demo --screenshot out.png --save-as Demo.comp` builds a layered
demo document, grabs the window and saves a project without any interaction
(works with `QT_QPA_PLATFORM=offscreen`); CI runs it as a smoke test.

`COMPOSITOR_DEBUG_LAYOUT=1` prints each toolbar's, dock's and central widget's
minimum size at startup, for chasing what stops the window from shrinking;
`COMPOSITOR_WINDOW_SIZE=1000x700` forces the initial window size, for looking at
the layout as a laptop or a tiling window manager would show it.

## Installing

`sudo cmake --install build` installs the binary to `/usr/local/bin`, the
launcher entry, icon and `.comp` MIME type under `/usr/local/share`; follow it
with `sudo update-mime-database /usr/local/share/mime` and
`sudo update-desktop-database /usr/local/share/applications` so the file type
and launcher pick it up at once. `cmake --install build --prefix ~/.local` does
the same for one user (make sure `~/.local/bin` is on the launcher's PATH).

For an AppImage, `tools/integrate-appimage.sh <file>` copies it to
`~/Applications` and writes the launcher entry, icon and MIME type under
`~/.local/share`; `--remove` undoes it.

## G'MIC

Filter > G'MIC runs the `gmic` executable (an optional runtime dependency:
`pacman -S gmic` / `apt install gmic`; `COMPOSITOR_GMIC` points at a specific
binary) on the active layer's pixels through a PNG round trip. The dialog lists
a few essentials from G'MIC's core, and the whole catalogue once its definition
file is available: Update Filters downloads `https://gmic.eu/update<version>.gmic`
into the app data folder's `gmic/`, and an existing G'MIC-Qt copy under
`~/.config/gmic/` is used until then. The `#@gui` lines of that file describe
each filter's parameters; the dialog builds the controls from them (the
declaration's separators and short notes become folding sections, long notes a
Notes fold), lists filters under the catalogue's folders, shows the resulting
command line, and previews on a reduced copy. Filters that change the
image size are rejected. Over automation: `pixels.gmic` (filter names and
numbers only, since G'MIC can run shell commands; `COMPOSITOR_GMIC_UNRESTRICTED=1`
lifts that) and `gmic.filters`.

The file gmic.eu serves is compressed (a one-image G'MIC file: two header
lines, then zlib), and a download that holds no filters never replaces a
working catalogue. The parser reads G'MIC-Qt's forms: controls marked `_` or
`~`, `()`, `[]` or `{}` around their arguments, a preview-behaviour suffix
(`_0`, `_1`, `_2`, `+`), hex colours with alpha, `value()` with several numbers,
buttons (they pass 0), folders nested by underscores and author subfolders in
italics; entries with no command (About and the like) and filters needing a
file or folder chooser are left out. A command runs through a one-line script
(`-m command.gmic`), so G'MIC parses its arguments as G'MIC-Qt's do, text with
spaces and quotes included. G'MIC runs with no `DISPLAY` or `WAYLAND_DISPLAY`:
some catalogue entries are interactive programs (games, tools waiting for
clicks in a window of their own), and without a display they fail at once.
Previews stop after 30 s and Apply after 5 minutes.

`tools/gmic-sweep.py` runs every filter once at its defaults, as the editor
runs it, and writes `src/app/gmic/unsupported.txt`: the filters that change the
image size, make several layers, fail or give a blank image. The dialog hides
those unless Show all filters is on, and `gmic.filters` leaves them out unless
`all: true`. With G'MIC 4.0.5 about 890 of the catalogue's 1,151 filters show;
rerun the sweep when G'MIC updates (`--gallery` also writes a captioned image
per working filter).

When the build is configured with `-DCOMPOSITOR_WITH_LIBGMIC=ON` and finds libgmic (off by
default, so an installed binary never depends on a library that a later upgrade may remove), setting
`COMPOSITOR_GMIC_INPROCESS=1` runs filters in-process through one interpreter
kept warm with the catalogue's commands, with no PNG round trip (12 MP through
the executable costs about six seconds of encode and decode). It stays opt-in
because libgmic 4.0.5 crashes inside `sharpen` when called as a library, and a
crash in-process takes the editor down. Licensing is no obstacle: libgmic's
CeCILL licences are GPL-compatible, and NekoPhoto is GPL-3.0-or-later.

## Appearance

Edit > Preferences > Appearance: System, Dark or Light. Dark and Light use Qt's
Fusion style with a fixed palette. System keeps the desktop's own Qt theme when
it can load (a distribution package under KDE or GNOME), and otherwise asks the
XDG desktop portal for the colour scheme, which is how the AppImage matches a
dark desktop. `COMPOSITOR_THEME=dark|light|system` overrides for one run.

## Automation

`--rpc` opens a JSON-RPC socket an agent or script can drive the editor
through, `--headless` does so without a window, and `mcp/nekophoto_mcp.py`
bridges it to MCP. `docs/automation.md` has the protocol and method list.

## Releases

`.github/workflows/release.yml` runs when a `v*` tag is pushed. It builds on
Ubuntu 22.04 with Qt 6.7 from the Qt installer (so the AppImage runs on
distributions back to 2022), runs the tests and the offscreen smoke test,
stages `cmake --install` into an AppDir, bundles Qt (Wayland, xcb and
offscreen platforms) and libpng with linuxdeploy (OpenCV is built in),
copies the Debian copyright file of every bundled system library into
`share/doc/nekophoto/bundled/` next to the installed licence texts,
runs the packaged app once, and publishes `NekoPhoto-<version>-x86_64.AppImage`
(with a zsync file for AppImageUpdate), a tarball, and `SHA256SUMS` on a
GitHub release with generated notes. To cut a release:

```bash
# bump the version in the root CMakeLists.txt first, commit, then
git tag v0.2.0 && git push origin v0.2.0
```

"Run workflow" on the Actions tab does a dry run that only attaches the
artifacts to the workflow run. The app reports the version it was built with
(`nekophoto --version`, Help > About); tagged builds get the tag's
number, local builds the CMake project version.

OpenCV is the vendored static build of `tools/build-opencv.sh` (4.14, `core`,
`imgproc` and `dnn` only), linked through its CMake config, so the AppImage
carries no OpenCV shared libraries and every release runs the model on the
same version.

A second job in the same workflow builds the app on a macOS Apple Silicon
runner with Homebrew's Qt and libpng and the same vendored OpenCV, runs the tests and the offscreen
smoke test, bundles Qt with `macdeployqt`, signs the bundle ad hoc (unsigned
arm64 binaries do not launch at all; ad hoc signed ones do after Gatekeeper's
Open Anyway) and zips it as `NekoPhoto-<version>-macos-arm64.zip`; a
publish job then attaches both platforms' files to the release. The bundle's
Info.plist comes from `packaging/Info.plist.in` and its icon from
`packaging/nekophoto.icns`, built from the same SVG as the Linux icon.
`.github/workflows/macos.yml` runs the same build on every push so the Mac
side stays compiling; its zip is an artifact on the workflow run. Proper
signing and notarisation are not set up: this is a way to try the editor on a
Mac, not a Mac product.

## Layout

```
CMakeLists.txt                  root build
src/pixels/                     compositor_pixels: the existing C routines
src/core/                       compositor_core: portable C++20 editor core
    include/compositor/*.h      geometry, image, transform, document, history,
                                blend, render, brush, selection, png, project
src/app/                        the Qt 6 Widgets application
src/third_party/nlohmann/       JSON (MIT)
LICENSE, LICENSES/              GPL-3.0-or-later for the port; upstream MIT and third-party texts
THIRD-PARTY-NOTICES.md          every bundled or linked component and its licence
src/app/icons/                  tool icons from Lucide (ISC), tinted to the palette at runtime
tests/                          pixels_tests, core_tests, golden_tests (+ golden PNGs)
packaging/                      .desktop, icon, MIME type
.github/workflows/linux.yml     GCC and Clang builds, tests, offscreen smoke test
```

## What works

- New canvas; import PNG, JPEG, TIFF, WebP, HEIC and anything else Qt can
  decode (EXIF orientation applied); drag and drop of files and images onto
  the window.
- Clip Studio projects (`.clip`, `src/core/src/clip.cpp` over the C reader
  `csp_clip_import.c`): the CSFCHUNK container's tile streams and its embedded
  SQLite database (Layer, Mipmap, MipmapInfo, Offscreen) give the layers,
  folders, masks, opacity, visibility, clipping and blend modes. A layer's
  bitmap sits at `LayerOffset` + `LayerRenderOffscrOffset` (it grows in whole
  tiles, so a layer moved past the edge has a bitmap starting off the canvas),
  and each layer is cropped to what it paints. Blend modes without a
  counterpart take the nearest (as for PSD) and are listed. Vector, text and
  other special layers come in as the pixels Clip Studio cached; 16- and
  32-bit and 1-bit layers are refused with a note. Checked against twelve real
  files and their PSD exports (identical renders) and fuzzed under ASan.
- Crash recovery (`src/app/Autosave.cpp`): in an ordinary launch, every tab
  with unsaved changes is saved as a project every few minutes (Preferences,
  `autosave/minutes`, default 3, 0 off) into
  `~/.local/share/nekophoto/nekophoto/recovery/<instance>/`, on
  a worker thread from a copy of the document. A save or closing the tab
  removes its copy; a clean quit removes the folder. Each instance holds
  `<instance>.lock` (a QLockFile, stale only when its process is gone), so the
  next launch offers back only what a crashed instance left. Headless, batch,
  demo and screenshot runs do not autosave. For tests,
  `COMPOSITOR_AUTOSAVE_MS` sets the interval and `COMPOSITOR_RECOVERY_ANSWER`
  (recover, discard, later) answers the offer.
- Size limits: 30,000 pixels a side and 100 megapixels for any one canvas,
  layer or mask, as on the Mac; up to a gigapixel for all layers together
  (and as much again for masks), where the Mac stops at 100 megapixels, so a
  stack of 4K game textures opens and saves. Saving a project past the Mac's
  total says so in the status bar (`macCompatible` over automation), since
  Compositor for macOS will not open it.
- Layers: create, delete, duplicate, rename inline or from the menu, reorder
  and nest by drag and drop, move out of a folder, folders, visibility (with
  the eye-swipe), opacity, all thirteen blend modes with a hover preview,
  Ctrl+E merges down / merges the selected layers / merges a folder with
  blend modes, masks and clipping baked in, group selected layers.
- Layer masks (reveal all, hide all, from the selection), enable/disable,
  link/unlink, invert, apply, delete; folder masks; clipping masks (create /
  release, contiguous stacks share the base's alpha).
- Move / transform: drag, resize handles (Shift reverses the ratio lock, Alt
  scales about the centre), rotate near a corner, snapping to the canvas and
  other layers with guides, arrow-key nudging, numeric fields in the options
  bar, Ctrl+T for a pending transform applied with Enter. Non-destructive:
  pixels keep their resolution. Flip layer and flip canvas.
- Brush and eraser with size, hardness and opacity, Shift-click straight
  lines, `[` and `]` for size, painting on masks (white reveals, black hides).
- MyPaint brushes: the 196 presets of mypaint-brushes 2.0.2 (CC0), compiled in
  and chosen from the Brush tool's options bar, painted by libmypaint on 15-bit
  tiles filled from the layer so smudging reads the real paint. They follow pen
  pressure and tilt (a mouse is half pressure), take Size, Opacity, the colour
  and Erase from the options, paint layer pixels (a mask gets the round tip),
  and undo like any stroke. `.myb` files in
  `~/.local/share/nekophoto/nekophoto/brushes` appear as My
  Brushes. libmypaint's newer `stroke_to_2` reads uninitialised memory in 1.6,
  so the engine uses `stroke_to`: the 19 Dieterle presets that ask for pigment
  mixing paint with ordinary RGB mixing.
- Tip brushes and brush import: a second engine stamps a tip image along the
  stroke (spacing, angle or following the stroke, jitter of size, angle and
  flow, roundness, scatter with a count, flips, pressure on size and flow, a
  grain texture) into the round brush's coverage, so opacity, the selection,
  masks, erasing and undo work as for any brush. File > Import Brushes reads
  Photoshop `.abr` (versions 1, 2 and 6 to 10: sampled and computed tips, the
  presets' dynamics), Procreate `.brushset` and `.brush` (Brush.archive keyed
  archives; key names after the MIT procreate-brush-decoder schema), Clip
  Studio `.sut` (the settings from its SQLite tables, and the tip images and
  paper textures embedded as materials: each a tar holding a C2F layer file
  whose SQLite pages from page 6 are plain and are read without SQLite, the
  image being a PNG or 256-pixel zlib tiles; the file does not link materials
  to brushes, so each kind is matched in the order the brushes use them) and
  images (alpha, or darkness when
  opaque). Each brush is saved as an open folder (`brush.json`, `tip.png`,
  optional `grain.png`, `preview.png`) under
  `~/.local/share/nekophoto/nekophoto/brushes/imported/<set>/`,
  and the import lists what it could not carry over (texture and dual brush,
  wet mixing, Procreate's built-in shapes and grains, Clip Studio's pressure
  curves and texture rotation, brightness and contrast). A file may decode at most 256 megapixels of tips.
- Spot Healing Brush (Content-Aware, Create Texture, Proximity Match) and
  Clone Stamp (aligned or not, sampling one layer or all). Content-Aware
  healing and Content-Aware Fill synthesise from the surroundings with
  PatchMatch (matching on colour and gradient, keeping repeating patterns in
  phase), so edges and patterns continue; a healing stroke previews its result
  once the pointer pauses; the fill grows the layer past its edge when the
  selection reaches out.
- Cut, Copy, Copy Merged, Paste (as a new layer, back in place, or centred
  for images from other apps) and Layer via Copy, through the system clipboard.
- Selections: rectangular and elliptical marquee, freehand and polygonal
  lasso, magic wand (tolerance, contiguous, sample all layers), quick select
  by scribble (GrabCut) or by click (EfficientSAM, a download) on Q, add/subtract
  with Shift/Alt, move the outline, select all, deselect, inverse, expand,
  contract, fill, clear, crop to selection, mask from selection.
- Crop tool with snapping to canvas and layer edges, ratio presets and Alt
  for symmetric cropping; Canvas Size (with anchor); Image Size with a
  resampling choice.
- Undo/redo of everything, with Photoshop-style shortcuts throughout.
- `.comp` projects: open and save version 1–7 packages written by the Mac app,
  keeping unknown fields; PNG export with resolution metadata; JPEG export with
  a live preview.
- Wayland, X11, HiDPI and fractional scaling; tablet input (as a pointer).
- Adjustment layers: Levels (with histogram, Auto and black / gray / white
  point samplers), Curves (draggable
  points), Hue/Saturation (per-range, colorize, an optional Photoshop-style saturation curve), Exposure, Gradient Map and
  Grain, edited live in the Adjustments panel, saved in the Mac's format.
- Image > Adjustments applies the same six to a layer's pixels with a live
  preview, inside the selection; Invert works on pixels and masks.
- Filter menu: Gaussian Blur and Motion Blur (spreading past the layer's
  edges, as on the Mac), Add Noise, Lens Correction, all with a live preview.

- Move tool drags selected pixels (Alt duplicates, Ctrl-arrows nudge); Ctrl+T
  with a selection floats the pixels for a transform; Ctrl-drag a handle for
  free distort (Shift locks an axis); several layers or a folder transform
  together; Alt-drag duplicates a layer while moving it.
- Gradient tool (linear / radial, foreground to background or to transparent,
  reverse, opacity; Shift snaps the angle; Enter applies, Esc discards), Shape
  tool (rectangle, rounded rectangle, ellipse; Shift-U switches), Text tool
  (click to add text in the foreground colour, click a text layer to edit it, or
  double-click text with any tool, or its thumbnail in the Layers panel;
  the options bar sets font, size, bold, italic and alignment, the editor also
  colour, line and letter spacing; the font picker folds families sharing a
  leading name into one expandable row, filters as you type and keeps recent
  fonts on top; a text layer is a raster with its text and style beside it in
  the manifest, so the Mac app sees pixels), Blur /
  Smudge / Liquify tool (Blur works on masks too, to feather them; Liquify
  accumulates a displacement field and resamples the original through it, so
  long strokes stay sharp).
- Opacity number keys, Shift-[ ] for hardness, Shift-M / Shift-L kinds,
  snapping while resizing, crop ratio presets, a Select menu (All, Deselect,
  Inverse; Modify: Expand, Contract, Feather, Smooth, Border; Load as Selection
  from a layer's pixels or mask, replacing, adding, subtracting or
  intersecting), a bake or
  release prompt when deleting a clipping base, Alt-click to clip, Alt-drag
  to copy a mask, Hue/Saturation eyedroppers and targeted-adjustment drag.
- Multiple projects in tabs (Ctrl+N opens a new tab, Ctrl+W closes, Ctrl+Tab cycles); drag a layer from the Layers panel onto another tab to
  copy it there.
- Image Size resamples every layer and mask, as the Mac does. "High quality" is a
  Lanczos-3 resample for pure scaling and Catmull-Rom bicubic on rotated or
  distorted layers; "Smooth" is an area-weighted triangle and bilinear; "Nearest"
  keeps pixels hard.

- Remove Background: off until enabled in Edit > Preferences, which offers the
  IS-Net model (rembg's Apache-2.0 ONNX release, ~180 MB), the U2Net portrait
  model, a 4.6 MB U2Net, and Baidu's 6 MB PP-HumanSeg from OpenCV's model zoo
  (a coarse mask in milliseconds; when downloaded it also gives the dialog an
  instant preview while a slower model runs), downloads them with a checksum
  check into the app data directory, and can remove them again. The model runs through OpenCV's DNN module, with the Mac panel's
  Advanced refinement (guided-filter edge refine for hair, matte contrast,
  edge shift) plus a Matting slider that solves the true opacity of hair and
  fur in a band around the edge from foreground and background colour samples.
  The result is a layer mask, multiplied with any existing mask,
  limited to the selection when there is one. OpenCV is optional at build
  time; `COMPOSITOR_MODEL_DIR` overrides where models are kept.

## Not ported

Nothing on the Mac README's feature list is missing now. Remove Background
uses a different model than Apple's Vision, so its cutouts differ in detail.

## Keyboard shortcuts

| Keys | Action |
|---|---|
| V M L W C B E J S R G U T I H Z | Tools: Move, Marquee, Lasso, Wand, Crop, Brush, Eraser, Spot Healing, Clone Stamp, Smudge, Gradient, Shape, Text, Eyedropper, Hand, Zoom |
| Shift M / L / U | Switch the Marquee, Lasso or Shape kind |
| 1…9, 0 | Opacity 10%…90%, 100% (two digits quickly for an exact value) |
| [ ], Shift [ ] | Brush size, hardness |
| X, D | Swap / reset colours |
| Space + drag, middle drag, wheel | Pan |
| Ctrl + wheel, Ctrl +/−, Ctrl 0, Ctrl 1 | Zoom, fit, 100% |
| Ctrl T, Enter, Esc | Free transform (of the selection when there is one), apply, cancel |
| Ctrl + drag handle | Free distort |
| Ctrl + arrows | Nudge selected pixels |
| Arrows with a selection tool | Nudge the selection outline |
| Ctrl Shift [ | Move the layer out of its folder |
| Ctrl N, Ctrl W, Ctrl Tab | New tab, close tab, next tab |
| Ctrl Shift N, Ctrl G, Ctrl J, Ctrl E | New layer, group, duplicate, merge down |
| Ctrl Alt G | Clipping mask |
| Ctrl ] / Ctrl [ | Bring forward / send backward |
| Ctrl A, Ctrl D, Ctrl Shift I | Select all, deselect, inverse |
| Ctrl L, Ctrl M, Ctrl U, Ctrl I | Levels, Curves, Hue/Saturation, Invert |
| Ctrl X, Ctrl C, Ctrl Shift C, Ctrl V | Cut, copy, copy merged, paste |
| Alt Backspace, Ctrl Backspace, Delete, Shift F5 | Fill foreground / background, clear, content-aware fill |
| Ctrl Z, Ctrl Shift Z | Undo, redo |
| Ctrl Shift E, Ctrl Alt Shift S | Export PNG, export JPEG |
| Ctrl , | Preferences |
