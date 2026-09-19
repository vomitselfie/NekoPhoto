# compositor-linux

compositor-linux is a Qt 6 front end over a portable C++ core that re-implements
the Mac app's document model, compositor, brush, history and `.comp` project
format. The existing C pixel routines under `Compositor/Rendering` are compiled
unchanged. The macOS application and its Xcode project are untouched; see
`docs/linux-port-architecture.md` for how the two relate.

## Building

Requirements: CMake 3.22+, Ninja (or Make), GCC 12+ or Clang 15+, Qt 6.4+
(Core, Gui, Widgets, Network, Svg, plus the Wayland platform plugin), libpng;
OpenCV for Remove Background.

Arch / Manjaro:

```bash
sudo pacman -S cmake ninja qt6-base qt6-svg qt6-wayland qt6-imageformats libpng opencv
```

Ubuntu 24.04:

```bash
sudo apt install cmake ninja-build qt6-base-dev qt6-svg-dev qt6-wayland qt6-image-formats-plugins libpng-dev libgl1-mesa-dev libopencv-dev
```

Then:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/src/app/compositor-linux      # or: ./build/src/app/compositor-linux Photo.comp
```

Under a Wayland session Qt picks the Wayland platform on its own; force it with
`QT_QPA_PLATFORM=wayland` if needed. `QT_QPA_PLATFORM=xcb` runs under X11 or
XWayland.

Options: `-DCOMPOSITOR_BUILD_APP=OFF` builds only the core and tests;
`-DCOMPOSITOR_WITH_OPENCV=OFF` leaves out the Remove Background model;
`-DCOMPOSITOR_WARNINGS_AS_ERRORS=ON` is what CI uses.
`-DCOMPOSITOR_QT_TOOL_DIR=<dir>` points the build at copies of `moc`, `uic`
and `rcc` for shells that cannot execute binaries under `/usr/lib`.

`compositor-linux --download-model isnet` fetches a model without the GUI (into
`COMPOSITOR_MODEL_DIR` when set); `--preferences` opens the Preferences dialog;
`--tool brush` (or any tool name from `--help`) selects a tool, and `--dialog new`
(or canvas-size, image-size, jpeg, levels, curves, hue, exposure, gradient-map,
grain, blur, motion-blur, noise, lens) opens that dialog; with `--screenshot` the
dialog is what gets grabbed.
`compositor-linux --demo --screenshot out.png --save-as Demo.comp` builds a layered
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
each filter's parameters; the dialog builds the controls from them, shows the
resulting command line, and previews on a reduced copy. Filters that change the
image size are rejected. Over automation: `pixels.gmic` and `gmic.filters`.

When the build finds libgmic (`COMPOSITOR_WITH_LIBGMIC`, on by default), setting
`COMPOSITOR_GMIC_INPROCESS=1` runs filters in-process through one interpreter
kept warm with the catalogue's commands, with no PNG round trip (12 MP through
the executable costs about six seconds of encode and decode). It stays opt-in
because libgmic 4.0.5 crashes inside `sharpen` when called as a library, and a
crash in-process takes the editor down.

## Appearance

Edit > Preferences > Appearance: System, Dark or Light. Dark and Light use Qt's
Fusion style with a fixed palette. System keeps the desktop's own Qt theme when
it can load (a distribution package under KDE or GNOME), and otherwise asks the
XDG desktop portal for the colour scheme, which is how the AppImage matches a
dark desktop. `COMPOSITOR_THEME=dark|light|system` overrides for one run.

## Automation

`--rpc` opens a JSON-RPC socket an agent or script can drive the editor
through, `--headless` does so without a window, and `mcp/compositor_mcp.py`
bridges it to MCP. `docs/automation.md` has the protocol and method list.

## Releases

`.github/workflows/release.yml` runs when a `v*` tag is pushed. It builds on
Ubuntu 22.04 with Qt 6.7 from the Qt installer (so the AppImage runs on
distributions back to 2022), runs the tests and the offscreen smoke test,
stages `cmake --install` into an AppDir, bundles Qt (Wayland, xcb and
offscreen platforms), libpng and the three OpenCV modules with linuxdeploy,
runs the packaged app once, and publishes `compositor-linux-<version>-x86_64.AppImage`
(with a zsync file for AppImageUpdate), a tarball, and `SHA256SUMS` on a
GitHub release with generated notes. To cut a release:

```bash
# bump the version in the root CMakeLists.txt first, commit, then
git tag v0.2.0 && git push origin v0.2.0
```

"Run workflow" on the Actions tab does a dry run that only attaches the
artifacts to the workflow run. The app reports the version it was built with
(`compositor-linux --version`, Help > About); tagged builds get the tag's
number, local builds the CMake project version.

OpenCV is linked as `core`, `imgproc` and `dnn` only via its CMake config
(pkg-config's entry would drag every module into the bundle).

## Layout

```
CMakeLists.txt                  root build
src/pixels/                     compositor_pixels: the existing C routines
src/core/                       compositor_core: portable C++20 editor core
    include/compositor/*.h      geometry, image, transform, document, history,
                                blend, render, brush, selection, png, project
src/app/                        the Qt 6 Widgets application
src/third_party/nlohmann/       JSON (MIT)
src/app/icons/                  tool icons from Lucide (ISC), tinted to the palette at runtime
tests/                          pixels_tests, core_tests, golden_tests (+ golden PNGs)
packaging/                      .desktop, icon, MIME type
.github/workflows/linux.yml     GCC and Clang builds, tests, offscreen smoke test
```

## What works

- New canvas; import PNG, JPEG, TIFF, WebP, HEIC and anything else Qt can
  decode (EXIF orientation applied); drag and drop of files and images onto
  the window.
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
  lasso, magic wand (tolerance, contiguous, sample all layers), add/subtract
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
  snapping while resizing, crop ratio presets, Load as Selection, a bake or
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
