#!/bin/bash
#	Builds the preset-walk harness. No AE running, but it does need AE
#	INSTALLED, because the whole point is to walk the real Presets tree.
#
#	It #includes pieFX.cpp so it can reach the static walk — see the note at
#	the top of presets_test.cpp.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
SDK="$(cd "${ROOT}/../../.." && pwd)"
H="${SDK}/Examples/Headers"
OUT="${ROOT}/poc/overlay/src-tauri/target/release"
mkdir -p "${OUT}"
clang++ -std=c++17 -O2 -Wno-unused \
	-I"${H}" -I"${SDK}/Examples/Util" -I"${H}/SP" -I"${H}/Mac" -I"${SDK}/Examples/Resources" \
	-framework Cocoa -framework CoreGraphics \
	"${SDK}/Examples/Util/AEGP_SuiteHandler.cpp" \
	"${SDK}/Examples/Util/MissingSuiteError.cpp" \
	"${HERE}/presets_test.cpp" \
	"${HERE}/pieFX_fifo.cpp" \
	"${HERE}/pieFX_launch.cpp" \
	"${HERE}/pieFX_gesture.mm" \
	"${HERE}/pieFX_paths.mm" \
	"${HERE}/pieFX_text.cpp" \
	"${HERE}/pieFX_clipboard.mm" \
	-o "${OUT}/pieFX_presets_test"
echo "built ${OUT}/pieFX_presets_test"
