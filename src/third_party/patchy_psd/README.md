# Patchy PSD primitives

Vendored unmodified from Patchy (https://github.com/SethRobinson/Patchy, MIT, © 2026 Seth A. Robinson),
commit 7d14d1f6ede2dc8fb52c11eefcc7cc8783473711:

- `psd/psd_binary.{hpp,cpp}`: big-endian reader/writer, the PSD/PSB header.
- `psd/psd_descriptor.{hpp,cpp}`: Photoshop ActionDescriptors (read and byte-exact write), PackBits.
- `support/translate_noop.hpp`: the one macro they use.

The licence is in `LICENSE`. Keep these files as Patchy has them so updates stay a copy; NekoPhoto's
adapters live in `src/core`. Built as the `patchy_psd` static library (src/core/CMakeLists.txt).
