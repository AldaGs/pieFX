# Radial Menu — macOS session checklist

Everything here was written on Windows and **has never been compiled or run**.
Treat it as a starting point that is probably wrong in small ways, not as
working code. Same status as the God Rays Metal kernels.

The point of this file is that Mac time is the scarce resource. Spend it
testing, not remembering.

---

## What is already written

| File | What it is |
|---|---|
| `RadialMenuMac.mm` | The whole Mac spike: S1, S3-Mac, S4, S5-Mac. Self-contained. |
| `RadialMenuMac_PiPL.r` | PiPL declaring `CodeMacIntel64` / `CodeMacARM64`. |
| `Mac/RadialMenuMac.plugin-Info.plist` | Bundle plist. `AEgx` package type, `FXTC` signature. |

It is a **separate plug-in** from the Windows `RadialMenu.aex`, with its own
menu names, so both can be installed without colliding. That is deliberate: the
Windows spike is verified and working, and refactoring it into a shared core
plus two platform layers would risk that for no gain while the Mac side is still
unknown. Duplicating ~80 lines of AEGP glue is the cheaper trade. Merge later,
if ever.

## What is NOT written

**The Xcode project.** Hand-writing a `project.pbxproj` blind is not worth it.
Build it from the sample instead:

1. Copy `Examples/AEGP/Persisto/Mac/Persisto.xcodeproj` next to `RadialMenuMac.mm`.
2. Rename the project and the target to `RadialMenuMac`.
3. Remove Persisto's sources; add `RadialMenuMac.mm`, plus
   `Examples/Util/AEGP_SuiteHandler.cpp` and `Examples/Util/MissingSuiteError.cpp`.
4. Point Info.plist at `Mac/RadialMenuMac.plugin-Info.plist`.
5. Header search paths — same set the Windows project uses:
   `Examples/Headers`, `Headers/SP`, `Headers/Mac`, `Resources`, `Util`.
6. Link `Cocoa.framework` and `CoreGraphics.framework`. **The Windows project
   needs neither; this is the one build-settings difference that will not be
   obvious from the sample.**
7. Wrapper extension `plugin`, package type `AEgx`.

Use whatever recipe [[mac-port-workstream]] already records from the six-plugin
port — that is the same toolchain and the same SDK.

---

## Order of work

Do these in order. Each one makes the next interpretable; out of order, a
failure has too many possible causes.

### 1. S1-Mac — does anything load at all?

Build, install, launch AE, and look for **Window → Radial Menu (Mac) S1**.

Before blaming the code, check the export:

```
nm -gU RadialMenuMac.plugin/Contents/MacOS/RadialMenuMac | grep EntryPoint
```

It must show a bare `_EntryPointFunc`. A mangled `__Z...` symbol means the
five-parameter entry point signature drifted — that exact mistake cost a launch
on Windows, because a wrong signature is a legal C++ *overload* that links
clean.

Then: select a layer, run it, confirm the anchor moves and the layer does not.

**If this fails, stop.** Nothing below is interpretable until the AEGP layer
loads.

### 2. S5-Mac — the control

Run **S5: Dump Effects Catalogue**. A different total from Windows' 519 is
expected — different machine, different plug-ins. What matters is that
`walked == claimed`.

This is here as a control: S5 is pure AEGP and passed on Windows, so if it
misbehaves the problem is the Mac build, not the API.

### 3. S4 — the actual spike

**3a. Menu timing first, and this is the one that shapes everything.**

Turn the watch ON, leave swallow OFF, and right-click normally. Watch *when*
AE's context menu appears.

- Menu on **press** → same as Windows. The swallow-the-DOWN-and-replay strategy
  in the code is correct as written.
- Menu on **release** → better. Swallow only the UP of a press that already
  crossed the threshold, and delete the whole `CGEventPost` replay path.

**Judge this by eye.** On Windows a message trace said the opposite of what was
actually on screen and cost a full test cycle. What the user sees is the claim.

**3b. Detection.** Press and hold perfectly still — beep at ~200ms. Check the
log says `via timer`. If it says `via drag` only, then a motionless press is not
being caught and the clock needs to move to a `CFRunLoopTimer` registered in the
event-tracking run loop mode.

**3c. Swallow.** Arm it, then check by eye: a hold shows no menu; a short
right-click shows the same menu in the right place; no double or stuck menus;
and AE still feels normal afterwards — drags, selection, panel switching.

**3d. Multi-monitor replay.** If there is a second display, especially one
positioned above or below the primary, test a replayed short click there. The
`NSPoint` → `CGPoint` flip in `ReplayRightClick` is against the *primary*
screen, and a wrong flip would show up as the menu opening in the wrong place.

### 4. S3-Mac — the overlay

Run it, move the cursor over the comp viewer, wait for the ring.

- Does it sit above everything? Comp viewer, timeline, a CEP panel, second
  monitor. If it loses to something, try `NSScreenSaverWindowLevel` before
  concluding the spike failed.
- Are the edges soft?
- The alert reports the selection count before and after. On Windows selection
  survived; if it does not here, the overlay must not take focus.

---

## What Windows already settled — carried as hypotheses, not facts

Each is a claim about AE's behaviour, and AE on macOS is a different build.

| Finding | Confidence on Mac |
|---|---|
| Selection is document state, not focus state | High — it is AE's data model, not an OS behaviour |
| Nothing can identify an AE panel | High, and it no longer matters — the design gates on selection state |
| AE opens its context menu on mouse-DOWN | **Low. Verify first.** macOS menus are NSMenu |
| AE's modal drag loop starves the idle hook | Unknown, and probably moot — `dispatch_after` does not depend on idle time |

## The gate this closes

Per the roadmap's table:

| S2 | S4 | Outcome |
|---|---|---|
| pass | pass | The full product. Proceed. |
| pass | **fail** | Hold gesture on Windows, hotkey on Mac. Still ship both. |

S2 has passed. So S4 cannot sink the project — it only decides whether macOS
gets the gesture or a hotkey. Worth knowing before the POC, not worth blocking
on.

The specific way S4 fails is `addLocalMonitorForEventsMatchingMask` returning
nil, or the monitor never seeing right-button events. The code reports that case
explicitly rather than failing quietly.
