#!/usr/bin/env bash
# Packages a release build as an AppImage with linuxdeploy and its Qt plugin: the release workflow runs it,
# and so does `RELEASE=1 APPIMAGE=1 tools/ci-in-docker.sh ubuntu:22.04` to check packaging before tagging.
# linuxdeploy is pinned to tagged builds and checked against their SHA-256, since the release job can
# publish; to move to newer ones, change the tags and the sums together.
# Usage: tools/package-appimage.sh <build dir> <version>   (writes NekoPhoto-<version>-x86_64.AppImage here)
set -euo pipefail
build="$1"
version="$2"
root="$(cd "$(dirname "$0")/.." && pwd)"

linuxdeploy_tag="1-alpha-20251107-1"
linuxdeploy_sum="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
plugin_qt_tag="1-alpha-20250213-1"
plugin_qt_sum="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"

fetch() {   # fetch <file> <url> <sha256>
    curl -sSL --fail -o "$1" "$2"
    echo "$3  $1" | sha256sum --check --quiet || { echo "checksum mismatch for $2" >&2; exit 1; }
    chmod +x "$1"
}
tools="$(mktemp -d)"
fetch "$tools/linuxdeploy" "https://github.com/linuxdeploy/linuxdeploy/releases/download/$linuxdeploy_tag/linuxdeploy-x86_64.AppImage" "$linuxdeploy_sum"
fetch "$tools/linuxdeploy-plugin-qt" "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/$plugin_qt_tag/linuxdeploy-plugin-qt-x86_64.AppImage" "$plugin_qt_sum"
export PATH="$tools:$PATH"

rm -rf AppDir
cmake --install "$build" --prefix AppDir/usr >/dev/null
export NO_STRIP=1   # linuxdeploy's bundled strip predates .relr.dyn sections
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
export EXTRA_QT_MODULES="svg;waylandcompositor"
# Qt 6.7 ships libqwayland-egl/-generic, newer Qt a single libqwayland; take whatever is there.
plugins="$("$QMAKE" -query QT_INSTALL_PLUGINS)/platforms"
# offscreen too, so the packaged app can be smoke-tested headless.
export EXTRA_PLATFORM_PLUGINS="$(ls "$plugins" | grep -E '^libqwayland|^libqoffscreen' | paste -sd ';')"
echo "Wayland plugins: $EXTRA_PLATFORM_PLUGINS"
export LDAI_OUTPUT="NekoPhoto-$version-x86_64.AppImage"
if [ -n "${GITHUB_REPOSITORY:-}" ]; then
    export LDAI_UPDATE_INFORMATION="gh-releases-zsync|${GITHUB_REPOSITORY_OWNER}|${GITHUB_REPOSITORY#*/}|latest|NekoPhoto-*-x86_64.AppImage.zsync"
fi
linuxdeploy --appdir AppDir \
    --desktop-file AppDir/usr/share/applications/nekophoto.desktop \
    --icon-file "$root/packaging/nekophoto.svg" \
    --plugin qt
# Ship the copyright file of every distribution package whose library was bundled.
# Qt comes from aqtinstall, not dpkg; its licences are in LICENSES/ and THIRD-PARTY-NOTICES.md.
bundled=AppDir/usr/share/doc/nekophoto/bundled
mkdir -p "$bundled"
for lib in AppDir/usr/lib/*.so*; do
    pkg="$(dpkg -S "*/$(basename "$lib")" 2>/dev/null | head -n1 | cut -d: -f1 || true)"   # Qt's own libraries are no package's
    if [ -n "$pkg" ] && [ -f "/usr/share/doc/$pkg/copyright" ]; then
        cp "/usr/share/doc/$pkg/copyright" "$bundled/$pkg.copyright"
    fi
done
ls "$bundled"
linuxdeploy --appdir AppDir --output appimage
rm -rf "$tools"
ls -la ./*.AppImage*
