# pieFX — macOS Phase 0 results

Everything in `MAC_SESSION.md` was written on Windows and had never been
compiled or run. It has now been built and run on macOS. **All four stations
pass.** macOS gets the real hold gesture, not the fallback hotkey — and it
needs no system permission.

Machine: Apple silicon, macOS 26.1, Xcode 26.1, After Effects 2026 (Spanish).

| Spike | Question | Result |
|---|---|---|
| S1 | Does the AEGP layer load at all? | **Pass** |
| S5 | Control: pure AEGP that passed on Windows | **Pass** — walked 454, claimed 454 |
| S4 | The right-hold gesture | **Pass** — detection *and* swallow |
| S3 | Overlay above AE, and focus vs. selection | **Pass** — both halves |

---

## The Xcode project

`MAC_SESSION.md` left this unwritten, on the grounds that hand-writing a
`project.pbxproj` blind was not worth it. It is now written, at
`Mac/pieFXMac.xcodeproj`, built from the Persisto sample exactly as that
file described. It compiled on the first attempt.

`Mac/build_and_install.sh` is one command per test cycle: build, verify the
export symbol, verify the PiPL, refuse to install while AE is running, install.
The copy needs `sudo` because AE's `Plug-ins` folder is root-owned.

The three checks that file said to make by hand, made automatically instead:

- `nm -gU` shows a bare `_EntryPointFunc`, not a mangled `__Z...`.
- The PiPL carries `8BIMkind` = `AEgx` plus `mi64` and `ma64` code descriptors.
- The bundle is a universal binary with `PkgInfo` = `AEgxFXTC`.

---

## S4 — what the bench found that the plan did not

### Detection: settled, decisively

**23 holds out of 23, every one `via timer`, all between 202 and 211 ms**
against a 200 ms threshold. Not one fell through to the drag fallback.

So `dispatch_after` on the main queue keeps running during a press. Cocoa's run
loop is not Windows' modal loop, and the `CFRunLoopTimer` contingency in
`MAC_SESSION.md` is not needed.

### A tracking menu eats the mouse-UP

The first watch-only run logged **23 right-button DOWNs and exactly 1 UP.**

Once AE opens a context menu, NSMenu's tracking loop consumes mouse events
before any `addLocalMonitorForEventsMatchingMask` handler sees them. The single
press that *did* report an UP was at `(1308, 448)`, well away from the
`y ≈ 760–800` band where the other 22 landed — presumably a spot with no
context menu, so nothing opened and nothing swallowed the release.

Two consequences:

1. It confirms AE raises its context menu on **mouse-DOWN**, as on Windows. A
   menu can only eat the UP if it is already tracking.
2. **Swallow is not optional.** Watch-only mode can never observe the end of a
   press, which is why that run logged zero short clicks. Swallowing the DOWN
   prevents the menu, which prevents the tracking loop, which makes the UP
   visible again.

### The permission premise was half wrong

The top of `pieFXMac.mm` justified choosing a local monitor over a
`CGEventTap` partly on the grounds of "no Accessibility permission prompt."
macOS raised that prompt the moment swallow was first armed.

The split turned out to be the useful part:

- `addLocalMonitorForEventsMatchingMask` — **free.** Watching and swallowing ran
  unprompted through every hold and drag. This half of the premise held.
- `CGEventPost` — **gated.** The *replay* tripped it. Synthesising input at the
  HID layer is gated on `AXIsProcessTrusted`, and being in-process is no
  exemption.

The prompt names **After Effects**, not the plug-in — a plug-in cannot hold its
own TCC entitlement. Shipping it would have meant every user granting
Accessibility to AE before a short right-click behaved normally, and users on
managed Macs being unable to at all.

### The fix, and how it was verified

The replay no longer synthesises anything. The original `NSEvent` is retained
the instant it is swallowed and handed straight back to AE's own queue:

```objc
[NSApp postEvent:S_swallowed_down atStart:NO];
[NSApp postEvent:upE              atStart:NO];
```

