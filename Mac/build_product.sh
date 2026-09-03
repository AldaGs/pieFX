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
#	The overlay has to be UNIVERSAL too, and it is the half that is easy to
#	forget: the plug-in gets `-arch arm64 -arch x86_64` explicitly, while
#	`cargo build --release` quietly builds for the host and nothing complains.
#	An Intel machine would then load the plug-in — its x86_64 slice is there —
#	and fail to exec the overlay, so the wheel would simply never appear.
#
#	Both slices are lipo'd together when both exist. When only one does the
#	bundle is still built, because that is the development case and blocking it
#	would be worse, but it says so LOUDLY: a single-arch overlay is only wrong
#	when it ships.
#
#	  rustup target add x86_64-apple-darwin
#	  cd poc/overlay/src-tauri && cargo build --release --target x86_64-apple-darwin
TGT="${ROOT}/poc/overlay/src-tauri/target"
OVERLAY="${TGT}/release/pieFX-overlay"
OV_X86="${TGT}/x86_64-apple-darwin/release/pieFX-overlay"
OV_ARM="${TGT}/aarch64-apple-darwin/release/pieFX-overlay"
OAPP="${APP}/Contents/MacOS/pieFX-overlay.app"

SLICES=()
[ -x "${OVERLAY}" ] && SLICES+=("${OVERLAY}")
for extra in "${OV_X86}" "${OV_ARM}"; do
	if [ -x "${extra}" ]; then
		#	Skip a slice we already have from the host build.
		have=0
		for s in "${SLICES[@]}"; do
			if lipo -info "${s}" 2>/dev/null | grep -q "$(lipo -info "${extra}" | sed 's/.*: //')"; then
				have=1
			fi
		done
		[ "${have}" = "0" ] && SLICES+=("${extra}")
	fi
done

if [ "${#SLICES[@]}" -gt 0 ]; then
	mkdir -p "${OAPP}/Contents/MacOS" "${OAPP}/Contents/Resources"
	if [ "${#SLICES[@]}" -gt 1 ]; then
		lipo -create "${SLICES[@]}" -output "${OAPP}/Contents/MacOS/pieFX-overlay"
	else
		cp "${SLICES[0]}" "${OAPP}/Contents/MacOS/pieFX-overlay"
	fi
	OARCHS="$(lipo -info "${OAPP}/Contents/MacOS/pieFX-overlay" | sed 's/.*: //')"
	case "${OARCHS}" in
		*x86_64*arm64*|*arm64*x86_64*) echo "    overlay is universal (${OARCHS})" ;;
		*) echo "    !! overlay is ${OARCHS} ONLY — fine for development, WRONG to ship"
		   echo "       build the other slice: cargo build --release --target x86_64-apple-darwin" ;;
	esac

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
#	that against `overlay_dir` — which inside a bundle is Contents/RESOURCES.
#
#	They lived in Contents/MacOS for exactly one build, and that was a mistake
#	with a hard consequence: codesign treats everything in MacOS as code that
#	must itself be signed, so two text files beside the binary made the ENTIRE
#	bundle fail to sign with "code object is not signed at all". Resources is
#	the one place a bundle may carry things that are not code.
#
#	The snippet the wheel sends carries a fallback that hunts through AE's user
#	Scripts folders, so a missing file degrades to a thrown error naming the
#	path rather than to silence — but shipping it is the point.
SCRIPTS="${OAPP}/Contents/Resources/scripts"
mkdir -p "${SCRIPTS}"
if compgen -G "${ROOT}/poc/scripts/*.jsx" > /dev/null; then
	cp "${ROOT}"/poc/scripts/*.jsx "${SCRIPTS}/"
	echo "    scripts: $(ls "${SCRIPTS}" | tr '\n' ' ')"
else
	echo "    NOTE: no .jsx scripts found to copy"
fi

#	An AD-HOC signature over the finished bundle, inside out.
#
#	Not a substitute for Developer ID (see Mac/sign_product.sh) — it identifies
#	nobody and Gatekeeper is unmoved by it. It is here for two smaller reasons.
#
#	arm64 macOS refuses to run code with NO signature at all, and `lipo -create`
#	produces a fat file whose slices carry whatever they carried: the Rust
#	x86_64 slice has none, so `codesign --verify` on the merged binary reports
#	"code object is not signed at all" even though the arm64 slice is fine. That
#	is a confusing thing to hand somebody who is debugging on an Intel machine.
#
#	And it rehearses the ORDER. Inner bundle first, outer second, because a
#	signature covers everything beneath it — the same rule sign_product.sh
#	depends on, exercised on every build rather than once, months later, with a
#	real certificate.
if [ -d "${OAPP}" ]; then
	echo "==> ad-hoc signing"
	codesign --force --sign - "${OAPP}" >/dev/null 2>&1 || echo "    (overlay bundle: codesign declined)"
fi
codesign --force --sign - "${APP}" >/dev/null 2>&1 || echo "    (plug-in bundle: codesign declined)"
if codesign --verify --deep --strict "${APP}" 2>/dev/null; then
	echo "    verifies"
else
	echo "    !! the bundle does not verify even ad-hoc — it may not load"
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
