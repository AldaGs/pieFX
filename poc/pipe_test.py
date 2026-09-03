#!/usr/bin/env python3
"""Consolidation harness, macOS. The port of pipe_test.ps1.

Same assertions, same strokes, same order — deliberately, because the point of
this file is to be the SAME test over a different transport. Where it differs
from the PowerShell it is because a FIFO differs from a named pipe, and each of
those places is commented.

Run it with After Effects closed and no plug-in involved: it is the offline
harness, and the whole reason it exists is to catch transport bugs in a shell
rather than in a session with AE open.

    python3 poc/pipe_test.py [path-to-pieFX-overlay]
"""

import json
import os
import select
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# NON-default names, as on Windows: this is the path a second AE instance takes,
# and running it here means the harness cannot collide with an overlay that a
# live After Effects already owns.
RUN = f"pieFX-test99-{os.getpid()}"
TMP = os.environ.get("TMPDIR", "/tmp")
EVENTS = os.path.join(TMP, RUN + ".events")
ACTIONS = os.path.join(TMP, RUN + ".actions")
OVERLAY_LOG = os.path.join(TMP, "piefx_overlay.log")

DEFAULT_EXE = os.path.join(
    HERE, "overlay", "src-tauri", "target", "release", "pieFX-overlay"
)

fails = 0


def out(s):
    print(s, flush=True)


def fail(s):
    global fails
    fails += 1
    out(s)


