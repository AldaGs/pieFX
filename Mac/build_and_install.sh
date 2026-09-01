#!/bin/bash
#
#	Build RadialMenuMac and install it into After Effects.
#
#	Mac time is the scarce resource, so this is one command per test cycle:
#	build, check the export symbol, check the PiPL, install, done.
#
#	The copy needs admin rights because AE's Plug-ins folder is root-owned,
#	so sudo will ask for a password once per run.
#
#	Usage:  ./Mac/build_and_install.sh [AE version]      (default 2026)
#
set -euo pipefail

AE_VERSION="${1:-2026}"
AE_PLUGINS="/Applications/Adobe After Effects ${AE_VERSION}/Plug-ins"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="${HERE}/RadialMenuMac.xcodeproj"
BUILT="${HERE}/build/Debug/RadialMenuMac.plugin"
BINARY="${BUILT}/Contents/MacOS/RadialMenuMac"

if pgrep -qf "Adobe After Effects"; then
	echo "!! After Effects is running. Quit it first - AE only reads Plug-ins at launch,"
	echo "   and overwriting a loaded bundle is how you get a crash instead of a result."
	exit 1
fi

echo "==> building"
xcodebuild -project "${PROJ}" -target RadialMenuMac -configuration Debug \
	SYMROOT="${HERE}/build" build > /tmp/RadialMenuMac_build.log 2>&1 \
	|| { echo "!! build FAILED. Log: /tmp/RadialMenuMac_build.log"; tail -40 /tmp/RadialMenuMac_build.log; exit 1; }

#	A five-parameter drift in the entry point is a legal C++ overload: it links
#	clean and exports mangled, and AE then says "Couldn't find main entry point".
#	That cost a launch on Windows. Catch it here instead of in AE.
echo "==> checking the export"
if ! nm -gU "${BINARY}" | grep -q '^[0-9a-f]* T _EntryPointFunc$'; then
	echo "!! EntryPointFunc is not exported as a bare C symbol:"
	nm -gU "${BINARY}" | grep -i entrypoint || echo "   (no EntryPoint symbol at all)"
	exit 1
fi
echo "    _EntryPointFunc OK"

#	An empty or missing PiPL means AE ignores the bundle without saying why.
echo "==> checking the PiPL"
RSRC="${BUILT}/Contents/Resources/RadialMenuMac.rsrc"
[ -s "${RSRC}" ] || { echo "!! ${RSRC} is missing or empty - Rez did not run."; exit 1; }
for tag in 8BIMkind 8BIMmi64 8BIMma64; do
	strings "${RSRC}" | grep -q "${tag}" || { echo "!! PiPL is missing ${tag}"; exit 1; }
done
echo "    kind + CodeMacIntel64 + CodeMacARM64 OK"

echo "==> arch: $(lipo -info "${BINARY}" | sed 's/.*are: //')"

[ -d "${AE_PLUGINS}" ] || { echo "!! No such folder: ${AE_PLUGINS}"; exit 1; }

echo "==> installing to ${AE_PLUGINS} (sudo)"
sudo rm -rf "${AE_PLUGINS}/RadialMenuMac.plugin"
sudo cp -R "${BUILT}" "${AE_PLUGINS}/RadialMenuMac.plugin"

echo
echo "Installed. Launch After Effects and look for these under the Window menu:"
echo "    Radial Menu (Mac) S1: Anchor to Center"
echo "    Radial Menu (Mac) S5: Dump Effects Catalogue"
echo "    Radial Menu (Mac) S4: Watch Right-Hold"
echo "    Radial Menu (Mac) S4: Swallow Hold OFF/ON"
echo "    Radial Menu (Mac) S3: Overlay Test"
echo
echo "Run them in the order MAC_SESSION.md gives: S1, then S5, then S4, then S3."
