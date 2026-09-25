# Licences and third-party notices

NekoPhoto is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option) any
later version. It is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE. The full text is in [LICENSE](LICENSE).

Copyright (C) 2026 the NekoPhoto contributors.

NekoPhoto (formerly compositor-linux) began as a port of [Compositor](https://github.com/robbietilton/Compositor)
for macOS, copyright (c) 2026 Wonder Assembly LLC and released under the MIT
licence. The code taken from it, the macOS sources under `Compositor/`
(including the C pixel routines in `Compositor/Rendering` that the Linux build
compiles unchanged) and the parts of the port derived from them, keeps that
notice: [LICENSES/MIT-Compositor.txt](LICENSES/MIT-Compositor.txt). Those files
may still be used on their own under the MIT licence; the program as a whole is
distributed under the GPL.

The texts named below are in [LICENSES/](LICENSES). Installed builds carry
this file, `LICENSE` and `LICENSES/` in `share/doc/nekophoto/` (inside
the AppImage and the tarball) or in `Contents/Resources/licenses/` (macOS).

## Compiled into the program

| Component | Licence | Text |
|---|---|---|
| [OpenCV](https://opencv.org) 4.14.0, modules core, imgproc and dnn; linked statically in release builds (`tools/build-opencv.sh`) | Apache-2.0 | [Apache-2.0.txt](LICENSES/Apache-2.0.txt), copyright holders in [OpenCV-COPYRIGHT.txt](LICENSES/OpenCV-COPYRIGHT.txt) |
| Protocol Buffers, as bundled with OpenCV's dnn module | BSD-3-Clause | [protobuf.txt](LICENSES/protobuf.txt) |
| Berkeley SoftFloat 3c, as bundled with OpenCV's core module | BSD-3-Clause | [Berkeley-SoftFloat.txt](LICENSES/Berkeley-SoftFloat.txt) |
| DLPack headers, as bundled with OpenCV's dnn module | Apache-2.0 | [Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| [Patchy](https://github.com/SethRobinson/Patchy) PSD primitives, binary I/O and Photoshop descriptors (`src/third_party/patchy_psd`), (c) 2026 Seth A. Robinson; its documented Photoshop write rules also inform `src/core/src/psd_writer.cpp` | MIT | [Patchy-MIT.txt](LICENSES/Patchy-MIT.txt) |
| [Little CMS](https://www.littlecms.com) 2.17, core (`src/third_party/lcms2`), © Marti Maria Saguer: CMYK files through their own colour profiles | MIT | [LittleCMS-MIT.txt](LICENSES/LittleCMS-MIT.txt) |
| [nlohmann/json](https://github.com/nlohmann/json) (`src/third_party/nlohmann`) | MIT | [nlohmann-json.txt](LICENSES/nlohmann-json.txt) |
| [Lucide](https://lucide.dev) icons (`src/app/icons`) | ISC | [Lucide-ISC.txt](LICENSES/Lucide-ISC.txt) |
| [mypaint-brushes](https://github.com/mypaint/mypaint-brushes) 2.0.2 (`src/app/brushes/mypaint`): 196 presets by Martin Renold and the MyPaint team, David Revoy, Ramón Miranda, Marcelo "Tanda" Cerviño, Guillaume Loussarévian and Brien Dieterle | CC0-1.0 (public domain) | [CC0-1.0.txt](LICENSES/CC0-1.0.txt); authors per set in `src/app/brushes/mypaint/Licenses.dep5` |

## Shipped as shared libraries

| Component | Licence | Text |
|---|---|---|
| [Qt](https://www.qt.io) 6: Core, Gui, Widgets, Network, Svg, DBus, Wayland and the image format plugins, bundled in the AppImage and the macOS app | LGPL-3.0 (used under its terms; the libraries are unmodified and can be replaced) | [LGPL-3.0.txt](LICENSES/LGPL-3.0.txt) with [LICENSE](LICENSE); Qt's own third-party components are listed at <https://doc.qt.io/qt-6/licenses-used-in-qt.html> |
| [libpng](http://www.libpng.org) | libpng / PNG Reference Library License v2 | [libpng.txt](LICENSES/libpng.txt) |
| [zlib](https://zlib.net) | zlib | [zlib.txt](LICENSES/zlib.txt) |
| [libmypaint](https://github.com/mypaint/libmypaint) 1.6, the MyPaint brush engine | ISC | [libmypaint-ISC.txt](LICENSES/libmypaint-ISC.txt) |
| [json-c](https://github.com/json-c/json-c), which libmypaint uses to read presets | MIT | [json-c-MIT.txt](LICENSES/json-c-MIT.txt) |
| [SQLite](https://sqlite.org), which reads Clip Studio brushes | Public domain | none required |

The AppImage also carries system libraries that Qt depends on (fonts, text
shaping, input, graphics). Their copyright files are collected at build time
into `share/doc/nekophoto/bundled/` inside the AppImage and the tarball.

The source for the GPL program is this repository at the tag of each release.
Qt, libpng, zlib, libmypaint and json-c are unmodified upstream releases; OpenCV is built from the
unmodified 4.14.0 source archive by `tools/build-opencv.sh`.

## Formats and references

The brush importers are written from public format descriptions: PKWARE's ZIP
APPNOTE, Apple's binary property list format (CoreFoundation, APSL), the
Photoshop brush format as documented by the Archive Team file format wiki and
read by GIMP and Krita, and, for Procreate's setting names and ranges, the
schema of [procreate-brush-decoder](https://github.com/aumlette-lab/procreate-brush-decoder)
by aumlette-lab (MIT licence). No code was taken from these. A
CC BY-NC brush converter was looked at only for the facts its README states;
none of its code is used.

## Used at run time, not distributed

| Component | Licence | Notes |
|---|---|---|
| [G'MIC](https://gmic.eu) | CeCILL v2.1 (GPL-compatible) or CeCILL-C | Filter > G'MIC runs the `gmic` program if it is installed. Builds configured with `-DCOMPOSITOR_WITH_LIBGMIC=ON` link `libgmic` from the system instead; release builds do not |
| G'MIC filter catalogue (`update<version>.gmic`) | CeCILL v2.1 | Downloaded by Update Filters from gmic.eu |
| IS-Net general use (DIS), via [rembg](https://github.com/danielgatis/rembg) | Apache-2.0 | Downloaded on request from Preferences (AI background removal) |
| U²-Net human segmentation and U²-Netp, via rembg | Apache-2.0 | Downloaded on request |
| PP-HumanSeg (PaddleSeg), via [OpenCV Zoo](https://github.com/opencv/opencv_zoo) | Apache-2.0 | Downloaded on request |
| EfficientSAM-Ti, via OpenCV Zoo | Apache-2.0 | Downloaded on request for Quick Select's Click engine |
