# Compositor on Linux

The Linux port is a Qt 6 front end over a portable C++ core that re-implements
the Mac app's document model, compositor, brush, history and `.comp` project
format. The existing C pixel routines under `Compositor/Rendering` are compiled
unchanged. The macOS application and its Xcode project are untouched; see
`docs/linux-port-architecture.md` for how the two relate.

## Building

Requirements: CMake 3.22+, Ninja (or Make), GCC 12+ or Clang 15+, Qt 6.4+
(Core, Gui, Widgets, plus the Wayland platform plugin), libpng.

Arch / Manjaro:

```bash
sudo pacman -S cmake ninja qt6-base qt6-wayland qt6-imageformats libpng opencv
```

Ubuntu 24.04:

```bash
sudo apt install cmake ninja-build qt6-base-dev qt6-wayland qt6-image-formats-plugins libpng-dev libgl1-mesa-dev libopencv-dev
```

Then:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/src/app/compositor            # or: ./build/src/app/compositor Photo.comp
```

Under a Wayland session Qt picks the Wayland platform on its own; force it with
`QT_QPA_PLATFORM=wayland` if needed. `QT_QPA_PLATFORM=xcb` runs under X11 or
XWayland.

Options: `-DCOMPOSITOR_BUILD_APP=OFF` builds only the core and tests;
`-DCOMPOSITOR_WITH_OPENCV=OFF` leaves out the Remove Background model;
`-DCOMPOSITOR_WARNINGS_AS_ERRORS=ON` is what CI uses.
`-DCOMPOSITOR_QT_TOOL_DIR=<dir>` points the build at copies of `moc`, `uic`
and `rcc` for shells that cannot execute binaries under `/usr/lib`.

`compositor --download-model isnet` fetches a model without the GUI (into
`COMPOSITOR_MODEL_DIR` when set); `--preferences` opens the Preferences dialog.
`compositor --demo --screenshot out.png --save-as Demo.comp` builds a layered
demo document, grabs the window and saves a project without any interaction
(works with `QT_QPA_PLATFORM=offscreen`); CI runs it as a smoke test.

## Layout

```
CMakeLists.txt                  root build
src/pixels/                     compositor_pixels: the existing C routines
src/core/                       compositor_core: portable C++20 editor core
    include/compositor/*.h      geometry, image, transform, document, history,
                                blend, render, brush, selection, png, project
src/app/                        the Qt 6 Widgets application
src/third_party/nlohmann/       JSON (MIT)
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
  Clone Stamp (aligned or not, sampling one layer or all), both on the
  existing C healing code; Content-Aware Fill of a selection, growing the
  layer past its edge when the selection reaches out.
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
  points), Hue/Saturation (per-range, colorize), Exposure, Gradient Map and
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
  tool (rectangle, rounded rectangle, ellipse; Shift-U switches), Blur /
  Smudge / Liquify tool (Blur works on masks too, to feather them).
- Opacity number keys, Shift-[ ] for hardness, Shift-M / Shift-L kinds,
  snapping while resizing, crop ratio presets, Load as Selection, a bake or
  release prompt when deleting a clipping base, Alt-click to clip, Alt-drag
  to copy a mask, Hue/Saturation eyedroppers and targeted-adjustment drag.
- Multiple projects in tabs (Ctrl+N opens a new tab, Ctrl+W closes, Ctrl+Tab cycles); drag a layer from the Layers panel onto another tab to
  copy it there.
- Image Size resamples every layer and mask, as the Mac does.

- Remove Background: off until enabled in Edit > Preferences, which offers the
  IS-Net model (rembg's Apache-2.0 ONNX release, ~180 MB) or a 4.6 MB U2Net,
  downloads it with a checksum check into the app data directory, and can
  remove it again. The model runs through OpenCV's DNN module, with the Mac panel's
  Advanced refinement (guided-filter edge refine for hair, matte contrast,
  edge shift). The result is a layer mask, multiplied with any existing mask,
  limited to the selection when there is one. OpenCV is optional at build
  time; `COMPOSITOR_MODEL_DIR` overrides where models are kept.

## Not ported

Nothing on the Mac README's feature list is missing now. Remove Background
uses a different model than Apple's Vision, so its cutouts differ in detail.

## Keyboard shortcuts

| Keys | Action |
|---|---|
| V M L W C B E J S R G U I H Z | Tools: Move, Marquee, Lasso, Wand, Crop, Brush, Eraser, Spot Healing, Clone Stamp, Smudge, Gradient, Shape, Eyedropper, Hand, Zoom |
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