Order matters: `postEvent` appends to the queue, while a value returned from the
monitor is dispatched in the *current* cycle. So the UP must be posted after the
DOWN rather than returned, or AE sees the release before the press.

Verifying this needed care, because Accessibility had already been granted by
then — neither path would prompt again, so watching for a prompt proved nothing.
The permission was revoked and the alerts made to report `AXIsProcessTrusted()`,
so the precondition was measured rather than assumed. Within one AE session the
recorded trust state flips mid-log:

```
right button UP after 138ms - short CLICK #2
<< re-posted the ORIGINAL DOWN+UP via NSApp postEvent (#2, AE is NOT trusted)
```

With the permission revoked: no prompt, no `REPLAY SKIPPED`, no `REPLAY FAILED`,
and **AE's context menu still opened under the cursor.** That last part is the
half no log can see, and the half that decides it — silent discard would have
produced an identical log.

**The Accessibility requirement is gone from the product.**

### What the result deleted

- The whole `CGEventPost` replay path, and the mode toggle that existed only to
  compare the two.
- The primary-screen `NSPoint → CGPoint` flip. A re-posted event carries its own
  true window, view and location, so there is no coordinate maths left to get
  wrong.
- **S4·3d, the multi-monitor test, with it.** The failure mode it existed to
  catch cannot occur any more.

---

## S3 — overlay

The ring sat above everything at `NSStatusWindowLevel`;
`NSScreenSaverWindowLevel` was never needed. With the counter fixed (below), the
**selection survived the focus round-trip**.

So selection is document state, not focus state, on macOS as on Windows. The
design's gate on selection state is safe on both platforms.

---

## Three bugs the bench caught

**1. `CountSelection` measured nothing.** It walked project items from
`AEGP_GetFirstProjItem(NULL, …)`. `NULL` is not a valid `AEGP_ProjectH` — the
only other call site in the SDK passes one from `AEGP_GetProjectByIndex` — so
the first call set `err`, the loop never ran, and it returned 0 unconditionally.
It was also counting the wrong thing: project-panel items, where the Windows
spike counts selected layers in the active comp. Now ported from Windows
verbatim, so both platforms compare the same number.

The first run duly reported "0 before, 0 after" on a comp full of selected
layers, and `SELECTION SURVIVED` was true for the wrong reason. A measurement
that cannot fail is not a measurement.

**2. The replay guard would have desynced.** It counted down from 2, expecting
its injected DOWN and UP both to return. The 23-to-1 finding says the UP usually
will not, so the counter would have stuck at 1 and silently eaten the next real
press. Replaced with a 300 ms deadline, which cannot desync.

**3. Four `%ld` against a 32-bit `A_long`.** Caught at compile time. On arm64
varargs that reads eight bytes from a four-byte slot, so S5's `walked` and
`claimed` could have printed garbage in the high bits — a false MISMATCH on the
one comparison S5 exists to make.

---

## Windows hypotheses, settled

| Claim | Verdict on macOS |
|---|---|
| Selection is document state, not focus state | **Confirmed** — survived the focus round-trip |
| Nothing can identify an AE panel | **Moot** — the design gates on selection state |
| AE opens its context menu on mouse-DOWN | **Confirmed** — swallowing the DOWN suppresses it |
| AE's modal drag loop starves the idle hook | **False here** — 23/23 via `dispatch_after` |
| A local monitor sees the whole press | **False, and new** — a tracking menu eats the UP |
| A local monitor needs no Accessibility permission | **True as rebuilt** — false while the replay synthesised events |

## The gate

| S2 | S4 | Outcome |
|---|---|---|
| pass | **pass** | **The full product. Hold gesture on both platforms.** |
| pass | fail | Hold on Windows, hotkey on macOS. *Not the branch we are on.* |

---

---

# The overlay — MAC_PORT.md step 1

The one real unknown, measured. `MAC_PORT.md` said the Tauri overlay had never
been launched on a Mac, that four properties had to hold, and that everything
else on the page was conditional on them. **All four hold.** One of them only
holds after a design change, and that change is the finding worth carrying.

