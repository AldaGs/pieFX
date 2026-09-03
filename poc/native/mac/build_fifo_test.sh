#!/bin/bash
#	Builds the offline transport test. No AE, no Xcode project, no SDK headers —
#	the transport is deliberately free of AEGP so it can be exercised alone.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
#	Built BESIDE the overlay, not into $TMPDIR. PieFX_LaunchOverlay finds the
#	overlay next to the binary that contains it — dladdr, the macOS answer to
#	GetModuleFileName — so putting the test there is what makes that lookup
#	the real one rather than something faked with a symlink.
OUT="$HERE/../../overlay/src-tauri/target/release/pieFX_fifo_test"
clang++ -std=c++17 -Wall -Wextra -O1 \
    "$HERE/pieFX_fifo.cpp" "$HERE/pieFX_launch.cpp" "$HERE/fifo_test.cpp" -o "$OUT"
echo "built $OUT"
