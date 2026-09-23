# Licences and third-party notices

compositor-linux is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option) any
later version. It is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE. The full text is in [LICENSE](LICENSE).

Copyright (C) 2026 the compositor-linux contributors.

compositor-linux is a port of [Compositor](https://github.com/robbietilton/Compositor)
for macOS, copyright (c) 2026 Wonder Assembly LLC and released under the MIT
licence. The code taken from it, the macOS sources under `Compositor/`
(including the C pixel routines in `Compositor/Rendering` that the Linux build
compiles unchanged) and the parts of the port derived from them, keeps that
notice: [LICENSES/MIT-Compositor.txt](LICENSES/MIT-Compositor.txt). Those files
may still be used on their own under the MIT licence; the program as a whole is
distributed under the GPL.

The texts named below are in [LICENSES/](LICENSES). Installed builds carry
this file, `LICENSE` and `LICENSES/` in `share/doc/compositor-linux/` (inside
the AppImage and the tarball) or in `Contents/Resources/licenses/` (macOS).

## Compiled into the program

| Component | Licence | Text |
|---|---|---|
| [OpenCV](https://opencv.org) 4.14.0, modules core, imgproc and dnn; linked statically in release builds (`tools/build-opencv.sh`) | Apache-2.0 | [Apache-2.0.txt](LICENSES/Apache-2.0.txt), copyright holders in [OpenCV-COPYRIGHT.txt](LICENSES/OpenCV-COPYRIGHT.txt) |
| Protocol Buffers, as bundled with OpenCV's dnn module | BSD-3-Clause | [protobuf.txt](LICENSES/protobuf.txt) |
| Berkeley SoftFloat 3c, as bundled with OpenCV's core module | BSD-3-Clause | [Berkeley-SoftFloat.txt](LICENSES/Berkeley-SoftFloat.txt) |
| DLPack headers, as bundled with OpenCV's dnn module | Apache-2.0 | [Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| [nlohmann/json](https://github.com/nlohmann/json) (`src/third_party/nlohmann`) | MIT | [nlohmann-json.txt](LICENSES/nlohmann-json.txt) |
| [Lucide](https://lucide.dev) icons (`src/app/icons`) | ISC | [Lucide-ISC.txt](LICENSES/Lucide-ISC.txt) |

## Shipped as shared libraries

| Component | Licence | Text |
|---|---|---|
| [Qt](https://www.qt.io) 6: Core, Gui, Widgets, Network, Svg, DBus, Wayland and the image format plugins, bundled in the AppImage and the macOS app | LGPL-3.0 (used under its terms; the libraries are unmodified and can be replaced) | [LGPL-3.0.txt](LICENSES/LGPL-3.0.txt) with [LICENSE](LICENSE); Qt's own third-party components are listed at <https://doc.qt.io/qt-6/licenses-used-in-qt.html> |
| [libpng](http://www.libpng.org) | libpng / PNG Reference Library License v2 | [libpng.txt](LICENSES/libpng.txt) |
| [zlib](https://zlib.net) | zlib | [zlib.txt](LICENSES/zlib.txt) |

The AppImage also carries system libraries that Qt depends on (fonts, text
shaping, input, graphics). Their copyright files are collected at build time
into `share/doc/compositor-linux/bundled/` inside the AppImage and the tarball.

The source for the GPL program is this repository at the tag of each release.
Qt, libpng and zlib are unmodified upstream releases; OpenCV is built from the
unmodified 4.14.0 source archive by `tools/build-opencv.sh`.

## Used at run time, not distributed

| Component | Licence | Notes |
|---|---|---|
| [G'MIC](https://gmic.eu) | CeCILL v2.1 (GPL-compatible) or CeCILL-C | Filter > G'MIC runs the `gmic` program if it is installed. Builds configured with `-DCOMPOSITOR_WITH_LIBGMIC=ON` link `libgmic` from the system instead; release builds do not |
| G'MIC filter catalogue (`update<version>.gmic`) | CeCILL v2.1 | Downloaded by Update Filters from gmic.eu |
| IS-Net general use (DIS), via [rembg](https://github.com/danielgatis/rembg) | Apache-2.0 | Downloaded on request from Preferences (AI background removal) |
| U²-Net human segmentation and U²-Netp, via rembg | Apache-2.0 | Downloaded on request |
| PP-HumanSeg (PaddleSeg), via [OpenCV Zoo](https://github.com/opencv/opencv_zoo) | Apache-2.0 | Downloaded on request |
| EfficientSAM-Ti, via OpenCV Zoo | Apache-2.0 | Downloaded on request for Quick Select's Click engine |
