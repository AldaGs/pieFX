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
overlay binary. Thirteen assertions at the time; twenty-three now, with launch
and lifetime folded in. Stable across repeated runs.

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
(`poc/native/mac/pieFX_launch.cpp`), and it outlives nothing. Twenty-three
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

---

# The gesture — MAC_PORT.md step 4

Moved out of the frozen Phase 0 spike into a product-shaped module,
`poc/native/mac/pieFX_gesture.mm`. The mechanism is not new work — S4 measured
it at 23 holds out of 23, swallow and replay included, with no Accessibility
permission — so this is proven code changing address, with the product's
semantics attached.

**Not yet inside the plug-in.** `Mac/pieFXMac.xcodeproj` still builds the Phase
0 spike rather than `poc/native/pieFX.cpp`, and standing up a macOS target for
the product plug-in is its own job. What exists now is the module, armed and
exercised in a host that is not After Effects.

## The state machine is the Windows one, deliberately

DOWN starts a clock and is swallowed; a press that outlives the threshold is
ours; a press that does not is handed back. Ported call for call from
`MouseProc`, including the parts that are easy to lose:

- **Raw cursor only, de-duplicated.** The overlay owns the wheel geometry and
  does its own hit-testing; this side deliberately has no opinion about which
  slot is under the cursor.
- **The release decides nothing.** The overlay knows which slot the cursor is
  on and sends back a finished action. Deciding here as well would fire twice.
- **Drags are swallowed too**, not just the DOWN and UP: while a press is ours,
  AE must not see the drag either.

What differs is every mechanism underneath, and each one is argued where it is
made: a local monitor instead of `SetWindowsHookEx`, `dispatch_after` with a
generation counter instead of `SetTimer`/`KillTimer` (a dispatch block cannot
be cancelled, so a stale one is invalidated rather than stopped), and the
original `NSEvent` re-posted instead of `SendInput`.

Two Phase 0 findings are carried in as code rather than as comments:

- **Swallow is not optional**, so there is no toggle for it. AE's context menu,
  once open, eats the mouse-UP before any local monitor sees it — 23 DOWNs and
  exactly 1 UP. Swallowing the DOWN prevents the menu, which prevents the
  tracking loop, which is what makes the UP visible at all.
- **The replay guard is a deadline, not a count.** A count of 2 desyncs for the
  same reason: the replayed UP usually never comes back, the count sticks at 1,
  and the next real press is silently eaten as if it were ours.

## Coordinates: converted once, and checked against something else

AppKit hands out bottom-left points; the protocol is top-left. The flip happens
once, in `ToTopLeft`, against the PRIMARY screen's height — screens[0], the one
with the menu bar — because the top-left space is anchored to its top-left
corner. Not the screen the cursor is on, which would make the origin move with
the mouse.

Verified against an independent source rather than by restating the formula:
`CGEvent` reports the cursor in the top-left space directly.

```
screens[0] height (points): 982.0
NSEvent.mouseLocation (bottom-left): (898.94, 355.86)
CGEvent location     (top-left):     (898.94, 626.14)
my ToTopLeft formula (top-left):     (899.0,  626.0)
MATCH (within rounding)
```

## Why this one needs a window

The transport could be proven headlessly. This cannot: a local monitor sees
only events destined for its own application — the very property that makes it
work without an Accessibility grant is what makes it untestable from outside.
So `poc/native/mac/gesture_test.mm` supplies an application.

Its window carries a **context menu**, on purpose. That is AE's behaviour in
miniature, and it makes both halves visible rather than merely logged:

| | expected | what it proves |
|---|---|---|
| a SHORT right-click | the menu OPENS | the DOWN was swallowed and handed back |
| a right-click HELD | the menu does NOT open, a hold is reported | the DOWN never reached the app |

If the menu opens on a hold, the swallow is broken. If it never opens on a
short click, the replay is broken. **Neither shows up in a log that only
records what the monitor saw** — which is the same shape as the Phase 0 finding
that a silent discard and a correct replay produce identical logs.

```bash
./poc/native/mac/build_gesture_test.sh
poc/overlay/src-tauri/target/release/pieFX_gesture_test
```

