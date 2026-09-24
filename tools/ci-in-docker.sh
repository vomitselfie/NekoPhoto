#!/usr/bin/env bash
# Builds and tests the tree the way CI does, inside an Ubuntu container, so a toolchain older than the
# desktop's can be checked before pushing: Ubuntu 24.04 is the CI workflow (GCC 13, Clang 18, Qt 6.4),
# Ubuntu 22.04 the release image (GCC 11; its apt Qt is too old for the app, so that run builds the core
# and the tests only). The dependency image is built once and cached by Docker.
# With RELEASE=1 the 22.04 image gets Qt 6.7.3 through aqtinstall, as the release workflow does, so the
# whole application builds there too (about a gigabyte on the first run, cached afterwards).
# OpenCV is the vendored build of tools/build-opencv.sh, as in CI, kept in a Docker volume per image so it
# is built once (several minutes) and reused.
# With APPIMAGE=1 as well, the release build is then packaged by tools/package-appimage.sh (the pinned
# linuxdeploy) and the AppImage is started headless, as the release workflow does.
# Usage: [RELEASE=1 [APPIMAGE=1]] tools/ci-in-docker.sh [ubuntu:24.04|ubuntu:22.04] [gcc|clang]
set -euo pipefail
image="${1:-ubuntu:24.04}"
compiler="${2:-gcc}"
release="${RELEASE:-0}"
root="$(cd "$(dirname "$0")/.." && pwd)"
tag="compositor-ci:${image##*:}-v5"
volume="compositor-opencv-${image##*:}"
qtprefix=""
if [ "$release" = 1 ]; then
    tag="$tag-release"
    qtprefix="/opt/qt/6.7.3/gcc_64"
    docker build -q -t "$tag" - <<DOCKERFILE >/dev/null
FROM $image
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -q && apt-get install -y -q ninja-build cmake pkg-config libpng-dev libmypaint-dev libsqlite3-dev libgl1-mesa-dev g++ python3 python3-pip curl \\
    libxkbcommon-x11-0 libxcb-cursor0 libxcb-icccm4 libxcb-keysyms1 libxcb-shape0 libxcb-xkb1 libglib2.0-0 libfontconfig1 libdbus-1-3 \\
    && pip3 install -q aqtinstall && aqt install-qt -O /opt/qt linux desktop 6.7.3 linux_gcc_64 -m qtimageformats >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKERFILE
else
    docker build -q -t "$tag" - <<DOCKERFILE >/dev/null
FROM $image
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -q && apt-get install -y -q ninja-build cmake pkg-config libpng-dev libmypaint-dev libsqlite3-dev qt6-base-dev libgl1-mesa-dev clang g++ python3 curl \\
    && (apt-get install -y -q qt6-svg-dev || apt-get install -y -q libqt6svg6-dev) && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi
app=ON
[ "${image##*:}" = "22.04" ] && [ "$release" != 1 ] && app=OFF
docker volume create "$volume" >/dev/null
docker run --rm -v "$root:/src:ro" -v "$volume:/opt/opencv" -e "COMPILER=$compiler" -e "APP=$app" -e "QTPREFIX=$qtprefix" -e "APPIMAGE=${APPIMAGE:-0}" "$tag" bash -c '
set -e
/src/tools/build-opencv.sh /opt/opencv
if [ "$COMPILER" = clang ]; then export CC=clang CXX=clang++; else export CC=gcc CXX=g++; fi
$CXX --version | head -1
prefix=""; [ -n "$QTPREFIX" ] && prefix="-DCMAKE_PREFIX_PATH=$QTPREFIX"
cmake -S /src -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOMPOSITOR_WARNINGS_AS_ERRORS=ON -DCOMPOSITOR_BUILD_APP=$APP -DOpenCV_DIR=/opt/opencv/lib/cmake/opencv4 $prefix >/dev/null
cmake --build /tmp/build -j"$(nproc)"
ctest --test-dir /tmp/build --output-on-failure
if [ "$APP" = ON ]; then
  cd /tmp && QT_QPA_PLATFORM=offscreen /tmp/build/src/app/nekophoto --demo --screenshot demo.png --save-as Demo.comp
  /tmp/build/src/app/nekophoto --headless --rpc-socket /tmp/rpc.sock --demo & sleep 2; COMPOSITOR_BIN=/tmp/build/src/app/nekophoto python3 /src/tools/rpc_smoke.py /tmp/rpc.sock; kill %1
fi
if [ "$APPIMAGE" = 1 ] && [ -n "$QTPREFIX" ]; then
  (apt-get update -q && apt-get install -y -q file libwayland-cursor0 libwayland-egl1 libwayland-client0) >/dev/null   # present on the GitHub runner
  mkdir -p /tmp/pkg && cd /tmp/pkg
  export QMAKE="$QTPREFIX/bin/qmake" LD_LIBRARY_PATH="$QTPREFIX/lib" APPIMAGE_EXTRACT_AND_RUN=1
  /src/tools/package-appimage.sh /tmp/build 0.0.0-docker
  QT_QPA_PLATFORM=offscreen ./NekoPhoto-0.0.0-docker-x86_64.AppImage --version
  QT_QPA_PLATFORM=offscreen ./NekoPhoto-0.0.0-docker-x86_64.AppImage --demo --screenshot appimage-demo.png && ls -la appimage-demo.png
  ls AppDir/usr/lib | grep -E "mypaint|sqlite|json" || true
fi
echo "CI-IN-DOCKER OK ($COMPILER, $(grep -oP "VERSION_ID=\"\K[^\"]+" /etc/os-release), app=$APP)"
'
