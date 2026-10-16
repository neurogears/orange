#!/usr/bin/env bash
# Install a CMake build (release/orange, from ./build.sh) for the current user:
# the `orange` command plus an "Orange" entry in the desktop's applications.
# For the Nix package, see NIX.md instead.
#
# Usage: ./install.sh [PREFIX]      (default PREFIX: ~/.local)
set -euo pipefail

APP_NAME="orange"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_SRC="$SRC_DIR/release/$APP_NAME"

PREFIX="${1:-"$HOME/.local"}"
BINDIR="$PREFIX/bin"
APPDIR="$PREFIX/opt/$APP_NAME" # binary + fonts + helpers
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
DESKTOP_DIR="$DATA_HOME/applications"
ICON_DIR="$DATA_HOME/icons/hicolor/512x512/apps"

echo "[*] Installing $APP_NAME into: $PREFIX"

if [[ ! -f "$BIN_SRC" ]]; then
    echo "ERROR: $BIN_SRC not found. Run ./build.sh first." >&2
    exit 1
fi

mkdir -p "$APPDIR" "$BINDIR" "$DESKTOP_DIR" "$ICON_DIR"

# App dir: binary, fonts (found next to the executable), sudo askpass helper
install -m 755 "$BIN_SRC" "$APPDIR/$APP_NAME"
rm -rf "$APPDIR/fonts"
cp -a "$SRC_DIR/fonts" "$APPDIR/fonts"
install -m 755 "$SRC_DIR/packaging/orange-askpass.sh" "$APPDIR/orange-askpass"
git -C "$SRC_DIR" describe --always --dirty >"$APPDIR/VERSION" 2>/dev/null ||
    echo unknown >"$APPDIR/VERSION"

# Launcher: per-run log in ~/orange_data/logs, sudo, runs from the app dir
install -m 755 "$SRC_DIR/packaging/orange-launcher.sh" "$BINDIR/$APP_NAME"

# Desktop entry and icon
install -m 644 "$SRC_DIR/icon.png" "$ICON_DIR/$APP_NAME.png"
sed "s|@bindir@|$BINDIR|g" "$SRC_DIR/packaging/orange.desktop" \
    >"$DESKTOP_DIR/$APP_NAME.desktop"
command -v update-desktop-database >/dev/null 2>&1 &&
    update-desktop-database "$DESKTOP_DIR" || true
command -v gtk-update-icon-cache >/dev/null 2>&1 &&
    gtk-update-icon-cache -q -t "$DATA_HOME/icons/hicolor" || true

echo
echo "✔ Install complete."
echo "➤ Run: $APP_NAME   (or open \"Orange\" from the applications menu)"
echo "   Logs: ~/orange_data/logs/orange_<date>_<time>.log"
case ":$PATH:" in
*":$BINDIR:"*) ;;
*) echo "   Note: $BINDIR is not in PATH; add: export PATH=\"$BINDIR:\$PATH\"" ;;
esac