---

# The target — the plug-in builds on macOS

`poc/native/pieFX.cpp` compiles and links on macOS, and `Mac/build_product.sh`
produces a loadable `pieFX.plugin`: universal (arm64 + x86_64), `_EntryPointFunc`
exported as a bare C symbol, a 478-byte PiPL carrying `8BIMkind` = `AEgx` with
`mi64` and `ma64`, `PkgInfo` = `AEgxFXTC`, and the overlay copied in beside the
binary where `dladdr` will find it.

**It has not been loaded by After Effects yet.** That needs AE quit and an
install into a root-owned folder, which is a person at a keyboard.

## The partition, and what it cost

347 compile errors at the start. They were never 347 problems:

| | |
|---|---|
| **vocabulary** | `BOOL`, `MAX_PATH`, `sprintf_s`, `ZeroMemory`, `CRITICAL_SECTION` — spread thinly over code that is otherwise pure AEGP |
| **five regions** | the pipe server, the overlay launch, the gesture, the clipboard, the paths |

Only the second group is a port. The first is translated once, in
`poc/native/mac/pieFX_compat.h`, which leaves the ~300 lines that merely SPEAK
Windows **byte-identical across both platforms** — the property that matters,
because the Windows product is shipping and a port is not a licence to churn
it.

The result is 337 added lines against **10 deleted**, and all ten are the
`Arm` hook-install block, which had to be restructured because installing the
gesture is the one step of arming that fails differently on each platform.
Everything else is `#ifdef` nesting around code that was not touched.

The compat header deliberately does not emulate Windows. It covers what
pieFX.cpp uses and stops; anything with a real decision in it went to a module
instead. `CRITICAL_SECTION` is the interesting case — it maps to a RECURSIVE
pthread mutex, because a Windows critical section is recursive and quietly
changing the locking semantics of a queue written on one thread and drained on
another is not a translation.

## Phase 0's third bug, in new code

`-Wall` found four `%ld` against a 32-bit `A_long` — the same defect
MAC_RESULTS already records from the S5 spike. Harmless on Windows, where
`long` is 32 bits. On arm64 varargs it reads eight bytes from a four-byte slot,
and one of the four was in `SendSummon`: the summon coordinates, on every
gesture.

It is worth noting HOW it was found. Nothing detected it on Windows because
there is nothing to detect there; it appeared the moment the same source was
compiled for a platform with a different `long`. Cross-compiling is its own
kind of test.

## What was stubbed, and why loudly

Four functions logged "not implemented on macOS yet (MAC_PORT step 5)" rather
than failing silently. **Three of the four are now written**; see the three
sections below. `WritePresets` is the one that remains, and returning 0 is
honest there in a way the clipboard stub could not be: the catalogue is a LIST,
so an empty one degrades to "no presets found" rather than to a silent wrong
answer.

The `%APPDATA%` ones shared a reason worth stating: the overlay reads and writes
those same files from its own side, so where they live is a two-sided
agreement, and taking half of it here would have been worse than taking none.
It was taken whole — below.

## The `%APPDATA%` decision, taken on both sides at once

`~/Library/Application Support/pieFX/`. Not a container: After Effects is not
sandboxed, so it has no container to share, and the two processes have to land
on the same directory or the feature does not exist.

The decision is one decision in two places, and both were changed together:

| side | before | now |
|---|---|---|
| `poc/native/pieFX.cpp` | two open-coded `%APPDATA%` sites | `PieFX_ConfigPath`, split once |
| `poc/native/mac/pieFX_paths.cpp` | — | the macOS half, with `mkdir -p` |
| `poc/overlay/src-tauri/src/lib.rs` | `APPDATA` or `None` in three functions | one `piefx_dir()`, used by all three |
| `poc/native/pieFX.h` | `"pieFX\\settings.json"` | `"pieFX" PIEFX_PATH_SEP "settings.json"` |

Two things fell out of doing it properly rather than minimally.