Machine: Apple silicon, macOS 26.1, After Effects 2026. Two displays with
DIFFERENT scale factors — built-in Retina at 2x, a BenQ GW2780 at 1x — which
turned out to be the configuration that mattered.

| Property | Verdict |
|---|---|
| transparent, borderless | **Pass**, but it needs a private API |
| click-through | **Pass**, drawn pixels included |
| all displays, one coordinate space | **Pass — by moving, not spanning** |
| above AE, without stealing focus | **Pass**, once the app stopped activating |

The product logic came along with it. `poc/pipe_test.py` — the macOS harness,
written before the transport it tests, as that page argued for — passes every
assertion the PowerShell does: all eight strokes resolve to the same commands
as Windows, all five context-gating cases, both search-window cases, cancel,
and the `quit` path.

---

## Transparency is gated on a private API

`WebviewWindowBuilder` has **no `transparent` method on macOS** unless Tauri is
built with the `macos-private-api` feature. Without it the build does not
compile, and the main window's `"transparent": true` is inert. Both are now
set, in `Cargo.toml` and `tauri.conf.json`.

It is named accurately: Apple rejects App Store submissions carrying it. That
costs nothing here — the overlay is a helper binary launched by an AE plug-in,
not an App Store product — but it belongs in the distribution question
`MAC_PORT.md` defers, because it forecloses one distribution route entirely.

## The window level decides two things at once

Tauri's `alwaysOnTop` gives level 5. That is above After Effects' windows
(level 0), so the "above AE" half looked fine. It is BELOW the main menu bar
(24), and **AppKit constrains any window at or below that level to sit under
the menu bar**.

The probe caught it. A canvas asked for `{0,0}` came back at `{0,34}`:

```
overlay window:  {{0, 34}, {1512, 982}}   on a {{0,0},{1512,982}} screen
```

So the window hung 34pt off the bottom of the screen, the bottom strip was
uncovered, and every screen-to-local conversion in the frontend was displaced
by the height of the menu bar. On Windows the same code places the canvas
exactly, which is why nothing upstream anticipated it.

`NSStatusWindowLevel` (25) is above the menu bar, so the constraint does not
apply — and it is the level S3 measured sitting above After Effects. One change
answers both properties. It has to run BEFORE the frame is set: a constrained
frame is not retroactively released by a later level change.

## The overlay was taking the foreground from AE

`"focus": false` is set on the main window, and it was not enough. Measured
with `NSWorkspace.frontmostApplication`, before and after launching it:

```
frontmost: After Effects        ->   frontmost: pieFX-overlay
```

The setting governs the WINDOW. What activated was the APPLICATION — a
non-bundled binary that creates a window becomes a regular, activatable app.

`NSApplicationActivationPolicyAccessory` fixes it: no Dock icon, no cmd-tab
entry, and no activation merely from showing a window. The windows still draw,
which is all the wheel needs, because the plug-in owns the mouse.

It does not lose the ability to take focus deliberately. `raise()` — the
settings and search path — now calls `activateIgnoringOtherApps:`, which is
exactly the mirror of the `AttachThreadInput` dance on the Windows side, and
for the same reason: an accessory app must ask, because it is deliberately not
activatable by the ordinary route. Confirmed by hand: the settings window comes
forward and takes the first keystroke, and no pieFX entry appears in the Dock
or in cmd-tab.

## One window CANNOT span two displays

**This is the design change, and it is the reason this section exists.**

The Windows overlay is one window covering the whole virtual desktop. macOS
declines. "Displays have separate Spaces" is ON by default
(`com.apple.spaces spans-displays` unset), and it clips a window to one screen.

The measurement is `Mac/span_test.swift`: a hand-built `NSWindow` at status
level, sized to the union of both screens, washed in translucent blue.
Deliberately NOT built on Tauri, whose own geometry was already suspect — a
Tauri window that failed to span would not have said whether Spaces or the
arithmetic did it.

