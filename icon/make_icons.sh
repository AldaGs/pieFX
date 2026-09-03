#!/bin/bash
#
#	Build the overlay's icon set from `pieFX LOGO.svg`.
#
#	The SVG is the master, not ICON.png: the PNG is 600x600 and macOS wants
#	1024 for the largest .icns slot, so anything derived from the PNG would be
#	upscaled. The SVG is pure vector — no embedded rasters, no fonts, no text —
#	so it renders at any size.
#
#	Rendering is done by `render_svg.swift`, NOT by `qlmanage -t`. That was the
#	obvious one-liner and it is wrong in a way that would have shipped: it
#	reports `hasAlpha: yes` and hands back an image whose corner pixel is opaque
#	WHITE, because a thumbnailer composites onto a white card. The icon would
#	have had a white box behind it at every size and nothing would have said so.
#
#	The render is still CHECKED afterwards — size, alpha, and a transparent
#	corner — because that is the assertion that caught qlmanage, and a renderer
#	that is right today is not a reason to stop asking.
#
#	  ./icon/make_icons.sh
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
SVG="${HERE}/pieFX LOGO.svg"
OUT="${ROOT}/poc/overlay/src-tauri/icons"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

[ -f "${SVG}" ] || { echo "!! no ${SVG}"; exit 1; }

#	The artwork is recoloured to the WHEEL'S accent before it is rendered, and
#	the accent is read out of hexdraw.js rather than typed here. The icon and the
#	hexagons it depicts are then the same purple by construction: change
#	DEFAULT_ACCENT and the icon follows on the next build, instead of drifting
#	apart the way they already had once (the art was #BE55EE against the wheel's
#	#C74FD6).
#
#	The logo's 26 purples are a pure LIGHTNESS ramp — hue 281 and saturation 82
#	across all of them, lightness 55 to 70 — which is what makes this a safe
#	transform rather than a repaint. Each shade takes the accent's hue and
#	saturation and keeps its own place in the ramp, so the bevel survives.
#
#	The 357 white slivers are specular highlights and are deliberately left
#	alone: they are not part of the ramp, and a highlight that is not white is
#	a different drawing.
echo "==> recolouring to the wheel's accent"
SRC="${TMP}/recoloured.svg"
python3 - "${SVG}" "${ROOT}/poc/overlay/src/hexdraw.js" "${SRC}" <<'PY'
import colorsys, re, sys

svg_path, hexdraw_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
svg = open(svg_path).read()

m = re.search(r'DEFAULT_ACCENT\s*=\s*"(#[0-9a-fA-F]{6})"', open(hexdraw_path).read())
if not m:
    sys.exit("!! DEFAULT_ACCENT not found in hexdraw.js — refusing to guess the accent")
accent = m.group(1).lower()

def to_rgb(h):
    return tuple(int(h[i:i+2], 16) / 255 for i in (1, 3, 5))
def to_hex(r, g, b):
    return "#%02x%02x%02x" % tuple(max(0, min(255, round(c * 255))) for c in (r, g, b))

a_h, a_l, a_s = colorsys.rgb_to_hls(*to_rgb(accent))

#	White is a highlight, not a member of the ramp.
shades = sorted({c.lower() for c in re.findall(r'#[0-9a-fA-F]{6}', svg)} - {"#ffffff"})
if not shades:
    sys.exit("!! no colours found in the SVG")
