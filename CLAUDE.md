# compositor-linux

A Qt 6 / C++20 image editor: a Linux port of the macOS app Compositor. The Mac
sources under `Compositor/` and `Compositor.xcodeproj` are a reference and stay
untouched; the port lives in `src/`, `tests/`, `mcp/`, `tools/`, `docs/`.

## Build and test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # once
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/src/app/compositor-linux
```

CI builds with `-DCOMPOSITOR_WARNINGS_AS_ERRORS=ON` on GCC and Clang and runs
`tools/rpc_smoke.py` against a headless instance; keep both green.

## Driving the app

- `./build/src/app/compositor-linux --headless --rpc-socket /tmp/c.sock --demo &`
  then `--call <method> [--params '{...}'] --rpc-socket /tmp/c.sock`, or the
  MCP server in `.mcp.json` (tools mirror the methods in `docs/automation.md`).
- `--headless --batch script.jsonl` runs requests without a socket.
- Screenshots for checking UI work: `QT_QPA_PLATFORM=offscreen ./build/src/app/compositor-linux --demo --tool brush --dialog levels --screenshot out.png`
  (`COMPOSITOR_WINDOW_SIZE=1000x700` forces the size; `QT_SCALE_FACTOR=2` a scale).

## Conventions

- Core (`src/core`) is Qt-free C++; the app (`src/app`) is Qt Widgets. Pixel
  buffers are premultiplied RGBA8, top-down, explicit stride; masks are 8-bit.
- Every user-visible edit is one undo step: wrap changes in
  `beginEdit(name)` / `endEdit()` on `EditorSession`.
- Behaviour follows the Mac app; when in doubt read the Swift source it ports
  (`docs/linux-port-architecture.md` maps files).
- An automation method is one `add("name", handler)` in
  `src/app/Automation.cpp`; add a matching tool in `mcp/compositor_mcp.py`,
  a line in `docs/automation.md`, and cover it in `tools/rpc_smoke.py`.