```
union asked for:                 {{0, -98}, {3432, 1080}}
frame the window ACTUALLY got:   {{0, -98}, {3432, 1080}}
```

The window server **accepted the frame in full, unclamped — and then rendered
it on exactly one screen.** Frame acceptance and rendering are separate things,
and only the second one matters. A test that had checked the frame and stopped
would have reported a pass.

It rendered on the BenQ, not the built-in Retina — not the primary, and not the
screen holding the origin. So nothing should be built on an assumption about
WHICH screen a spanning window lands on either.

**The overlay now covers one screen and moves to the screen holding the summon
point.** No loss: the wheel is summoned at the cursor, and the cursor is on one
display. It is a simplification, because a window on a single screen has a
single scale factor — which is the other half of this story.

## The union was computed in mixed units

Before the spanning question could even be asked, the geometry was wrong.

Tauri reports each monitor in "physical px", which is that monitor's points
multiplied by **its own** scale factor:

```
Retina  pos (0,0)     size 3024x1964  scale 2
BenQ    pos (1512,0)  size 1920x1080  scale 1
```

Those two positions are not in the same space, so the union of them means
nothing. It came out 3432 wide; Tauri then converted back through the primary's
scale and halved it:

```
overlay window:  {{0, 0}, {1716, 982}}      <- 3432 / 2
BenQ occupies:   1512 -> 3432
```

The window overlapped the second display by a 204pt sliver. The wheel had
nowhere to draw, which is why it never appeared rather than appearing displaced.

Divided by each monitor's OWN scale, the same numbers tile exactly — top-left
origin, y down, the same convention as Windows:

```
Retina  (0, 0)     1512x982
BenQ    (1512, 0)  1920x1080
```

The frame is now set through `NSWindow setFrame:display:` in AppKit points
rather than Tauri's physical px. Tauri converts using a scale factor that
depends on where the window currently IS, so a move between a 2x and a 1x
screen would have to guess which scale applied to which half of the move.
Points have no such ambiguity.

## The protocol: macOS sends POINTS

Settled by the above, and it is a **prerequisite for the plug-in side**, not a
detail to be discovered during it.

macOS summon and cursor coordinates are **points, top-left origin** — the same
convention as Windows, a different unit. `NSEvent` gives the plug-in points, so
nothing converts. CSS px ARE points, so the frontend divides by nothing:
`overlay_origin` returns a third value saying whether `devicePixelRatio`
applies, Rust decides it, and the JS does not sniff the platform.

Dividing by `devicePixelRatio` on macOS would also have been a live hazard on
its own: it changes when a window moves between a 2x and a 1x screen, and
nothing guarantees it has updated by the time a summon draws.

The origin CHANGES per summon now, so it travels WITH the summon rather than
being fetched separately — a separate fetch would race the drawing it exists to
position. The move and the emit both happen on the main thread, in that order,
so the frontend can never draw against a stale frame. Confirmed by eye: the
first frame after switching screens is already in the right place.

Measured, across both screens:

```
summon (2400, 500)  -> window on BenQ    {{1512,0},{1920,1080}}  local 888,500
summon (300, 300)   -> window on Retina  {{0,0},{1512,982}}      local 300,300
```

And by hand, against the predicted screen centres — (756, 491) on the Retina,
(2472, 540) on the BenQ — both landed within eyeball error.

## What this deleted

- The virtual-desktop union, on macOS. There is no virtual desktop to span.
- The `devicePixelRatio` division, on macOS, and the mixed-DPI arithmetic with
  it. The harness had already caught the smell: the same stroke selected anchor
  cell 4 on the Retina and cell 0 on the BenQ, because a fixed pixel distance
  meant different things on each. It now returns cell 0 on both, which is what
  its own label says it should.

## Tools left behind

