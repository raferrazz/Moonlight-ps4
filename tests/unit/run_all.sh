#!/usr/bin/env bash
# Build + run the host unit suites (pure logic: quick menu, touchpad
# gestures, Wake-on-LAN, saved-PC config list). Run from the repo root or
# via tests/unit path.
set -euo pipefail
cd "$(dirname "$0")/../.."

mkdir -p build-tests

CC=${CC:-gcc}
CFLAGS=(-D_DEFAULT_SOURCE -std=c11 -O1 -g -fsanitize=address,undefined
        -Isrc -Ithird_party/moonlight-common-c/src -Ithird_party/mbedtls/include)

# Per-suite sources: every suite links log.c; the rest is what it exercises.
sources_for() {
    case "$1" in
    quick_menu_test)
        echo "src/ui/quick_menu.c src/input/touch_gesture.c src/gamestream/discovery.c" ;;
    touch_gesture_test)
        echo "src/input/touch_gesture.c" ;;
    wol_test)
        echo "src/gamestream/discovery.c" ;;
    pc_list_test)
        echo "src/config.c" ;;
    esac
}

failed=0
for suite in quick_menu_test touch_gesture_test wol_test pc_list_test; do
    echo "== $suite =="
    # shellcheck disable=SC2046
    "$CC" "${CFLAGS[@]}" "tests/unit/$suite.c" \
        $(sources_for "$suite") src/log.c -lm -lpthread \
        -o "build-tests/$suite"
    ASAN_OPTIONS=detect_leaks=0 "build-tests/$suite" || failed=1
done

if [[ $failed -ne 0 ]]; then
    echo "== unit suites FAILED ==" >&2
    exit 1
fi
echo "== all unit suites passed =="
