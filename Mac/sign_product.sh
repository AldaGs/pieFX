#!/bin/bash
#
#	Sign, notarize and staple pieFX.plugin.
#
#	*** THIS SCRIPT HAS NEVER BEEN RUN. ***
#
#	It was written on a machine with `0 valid identities found`, so every step
#	below is from the documented behaviour of these tools and NOT from a
#	successful run. Read it before you trust it, and expect to fix something.
#	It is here rather than absent because the machine it was written on is
#	borrowed: the bundle layout it encodes — which binary is nested inside
#	which — is knowable today and would have to be rediscovered later.
#
#	It is deliberately noisy and it verifies after every step, because the
#	failure mode of signing is a bundle that looks signed and is rejected on
#	someone else's machine.
#
#	WHAT SIGNING IS AND IS NOT FOR HERE
#
#	It is NOT needed for pieFX to load. After Effects ships
#	com.apple.security.cs.disable-library-validation, so its hardened runtime
#	accepts a plug-in signed by anyone or by no one, and a QUARANTINED unsigned
#	plug-in was tested by hand and loaded normally. Signing buys Gatekeeper's
#	approval of the container a user DOWNLOADS. See docs/MAC_RESULTS.md.
#
#	  ./Mac/sign_product.sh "Developer ID Application: Your Name (TEAMID)"
#	  ./Mac/sign_product.sh "Developer ID Application: ..." --notarize KEYCHAIN_PROFILE
#
#	The keychain profile is made once, and stores an app-specific password:
#	  xcrun notarytool store-credentials KEYCHAIN_PROFILE \
#	      --apple-id you@example.com --team-id TEAMID --password xxxx-xxxx-xxxx-xxxx
#
set -euo pipefail

IDENTITY="${1:-}"
NOTARIZE=""
if [ "${2:-}" = "--notarize" ]; then NOTARIZE="${3:-}"; fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
APP="${HERE}/build/product/pieFX.plugin"
OAPP="${APP}/Contents/MacOS/pieFX-overlay.app"

if [ -z "${IDENTITY}" ]; then
	echo "usage: $0 \"Developer ID Application: Name (TEAMID)\" [--notarize PROFILE]"
	echo
	echo "identities available here:"
	security find-identity -v -p codesigning || true
	exit 2
fi
[ -d "${APP}" ]  || { echo "!! no ${APP} — run ./Mac/build_product.sh first"; exit 1; }
[ -d "${OAPP}" ] || { echo "!! no overlay bundle inside the plug-in"; exit 1; }

#	INSIDE OUT, and this order is not a preference.
#
#	A signature covers everything beneath it, so signing the outer bundle first
#	and the inner one second invalidates the outer immediately — and it stays
#	broken silently until someone runs `codesign --verify --deep` or Gatekeeper
#	does it for them on a stranger's machine.
#
#	--options runtime is required for notarization. --timestamp is required too,
#	and it needs network access: a signature without one expires with the
#	certificate instead of outliving it.
SIGN=(codesign --force --timestamp --options runtime --sign "${IDENTITY}")

echo "==> 1/3  the overlay's executable (deepest first)"
"${SIGN[@]}" "${OAPP}/Contents/MacOS/pieFX-overlay"

echo "==> 2/3  the overlay bundle"
#	NOT --deep. Apple deprecated it and it signs nested code with the WRONG
#	identity and no entitlements; each piece is signed explicitly above.
"${SIGN[@]}" "${OAPP}"
codesign --verify --strict --verbose=2 "${OAPP}"

echo "==> 3/3  the plug-in bundle"
"${SIGN[@]}" "${APP}"
codesign --verify --deep --strict --verbose=2 "${APP}"

echo "==> what got signed"
codesign -dv --verbose=4 "${APP}" 2>&1 | grep -E "Authority|TeamIdentifier|Timestamp|flags" || true

if [ -z "${NOTARIZE}" ]; then
	echo
	echo "Signed, NOT notarized. A user who downloads this will still see"
	echo "Gatekeeper complain, because notarization is the half that tells"
	echo "Apple the software exists. Re-run with --notarize PROFILE."
	exit 0
fi

#	Notarization takes an ARCHIVE, not a bundle, and ditto is the only zip that
#	preserves the symlinks and extended attributes a bundle depends on. `zip`
#	will produce something that uploads and then fails validation.
ZIP="${HERE}/build/pieFX.plugin.zip"
echo "==> zipping for notarization (ditto, not zip)"
rm -f "${ZIP}"
/usr/bin/ditto -c -k --keepParent "${APP}" "${ZIP}"

echo "==> notarizing (this waits for Apple)"
xcrun notarytool submit "${ZIP}" --keychain-profile "${NOTARIZE}" --wait

#	The ticket is stapled to the BUNDLE, so it travels with the plug-in and
#	validates on a machine with no network. The zip above was only a transport
#	and is now stale — re-make it after stapling if you ship a zip.
echo "==> stapling the ticket"
xcrun stapler staple "${APP}"
xcrun stapler validate "${APP}"

rm -f "${ZIP}"
/usr/bin/ditto -c -k --keepParent "${APP}" "${ZIP}"
echo "==> stapled; shippable archive at ${ZIP}"

#	The real test, and it must be done on a DOWNLOADED copy: spctl assesses
#	quarantined files differently from ones that never left the machine.
echo
echo "Verify on a downloaded copy, not this one:"
echo "  spctl -a -vvv -t install /path/to/downloaded/pieFX.plugin"
