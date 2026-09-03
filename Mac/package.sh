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
#	plug-in was tested by hand and loaded normally. See docs/MAC_RESULTS.md.
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
pieFX for macOS
===============

A cursor-anchored radial menu for After Effects. Free, MIT licensed.
Source and full documentation: github.com/AldaGs/pieFX

This build is universal — Apple Silicon and Intel. It is NOT signed by a
developer certificate, and that is expected: pieFX is free and does not earn
one. After Effects allows unsigned plug-ins, and a quarantined copy has been
tested and loads normally, so there is no Gatekeeper step to work around.


INSTALL
-------
1. Quit After Effects completely. It only reads Plug-ins at launch.

2. Copy `pieFX.plugin` into:

     /Applications/Adobe After Effects <year>/Plug-ins/

   Replace <year> with your version. In the Finder, press Shift-Command-G and
   paste that path to get there. macOS will ask for your password: that folder
   is owned by the system.

   If you run several versions of After Effects, copy it into each one.

3. Start After Effects. `pieFX (Show/Hide)` appears in the Window menu.
   It arms itself on launch, so there is usually nothing to click.


USING IT
--------
Press and HOLD the right mouse button for a moment (200ms by default), and a
ring of hexagons appears under the cursor. Keep holding, flick toward one, and
release. A short right-click still opens After Effects' normal context menu.

The centre hexagon is cancel — release there and nothing fires.

The top hexagon opens the effect search: type a name, press Return, and it is
applied to your selection.

A slot that needs a layer selection or a comp you do not have is drawn dead
and will not fire, so nothing happens silently.

Rebind any of it in Window > pieFX Settings.


IF SOMETHING GOES WRONG
-----------------------
`report.sh` collects versions, architectures and the logs into one block of
text you can paste into a bug report. Open Terminal, drag `report.sh` into the
window, press Return. It gathers no personal data and you can read it first.

Issues: github.com/AldaGs/pieFX/issues


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
