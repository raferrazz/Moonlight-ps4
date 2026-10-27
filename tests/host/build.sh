#!/usr/bin/env bash
# Build + run the host UI harness (real menu code, scripted pad, captured
# frames) AND the HUD renderer (quick menu / perf overlay / hold ring /
# hint pill over a synthetic game frame). Both binaries are rebuilt from
# the CURRENT sources every run - a stale HUD binary once shipped shots
# that missed a font fix the menu shots already carried, which is
# exactly what this guards against.
# Usage: tests/host/build.sh [shots-dir] [hud-dir]
set -euo pipefail
cd "$(dirname "$0")/../.."

SHOTS="${1:-build-tests/shots}"
HUD="${2:-build-tests/hud}"
mkdir -p "$SHOTS" "$HUD" build-tests/fixture

python3 tests/host/make_covers.py build-tests/fixture/art

COMMON_FLAGS=(-D_DEFAULT_SOURCE -D_GNU_SOURCE -std=c11 -O1 -g
    -fsanitize=address,undefined -fno-omit-frame-pointer
    -Isrc -Ithird_party/moonlight-common-c/src -Ithird_party/mbedtls/include
    -include tests/host/host_video_decl.h)

gcc "${COMMON_FLAGS[@]}" \
    tests/host/render_ui.c tests/host/host_stubs.c \
    src/ui/ui_menu.c src/ui/ui_draw.c src/ui/ui_theme.c src/ui/ui_art.c \
    src/ui/quick_menu.c src/config.c src/log.c src/gamestream/discovery.c \
    -lm -lpthread -o build-tests/render_ui

gcc "${COMMON_FLAGS[@]}" \
    tests/host/render_hud.c src/ui/ui_draw.c src/ui/stream_hud.c \
    -lm -o build-tests/render_hud

ASAN_OPTIONS=detect_leaks=0 build-tests/render_ui "$SHOTS"
HUD_SHOTS="$HUD" ASAN_OPTIONS=detect_leaks=0 build-tests/render_hud

python3 - "$SHOTS" "$HUD" <<'EOF'
import sys
from pathlib import Path
from PIL import Image
n = 0
for d in map(Path, sys.argv[1:]):
    for f in sorted(d.glob("*.ppm")):
        Image.open(f).save(f.with_suffix(".png"))
        f.unlink()
        n += 1
print(f"converted {n} captures to PNG")
EOF