**The bodies unified.** `ReadSettings` and `WriteEffectCatalogue` had been
`#ifdef`-split with a macOS stub, and the split was in the wrong place: only
the PATH differed, and everything else was stdio, a two-field scan, and an
AEGP walk — all portable, and all of it code that would have had to be
maintained twice. Both are now single-bodied, with `PieFX_ConfigPath` as the
only platform seam. That deletes a prospective duplicate JSON parser and a
prospective duplicate 90-line catalogue walker rather than adding them.

**The directory is created from the path, not from a second spelling.** The
Windows original built `%APPDATA%\pieFX` to call `CreateDirectoryA` on, then
built the full path again from the same pieces. The folder name appearing
twice is a folder name that can be changed once; the helper now truncates the
finished path at its last separator instead.

### Why no harness could have caught a wrong default

Every driver in this project passes `--settings` / `--effects` explicitly —
which is the right way to drive a UI against a KNOWN file, and exactly why a
wrong default would have gone unnoticed by all of them. So the agreement was
checked two ways instead:

- `poc/native/mac/paths_test.cpp` pins the C++ side against the three relative
  names spelled the way `lib.rs` spells them. 22 assertions.
- End to end, with no AE: a `settings.json` written by hand at the agreed path,
  and the overlay started with **no arguments at all**. Its log said
  `load_settings: 38 bytes` — the file the plug-in side would have written,
  found by the side that reads it.

```
  system encoding: 0  (MacRoman is 0)
  [ok] it is ~/Library/Application Support
  [ok] settings.json is where the overlay looks
  [ok] effects.json is where the overlay looks
  [ok] the pieFX folder exists after PieFX_ConfigPath
  22 passed, 0 failed
```

## The effect catalogue, and the encoding — try UTF-8 FIRST

`WriteEffectCatalogue` came back with the path, and with the conversion the
earlier note said it needed. The conversion has one decision in it and it is
not the obvious one.

The obvious implementation is `CFStringCreateWithCString` with
`CFStringGetSystemEncoding()`. **That cannot fail.** This machine reports
encoding 0, MacRoman, and MacRoman has a meaning for every one of the 256 byte
values — so handing it bytes that are actually UTF-8 does not produce an error,
it produces mojibake. A conversion that cannot fail is a conversion whose
result cannot be checked.

So `PieFX_LegacyToUtf8` tests the bytes against the stricter grammar first: if
they are already well-formed UTF-8 they are copied through untouched, and only
otherwise are they decoded as the system encoding. An English install takes the
first path for free, since ASCII is a subset.

Measured, both directions:

```
  [ok] byte-identical, not round-tripped      "Distorsión" (UTF-8 in)
  [ok] decoded to the right character, in UTF-8
         Distorsión                           (0x97 MacRoman in)
  [ok] a too-small buffer is refused
  [ok] and the buffer is empty, so a fallback is unambiguous
```

The refusal path matters as much as the success path, because failure has a
defined fallback: `AEGP_GetEffectMatchName`, which the SDK header marks
`UTF8!!` and which is a stable, non-localised identifier. An unlocalised name
is worse than a localised one and much better than a wrong one — and a
`match` field is already in the file, so the fallback costs nothing.

**Written for both platforms.** The Windows half —
`MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, ...)` then `CP_UTF8` — sits
beside the macOS one at the same call site. `MB_ERR_INVALID_CHARS` is the flag
that makes the Windows conversion checkable, which is the same property the
UTF-8 pre-test buys here. This was never a macOS bug; a Spanish Mac just got
to it first.

## The clipboard: 230 lines become about 40

The one place in the port where the code gets smaller, and the reason is worth
recording rather than just enjoying.

Windows needs three clipboard formats — `"PNG"`, `CF_DIBV5`, `CF_DIB` —
because `CF_DIB` cannot express alpha and something has to be there for
consumers that know nothing else. Producing the two DIBs means decoding the PNG
with WIC and building bitmap headers by hand. `NSPasteboard` takes the PNG
bytes as they are under `NSPasteboardTypePNG`, and AppKit derives a bitmap for
anything that asks — with alpha intact. The WIC decode, `GlobalFrom`, the DIB
construction and the force-opaque fallback all disappear.