lit = sorted((colorsys.rgb_to_hls(*to_rgb(c))[1], c) for c in shades)
mid_l = lit[len(lit) // 2][0]

mapping = {}
for c in shades:
    _, l, _ = colorsys.rgb_to_hls(*to_rgb(c))
    mapping[c] = to_hex(*colorsys.hls_to_rgb(a_h, max(0.0, min(1.0, l + (a_l - mid_l))), a_s))

#	Case-insensitive, because an exporter may mix them.
def swap(mo):
    return mapping.get(mo.group(0).lower(), mo.group(0))
open(out_path, "w").write(re.sub(r'#[0-9a-fA-F]{6}', swap, svg))

print(f"    {len(shades)} shades -> hue {a_h*360:.0f} sat {a_s*100:.0f} (accent {accent}), ramp preserved")
PY
[ -s "${SRC}" ] || { echo "!! the recolour produced nothing"; exit 1; }

echo "==> rendering ${SVG##*/} at 1024"
MASTER="${TMP}/master.png"
swift "${HERE}/render_svg.swift" "${SRC}" "${MASTER}" 1024
[ -s "${MASTER}" ] || { echo "!! the renderer produced nothing"; exit 1; }

W=$(sips -g pixelWidth "${MASTER}" | awk '/pixelWidth/{print $2}')
A=$(sips -g hasAlpha  "${MASTER}" | awk '/hasAlpha/{print $2}')
[ "${W}" = "1024" ] || { echo "!! rendered ${W}px, wanted 1024"; exit 1; }
[ "${A}" = "yes" ]  || { echo "!! rendered with no alpha channel"; exit 1; }
CORNER=$(python3 - "${MASTER}" <<'PY'
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
pos, idat = 8, b''
while pos < len(d):
    ln = struct.unpack('>I', d[pos:pos+4])[0]
    typ, data = d[pos+4:pos+8], d[pos+8:pos+8+ln]
    if typ == b'IHDR': w, h, bd, ct = struct.unpack('>IIBB', data[:10])
    if typ == b'IDAT': idat += data
    pos += 12 + ln
raw = zlib.decompress(idat)
f, stride = raw[0], w * 4
line = bytearray(raw[1:1+stride])
for x in range(stride):
    a = line[x-4] if x >= 4 else 0
    if f in (1, 4):  line[x] = (line[x] + a) & 255
    elif f == 3:     line[x] = (line[x] + a // 2) & 255
print(line[3])
PY
)
[ "${CORNER}" = "0" ] || { echo "!! top-left pixel has alpha ${CORNER}, not 0 — the render was flattened"; exit 1; }
echo "    1024px, alpha, transparent corner"

echo "==> .icns"
SET="${TMP}/pieFX.iconset"
mkdir -p "${SET}"
for spec in "16 icon_16x16" "32 icon_16x16@2x" "32 icon_32x32" "64 icon_32x32@2x" \
            "128 icon_128x128" "256 icon_128x128@2x" "256 icon_256x256" \
            "512 icon_256x256@2x" "512 icon_512x512" "1024 icon_512x512@2x"; do
	set -- ${spec}
	sips -z "$1" "$1" "${MASTER}" --out "${SET}/$2.png" >/dev/null
done
iconutil -c icns "${SET}" -o "${OUT}/icon.icns"

echo "==> the PNGs tauri.conf.json names"
sips -z 32   32   "${MASTER}" --out "${OUT}/32x32.png"        >/dev/null
sips -z 128  128  "${MASTER}" --out "${OUT}/128x128.png"      >/dev/null
sips -z 256  256  "${MASTER}" --out "${OUT}/128x128@2x.png"   >/dev/null
cp "${MASTER}" "${OUT}/icon.png"

#	The Windows Store square logos. Regenerated rather than deleted: they are
#	only read by the MSIX target, but leaving Tauri's stock artwork in the tree
#	is precisely the thing this script exists to end.
for s in 30 44 71 89 107 142 150 284 310; do
	sips -z "${s}" "${s}" "${MASTER}" --out "${OUT}/Square${s}x${s}Logo.png" >/dev/null
done
sips -z 50 50 "${MASTER}" --out "${OUT}/StoreLogo.png" >/dev/null

echo "==> .ico (Windows)"
python3 - "${OUT}" <<'PY'
import os, struct, subprocess, sys, tempfile
out = sys.argv[1]
sizes = [16, 32, 48, 64, 128, 256]
tmp = tempfile.mkdtemp()
blobs = []
for s in sizes:
    p = os.path.join(tmp, f"{s}.png")
    subprocess.run(["sips", "-z", str(s), str(s), os.path.join(out, "icon.png"),
                    "--out", p], check=True, capture_output=True)
    blobs.append(open(p, "rb").read())
#	A PNG-compressed ICO, which every Windows since Vista reads. Writing BMP
#	entries by hand would mean an AND mask and bottom-up rows for no gain.
hdr = struct.pack("<HHH", 0, 1, len(sizes))
offset = 6 + 16 * len(sizes)
entries, data = b"", b""
for s, b in zip(sizes, blobs):
    entries += struct.pack("<BBBBHHII", s if s < 256 else 0, s if s < 256 else 0,
                           0, 0, 1, 32, len(b), offset)
    offset += len(b)
    data += b
open(os.path.join(out, "icon.ico"), "wb").write(hdr + entries + data)
print(f"    icon.ico: {len(sizes)} sizes, {len(hdr+entries+data)} bytes")
PY

echo "==> done"
ls -la "${OUT}" | awk 'NR>3 {printf "    %-24s %s\n", $9, $5}'
