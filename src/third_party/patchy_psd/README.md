# Patchy PSD primitives

Vendored unmodified from Patchy (https://github.com/SethRobinson/Patchy, MIT, © 2026 Seth A. Robinson),
commit 7d14d1f6ede2dc8fb52c11eefcc7cc8783473711:

- `psd/psd_binary.{hpp,cpp}`: big-endian reader/writer, the PSD/PSB header.
- `psd/psd_descriptor.{hpp,cpp}`: Photoshop ActionDescriptors (read and byte-exact write), PackBits.
- `support/translate_noop.hpp`: the one macro they use.

The licence is in `LICENSE`. Keep these files as Patchy has them so updates stay a copy; NekoPhoto's
adapters live in `src/core`. Built as the `patchy_psd` static library (src/core/CMakeLists.txt).

Ported rather than vendored (NekoPhoto's own code, under the same MIT terms, citing this file):

- `src/core/src/affinity.cpp` and `affinity_tree.{h,cpp}`: the Affinity document reader, from Patchy's
  `src/formats/af_document_io.cpp` and `af_tree.{hpp,cpp}` at the commit above (the container and stream table,
  the doc.dat tree, the tile planes, the blend-mode table, placement, masks, artboards, the Erase fold and the
  parametric shapes). See `docs/affinity-import.md`.