The seam moved as it did for settings: `CopyFrameToClipboard` itself is
ExtendScript plus string handling and had no Windows in it beyond vocabulary,
so it is now shared, and only `PngFileToClipboard` is split. Two smaller things
came with it — `DeleteFileA` went to `pieFX_compat.h` as `unlink` (Windows
returns FALSE when the file was already absent, and pieFX does not look,
because absent is the outcome it wanted either way), and `WaitForFrameFile`'s
`WIN32_FILE_ATTRIBUTE_DATA` became a two-line `FrameFileSize`. Emulating that
struct in the compat header would have been precisely the Windows emulation
that header promises not to do.

`poc/native/mac/clipboard_test.mm` asserts the thing that actually matters,
which is not "did `setData` return YES":

```
  [ok] byte-identical to the file, i.e. nothing re-encoded it
  [ok] an NSImage can be built from the pasteboard
  [ok] at the original 4x4, not rescaled
  [ok] with alpha, NOT flattened
  [ok] the opaque half is still opaque
  [ok] the transparent half is still transparent
  14 passed, 0 failed
```

It writes a PNG with a transparent half, puts it on the pasteboard, and reads
it back both ways a real consumer would — as PNG data, and as an `NSImage`,
which is the path that would silently flatten alpha if anything here were
wrong. That last assertion is the one the Windows `CF_DIB` fallback cannot
satisfy at all.

The harness replaces the clipboard when it runs. That is inherent to testing a
clipboard, and it is why it is a separate binary rather than something the
other tests do in passing.

## Two bugs the AE hand-check found, and the harnesses did not

Both were found the first time a human used the features on a real machine,
and both are worth recording because in each case a harness was PASSING over
the defect. **Both fixes have since been confirmed in AE**, on the same
machine and — for the clipboard — on the same 6656x2270 comp that exposed the
bug.

### A 6656x2270 frame pasted as 6656x804

`WaitForFrameFile` waited for "the same non-zero size twice in a row, 40ms
apart". That is a guess dressed as a test. AE does not write a large PNG in one
push, so any pause longer than the poll interval looked exactly like a finished
file.

What makes this the worst possible shape of failure is what a truncated PNG
does next. Its IHDR is in the first 33 bytes and its pixel rows are not — so it
does not fail to decode. It decodes at the FULL advertised size, with only the
rows that arrived. Hence the report: the clipboard said 6656x2270, and the
paste produced 6656x804. Every check in the chain passed. Measured directly:

```
  [ok] a half-written PNG still produces an image rep
  [ok] at the FULL advertised size — so size proves nothing
         94162 bytes whole, 47081 truncated, still reports 1600x1200
```

The fix is to stop timing and start asking. A PNG says where it ends: the last
chunk is IEND, and its twelve bytes are a fixed constant — length zero, the
type, and the CRC of the type — because IEND carries no payload. So "has AE
finished?" has an exact answer. `PngIsComplete` checks the eight-byte signature
at the front and those twelve bytes at the back, and `WaitForFrameFile` polls
for that instead.

Two things follow from the check being exact rather than probabilistic:

- **The budget went from 4s to 15s.** The wait now ends the instant the file is
  whole, so a large comp costs what it costs and a small one costs nothing. 4s
  was ample time to be WRONG in; what the budget has to cover now is a genuine
  8K write. The cost is that a frame AE never finishes writing freezes the UI
  thread for 15s before the error toast — a worse failure than before, in a
  case that used to fail by silently pasting the wrong thing.
- **The size is logged on both paths.** "Grew to 40MB and stopped" and "never
  appeared" are different problems and used to produce the same message.

Confirmed in AE afterwards: the full frame pastes.

**This was never a macOS bug.** `WaitForFrameFile` is shared code and the
heuristic was inherited from Windows, where a big enough comp would do the same
thing. It is the third defect in this port that turned out to be latent on both
platforms.

The harness could not have caught it, and the reason is worth keeping: it only
ever handed the clipboard a COMPLETE file. It now truncates one — at 1600x1200,
because half of a 4x4 PNG loses the entire IDAT and fails honestly, so a small
test image would have reported that all was well.

### The settings and search windows opened on the primary display

