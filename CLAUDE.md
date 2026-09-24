# NekoPhoto

A Qt 6 / C++20 image editor: a Linux port of the macOS app Compositor. The Mac
sources under `Compositor/` and `Compositor.xcodeproj` are a reference and stay
untouched; the port lives in `src/`, `tests/`, `mcp/`, `tools/`, `docs/`.

## Build and test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # once
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/src/app/nekophoto
```

For background-removal work, `build/tests/matte_tool run <image.png> <model.onnx> <dir> --band 20`
dumps every stage (mask, trimap, chosen samples, band, composite) and `matte_tool eval <dir> <model>`
scores image/alpha pairs; the model lives under `~/.local/share/nekophoto/nekophoto/models`.
When comparing settings, use one document per headless process: `document.open` on an image with a
document already open imports it as a layer (File > Open's behaviour), so a second run would work on the
first document. The RPC socket path must stay under 107 bytes.

CI builds with `-DCOMPOSITOR_WARNINGS_AS_ERRORS=ON` on GCC and Clang and runs
`tools/rpc_smoke.py` against a headless instance; keep both green. CI's
toolchain is older than a rolling desktop's (Ubuntu 24.04: GCC 13, Clang 18,
Qt 6.4; the release image is 22.04 with GCC 11), so before pushing run
`tools/ci-in-docker.sh ubuntu:24.04 gcc` (and `clang`), and
`RELEASE=1 APPIMAGE=1 tools/ci-in-docker.sh ubuntu:22.04` before tagging a release (it also
packages the AppImage with the pinned linuxdeploy in `tools/package-appimage.sh` and starts it).
OpenCV is vendored: CI, the release jobs, the Mac bundle and the Docker check
build `tools/build-opencv.sh` (4.14, static, core/imgproc/dnn only, cached) and
configure with `-DOpenCV_DIR=<prefix>/lib/cmake/opencv4`; a local build takes
the system OpenCV unless you pass that too, so a model verified here may
behave differently in CI only if the versions differ.

## Driving the app

- `./build/src/app/nekophoto --headless --rpc-socket /tmp/c.sock --demo &`
  then `--call <method> [--params '{...}'] --rpc-socket /tmp/c.sock`, or the
  MCP server in `.mcp.json` (tools mirror the methods in `docs/automation.md`).
- `--headless --batch script.jsonl` runs requests without a socket.
- Brush latency: `QT_QPA_PLATFORM=offscreen ./build/src/app/nekophoto --bench-brush classic/dry_brush --bench-size 1086x1448`
  (add `QT_SCALE_FACTOR=2.25` for the user's HiDPI panel); see `docs/brush-latency.md`.
- Screenshots for checking UI work: `QT_QPA_PLATFORM=offscreen ./build/src/app/nekophoto --demo --tool brush --dialog levels --screenshot out.png`
  (`COMPOSITOR_WINDOW_SIZE=1000x700` forces the size; `QT_SCALE_FACTOR=2` a scale).

## Conventions

- Core (`src/core`) is Qt-free C++; the app (`src/app`) is Qt Widgets. Pixel
  buffers are premultiplied RGBA8, top-down, explicit stride; masks are 8-bit.
- Every user-visible edit is one undo step: wrap changes in
  `beginEdit(name)` / `endEdit()` on `EditorSession`.
- Behaviour follows the Mac app; when in doubt read the Swift source it ports
  (`docs/linux-port-architecture.md` maps files).
- An automation method is one `add("name", handler)` in the
  `src/app/Automation*.cpp` file for its area (document, layers, pixels,
  selection, paint; app, tabs, history and view in `Automation.cpp`); add a matching tool in `mcp/nekophoto_mcp.py`,
  a line in `docs/automation.md`, and cover it in `tools/rpc_smoke.py`.
