# compositor-linux

A Linux port of [Compositor](https://github.com/robbietilton/Compositor), the
small, focused, layer-based image editor for macOS by Wonder Assembly LLC. It
is built around the same compositing and post-processing workflow, with
Photoshop-style tools and shortcuts, and it reads and writes the same `.comp`
project packages as the Mac app.

The port is a Qt 6 front end over a portable C++ core. The Mac app's eight C
pixel routines are compiled unchanged; everything else (document model,
compositing, brush engine, selections, history, adjustments, filters, the
project format) is a C++ re-implementation checked against the Swift original.
The macOS sources and Xcode project remain in this tree, untouched, both as the
reference and so upstream changes can still be merged.

## Features

### Layers
- Layers and folders, with blend modes and opacity
- Layer masks: paint, fill, invert, blur and feather them; link or unlink them to transform a mask on its own
- Clipping masks and folder masks
- Adjustment layers: Hue/Saturation, Levels, Curves, Exposure, Gradient Map and Grain
- Merge Down, Merge Layers and Merge Group (Ctrl+E)
- Duplicate, rename inline, reorder and nest by drag and drop; Alt-drag to duplicate
- Drag layers between open projects

### Transform
- Non-destructive move, scale, rotate and flip; images keep their full resolution however small you make them
- Free distort (Ctrl-drag a handle), with Shift to lock to an axis
- Transform several layers, or a whole folder, together
- Snapping to canvas and layer edges and centers, with guides
- Exact values for position, size, scale and angle, stepped with the arrow keys
- Flip Layer and Flip Canvas, horizontal and vertical

### Selections
- Rectangle and Ellipse Marquee, Freehand and Polygonal Lasso, and Magic Wand
- Add to and subtract from selections, move the outline, or move and duplicate the pixels inside
- Load a layer's pixels or a mask as a selection
- Content-Aware Fill, which can also extend an image past its edges

### Painting and retouching
- Brush with size, hardness and opacity, and Shift for straight lines
- Spot Healing Brush (content-aware)
- Clone Stamp, aligned or not, sampling one layer or all of them
- Blur tool, on pixels or masks
- Gradient tool and Shape tool (rectangles, rounded rectangles and ellipses)
- Eyedropper and a full color picker

### Adjustments and filters
- Levels (with Auto), Curves, Hue/Saturation, Exposure, Gradient Map, Grain and Invert
- Gaussian Blur and Motion Blur that spread past a layer's edges
- Add Noise, Lens Correction and Remove Background
- Live previews, limited to the selection when there is one

### Canvas and files
- Multiple projects in tabs
- Crop with snapping, and Alt for symmetric cropping
- Canvas Size and Image Size
- Sharp high-quality downsampling when zoomed out, and a pixel grid when zoomed in
- Import JPEG, PNG, TIFF and WebP, including dropped images from other apps
- Export JPEG with a live preview; Copy Merged
- Photoshop-style keyboard shortcuts throughout

### Remove Background
Off by default. Edit > Preferences turns it on and downloads a segmentation
model (IS-Net or the small U2Net, from the rembg project, Apache-2.0) into the
app's data folder. The model runs locally through OpenCV's DNN module; nothing
is uploaded.

## Installing

Each release on the GitHub Releases page ships an AppImage (make it executable
and run it; works on any x86_64 distribution from 2022 on, Wayland or X11) and
a tarball with the binary, desktop file, icon and MIME type for `cmake --install`
style layouts.

## Building

Requirements: CMake 3.22+, Ninja (or Make), GCC 12+ or Clang 15+, Qt 6.4+
(Core, Gui, Widgets, Network, Svg and the Wayland platform plugin), libpng, and
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
./build/src/app/compositor-linux            # or: ./build/src/app/compositor-linux Photo.comp
sudo cmake --install build                  # optional: binary, .desktop, icon, MIME type
```

`docs/linux-port.md` covers build options, command-line flags, the source
layout and the keyboard shortcuts. `docs/linux-port-architecture.md` explains
how the port relates to the Mac sources, and `docs/project-format.md` the
`.comp` package format.

## Layout

```
CMakeLists.txt        root build for compositor-linux
src/pixels/           the Mac app's C pixel routines, compiled unchanged
src/core/             portable C++20 editor core (no Qt)
src/app/              the Qt 6 Widgets application
tests/                unit and golden-image tests (ctest)
packaging/            .desktop file, icon, MIME type
docs/                 port notes, project format, upstream README
Compositor/           the macOS app sources (reference, untouched)
Compositor.xcodeproj  the macOS Xcode project (untouched)
```

## Credits and license

Compositor is by Wonder Assembly LLC and is released under the MIT license;
the original README is kept at `docs/upstream-README.md`. compositor-linux is
MIT as well; see [LICENSE](LICENSE). Bundled third-party code: nlohmann/json
(MIT) and the Lucide icons (ISC), each with its license alongside.