Both were built with `.center()`, which centers on the primary monitor. The
wheel already moves to the screen holding the summon point; its two windows did
not follow. Settings could at least be dragged back. Search is undecorated by
design — "type, Enter, gone" — so it could not be moved at all.

`mac_place_on_cursor_screen` puts both on the screen under the cursor, reusing
`screens_in_points` and asking AppKit for `NSEvent.mouseLocation` rather than
Tauri for a cursor position, for the reason that section already exists: points
are the only coherent space across a mixed-DPI desktop.

The two windows get DIFFERENT policies, and the difference is what they are.
Settings is placed once, when it is created, and afterwards raised wherever the
user left it — it has a title bar, and a user who dragged it somewhere meant
it. Search is placed on every summon, because it cannot be dragged, so "where
it was last time" is not a position anyone chose.

**The trap, which cost a debugging round.** With the placement in, the log said
the frame had been set to the second screen:

```
  placing on the cursor's screen: cursor (3055, 877) screen (1512, 0) 1920x1080
  frame -> top-left (1882, 160) 1180x760
```

and `CGWindowListCopyWindowInfo` said the window was at `X=166.0 Y=128.0` —
centered on the PRIMARY. `.center()` is not only an initial position on macOS:
it is applied again when the window becomes visible, so it silently undoes an
explicit move made while the window was hidden. It is now `#[cfg(not(target_os
= "macos"))]`.

That is also the argument for measuring from OUTSIDE the process. The overlay's
own log is a record of what it asked for, not of what it got, and here those
were different. After the fix, the two agree:

```
  moved to top-left (1882, 160)
  pieFX-overlay window X=1882.0 Y=160.0 W=1180.0 H=760.0
```

**`setFrameTopLeftPoint:`, not `setFrame:display:`.** setFrame: needs a height,
and Tauri's `outer_size` reports 1180x760 for a decorated window built at
1180x760 INNER — it does not include the title bar, so passing that to setFrame:
resizes the window as a side effect of moving it. Asking AppKit for the true
frame would mean an NSRect return, whose calling convention differs between
arm64 and x86_64. setFrameTopLeftPoint: takes the frame's top-left corner in
screen coordinates: the flip needs only the primary height, and it cannot
resize by accident. `mac_set_frame_points` stays for the overlay window, which
genuinely does need to resize itself onto each screen.

Confirmed in AE afterwards: both windows open on the display the cursor is on.

Windows has the same `.center()` bug, untested and unfixed here — see
`HANDOFF_MAC.md`.

## The effect catalogue, confirmed in AE — and where the file actually is

456 entries, AE claims 456, no mismatch. `file` calls `effects.json` "Unicode
text, UTF-8 text", it parses, and 189 of its entries carry non-ASCII that is
correct Spanish rather than mojibake or a match-name fallback:

```
    CC Ball Action        | Simulación
    CC Color Neutralizer  | Corrección de color
    CC Glass Wipe         | Transición
```

That settles the encoding work end to end, on the install that motivated it.

**The file is at `~/Library/Application Support/pieFX/`, and the reason it
looked missing is that Finder hides `~/Library`.** All three files were there
the whole time. `open ~/Library/Application\ Support/pieFX` is the way in; the
plug-in also logs the absolute path it wrote to, which is the more reliable
answer and is why that log line names the path.

## Presets: the last stub, and the seam that made it shared

621 shipped presets found under `/Applications/Adobe After Effects 2026/Presets`,
which matches `find -name '*.ffx'` exactly. Zero user presets on this machine,
which is a real answer rather than a failure.

**The install's shape is the same on both platforms even though the paths are
not.** Windows keeps the plug-in in `<install>/Support Files/Plug-ins` and the
presets in `<install>/Support Files/Presets`; macOS uses `<install>/Plug-ins`
and `<install>/Presets`. Either way **Presets is a sibling of Plug-ins**, so
`ShippedPresetRoot`'s rule — walk up from our own module path until an ancestor
holds both — needed no macOS variant at all. It only needed more ancestors: a
`.plugin` is a bundle, so the binary sits five levels down rather than two, and
the existing limit of eight already covered it.

