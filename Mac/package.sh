#!/bin/bash
#
#	Make a zip somebody else can install, with the report script inside it.
#
#	ditto, not zip: only ditto preserves the symlinks and extended attributes a
#	bundle depends on, and a bundle that survives `zip` by luck is a bundle that
#	fails on the next machine.
#
#	The result is UNSIGNED beyond ad-hoc, which is fine and is measured: AE
#	ships com.apple.security.cs.disable-library-validation, and a quarantined
#	plug-in was tested by hand and loaded normally. See MAC_RESULTS.md.
#
#	  ./Mac/package.sh
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="${HERE}/build/product/pieFX.plugin"
OUT="${HERE}/build/pieFX-mac.zip"
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT

[ -d "${APP}" ] || { echo "!! no ${APP} — run ./Mac/build_product.sh first"; exit 1; }

#	The whole point of the package is the OTHER architecture, so shipping a
#	single-arch overlay is a hard error here even though it is only a warning
#	in build_product.sh.
OV="${APP}/Contents/MacOS/pieFX-overlay.app/Contents/MacOS/pieFX-overlay"
[ -f "${OV}" ] || { echo "!! no overlay inside the bundle"; exit 1; }
ARCHS="$(lipo -info "${OV}" | sed 's/.*: //')"
case "${ARCHS}" in
	*x86_64*arm64*|*arm64*x86_64*) ;;
	*) echo "!! the overlay is ${ARCHS} only. Packaging that would send someone"
	   echo "   a plug-in that loads, arms, and never shows a wheel."
	   echo "   rustup target add x86_64-apple-darwin"
	   echo "   cd poc/overlay/src-tauri && cargo build --release --target x86_64-apple-darwin"
	   echo "   ./Mac/build_product.sh"
	   exit 1 ;;
esac

mkdir -p "${STAGE}/pieFX-mac"
/usr/bin/ditto "${APP}" "${STAGE}/pieFX-mac/pieFX.plugin"
cp "${HERE}/report.sh" "${STAGE}/pieFX-mac/report.sh"

cat > "${STAGE}/pieFX-mac/READ ME.txt" <<'TXT'
pieFX for macOS — test build
============================

This build is universal (Apple Silicon and Intel) and is NOT signed by a
developer certificate. That is expected. After Effects allows unsigned
plug-ins, and a quarantined copy has been tested and loads normally.


INSTALL
-------
1. Quit After Effects completely. It only reads Plug-ins at launch.

2. Copy `pieFX.plugin` into:

     /Applications/Adobe After Effects <year>/Plug-ins/

   The Finder will ask for your password: that folder is owned by the system.

3. Start After Effects. `pieFX (Show/Hide)` appears in the Window menu.
   It arms itself on launch, so there is usually nothing to click.


USING IT
--------
Press and HOLD the right mouse button for a moment (200ms by default), and a
ring of hexagons appears under the cursor. Keep holding, flick toward one, and
release. A short right-click still opens After Effects' normal context menu.

The centre hexagon is cancel — release there and nothing fires.


WHAT WOULD BE USEFUL TO KNOW
----------------------------
This build has only ever run on one machine: an Apple Silicon Mac with a
SPANISH After Effects. So the interesting questions are the ordinary ones.

  * Does the wheel appear at all, under the cursor?
  * If you have two displays, does it appear on the one you are using?
  * Do the items fire? Try Comp Settings, and Master Null.
  * Open the effect search (the top hexagon) and type an effect name.
    Does the list look right?
  * Settings live in Window > pieFX Settings. Does that window open on the
    display you are working on?

Anything that looks odd is worth mentioning even if it still worked.


SENDING BACK A REPORT
---------------------
After using it for a minute, open Terminal, drag `report.sh` into the window,
press Return, and send back what it prints. It collects versions,
architectures and the logs — no personal data, and you can read it first.


UNINSTALL
---------
Quit After Effects and delete the plug-in from the Plug-ins folder. pieFX also
leaves three small files in
~/Library/Application Support/pieFX/ which can be deleted.
TXT

rm -f "${OUT}"
/usr/bin/ditto -c -k --keepParent "${STAGE}/pieFX-mac" "${OUT}"

echo "==> ${OUT}"
echo "    overlay: ${ARCHS}"
echo "    plug-in: $(lipo -info "${APP}/Contents/MacOS/pieFX" | sed 's/.*: //')"
echo "    $(du -h "${OUT}" | cut -f1)"
