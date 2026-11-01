#!/bin/sh
# Copies the web app and the app icon into one directory, the layout
# TerminalBackend serves from.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
destination=${1:-"$root/build/web"}
mkdir -p "$destination"
cp "$root/Resources/TerminalWeb/"*.html "$root/Resources/TerminalWeb/"*.js "$root/Resources/TerminalWeb/"*.css "$destination/"
cp "$root/Resources/TerminalWeb/app-icon.png" "$destination/"