- `poc/pipe_test.py` — the offline harness. The FIFO port of `pipe_test.ps1`,
  same assertions in the same order, so a divergence between platforms shows up
  as a divergence in the test. Two transport differences are commented where
  they live: a FIFO's two halves are not symmetric (`O_WRONLY` fails with
  `ENXIO` until a reader exists and is therefore a real connection event, while
  `O_RDONLY` proves nothing), and `SIGPIPE` has to be ignored explicitly where
  Windows just returned an error.
- `Mac/overlay_probe.swift` — screens, window levels, alpha, bounds, and the
  overlay-vs-AE z-order, with a verdict line. It says out loud that
  click-through is NOT observable through `CGWindowListCopyWindowInfo`, rather
  than appearing to cover four properties while covering three.
- `Mac/span_test.swift` — the Spaces measurement above.
- `Mac/overlay_drive.sh` — drives the overlay by hand with no plug-in and no
  AE: summon, cursor, release, settings, quit. What the by-eye checks used.

---

# The transport — MAC_PORT.md step 3, first half

The `mkfifo` pair, written and proven against the real overlay with **no After
Effects anywhere**. `poc/native/mac/pieFX_fifo.cpp`, ~450 lines, deliberately
free of AEGP so it builds and runs alone.

`poc/pipe_test.py` drives the OVERLAY — it plays the plug-in's part. This is
the mirror, and the half that had no harness on either platform:
`poc/native/mac/fifo_test.cpp` runs the real plug-in transport against the real
overlay binary. Thirteen assertions, stable across repeated runs.

```bash
./poc/native/mac/build_fifo_test.sh && $TMPDIR/pieFX_fifo_test
```

It paid for itself on the first run, which is the whole argument for writing it
before the transport it tests.

## poll() does not wake when the last writer closes

**The bug the harness caught.** On Windows the client going away makes the
parked `ReadFile` return 0, and that is how a disconnect is noticed. There is
no equivalent here.

Measured: a `poll()` parked on the read end of a FIFO whose last writer has
closed **never returns**. The server thread sat in it through a killed overlay,
a relaunched one, and every assertion in between, and only came out when the
stop byte arrived. Re-accept failed, so an overlay that went and came back was
never picked up again — which is arm/disarm/arm, the case the whole
single-overlay rule exists for.

The fix does not wait to be told. `poll` gets a 250ms cadence, and a departed
overlay is noticed by asking directly: **`O_WRONLY` on a FIFO fails with
`ENXIO` when there is no reader**, so no reader on the events FIFO means no
overlay. It is the same primitive the accept uses, asked in the other
direction.

That `read() == 0` is ambiguous, and the two cases are opposites: the overlay
has GONE, or it has opened events and not yet opened actions — the gap between
its two opens, which happens on every single connect. The same probe tells them
apart.

## The bounded write survives the port

`b9a73eb` made the Windows TX overlapped with a deadline, because a synchronous
`WriteFile` to an overlay that had stopped reading froze AE until someone
killed the overlay by hand. The same guarantee is needed here and costs less
machinery: the fd is already `O_NONBLOCK`, so a full pipe returns `EAGAIN`
instead of parking, and `poll()` supplies the deadline.

Tested the way the fault actually happens — `SIGSTOP` on the overlay, which
stops it reading while leaving it alive and the FIFO whole, the exact state a
synchronous write never returns from:

```
pipe: write timed out after 1000ms, treating the overlay as gone
PASS  a stalled overlay makes the write FAIL rather than hang — gave up after 1000ms
```

One deadline covers the whole line rather than each `write()` call: a slow
reader draining a byte at a time would otherwise renew it forever.

## SIGPIPE, and a decision taken inside somebody else's process

Writing to a pipe with no reader raises `SIGPIPE`, whose default action is to
**terminate the process** — and the process is After Effects. The identical
condition on Windows is an error return, so nothing in the original anticipates
it.

It is ignored process-wide, which deserves saying out loud rather than being
buried. The alternative — block it per-thread and drain it with `sigtimedwait`
— would have to run on AE's UI thread, since that is where `PipeWrite` is
called from, and leaving a signal pending on AE's UI thread is the more
invasive of the two. `MSG_NOSIGNAL`, which would avoid the question entirely,
is a socket facility and does not apply to FIFOs; it is the standing argument
for the Unix domain socket fallback `MAC_PORT.md` keeps in reserve.

