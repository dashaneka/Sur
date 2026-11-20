#!/usr/bin/env bash
# Installs Sur for the current user (no sudo).
#   ./install.sh            install or update
#   ./install.sh --default  also make Sur the default music player
set -euo pipefail
cd "$(dirname "$0")"

APP_ID=io.github.rajnish.Sur
PREFIX="${PREFIX:-$HOME/.local}"
APPS="$PREFIX/share/applications"
ICONS="$PREFIX/share/icons/hicolor"

make clean && make all

install -Dm755 sur "$PREFIX/bin/sur"
install -Dm644 "data/$APP_ID.svg" "$ICONS/scalable/apps/$APP_ID.svg"
mkdir -p "$APPS"
# Absolute path: the desktop session's PATH may not include ~/.local/bin.
sed "s|^Exec=sur|Exec=$PREFIX/bin/sur|" "data/$APP_ID.desktop" > "$APPS/$APP_ID.desktop"
chmod 644 "$APPS/$APP_ID.desktop"

update-desktop-database "$APPS" 2>/dev/null || true
gtk-update-icon-cache -q -t -f "$ICONS" 2>/dev/null || true

if [[ "${1:-}" == "--default" ]]; then
    mimes=$(sed -n 's/^MimeType=//p' "data/$APP_ID.desktop" | tr ';' ' ')
    # shellcheck disable=SC2086
    xdg-mime default "$APP_ID.desktop" $mimes
    echo "Sur is now the default music player."
fi

echo "Installed: $PREFIX/bin/sur"
