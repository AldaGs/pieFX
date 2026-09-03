#!/bin/bash
#	Builds the config-path + text-conversion harness. No AE, no overlay.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$(cd "${HERE}/../../overlay/src-tauri" && pwd)/target/release"
mkdir -p "${OUT}"
clang++ -std=c++17 -O2 -Wall -framework Foundation -framework CoreFoundation \
	"${HERE}/paths_test.cpp" \
	"${HERE}/pieFX_paths.mm" \
	"${HERE}/pieFX_text.cpp" \
	-o "${OUT}/pieFX_paths_test"
echo "built ${OUT}/pieFX_paths_test"
