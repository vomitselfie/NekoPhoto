# Patchy PSD primitives

Vendored unmodified from Patchy (https://github.com/SethRobinson/Patchy, MIT, © 2026 Seth A. Robinson),
commit 7d14d1f6ede2dc8fb52c11eefcc7cc8783473711:

- `psd/psd_binary.{hpp,cpp}`: big-endian reader/writer, the PSD/PSB header.
- `psd/psd_descriptor.{hpp,cpp}`: Photoshop ActionDescriptors (read and byte-exact write), PackBits.
- `support/translate_noop.hpp`: the one macro they use.

The licence is in `LICENSE`. Keep these files as Patchy has them so updates stay a copy; NekoPhoto's
adapters live in `src/core`. Built as the `patchy_psd` static library (src/core/CMakeLists.txt).

Ported (not vendored) from the same repository, adapted to NekoPhoto's core, each file saying so in its header:

- `src/formats/tga_document_io.cpp` -> `src/core/src/tga.cpp` (TGA reader and RLE writer).
- `src/formats/ico_document_io.cpp` -> `src/core/src/ico.cpp` (ICO/CUR reader, multi-size ICO writer).
- `src/formats/aseprite_document_io.cpp` -> `src/core/src/aseprite.cpp` (reader only; zlib instead of miniz).
- `src/formats/gif_document_io.cpp`'s LZW encoder -> `tests/formats_tests.cpp` (builds test GIFs; the GIF reader in
  `src/core/src/gif.cpp` is NekoPhoto's own, as Patchy only writes GIFs).
