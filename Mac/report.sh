#!/bin/bash
#
#	Collect everything worth knowing about a pieFX run on a machine that is not
#	the one it was built on. Prints one block; paste it back.
#
#	Written for two specific unknowns, because "it seemed to work" cannot
#	distinguish them from a subtle failure:
#
#	  INTEL   Only arm64 has ever run pieFX. Both binaries are universal now,
#	          but the overlay does its window placement through hand-rolled
#	          objc_msgSend calls with structs passed by value, and struct
#	          passing is exactly where the x86_64 and arm64 ABIs differ. A
#	          mistake there does not crash: it puts the wheel in the wrong
#	          place.
#
#	  ENGLISH Every observation in this port comes from a Spanish AE. The menu
#	          id lookup follows the UI language, and the effect-name encoding
#	          path has only ever been exercised on names that NEEDED converting.
#	          On English both take their other branch, which is the untested one.
#
#	  ./report.sh
#
set -u

echo "================ pieFX report ================"
echo
echo "-- machine --"
echo "  arch:      $(uname -m)"
echo "  macOS:     $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
echo "  rosetta:   $(sysctl -n sysctl.proc_translated 2>/dev/null || echo n/a)"

echo
echo "-- After Effects --"
for AE in /Applications/Adobe\ After\ Effects*/Adobe\ After\ Effects*.app; do
	[ -d "${AE}" ] || continue
	V=$(defaults read "${AE}/Contents/Info" CFBundleShortVersionString 2>/dev/null || echo "?")
	echo "  ${AE##*/}  version ${V}"
done
echo "  UI language: $(defaults read com.adobe.AfterEffects 2>/dev/null | grep -i -m1 'language\|locale' || echo '(not recorded in prefs; read it off the UI)')"

echo
echo "-- what is installed --"
for P in /Applications/Adobe\ After\ Effects*/Plug-ins/pieFX.plugin; do
	[ -d "${P}" ] || continue
	echo "  ${P}"
	echo "    plug-in slices: $(lipo -info "${P}/Contents/MacOS/pieFX" 2>/dev/null | sed 's/.*: //')"
	OV="${P}/Contents/MacOS/pieFX-overlay.app/Contents/MacOS/pieFX-overlay"
	if [ -f "${OV}" ]; then
		echo "    overlay slices: $(lipo -info "${OV}" 2>/dev/null | sed 's/.*: //')"
	else
		echo "    overlay:        MISSING — the wheel cannot appear"
	fi
	echo "    signature:      $(codesign -dv "${P}" 2>&1 | grep -o 'Signature=.*' || echo none)"
	echo "    quarantined:    $(xattr -p com.apple.quarantine "${P}" 2>/dev/null || echo no)"
done

echo
echo "-- plug-in log --"
L="${TMPDIR:-/tmp}/pieFX_poc.txt"
if [ -f "${L}" ]; then
	#	The lines that answer the two questions above, plus anything that
	#	announced itself as a problem.
	grep -E "pieFX armed|hold threshold|settings:|effects:|presets:|copy-frame:|overlay:|MISMATCH|not implemented|error" "${L}" | tail -30 | sed 's/^/  /'
else
	echo "  no log at ${L} — has pieFX been armed in this AE session?"
fi

echo
echo "-- overlay log --"
O="${TMPDIR:-/tmp}/piefx_overlay.log"
if [ -f "${O}" ]; then
	grep -E "overlay started|overlay_dir|activation policy|frame ->|moved to|placing|origin|FAILED|REJECTED|settings" "${O}" | tail -20 | sed 's/^/  /'
else
	echo "  no log at ${O} — the overlay never started"
fi

echo
echo "-- the effect catalogue (the encoding question) --"
C="${HOME}/Library/Application Support/pieFX/effects.json"
if [ -f "${C}" ]; then
	echo "  ${C}"
	echo "  encoding:  $(file -b "${C}")"
	python3 - "${C}" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1], encoding="utf-8"))
except Exception as e:
    print(f"  PARSE FAILED: {e}   <-- this is the failure that matters")
    sys.exit()
eff = d.get("effects", [])
print(f"  parses:    yes, {len(eff)} effects, {d.get('presets_found', '?')} presets")
print(f"  walked/claimed: {d.get('walked')}/{d.get('claimed')}"
      + ("" if d.get("walked") == d.get("claimed") else "   *** MISMATCH ***"))
acc = [e for e in eff if any(ord(c) > 127 for c in e["name"] + e["category"])]
print(f"  non-ASCII names: {len(acc)}"
      + ("   (an English install should have few or none)" if len(acc) < 5 else ""))
for e in acc[:4]:
    print(f"     {e['name']} | {e['category']}")
PY
else
	echo "  no catalogue at ${C} — pieFX has not armed, or WriteEffectCatalogue failed"
fi

echo
echo "-- settings --"
S="${HOME}/Library/Application Support/pieFX/settings.json"
[ -f "${S}" ] && echo "  ${S} ($(wc -c < "${S}" | tr -d ' ') bytes)" || echo "  none (defaults in use)"

echo
echo "=============== end of report ================"
