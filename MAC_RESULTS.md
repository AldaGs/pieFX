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

## Carried forward

This AE runs in Spanish, and `AEGP_GetEffectName` / `AEGP_GetEffectCategory`
return single-byte legacy text, not UTF-8 — `file` calls the S5 log "Non-ISO
extended-ASCII". Harmless for a spike. The product will need the Unicode
accessors before those names reach a menu.

## Reproducing

```bash
./Mac/build_and_install.sh
```

Quit After Effects first; the script refuses to run while it is up, because AE
only reads `Plug-ins` at launch. Then run the stations from the Window menu in
the order `MAC_SESSION.md` gives: S1, S5, S4, S3.

Logs are written to `$TMPDIR` — `pieFX_S4_gesture.txt` and
`pieFX_S5_effects.txt`. Note that turning the watch off and on truncates
the S4 log, so a swallow test has to happen inside a single watch session.
