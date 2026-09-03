#!/bin/bash
# Drive the overlay by hand, with no plug-in and no After Effects involvement.
#
# The three gate properties that no probe can reach — click-through, the
# settings window taking the keyboard, and behaviour across two displays — are
# all "does this feel right with AE open" questions. This puts the overlay up
# and gives you the four messages the plug-in would send, so you can answer
# them without building the plug-in first.
#
#   ./Mac/overlay_drive.sh
#
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
EXE="$HERE/poc/overlay/src-tauri/target/release/pieFX-overlay"
[ -x "$EXE" ] || { echo "no overlay binary at $EXE"; echo "build: cd poc/overlay/src-tauri && cargo build --release"; exit 1; }

E="${TMPDIR:-/tmp}/pieFX-drive.$$.events"
A="${TMPDIR:-/tmp}/pieFX-drive.$$.actions"
rm -f "$E" "$A"; mkfifo "$E" "$A"

cleanup() {
  # The echo loop first. It holds this script's stdout, so leaving it alive
  # hangs anything that pipes us into something else.
  [ -n "${ECHOER:-}" ] && kill "$ECHOER" 2>/dev/null
  [ -n "${OV:-}" ] && kill "$OV" 2>/dev/null
  exec 3>&- 2>/dev/null
  exec 4<&- 2>/dev/null
  rm -f "$E" "$A"
}
trap cleanup EXIT INT TERM

"$EXE" --events "$E" --actions "$A" --settings none &
OV=$!
echo "overlay pid $OV"

# The events end stays open for the whole session: closing it between messages
# would send the overlay an EOF and drop it back into its reconnect loop.
exec 3>"$E"
# A reader on the actions end, because the overlay's write-open BLOCKS until one
# exists. Anything the overlay fires is echoed, indented.
exec 4<>"$A"
( while IFS= read -r l <&4; do echo "    <- $l"; done ) &
ECHOER=$!

send() { printf '%s\n' "$1" >&3; }

cat <<'HELP'

  s [x y]   summon the wheel at screen x,y (default 900 500) and LEAVE IT UP
  c x y     move the cursor to x,y
  r         release — fires whatever is selected
  g         open the settings window
  q         quit the overlay (the path AE's death hook takes)
  h         this help
  <ctrl-d>  stop driving and kill the overlay

HELP

while IFS= read -r -p "drive> " line; do
  set -- $line
  case "${1:-}" in
    s) send "{\"type\":\"summon\",\"x\":${2:-900},\"y\":${3:-500},\"hasSelection\":true,\"hasComp\":true,\"layerCount\":1}" ;;
    c) send "{\"type\":\"cursor\",\"x\":${2:-900},\"y\":${3:-500}}" ;;
    r) send '{"type":"release"}' ;;
    g) send '{"type":"settings"}' ;;
    q) send '{"type":"quit"}'; sleep 1; break ;;
    h) echo "  s [x y] | c x y | r | g | q | ctrl-d" ;;
    "") ;;
    *) echo "  ? $1" ;;
  esac
done
echo "done"
