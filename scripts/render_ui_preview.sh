#!/usr/bin/env bash
# Render native previews of the ACTUAL menu code on a development PC.
# The harness (tests/host) compiles the real ui_menu.c/ui_draw.c/ui_theme.c/
# ui_art.c plus a scripted pad and captured VideoOut, then drives scripted
# sessions: theme tours, OSK flow, launch/fail screens, first boot, error
# and recovery states. Shots land in build-tests/<dir> as PNG.
#
# Usage: scripts/render_ui_preview.sh [shots-dir] [after|before]
set -euo pipefail
cd "$(dirname "$0")/.."

DIR="${1:-build-tests/shots-${2:-after}}"
exec bash tests/host/build.sh "$DIR"
