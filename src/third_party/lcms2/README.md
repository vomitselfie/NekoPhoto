# Little CMS 2.17 (core)

The core C sources of Little CMS 2.17 (https://www.littlecms.com, MIT, © Marti Maria Saguer; `LICENSE`), as
Patchy vendors them (its `src/color/lcms2`, core only, without the fast_float plug-ins). NekoPhoto uses it to
convert CMYK PSDs to sRGB through the file's own colour profile (`src/core/src/colour.cpp`). Built as the
`nekophoto_lcms2` static library (src/core/CMakeLists.txt).
