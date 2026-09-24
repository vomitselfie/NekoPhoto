#!/usr/bin/env bash
# Puts a NekoPhoto AppImage into the desktop like an installed app: a launcher entry,
# the icon, and the .comp and .clip file types, all under your home directory (no root needed). A launcher
# left by compositor-linux (its name before 1.0) is removed.
#
#   tools/integrate-appimage.sh ~/Downloads/NekoPhoto-0.1.0-x86_64.AppImage
#   tools/integrate-appimage.sh --remove
set -euo pipefail

data="${XDG_DATA_HOME:-$HOME/.local/share}"
dest="$HOME/Applications"
target="$dest/nekophoto.AppImage"
entry="$data/applications/nekophoto.desktop"
icon="$data/icons/hicolor/scalable/apps/nekophoto.svg"
mime="$data/mime/packages/nekophoto.xml"

# What compositor-linux (the name until 0.9.1) installed, replaced by NekoPhoto's.
old_files=("$dest/compositor-linux.AppImage" "$data/applications/compositor-linux.desktop" "$data/icons/hicolor/scalable/apps/compositor-linux.svg" "$data/mime/packages/compositor-linux.xml")

refresh() {
    command -v update-desktop-database >/dev/null && update-desktop-database "$data/applications" 2>/dev/null || true
    command -v update-mime-database >/dev/null && update-mime-database "$data/mime" 2>/dev/null || true
}

if [ "${1:-}" = "--remove" ]; then
    rm -f "$target" "$entry" "$icon" "$mime" "${old_files[@]}"
    refresh
    echo "Removed NekoPhoto from $dest and the launcher."
    exit 0
fi

src="${1:-}"
if [ ! -f "$src" ]; then
    echo "usage: $0 NekoPhoto-<version>-x86_64.AppImage   (or --remove)" >&2
    exit 2
fi

rm -f "${old_files[@]}"
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
[ -f "$tmp/squashfs-root/nekophoto.svg" ] || { echo "no icon inside the AppImage; is this a NekoPhoto AppImage?" >&2; exit 1; }
command cp -f "$tmp/squashfs-root/nekophoto.svg" "$icon"
[ -f "$tmp/squashfs-root/usr/share/mime/packages/nekophoto.xml" ] && command cp -f "$tmp/squashfs-root/usr/share/mime/packages/nekophoto.xml" "$mime"

version="$(basename "$src" | sed -n 's/^NekoPhoto-\(.*\)-x86_64\.AppImage$/\1/p')"
cat > "$entry" <<EOF
[Desktop Entry]
Type=Application
Name=NekoPhoto
GenericName=Photo Editor
Comment=Edit photos and paint in layers
Exec=$target %F
Icon=nekophoto
Terminal=false
Categories=Graphics;RasterGraphics;2DGraphics;
MimeType=image/png;image/jpeg;image/tiff;image/webp;image/vnd.adobe.photoshop;application/x-compositor-project;application/x-clip-studio-project;
Keywords=image;layers;compositing;photo;editor;paint;brush;psd;clip;
StartupWMClass=nekophoto
X-AppImage-Version=${version:-unknown}
EOF

refresh
command -v xdg-mime >/dev/null && xdg-mime default nekophoto.desktop application/x-compositor-project application/x-clip-studio-project 2>/dev/null || true

echo "Installed $target"
echo "It is in your app launcher as 'NekoPhoto' and opens .comp and .clip projects. Run again with a newer AppImage to update, or with --remove."
