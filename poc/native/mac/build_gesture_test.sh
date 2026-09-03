#!/bin/bash
#	Builds the gesture test host. Needs Cocoa, and no AE headers: the gesture
#	module is deliberately free of AEGP so it can be armed inside any app.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../../overlay/src-tauri/target/release/pieFX_gesture_test"
clang++ -std=c++17 -Wall -Wextra -O1 -fno-objc-arc \
    -framework Cocoa \
    "$HERE/pieFX_gesture.mm" "$HERE/gesture_test.mm" -o "$OUT"
echo "built $OUT"