# --- the transport ----------------------------------------------------------
#
# A named pipe has WaitForConnection. A FIFO has open() semantics instead, and
# the two halves are not symmetric:
#
#   O_WRONLY  fails with ENXIO until a READER exists. So a non-blocking write-
#             open that succeeds is a real connection event, and it is the
#             direct equivalent of WaitForConnection on the TX side.
#   O_RDONLY  succeeds immediately whether or not a writer exists. There is no
#             open-level proof of the far end on the RX side, so the proof used
#             here is the app level one: the first byte the overlay sends.
#
# Both of these are why MAC_PORT.md says a blocking open must never be on the
# UI thread. Nothing here blocks; every wait below has a deadline.
def wait_tx(path, timeout):
    """Open the events FIFO for writing. Returns an fd, or None on timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        except OSError:
            # ENXIO: no reader yet. The overlay opens events BEFORE actions, so
            # this is also what "the overlay has not got there yet" looks like.
            time.sleep(0.05)
    return None


class Rx:
    """The actions FIFO, read with a deadline.

    Opened O_RDWR rather than O_RDONLY on purpose. A read-only FIFO reports EOF
    the moment its last writer closes, and the overlay's writer comes and goes
    across a reconnect; holding a write handle of our own keeps the fd open so
    select() means "data" and never "the far end blinked".
    """

    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        self.buf = b""
        self.saw_data = False

    def readline(self, timeout):
        """One line, or "" if none arrived inside the timeout.

        Unlike the PowerShell there is no pending-task to carry: a timed-out
        read on an fd leaves nothing behind, so Fire and Expect can share this
        without the "stream is currently in use" dance.
        """
        deadline = time.time() + timeout
        while True:
            if b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                return line.decode("utf-8", "replace").strip()
            remaining = deadline - time.time()
            if remaining <= 0:
                return ""
            r, _, _ = select.select([self.fd], [], [], remaining)
            if r:
                chunk = os.read(self.fd, 65536)
                if chunk:
                    self.saw_data = True
                    self.buf += chunk

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass


def send(fd, obj):
    """A write with no reader raises SIGPIPE, which on Windows is just an error
    return. Python turns it into BrokenPipeError because main() disables the
    default handler — the plug-in will have to do the same explicitly."""
    try:
        os.write(fd, (json.dumps(obj) + "\n").encode("utf-8"))
    except BrokenPipeError:
        fail("  FAIL: overlay closed the events FIFO (SIGPIPE on write)")


# --- the strokes ------------------------------------------------------------
SUMMON = {"type": "summon", "x": 800, "y": 500, "hasSelection": True, "layerCount": 1}
NO_SEL = {"type": "summon", "x": 800, "y": 500,
          "hasSelection": False, "hasComp": True, "layerCount": 0}
NO_COMP = {"type": "summon", "x": 800, "y": 500,
           "hasSelection": False, "hasComp": False, "layerCount": 0}
TWO_SEL = {"type": "summon", "x": 800, "y": 500,
           "hasSelection": True, "hasComp": True, "layerCount": 2}


def cursor(x, y):
    return {"type": "cursor", "x": x, "y": y}


def stroke(tx, rx, steps):
    for s in steps:
        send(tx, s)
        time.sleep(0.220)
    send(tx, {"type": "release"})


def fire(tx, rx, label, steps):
    stroke(tx, rx, steps)
    line = rx.readline(8.0)
    if line:
        out(f"  {label} -> {line}")
    else:
        fail(f"  {label} -> FAIL (nothing)")


def expect(tx, rx, label, steps, want):
    """Silence is the PASS for a gated slot, so it gets its own runner rather
    than reading as a failure."""
    stroke(tx, rx, steps)
    got = rx.readline(3.0)
    ok = (got == "") if want == "silence" else (got != "")
    if ok:
        out(f"  {label} -> PASS  {got}")
    else:
        fail(f"  {label} -> FAIL  {got}")


def log_count(needle):
    if not os.path.exists(OVERLAY_LOG):
        return 0
    with open(OVERLAY_LOG, "r", encoding="utf-8", errors="replace") as f:
        return sum(1 for ln in f if needle in ln)


def main():
    # Never die on a write to a FIFO the overlay has closed; surface it instead.
    signal.signal(signal.SIGPIPE, signal.SIG_IGN)

    exe = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE
    if not os.path.exists(exe):
        out(f"FAIL: no overlay binary at {exe}")
        out("  build it first: cd poc/overlay/src-tauri && cargo build --release")
        return 1

    for p in (EVENTS, ACTIONS):
        if os.path.exists(p):
            os.unlink(p)
        os.mkfifo(p, 0o600)

    # `--settings none` pins the overlay to its BUILT-IN defaults, and disables
    # the recents file so a run of this script cannot write into the
    # developer's own recents. `--effects` does the same for the catalogue: the
    # search opens on a fixture carrying each of its sharp edges rather than on
    # whatever this machine has installed. Without both, a failure would read as
    # a transport bug, which is the one thing this script exists to catch.
    fixture = os.path.join(HERE, "overlay", "src", "effects-sample.json")
    argv = [exe,
            "--events", EVENTS, "--actions", ACTIONS,
            "--settings", "none", "--effects", fixture]

    # Its own process group, so the cleanup below can take the whole tree. The
    # overlay spawns WebKit children, and one left alive holds the release
    # binary and makes the next cargo build fail.
    proc = subprocess.Popen(argv, start_new_session=True)
    out(f"overlay pid {proc.pid} (custom FIFO names)")

    tx = wait_tx(EVENTS, 15.0)
    if tx is None:
        fail("FAIL: TX never opened (overlay did not open the events FIFO)")
        proc.kill()
        return 1
    rx = Rx(ACTIONS)
    out("PASS: connected on custom names (second-instance path works)")

    # DISTANCE MATTERS. Under the shipped `distance` arming rule the radius is
    # the depth and the threshold is the centre hexagon's edge (54px), so every
    # stroke that leaves the dead zone selects a child, and the labels say so.
    # A category's DEFAULT action is deliberately not covered: the band that
    # reaches it is about four pixels wide, so it is not reachable by hand and
    # there is nothing honest to assert about it.
    #
    # Comp's default and its S child are the SAME command, so a straight-out S
    # stroke cannot tell those two apart and is not asked to; the second leg
    # turns N instead, to `Comp Settings`, which nothing else can produce.
    fire(tx, rx, "S > Queue to Render ", [SUMMON, cursor(800, 600)])
    fire(tx, rx, "S > Comp Settings   ", [SUMMON, cursor(800, 800), cursor(800, 200)])
    # S then NE: `Comp > Copy to Clipboard`. A builtin, so what crosses the FIFO
    # is a name and nothing else — the frame, the PNG and the clipboard all
    # happen on the plug-in side, where the clipboard is.
    fire(tx, rx, "Comp > Copy Frame   ", [SUMMON, cursor(800, 800), cursor(973, 400)])
    fire(tx, rx, "NE > Area Center    ", [SUMMON, cursor(887, 450)])
    fire(tx, rx, "NE drill -> N     ",
         [SUMMON, cursor(1060, 350), cursor(802, 502), cursor(800, 200)])
    fire(tx, rx, "NW anchor c0      ",
         [SUMMON, cursor(540, 350), cursor(802, 502), cursor(760, 460)])
    fire(tx, rx, "SW layer -> N     ",
         [SUMMON, cursor(540, 650), cursor(802, 502), cursor(800, 200)])
    # The case the arming change was made for: ONE unbroken outward stroke into
    # the child that lies in the parent's own direction.
    fire(tx, rx, "SE straight through ",
         [SUMMON, cursor(870, 540), cursor(1060, 650)])

    # The mirror of that stroke: the centre still cancels, even though under
    # `distance` the centre is INSIDE the arming radius rather than the thing
    # that arms you.
    out("cancel:")
    expect(tx, rx, "SE opened, released in centre ",
           [SUMMON, cursor(1060, 650), cursor(803, 501)], "silence")

    # Same strokes, summoned with nothing selected. A slot that needs a
    # selection must stay silent; one that needs nothing must still fire.
    out("context gating:")
    expect(tx, rx, "no-sel NE (Master Null, dead) ", [NO_SEL, cursor(1060, 350)], "silence")
    expect(tx, rx, "no-sel S  (Render Queue, live)", [NO_SEL, cursor(800, 800)], "fire")
    expect(tx, rx, "no-sel SE>S (Comp, live)      ",
           [NO_SEL, cursor(1060, 650), cursor(802, 502), cursor(800, 800)], "fire")
    expect(tx, rx, "no-comp SE>N (Solid, dead)    ",
           [NO_COMP, cursor(1060, 650), cursor(802, 502), cursor(800, 200)], "silence")
    expect(tx, rx, "no-comp S (Render Queue, dead)", [NO_COMP, cursor(800, 800)], "silence")

    # N is `Effects`, the one slot whose release does NOT cross the FIFO: it
    # opens a focused window in this process, because a search needs a keyboard.
    # So silence on the action channel is only half the assertion — silence is
    # also what a slot that fires nothing looks like. The other half is the
    # overlay's own log saying the window was built.
    out("effect search:")
    before = log_count("search window")
    expect(tx, rx, "N Effects (nothing on the FIFO)", [SUMMON, cursor(800, 200)], "silence")
    time.sleep(1.2)
    if log_count("search window") > before:
        out("  search window opened (overlay log)  -> PASS")
    else:
        fail("  search window opened (overlay log)  -> FAIL (no log line; the release fired nothing)")

    # Two layers selected: the search must NOT open. The plug-in applies an
    # effect through AEGP_GetActiveLayer, which returns a layer only when
    # exactly one is selected, so a search that opened here would take a query
    # and an Enter and then apply nothing. Both halves are asserted, because
    # silence alone was also what the old mock did.
    before2 = log_count("search window")
    expect(tx, rx, "N Effects, 2 layers selected  ", [TWO_SEL, cursor(800, 200)], "silence")
    time.sleep(1.0)
    if log_count("search window") == before2:
        out("  no window opened for a multi-selection -> PASS")
    else:
        fail("  no window opened for a multi-selection -> FAIL (it opened anyway)")

    # The toast channel: nothing comes back, it is one-way to the user.
    send(tx, {"type": "toast", "level": "error", "text": "_mn is undefined"})
    out("  toast sent (check overlay log for receipt)")
    time.sleep(0.8)

    if not rx.saw_data:
        fail("FAIL: nothing ever arrived on the actions FIFO (RX half never connected)")

    # The quit message: the overlay must go on its own, while the FIFO is still
    # whole. This is the path AE's death hook takes, and it is the one that has
    # to work — terminating a process blocked in a read on a half-dead pipe
    # leaves it un-dead, which is exactly what happened in AE.
    send(tx, {"type": "quit"})
    try:
        proc.wait(timeout=5)
        out("PASS: overlay quit on request")
    except subprocess.TimeoutExpired:
        fail("FAIL: overlay ignored quit; falling back to kill")

    os.close(tx)
    rx.close()

    # Kill the GROUP, not just the launcher.
    if proc.poll() is None:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except OSError:
            pass
        try:
            proc.wait(timeout=4)
        except subprocess.TimeoutExpired:
            pass
    for p in (EVENTS, ACTIONS):
        if os.path.exists(p):
            os.unlink(p)

    # Only THIS run's overlay is this run's business. Counting every
    # pieFX-overlay on the machine would warn about the perfectly healthy one an
    # open After Effects owns — a harness that cries transport bug at a normal
    # desktop is the same failure as one that passes for the wrong reason,
    # pointed the other way.
    if proc.poll() is None:
        fail(f"WARNING: this run's overlay ({proc.pid}) is still running")
    else:
        out("no stray overlay from this run")
    others = subprocess.run(["pgrep", "-x", "pieFX-overlay"],
                            capture_output=True, text=True).stdout.split()
    others = [o for o in others if int(o) != proc.pid]
    if others:
        out(f"  note: {len(others)} other overlay(s) alive ({', '.join(others)})"
            " - an open After Effects owns one; not this run's")

    out(f"done ({fails} failure(s))" if fails else "done")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
