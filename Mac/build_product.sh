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

#	The overlay, as a BUNDLE rather than a bare executable.
#
#	It ran as a naked Mach-O for the whole port, which works but is not how
#	macOS expects a GUI process to exist: no bundle identifier, no icon, and
#	the accessory activation policy (the thing that keeps it out of the Dock
#	and stops it stealing focus from AE) set only in code. LaunchServices
#	reported `bundleID=[ NULL ]` for it.
#
#	`PieFX_LaunchOverlay` still execs the INNER binary directly rather than
#	going through /usr/bin/open, and that is deliberate twice over. `open`
#	would hand the process to LaunchServices, which breaks the teardown
#	guarantee — the overlay has to be a direct child in our own process group
#	so one kill takes its WebKit children with it. And exec'ing the inner
#	binary still gets bundle semantics, because NSBundle.main is derived from
#	the executable's PATH: Info.plist is read either way.
OVERLAY="${ROOT}/poc/overlay/src-tauri/target/release/pieFX-overlay"
OAPP="${APP}/Contents/MacOS/pieFX-overlay.app"
if [ -x "${OVERLAY}" ]; then
	mkdir -p "${OAPP}/Contents/MacOS" "${OAPP}/Contents/Resources"
	cp "${OVERLAY}" "${OAPP}/Contents/MacOS/pieFX-overlay"

	#	LSUIElement declares what mac_accessory_app() also sets at runtime.
	#	Both, deliberately: the plist is read before any of our code runs, so
	#	it closes the window in which a Dock icon could appear, and the runtime
	#	call keeps the bare-binary dev path behaving the same way.
	cat > "${OAPP}/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>pieFX-overlay</string>
	<key>CFBundleIdentifier</key><string>com.piefx.overlay</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>pieFX Overlay</string>
	<key>CFBundleDisplayName</key><string>pieFX Overlay</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleSignature</key><string>????</string>
	<key>CFBundleShortVersionString</key><string>0.1.0</string>
	<key>CFBundleVersion</key><string>0.1.0</string>
	<key>CFBundleIconFile</key><string>pieFX</string>
	<key>LSUIElement</key><true/>
	<key>LSMinimumSystemVersion</key><string>10.15</string>
	<key>NSHumanReadableCopyright</key><string>Aldair Gonzalez</string>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
	printf 'APPL????' > "${OAPP}/Contents/PkgInfo"

	ICNS="${ROOT}/poc/overlay/src-tauri/icons/icon.icns"
	if [ -f "${ICNS}" ]; then
		cp "${ICNS}" "${OAPP}/Contents/Resources/pieFX.icns"
	else
		echo "    NOTE: no icon.icns — run ./icon/make_icons.sh"
	fi
	echo "    overlay bundled at Contents/MacOS/pieFX-overlay.app"
else
	echo "    NOTE: no overlay built yet; the plug-in will log that it cannot find one"
fi

#	The .jsx snippets the wheel invokes by RELATIVE path. menu.js binds the
#	master-null slots to "scripts/ag_masterNull.jsx", and the overlay resolves
#	that against its OWN directory — current_exe().parent().
#
#	Which MOVED when the overlay became a bundle. They now belong beside the
#	inner binary, inside pieFX-overlay.app/Contents/MacOS, and putting them
#	where they used to be would break every script slot with a file-not-found
#	the wheel reports as a thrown error.
#
#	The snippet the wheel sends carries a fallback that hunts through AE's user
#	Scripts folders, so a missing file degrades to a thrown error naming the
#	path rather than to silence — but shipping it is the point.
SCRIPTS="${OAPP}/Contents/MacOS/scripts"
mkdir -p "${SCRIPTS}"
if compgen -G "${ROOT}/poc/scripts/*.jsx" > /dev/null; then
	cp "${ROOT}"/poc/scripts/*.jsx "${SCRIPTS}/"
	echo "    scripts: $(ls "${SCRIPTS}" | tr '\n' ' ')"
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