### The seam is `opendir`, and nothing above it

The walk has a depth cap, a count cap, a `.ffx` filter, a two-root search and a
category rule in it. All of that is logic, and none of it is platform-specific.
The single thing that genuinely differs is *listing a directory*, so that is
where the split went — three small functions, `DirOpen` / `DirNext` /
`DirClose`, with `FindFirstFileA` behind one and `opendir` behind the other,
and roughly 120 lines of walk shared above them.

`DirNext` carries one trap worth naming: `d_type` is a free answer on the
filesystems AE installs onto, but it is **not guaranteed**, and a filesystem
reporting `DT_UNKNOWN` would have stopped the recursion at the first subfolder.
That fails in the direction of looking fine — it reads as "this AE ships almost
no presets" rather than as an error — so there is a `stat` fallback.

### 136 presets were unfindable by typing their own name

The bug this file exists to record. Preset names come from `readdir`, and **APFS
is normalisation-preserving**: it stores whatever bytes the installer wrote. So
AE's own Presets tree is MIXED — measured here, 136 of 213 accented names are
decomposed and the other 77 are not.

A keyboard produces composed text. `search.js` matches with a plain lowercased
substring test. So typing a preset's own accented name would fail to find it:

```
  raw .ffx names: 621
    typing "caída" BEFORE:  6 matches
    typing "caída" AFTER:   8 matches
    names unfindable by their own accented spelling: 136
```

The preset would be sitting in the list and would not come back from a search
for itself — which is the same shape of failure as the truncated PNG: nothing
errors, and the wrong answer looks like a complete one.

The fix is NFC, applied where the catalogue is written rather than where it is
searched, so the file itself is canonical for anything that reads it later.
`PieFX_LegacyToUtf8` now does it, which means the effects catalogue gets it too.
That function grew a second job and is better for it: "AE's text, or the
filesystem's, made canonical" is one idea, and the two callers were both
already asking for it — one for the encoding, one for the normalisation.

**Paths are deliberately NOT normalised.** That string is opened, not read, so
it keeps exactly the bytes the directory gave us. All 621 still `stat`.

### A harness that includes the translation unit

`WritePresets` touches no AEGP suite — it is a directory walk and some JSON —
so it is testable with AE installed but not running. It is `static`, so
`presets_test.cpp` **includes `pieFX.cpp`** rather than linking against it.

That is deliberate: what runs in the harness is the SHIPPING function, not a
copy of it that could drift. It is also the only way to test the walk without
first extracting it into a module it does not otherwise need to be in.

The assertions worth having are the ones about the OUTPUT, not the count:
every path must `stat`, and no category may end in `.ffx`. That second one
caught a real bug during development — the joined relative path was being
passed where the folder belonged, so every category had the preset's filename
glued to the end of it.

## The backstop that nearly went missing

The idle hook carries a backstop for a press whose UP was never seen — a
right-release over a NON-AE window is invisible to a thread-local hook, so
`S_rdownB` would stay set and the wheel would be left on screen.

A local NSEvent monitor has the **identical** blind spot, for the identical
reason, so the backstop is not Windows trivia to be guarded away. It is
`PieFX_GesturePoll`, asking the HID layer through `CGEventSourceButtonState` —
the same question the Windows side asks through `GetAsyncKeyState`.

It would have been easy to `#ifdef` this one out and never notice: the failure
only shows up as a wheel stuck on screen after a release the plug-in could not
see.

## Building it

```bash
./Mac/build_product.sh              # build only
./Mac/build_product.sh --install    # and install (AE must be quit; asks for sudo)
```

Not an Xcode project, on purpose. A `.plugin` is a directory with a binary, an
Info.plist, a PkgInfo and a Rez'd PiPL; assembling that in twenty lines of
shell is far easier to read — and to review — than a `pbxproj`.
`Mac/pieFXMac.xcodeproj` still builds the Phase 0 spike, which is a separate
thing and stays as it is.

---

# Localisation — measured on a Spanish AE

`app.isoLanguage` = `es_ES`, AE 26.3x87. `poc/scripts/ag_localeProbe.jsx`.