Asserted directly: kill the overlay, write, and check we are still running.

## Names: a FIFO outlives the process that made it

`ResolvePipeNames` on Windows tries the base names and lets a second AE fall
back to a pid-suffixed pair. A named pipe exists only while its server holds
it, so `CreateNamedPipe` simply fails when another AE has it.

**A FIFO is a file.** It outlives its maker, so its existence proves nothing —
a crashed AE leaves one behind, and treating that as "taken" would push every
later session onto pid names forever.

So the liveness question is asked of a **lock**, not of the FIFOs:
`flock` on `$TMPDIR/pieFX.lock`, which the kernel releases when the holder
dies, crash included — exactly the property the FIFO lacks.

The base names are `$TMPDIR/pieFX.events` and `$TMPDIR/pieFX.actions`, and the
overlay carries matching defaults so an overlay started by hand still connects.
That agreement is the one place the two sides must not drift, and the symptom
of drift is an overlay that starts and silently never connects — so the harness
asserts it, by launching the overlay with no `--events`/`--actions` at all.

FIFOs are also removed on stop. Windows gets that for free.

## A phantom connection, and a hypothesis that was wrong

After the overlay exits, a reader reappears on the events FIFO about a second
later, and the server accepts it as a connection.

The first guess was a WebKit child inheriting the read fd and outliving its
parent — the macOS shape of what the Windows job object prevents. **That guess
was wrong, and it is recorded here because it was written down before it was
checked.** Measured: nothing survives the overlay's exit — `lsof` shows no
holder of the FIFO, `pgrep` finds no process — and Rust's `File::open` sets
`O_CLOEXEC` anyway, so no child could have inherited it in the first place.

What is left is the dying overlay's own reconnect loop briefly reopening events
on its way out. It is benign: the next liveness probe, at most 250ms later,
finds no reader and drops the connection again.

It is not harmless to a TEST, though, and that is the part worth keeping. A
connection assertion that runs while a phantom is up passes without proving
anything — which is exactly what happened, and it hid a launch that had failed
outright. See below.

---

# Launch and lifetime — MAC_PORT.md step 3, second half

The overlay is now started and stopped by the plug-in side
(`poc/native/mac/pieFX_launch.cpp`), and it outlives nothing. Nineteen
assertions in `fifo_test`, stable across repeated runs.

## The job object splits in two

Windows leans on ONE mechanism: a job object with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. It covers both ways After Effects can
go — quitting properly and crashing — because the kernel kills the job when
the last handle closes, and process exit closes handles either way.

macOS has no equivalent, so the guarantee is assembled from two halves that
cover one case each, and **neither is sufficient alone** — which is worth
saying because either one looks sufficient in isolation:

| | covers | mechanism |
|---|---|---|
| process **group** | a deliberate teardown | `setsid` before `exec`, then `kill(-pid)` takes the WebKit children too |
| `--owner-pid` | AE **crashing** | the overlay watches that pid with `kqueue` and exits unprompted |

`setsid` runs in the child BEFORE `exec`, for the same reason Windows creates
the process suspended and assigns it to the job before resuming it: a child
born before the grouping is established is outside it, and outside it is the
leak the whole mechanism exists to stop.

## The watchdog, measured

`watch_owner` was a `#[cfg(not(windows))]` no-op stub. It is now `kqueue` with
`EVFILT_PROC` / `NOTE_EXIT`.

Windows waits on a process HANDLE, which is a thing you can hold. A unix pid is
not — it can be reused the moment it is reaped — so the equivalent has to be a
subscription registered with the kernel, which fires once for that specific
process. Registration failing with `ESRCH` means the owner is already gone:
not an error, just the answer arriving early.

Asserted by killing the owner with `SIGKILL` — no death hook, no message, no
warning — and watching the overlay leave on its own:

```
owner pid 7989
owner 7989 exited -> quitting
```

