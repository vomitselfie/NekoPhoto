#!/usr/bin/env bash
# Cross-builds the Windows x86_64 version inside a Fedora container (MinGW-w64, Fedora's mingw64-* Qt 6 and
# libraries; json-c, libmypaint and the vendored OpenCV of tools/build-opencv.sh are built from source into the
# image, which Docker caches), then runs the test executables and the offscreen smoke test under Wine, headless.
# This is for iterating locally; the release build is the MSYS2 (UCRT64) job in .github/workflows/windows.yml.
# The build directory lives in a Docker volume, so a second run only recompiles what changed.
#
# Usage: tools/windows-cross.sh [jobs]          (default 6)
# Environment: TESTS=0 skips the Wine runs; SHELL_ONLY=1 opens a shell in the container instead.
set -euo pipefail
jobs="${1:-6}"
root="$(cd "$(dirname "$0")/.." && pwd)"
tag="nekophoto-wincross:f42-v3"
volume="nekophoto-wincross-build"

docker build -q -t "$tag" -f - "$root/tools" <<'DOCKERFILE' >/dev/null
FROM fedora:42
RUN dnf install -y -q mingw64-gcc-c++ mingw64-qt6-qtbase mingw64-qt6-qtsvg mingw64-qt6-qtimageformats mingw64-qt6-qttools mingw64-qt6-qttranslations qt6-linguist qt6-qttools-devel \
        mingw64-libpng mingw64-zlib mingw64-zstd mingw64-sqlite mingw64-LibRaw mingw64-winpthreads \
        cmake ninja-build make pkgconf python3 curl xz tar zip file findutils which wine-core wine-filesystem \
    && dnf clean all
ENV MINGW=/usr/x86_64-w64-mingw32/sys-root/mingw
# json-c and libmypaint (the MyPaint brushes): not packaged for mingw64 in Fedora.
RUN mkdir /tmp/src && cd /tmp/src \
    && curl -sSL --retry 3 -o json-c.tar.gz https://s3.amazonaws.com/json-c_releases/releases/json-c-0.17.tar.gz \
    && tar xzf json-c.tar.gz && mingw64-cmake -S json-c-0.17 -B json-c-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
         -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF -DBUILD_TESTING=OFF -DDISABLE_WERROR=ON >/dev/null \
    && cmake --build json-c-build && cmake --install json-c-build >/dev/null \
    && curl -sSL --retry 3 -o libmypaint.tar.xz https://github.com/mypaint/libmypaint/releases/download/v1.6.1/libmypaint-1.6.1.tar.xz \
    && tar xJf libmypaint.tar.xz && cd libmypaint-1.6.1 \
    && mingw64-configure --disable-introspection --without-glib --disable-gegl --disable-i18n --disable-openmp --disable-static >/dev/null \
    && make -j6 >/dev/null && make install >/dev/null \
    && rm -rf /tmp/src
# The vendored OpenCV, cross-compiled (CMake reads the toolchain from the environment).
COPY build-opencv.sh /opt/build-opencv.sh
RUN CMAKE_TOOLCHAIN_FILE=/usr/share/mingw/toolchain-mingw64.cmake /opt/build-opencv.sh /opt/opencv 6
# A Wine prefix made once. Creating it leaves something waiting for a display, so the first program run in a fresh
# prefix takes 5 minutes to return; afterwards (the server killed) each run takes a moment. Paid here, once.
RUN WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" timeout 900 wine cmd /c exit >/dev/null 2>&1; wineserver -k; test -d /root/.wine
DOCKERFILE

docker volume create "$volume" >/dev/null
it=(); [ "${SHELL_ONLY:-0}" = 1 ] && it=(-it)
docker run --rm "${it[@]}" -v "$root:/src:ro" -v "$volume:/build" -e "JOBS=$jobs" -e "TESTS=${TESTS:-1}" -e "SHELL_ONLY=${SHELL_ONLY:-0}" \
    -e DISPLAY= -e WAYLAND_DISPLAY= -e LANG=C.UTF-8 "$tag" bash -c '
set -e
export WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" QT_QPA_PLATFORM=offscreen
unset DISPLAY WAYLAND_DISPLAY
mingw64-cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOMPOSITOR_WARNINGS_AS_ERRORS=ON \
    -DOpenCV_DIR=/opt/opencv/lib/cmake/opencv4 >/build/configure.log || { tail -40 /build/configure.log; exit 1; }
grep -E "^-- (Remove Background|Brushes|RAW|Brush import|Affinity|PDF|G.MIC|Appearance|Translations)" /build/configure.log || true
[ "$SHELL_ONLY" = 1 ] && exec bash
cmake --build /build -j"$JOBS"
[ "$TESTS" = 1 ] || exit 0
# Wine finds the MinGW DLLs (Qt, libpng, libmypaint, ...) through WINEPATH and Qt its plugins through QT_PLUGIN_PATH.
export WINEPATH="Z:$MINGW/bin" QT_PLUGIN_PATH="Z:$MINGW/lib/qt6/plugins"
wineserver -p0
cd /build/tests
failed=()
for test in $(ctest --test-dir /build -N | sed -n "s/^ *Test *#[0-9]*: //p"); do
    exe="$test.exe"; [ -f "$exe" ] || { echo "SKIP $test (no $exe)"; continue; }
    if timeout 600 wine "$exe" >"/build/$test.log" 2>&1; then echo "PASS $test"
    else echo "FAIL $test"; tail -15 "/build/$test.log"; failed+=("$test"); fi
done
cd /tmp
app=/build/src/app/nekophoto.exe
timeout 120 wine "$app" --version | grep -qi "nekophoto" || failed+=("--version")
timeout 300 wine "$app" --demo --screenshot demo.png --save-as Demo.comp && [ -s demo.png ] && [ -f Demo.comp/manifest.json ] || failed+=("demo screenshot")
timeout 300 wine "$app" Demo.comp --screenshot reopened.png && [ -s reopened.png ] || failed+=("reopen screenshot")
# A file name outside ASCII (the manifest'"'"'s UTF-8 code page): save the project there and open it again.
timeout 300 wine "$app" Demo.comp --screenshot "ねこ写真.png" --save-as "ねこ写真.comp" && [ -s "ねこ写真.png" ] && [ -f "ねこ写真.comp/manifest.json" ] || failed+=("non-ASCII path")
printf "{\"method\":\"document.open\",\"params\":{\"path\":\"Z:/tmp/ねこ写真.comp\"}}\n{\"method\":\"document.info\"}\n" > open.jsonl
timeout 300 wine "$app" --headless --batch open.jsonl | grep -q "\"width\":640" || failed+=("non-ASCII open")
ls -la /tmp/*.png
# The automation socket (a named pipe on Windows) through Wine: start headless, then the --call client.
timeout 300 wine "$app" --headless --rpc-socket nekophoto-cross-test --demo >/build/rpc-server.log 2>&1 &
server=$!
sleep 8
timeout 60 wine "$app" --call app.info --rpc-socket nekophoto-cross-test || failed+=("rpc --call")
kill $server 2>/dev/null || true; wineserver -k 2>/dev/null || true
if [ ${#failed[@]} -gt 0 ]; then echo "WINDOWS-CROSS FAILED: ${failed[*]}"; exit 1; fi
echo "WINDOWS-CROSS OK"
'
