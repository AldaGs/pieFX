#!/bin/bash
#	Builds the offline transport test. No AE, no Xcode project, no SDK headers —
#	the transport is deliberately free of AEGP so it can be exercised alone.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${TMPDIR:-/tmp}/pieFX_fifo_test"
clang++ -std=c++17 -Wall -Wextra -O1 \
    "$HERE/pieFX_fifo.cpp" "$HERE/fifo_test.cpp" -o "$OUT"
echo "built $OUT"
