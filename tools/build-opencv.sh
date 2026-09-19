#!/usr/bin/env bash
# Builds the OpenCV the app ships with: the version pinned below, only the three modules Remove Background
# needs (core, imgproc, dnn), static, with every optional dependency off. The binary then carries one known
# OpenCV on every platform instead of whatever the distribution or Homebrew has, so a model that runs here
# runs in the AppImage and the Mac bundle too. CI and the release jobs cache the result keyed on this file;
# locally the system OpenCV is the default and this is opt-in.
#
# Usage: tools/build-opencv.sh <prefix> [jobs]
# then   cmake ... -DOpenCV_DIR=<prefix>/lib/cmake/opencv4
set -euo pipefail

version=4.14.0
sha256=ee8fb9b30eb60850431b4656447080e3737b56e45719c92b67f245950609f86e
url="https://github.com/opencv/opencv/archive/refs/tags/$version.tar.gz"

[ $# -ge 1 ] || { echo "usage: $0 <prefix> [jobs]" >&2; exit 2; }
mkdir -p "$1"
prefix="$(cd "$1" && pwd)"
jobs="${2:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"
checksum() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -c1-64; else shasum -a 256 "$1" | cut -c1-64; fi; }
stamp="$prefix/.compositor-opencv"
signature="$version static core,imgproc,dnn $(checksum "$0" | cut -c1-16)"
if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$signature" ] && [ -f "$prefix/lib/cmake/opencv4/OpenCVConfig.cmake" ]; then
    echo "OpenCV $version is already built in $prefix"
    exit 0
fi

work="$prefix/.build"
mkdir -p "$work"
archive="$work/opencv-$version.tar.gz"
if [ ! -f "$archive" ] || [ "$(checksum "$archive")" != "$sha256" ]; then
    echo "Downloading OpenCV $version"
    curl -sSL --retry 3 -o "$archive" "$url"
fi
[ "$(checksum "$archive")" = "$sha256" ] || { echo "OpenCV archive checksum mismatch" >&2; exit 1; }
rm -rf "$work/opencv-$version" "$work/build"
tar xzf "$archive" -C "$work"

generator=()
command -v ninja >/dev/null && generator=(-G Ninja)
echo "Configuring OpenCV $version (core, imgproc, dnn; static)"
cmake -S "$work/opencv-$version" -B "$work/build" ${generator[@]+"${generator[@]}"} \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_LIST=core,imgproc,dnn \
    -DOPENCV_GENERATE_PKGCONFIG=OFF \
    -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCS=OFF -DBUILD_opencv_apps=OFF \
    -DBUILD_JAVA=OFF -DBUILD_opencv_python2=OFF -DBUILD_opencv_python3=OFF \
    -DWITH_IPP=OFF -DWITH_ITT=OFF -DWITH_OPENCL=OFF -DWITH_CUDA=OFF -DWITH_VULKAN=OFF -DWITH_OPENVINO=OFF \
    -DWITH_FFMPEG=OFF -DWITH_GSTREAMER=OFF -DWITH_V4L=OFF -DWITH_1394=OFF -DWITH_GTK=OFF -DWITH_QT=OFF \
    -DWITH_AVFOUNDATION=OFF -DWITH_COCOA=OFF -DWITH_OPENGL=OFF \
    -DWITH_PNG=OFF -DWITH_JPEG=OFF -DWITH_TIFF=OFF -DWITH_WEBP=OFF -DWITH_OPENJPEG=OFF -DWITH_JASPER=OFF \
    -DWITH_OPENEXR=OFF -DWITH_IMGCODEC_HDR=OFF -DWITH_IMGCODEC_SUNRASTER=OFF -DWITH_IMGCODEC_PXM=OFF -DWITH_IMGCODEC_PFM=OFF \
    -DWITH_EIGEN=OFF -DWITH_LAPACK=OFF -DWITH_TBB=OFF -DWITH_OPENMP=OFF -DWITH_PTHREADS_PF=ON \
    -DWITH_ADE=OFF -DBUILD_opencv_gapi=OFF -DWITH_OBSENSOR=OFF \
    -DWITH_PROTOBUF=ON -DBUILD_PROTOBUF=ON -DPROTOBUF_UPDATE_FILES=OFF -DWITH_FLATBUFFERS=OFF \
    -DBUILD_ZLIB=OFF \
    -DOPENCV_DNN_OPENCL=OFF -DOPENCV_DNN_CUDA=OFF \
    -DCV_TRACE=OFF -DENABLE_PIC=ON \
    >"$work/configure.log" 2>&1 || { tail -40 "$work/configure.log" >&2; exit 1; }
echo "Building OpenCV $version with $jobs jobs"
cmake --build "$work/build" -j"$jobs" >"$work/build.log" 2>&1 || { tail -40 "$work/build.log" >&2; exit 1; }
cmake --install "$work/build" >"$work/install.log" 2>&1
echo "$signature" >"$stamp"
rm -rf "$work"
echo "OpenCV $version installed in $prefix (cmake -DOpenCV_DIR=$prefix/lib/cmake/opencv4)"