```
command                       english   spanish
--------------------------------------------------
Add to Render Queue             --      2161
Composition Settings...         --      2007
Pre-compose...                  --      2071
New Composition...              --      2000
Adjustment Layer                --      2279
```

Two findings, and the second is the one that matters.

## findMenuCommandId follows the UI language

Not one English name resolved. Every ae-command binding pieFX ships resolves
to **0** on this machine, which means the name-first design — the thing that
makes a binding checkable at the moment it is made — cannot be SEEDED in
English on a localised install.

The design itself is not broken. A user binding a command in their OWN
language gets the full check, because `findMenuCommandId` resolves their
spelling. It is the shipped DEFAULTS that cannot be validated this way.

## The ids are NOT localised

Every Spanish name resolved to **exactly the id pieFX already ships** for the
English one. 2161, 2007, 2071, 2000, 2279 — all five match
`ae-commands-2025.json`.

So an id is stable across LANGUAGES and fragile only across VERSIONS, which is
a much smaller claim than the one this project has been carrying. `MAC_PORT.md`
worried that "the id fallback would carry the whole wheel"; on a localised AE
it does, and that turns out to be survivable, because the thing it falls back
to is correct.

It also relocates the check. Id rot is a per-VERSION question, so it wants
answering once per AE release by whoever has an English install — not by every
user at runtime, where it cannot be answered at all.

## The bug that was hiding behind it

The executor preferred the name and, when the name did not resolve, returned
`NO SUCH MENU COMMAND` and stopped — **with the id sitting unused in the same
binding.** The comment three lines above says "Names are localised, so the id
stays as the fallback". The fallback was described and never implemented.

On this machine that meant every ae-command slot on the wheel failed with an
error toast: the gesture worked, the wheel drew, and nothing fired.

It now falls back to the id, and says so in the log rather than silently:

```
id 2161 (BY ID: the name did not resolve), layers 3->3, rq 0->1
```

The toast survives for the case it was actually written for — a name that does
not resolve AND no id to fall back to, which is a genuinely unbound slot rather
than one bound in another language.

This is the second time in this port that a localised AE has exposed a defect
that is not about localisation: the first was `%ld` against a 32-bit `A_long`,
which only appeared when the same source met a different compiler. Neither was
a macOS bug. Both were latent everywhere and needed a different environment to
show up.

## What is still open

- ~~**Effect names** still need the legacy-to-UTF-8 conversion (step 5).~~
  **DONE** — `PieFX_LegacyToUtf8`, on both platforms, with the match name as
  the fallback. There are still no Unicode accessors; see the correction below.
- **The wheel's own labels** are pieFX's vocabulary, not AE's menu strings, so
  they are a separate and optional translation job. A Spanish user reads
  "Comp Settings" on the wheel and gets `Ajustes de composición`; only the
  label is English, and nothing about it is broken.

## Carried forward

**The effect names, and a correction.** This AE runs in Spanish, and
`AEGP_GetEffectName` / `AEGP_GetEffectCategory` return single-byte legacy text,
not UTF-8 — `file` calls the S5 log "Non-ISO extended-ASCII". Harmless for a
spike.

This file previously said the product needs "the Unicode accessors". **There
are none.** `AEGP_EffectSuite5` is the newest and both calls still take
`A_char *`. The conversion has to be done by pieFX, at the single point those
strings are written into `effects.json` — and it is not a macOS-only job, since
the same bug is latent on Windows under a non-Latin locale.

`AEGP_GetEffectMatchName` is the exception: the header marks it `UTF8!!`, and a
match name is a stable, non-localised identifier. It is therefore both the
right fallback for a name that will not convert and the right key for anything
that has to be remembered across sessions.

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

### The two step-5 harnesses

Neither needs AE, and neither needs the overlay.

```bash
./poc/native/mac/build_paths_test.sh
poc/overlay/src-tauri/target/release/pieFX_paths_test

./poc/native/mac/build_clipboard_test.sh
poc/overlay/src-tauri/target/release/pieFX_clipboard_test    # replaces your clipboard
```

`paths_test` also prints `CFStringGetSystemEncoding()`, which is the number the
encoding argument above turns on.

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