## Two bugs the test found in the code that was testing it

**1. A launch that failed reported success.** `fork()` succeeding says nothing
about the program starting. The child only discovers `execl` has failed after
the parent has moved on, and `_exit(127)` into the void is indistinguishable
from a healthy launch — so `PieFX_LaunchOverlay` returned 1 for an overlay that
did not exist. `access(X_OK)` passing beforehand is not proof the exec will.

Fixed with a `FD_CLOEXEC` status pipe: a successful exec closes it and the
parent reads EOF, a failure writes `errno` into it first. The parent now knows
which happened, and reaps the corpse either way.

**2. The connect assertions were vacuous.** They ran while the phantom
connection above was still up, so they passed without a fresh overlay ever
existing — which is what concealed bug 1 for a full debugging pass. Each now
asserts DISCONNECTED first, and only then launches.

Both are the same failure in different clothes: something reported success
without being asked to prove it.

## An environment note, not a product one

The test binary is built BESIDE the overlay, in `target/release`, rather than
into `$TMPDIR`. `PieFX_LaunchOverlay` finds the overlay next to the binary that
contains it — `dladdr`, the macOS answer to `GetModuleFileName` — so building
it there makes that lookup the real one instead of something faked with a
symlink. The first attempt did use a symlink in `$TMPDIR`, and the sandbox the
build runs in refused to exec through it (`EACCES`, while `access(X_OK)` said
yes) — which is how bug 1 came to light.

## Carried forward

**The Unicode accessors.** This AE runs in Spanish, and `AEGP_GetEffectName` /
`AEGP_GetEffectCategory` return single-byte legacy text, not UTF-8 — `file`
calls the S5 log "Non-ISO extended-ASCII". Harmless for a spike. The product
will need the Unicode accessors before those names reach a menu.

**Points, not pixels, on the plug-in side.** The overlay now expects macOS
summon and cursor coordinates in points, top-left origin. `NSEvent` gives
points, so the plug-in should send what it has and convert nothing. This is
settled before the transport is written, deliberately.

**`macos-private-api` and distribution.** The overlay cannot be transparent on
macOS without it, and it forecloses the App Store. Not a problem for a helper
binary beside a plug-in, but it belongs in the distribution question rather
than being rediscovered there.

**One screen at a time.** Nothing downstream should assume the overlay covers
the desktop. It covers the screen the cursor is on, and it moves.

## Reproducing

### The plug-in spikes

```bash
./Mac/build_and_install.sh
```

Quit After Effects first; the script refuses to run while it is up, because AE
only reads `Plug-ins` at launch. Then run the stations from the Window menu in
the order `MAC_SESSION.md` gives: S1, S5, S4, S3.

Logs are written to `$TMPDIR` — `pieFX_S4_gesture.txt` and
`pieFX_S5_effects.txt`. Note that turning the watch off and on truncates
the S4 log, so a swallow test has to happen inside a single watch session.

### The overlay

Needs Rust; Node is NOT needed, because `frontendDist` is a static directory
with no build step and `cargo` alone produces the binary.

```bash
cd poc/overlay/src-tauri && cargo build --release
```

Then, with no plug-in and no After Effects involved:

```bash
python3 poc/pipe_test.py     # the offline harness, all assertions
./Mac/overlay_drive.sh       # drive it by hand: s / c / r / g / q
swift Mac/overlay_probe.swift    # levels, bounds, z-order vs AE
swift Mac/span_test.swift        # the Spaces measurement
```

The overlay writes to `$TMPDIR/piefx_overlay.log`. It is a windowed process
with no console, so that file is the only way to tell "the page never loaded"
from "the page loaded but the action never fired" — and it was silently empty
on macOS until `dlog` learned that `TEMP` is a Windows name and this platform
sets `TMPDIR`.

Two properties can only be checked by eye, and both need AE open:
click-through (click AE THROUGH a summoned wheel, not merely around it), and
the settings window taking the first keystroke rather than being visible but
keyboard-dead.
