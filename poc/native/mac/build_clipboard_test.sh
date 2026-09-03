#!/bin/bash
#	Builds the clipboard harness. No AE involved — but note it REPLACES the
#	clipboard contents when it runs, which is inherent to testing a clipboard.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$(cd "${HERE}/../../overlay/src-tauri" && pwd)/target/release"
mkdir -p "${OUT}"
clang++ -std=c++17 -O2 -Wall -fobjc-arc -framework AppKit \
	"${HERE}/clipboard_test.mm" \
	"${HERE}/pieFX_clipboard.mm" \
	-o "${OUT}/pieFX_clipboard_test"
echo "built ${OUT}/pieFX_clipboard_test"
