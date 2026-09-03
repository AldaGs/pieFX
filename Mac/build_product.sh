#!/bin/bash
#
#	Build the PRODUCT plug-in for macOS — poc/native/pieFX.cpp plus the seven
#	macOS modules — and, with --install, put it where After Effects will find
#	it.
#
#	Deliberately NOT an Xcode project. A .plugin is a directory with a binary,
#	an Info.plist, a PkgInfo and a Rez'd PiPL, and assembling it in twenty
#	lines of shell is far easier to read — and to review — than a pbxproj.
#	Mac/pieFXMac.xcodeproj still builds the Phase 0 spike, which is a separate
#	thing and stays as it is.
#
#	Usage:  ./Mac/build_product.sh [--install] [AE version]
#
set -euo pipefail

INSTALL=0
if [ "${1:-}" = "--install" ]; then INSTALL=1; shift; fi
AE_VERSION="${1:-2026}"
AE_PLUGINS="/Applications/Adobe After Effects ${AE_VERSION}/Plug-ins"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
SDK="$(cd "${ROOT}/../../.." && pwd)"
H="${SDK}/Examples/Headers"
OUT="${HERE}/build/product"
APP="${OUT}/pieFX.plugin"
BIN="${APP}/Contents/MacOS/pieFX"

INCS=(-I"${H}" -I"${SDK}/Examples/Util" -I"${H}/SP" -I"${H}/Mac" -I"${SDK}/Examples/Resources")

echo "==> compiling"
rm -rf "${APP}"
mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Resources"

#	Universal, as AE expects: the PiPL advertises both CodeMacARM64 and
#	CodeMacIntel64, and a bundle that claims a slice it does not have is a
#	load failure with no useful message.
ARCHS="-arch arm64 -arch x86_64"

clang++ -std=c++17 -O2 -Wall ${ARCHS} \
	-bundle \
	-framework Cocoa -framework CoreGraphics \
	"${INCS[@]}" \
	"${SDK}/Examples/Util/AEGP_SuiteHandler.cpp" \
	"${SDK}/Examples/Util/MissingSuiteError.cpp" \
	"${ROOT}/poc/native/pieFX.cpp" \
	"${ROOT}/poc/native/mac/pieFX_fifo.cpp" \
	"${ROOT}/poc/native/mac/pieFX_launch.cpp" \
	"${ROOT}/poc/native/mac/pieFX_gesture.mm" \
	"${ROOT}/poc/native/mac/pieFX_paths.mm" \
	"${ROOT}/poc/native/mac/pieFX_text.cpp" \
	"${ROOT}/poc/native/mac/pieFX_clipboard.mm" \
	-o "${BIN}"

#	A five-parameter drift in the entry point is a legal C++ overload: it links
#	clean and exports MANGLED, and AE then says "Couldn't find main entry
#	point". That cost a launch on Windows. Catch it here instead of in AE.
echo "==> checking the export"
if ! nm -gU "${BIN}" | grep -q '^[0-9a-f]* T _EntryPointFunc$'; then
	echo "!! EntryPointFunc is not exported as a bare C symbol:"
	nm -gU "${BIN}" | grep -i entrypoint || echo "   (no EntryPoint symbol at all)"
	exit 1
fi
echo "    _EntryPointFunc OK"

echo "==> PiPL"
#	An empty or missing PiPL means AE ignores the bundle without saying why.
xcrun Rez -o "${APP}/Contents/Resources/pieFX.rsrc" -useDF \
	-d __MACH__ -i "${H}" -i "${SDK}/Examples/Resources" \
	"${ROOT}/poc/native/pieFX_PiPL.r"
if [ ! -s "${APP}/Contents/Resources/pieFX.rsrc" ]; then
	echo "!! the PiPL resource is empty — AE would ignore this bundle silently"
	exit 1
fi
echo "    $(wc -c < "${APP}/Contents/Resources/pieFX.rsrc") bytes"

cat > "${APP}/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>pieFX</string>
	<key>CFBundleIdentifier</key><string>com.piefx.plugin</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>pieFX</string>
	<key>CFBundlePackageType</key><string>AEgx</string>
	<key>CFBundleSignature</key><string>FXTC</string>
	<key>LSRequiresCarbon</key><true/>
	<key>NSAppleScriptEnabled</key><string>No</string>
	<key>NSHumanReadableCopyright</key><string>Aldair Gonzalez</string>
</dict>
</plist>
PLIST
printf 'AEgxFXTC' > "${APP}/Contents/PkgInfo"

echo "==> built ${APP}"
lipo -info "${BIN}" | sed 's/^/    /'

#	The overlay must sit BESIDE the plug-in binary: PieFX_LaunchOverlay finds
#	it with dladdr, relative to the image it is compiled into.
OVERLAY="${ROOT}/poc/overlay/src-tauri/target/release/pieFX-overlay"
if [ -x "${OVERLAY}" ]; then
	cp "${OVERLAY}" "${APP}/Contents/MacOS/pieFX-overlay"
	echo "    overlay copied in beside the plug-in"
else
	echo "    NOTE: no overlay built yet; the plug-in will log that it cannot find one"
fi

#	The .jsx snippets the wheel invokes by RELATIVE path. menu.js binds the
#	master-null slots to "scripts/ag_masterNull.jsx", and the overlay resolves
#	that against its OWN directory (current_exe().parent()) — which inside the
#	bundle is Contents/MacOS. So they sit beside the overlay, not in Resources,
#	however much Resources looks like where they belong.
#
#	The snippet the wheel sends carries a fallback that hunts through AE's user
#	Scripts folders, so a missing file degrades to a thrown error naming the
#	path rather than to silence — but shipping it is the point.
mkdir -p "${APP}/Contents/MacOS/scripts"
if compgen -G "${ROOT}/poc/scripts/*.jsx" > /dev/null; then
	cp "${ROOT}"/poc/scripts/*.jsx "${APP}/Contents/MacOS/scripts/"
	echo "    scripts: $(ls "${APP}/Contents/MacOS/scripts" | tr '\n' ' ')"
else
	echo "    NOTE: no .jsx scripts found to copy"
fi

if [ "${INSTALL}" = "1" ]; then
	if pgrep -qf "Adobe After Effects"; then
		echo "!! After Effects is running. Quit it first — AE only reads Plug-ins at"
		echo "   launch, and overwriting a loaded bundle is how you get a crash"
		echo "   instead of a result."
		exit 1
	fi
	echo "==> installing (sudo: AE's Plug-ins folder is root-owned)"
	sudo rm -rf "${AE_PLUGINS}/pieFX.plugin"
	sudo cp -R "${APP}" "${AE_PLUGINS}/"
	echo "    installed to ${AE_PLUGINS}/pieFX.plugin"
fi
