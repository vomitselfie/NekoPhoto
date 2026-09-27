#!/usr/bin/env bash
# Packages a Windows build as a portable zip: nekophoto.exe, Qt's plugins (windeployqt when it is there, else the
# few plugins the app needs, copied by hand), every DLL they load from the toolchain's bin folder (found with
# objdump, so the same script works in MSYS2 on Windows and in the MinGW cross container), and the licences.
#
# Usage: tools/package-windows.sh <build dir> <version> [dll dir] [qt plugin dir]
#   dll dir        where the MinGW, Qt and library DLLs live (default: the folder of the compiler on PATH)
#   qt plugin dir  Qt's plugins folder, for the manual copy when windeployqt is missing
# Makes NekoPhoto-<version>-windows-x86_64.zip (and the folder of the same name) in the current directory.
set -euo pipefail
[ $# -ge 2 ] || { echo "usage: $0 <build dir> <version> [dll dir] [qt plugin dir]" >&2; exit 2; }
build="$(cd "$1" && pwd)"
version="$2"
root="$(cd "$(dirname "$0")/.." && pwd)"
dlldir="${3:-$(dirname "$(command -v gcc || command -v x86_64-w64-mingw32-gcc)")}"
plugindir="${4:-}"
objdump="$(command -v objdump || true)"
command -v x86_64-w64-mingw32-objdump >/dev/null && objdump="$(command -v x86_64-w64-mingw32-objdump)"
[ -n "$objdump" ] || { echo "objdump not found" >&2; exit 1; }

name="NekoPhoto-$version-windows-x86_64"
stage="$PWD/$name"
rm -rf "$stage" "$name.zip"
cmake --install "$build" --prefix "$stage" >/dev/null
[ -f "$stage/nekophoto.exe" ] || { echo "no nekophoto.exe after installing $build" >&2; exit 1; }
cp "$root/README.md" "$stage/"

deploy=""
for candidate in windeployqt6 windeployqt windeployqt-qt6; do
    command -v "$candidate" >/dev/null && { deploy="$candidate"; break; }
done
if [ -n "$deploy" ]; then
    "$deploy" --release --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime "$stage/nekophoto.exe"
    # --headless, --call and --batch run on the offscreen platform, which windeployqt leaves out.
    [ -n "$plugindir" ] || plugindir="$(qtpaths6 --query QT_INSTALL_PLUGINS 2>/dev/null || qmake6 -query QT_INSTALL_PLUGINS)"
    mkdir -p "$stage/platforms" && cp "$plugindir/platforms/qoffscreen.dll" "$stage/platforms/"
else
    [ -n "$plugindir" ] || { echo "no windeployqt: pass Qt's plugin folder as the fourth argument" >&2; exit 1; }
    echo "windeployqt not found: copying Qt plugins from $plugindir"
    for plugin in platforms/qwindows.dll platforms/qoffscreen.dll styles/qmodernwindowsstyle.dll styles/qwindowsvistastyle.dll \
                  iconengines/qsvgicon.dll imageformats/qsvg.dll imageformats/qjpeg.dll imageformats/qgif.dll imageformats/qico.dll \
                  imageformats/qwebp.dll imageformats/qtiff.dll imageformats/qicns.dll imageformats/qtga.dll imageformats/qwbmp.dll \
                  tls/qschannelbackend.dll tls/qopensslbackend.dll networkinformation/qnetworklistmanager.dll; do
        if [ -f "$plugindir/$plugin" ]; then mkdir -p "$stage/$(dirname "$plugin")"; cp "$plugindir/$plugin" "$stage/$plugin"; fi
    done
fi

# Every DLL any shipped binary imports, transitively, when the toolchain has it (system DLLs are not there).
declare -A seen=()
while true; do
    added=0
    while IFS= read -r binary; do
        while IFS= read -r dll; do
            key="${dll,,}"
            [ -n "${seen[$key]:-}" ] && continue
            seen[$key]=1
            source="$(find "$dlldir" -maxdepth 1 -iname "$dll" -print -quit)"
            if [ -n "$source" ] && [ ! -f "$stage/$(basename "$source")" ]; then cp "$source" "$stage/"; added=1; fi
        done < <("$objdump" -p "$binary" | sed -n 's/^\s*DLL Name: //p')
    done < <(find "$stage" -iname '*.exe' -o -iname '*.dll')
    [ "$added" = 0 ] && break
done

# In MSYS2, the licence folder of the package each bundled DLL came from, as the AppImage carries the Debian
# copyright files of its libraries (THIRD-PARTY-NOTICES.md).
if command -v pacman >/dev/null; then
    licenses="$(cygpath -u "$(dirname "$dlldir")" 2>/dev/null || dirname "$dlldir")/share/licenses"
    for dll in "$stage"/*.dll "$stage"/*/*.dll; do
        owner="$(pacman -Qqo "$dlldir/$(basename "$dll")" 2>/dev/null || pacman -Qqo "$dll" 2>/dev/null || true)"
        [ -n "$owner" ] || continue
        package="${owner#mingw-w64-ucrt-x86_64-}"
        if [ -d "$licenses/$package" ] && [ ! -d "$stage/LICENSES/bundled/$package" ]; then
            mkdir -p "$stage/LICENSES/bundled" && cp -r "$licenses/$package" "$stage/LICENSES/bundled/"
        fi
    done
fi

(cd "$PWD" && if command -v zip >/dev/null; then zip -qr9 "$name.zip" "$name"; else 7z a -tzip -mx=9 "$name.zip" "$name" >/dev/null; fi)
echo "$name.zip: $(find "$stage" -type f | wc -l) files, $(du -sh "$name.zip" | cut -f1)"
