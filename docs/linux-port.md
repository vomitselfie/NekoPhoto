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
sudo pacman -S cmake ninja qt6-base qt6-wayland qt6-imageformats libpng
```

Ubuntu 24.04:

```bash
sudo apt install cmake ninja-build qt6-base-dev qt6-wayland qt6-image-formats-plugins libpng-dev libgl1-mesa-dev
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
`-DCOMPOSITOR_WARNINGS_AS_ERRORS=ON` is what CI uses.
`-DCOMPOSITOR_QT_TOOL_DIR=<dir>` points the build at copies of `moc`, `uic`
and `rcc` for shells that cannot execute binaries under `/usr/lib`.

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
- Layers: create, delete, duplicate, rename inline, reorder and nest by drag
  and drop, folders, visibility (with the eye-swipe), opacity, all thirteen
  blend modes, merge down, group selected layers.
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
- Selections: rectangular and elliptical marquee, freehand and polygonal
  lasso, magic wand (tolerance, contiguous, sample all layers), add/subtract
  with Shift/Alt, move the outline, select all, deselect, inverse, expand,
  contract, fill, clear, crop to selection, mask from selection.
- Crop tool, Canvas Size (with anchor), Image Size.
- Undo/redo of everything, with Photoshop-style shortcuts throughout.
- `.comp` projects: open and save version 1–7 packages written by the Mac app,
  keeping unknown fields; PNG export with resolution metadata; JPEG export with
  a live preview.
- Wayland, X11, HiDPI and fractional scaling; tablet input (as a pointer).

## Not ported yet

- Adjustment layers render as pass-through (their settings survive a round
  trip untouched). Levels, Curves, Hue/Saturation, Exposure, Gradient Map and
  Grain, and the destructive filters (blurs, noise, lens correction, invert,
  remove background) are the next milestone; the C routines for Gradient Map,
  Grain, Noise and Lens are already built into the core.
- Spot Healing, Clone Stamp, Blur/Smudge, Gradient and Shape tools; free
  distort; moving and duplicating selected pixels; copy/paste.
- Multiple projects in tabs.
- Image Size scales layer placement rather than resampling pixels (the layers
  keep their full-resolution sources); the Mac resamples.

## Keyboard shortcuts

| Keys | Action |
|---|---|
| V M L W C B E I H Z | Tools: Move, Marquee, Lasso, Wand, Crop, Brush, Eraser, Eyedropper, Hand, Zoom |
| X, D | Swap / reset colours |
| Space + drag, middle drag, wheel | Pan |
| Ctrl + wheel, Ctrl +/−, Ctrl 0, Ctrl 1 | Zoom, fit, 100% |
| Ctrl T, Enter, Esc | Free transform, apply, cancel |
| Ctrl Shift N, Ctrl G, Ctrl J, Ctrl E | New layer, group, duplicate, merge down |
| Ctrl Alt G | Clipping mask |
| Ctrl ] / Ctrl [ | Bring forward / send backward |
| Ctrl A, Ctrl D, Ctrl Shift I | Select all, deselect, inverse |
| Alt Backspace, Ctrl Backspace, Delete | Fill foreground / background, clear |
| Ctrl Z, Ctrl Shift Z | Undo, redo |
| Ctrl Shift E, Ctrl Alt Shift S | Export PNG, export JPEG |
