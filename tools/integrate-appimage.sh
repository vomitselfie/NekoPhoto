#!/usr/bin/env bash
# Puts a compositor-linux AppImage into the desktop like an installed app: a launcher entry,
# the icon, and the .comp file type, all under your home directory (no root needed).
#
#   tools/integrate-appimage.sh ~/Downloads/compositor-linux-0.1.0-x86_64.AppImage
#   tools/integrate-appimage.sh --remove
set -euo pipefail

data="${XDG_DATA_HOME:-$HOME/.local/share}"
dest="$HOME/Applications"
target="$dest/compositor-linux.AppImage"
entry="$data/applications/compositor-linux.desktop"
icon="$data/icons/hicolor/scalable/apps/compositor-linux.svg"
mime="$data/mime/packages/compositor-linux.xml"

refresh() {
    command -v update-desktop-database >/dev/null && update-desktop-database "$data/applications" 2>/dev/null || true
    command -v update-mime-database >/dev/null && update-mime-database "$data/mime" 2>/dev/null || true
}

if [ "${1:-}" = "--remove" ]; then
    rm -f "$target" "$entry" "$icon" "$mime"
    refresh
    echo "Removed compositor-linux from $dest and the launcher."
    exit 0
fi

src="${1:-}"
if [ ! -f "$src" ]; then
    echo "usage: $0 compositor-linux-<version>-x86_64.AppImage   (or --remove)" >&2
    exit 2
fi

mkdir -p "$dest" "$data/applications" "$data/icons/hicolor/scalable/apps" "$data/mime/packages"
if [ "$(realpath "$src")" != "$(realpath -m "$target")" ]; then
    command cp -f "$src" "$target"
fi
chmod +x "$target"

# The icon and the MIME description come out of the AppImage itself (a full extract: the runtime's
# pattern extraction is unreliable, and this takes a few seconds once).
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
( cd "$tmp" && "$target" --appimage-extract >/dev/null 2>&1 ) || { echo "couldn't read the AppImage (try: $target --appimage-extract)" >&2; exit 1; }
[ -f "$tmp/squashfs-root/compositor-linux.svg" ] || { echo "no icon inside the AppImage; is this a compositor-linux AppImage?" >&2; exit 1; }
command cp -f "$tmp/squashfs-root/compositor-linux.svg" "$icon"
[ -f "$tmp/squashfs-root/usr/share/mime/packages/compositor-linux.xml" ] && command cp -f "$tmp/squashfs-root/usr/share/mime/packages/compositor-linux.xml" "$mime"

version="$(basename "$src" | sed -n 's/^compositor-linux-\(.*\)-x86_64\.AppImage$/\1/p')"
cat > "$entry" <<EOF
[Desktop Entry]
Type=Application
Name=compositor-linux
GenericName=Image Compositor
Comment=A small, focused, layer-based image compositor
Exec=$target %F
Icon=compositor-linux
Terminal=false
Categories=Graphics;RasterGraphics;2DGraphics;
MimeType=image/png;image/jpeg;image/tiff;image/webp;application/x-compositor-project;
Keywords=image;layers;compositing;photo;editor;
StartupWMClass=compositor-linux
X-AppImage-Version=${version:-unknown}
EOF

refresh
command -v xdg-mime >/dev/null && xdg-mime default compositor-linux.desktop application/x-compositor-project 2>/dev/null || true

echo "Installed $target"
echo "It is in your app launcher as 'compositor-linux' and opens .comp projects. Run again with a newer AppImage to update, or with --remove."
