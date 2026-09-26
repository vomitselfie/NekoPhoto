# Fuzzing the file parsers

Imported files are hostile input: a PSD can nest smart objects, claim huge sizes, and hold hand-written descriptors
and caches. Two libFuzzer targets in `tests/fuzz/` run the readers under AddressSanitizer and
UndefinedBehaviorSanitizer:

- `fuzz_psd` feeds whole files to `importPsdBytes` (layers, masks, text, styles, smart objects decoded and nested,
  warps, Smart Filters drawn).
- `fuzz_psd_parts` feeds one block parser at a time, the first byte choosing which: vector masks, type ('TySh'),
  placements ('SoLd', 'PlLd') and their patch and warp writers, Smart Filter stacks, the 'FEid' cache, linked-file
  blocks, mask parameters, fill gradients and patterns, the project's smart object and carry sidecars. It reaches
  their depths without first building a valid PSD around them.

A separate, instrumented build (the targets need Clang):

```bash
cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCOMPOSITOR_BUILD_APP=OFF -DCOMPOSITOR_BUILD_TESTS=OFF -DCOMPOSITOR_BUILD_FUZZERS=ON \
  -DCOMPOSITOR_WITH_OPENCV=OFF -DCOMPOSITOR_WITH_MYPAINT=OFF -DCOMPOSITOR_WITH_SQLITE=OFF \
  "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined,fuzzer-no-link -fno-omit-frame-pointer" \
  "-DCMAKE_C_FLAGS=-fsanitize=address,undefined,fuzzer-no-link" "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"
cmake --build build-fuzz --target fuzz_psd fuzz_psd_parts
```

Seed `fuzz_psd` with small real files (Patchy's `test-fixtures/psd`) and `fuzz_psd_parts` with blocks cut from them,
each prefixed with its selector byte (0 vmsk, 1 TySh, 2 SoLd, 3 PlLd, 4 SoLd as a filter stack, 5 FEid, 6 lnk2,
8 GdFl / PtFl, 11 SoLd through the edit writers). Run with a memory ceiling so a size bomb shows as a finding:

```bash
ASAN_OPTIONS=alloc_dealloc_mismatch=0:detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  build-fuzz/tests/fuzz/fuzz_psd -max_total_time=600 -malloc_limit_mb=2048 -rss_limit_mb=6144 -timeout=10 \
  -jobs=4 -workers=4 corpus_psd
```

`alloc_dealloc_mismatch=0` because GCC 16's libstdc++ `std::stable_sort` buffer trips it under Clang (not ours);
`detect_leaks=0` where LeakSanitizer cannot run (under ptrace, in a sandbox). Strip `DISPLAY` / `WAYLAND_DISPLAY`
for any long run.

## Found and fixed (September 2026)

- The 'FEid' cache reader computed rectangle sizes in `int`: hostile edges overflowed (now 64-bit, capped at
  Photoshop's 300,000).
- The merged image reserved memory by row counts before checking they lay in the file: 25 KB files asked for 7 to
  29 GB (the counts, then their total, are checked first).
- A cache canvas past what a buffer can hold left the Smart Filter stack writing into an empty image (such canvases
  are not drawn; the helpers refuse empty buffers).
- Median, Dust & Scratches and Unsharp Mask widened their input to the canvas without keeping the layer inside it
  when the layer reached past the canvas (the widened area now always holds the layer).
- A degenerate placement quad drew an enormous raster that the filters then worked through (4.8 s for a 97 KB,
  40 x 40 file): placements are drawn clipped to the filter canvas and never past the document's pixel budget.
