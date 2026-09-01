# pieFX — Phase 0 spike log

One line per spike, written down *before* moving on. See the roadmap for the gate.

| Spike | Question | Outcome |
|---|---|---|
| S1 | AEGP loads, menu command registers, ExtendScript dispatch works | **PASS** (2026-08-27, AE 2026) |
| S2 | Comp-viewer window subclassing sees right-click-and-hold | not started |
| S3 | Transparent always-on-top overlay; selection survives focus round-trip | not started |
| S4 | macOS parity of S2 via `NSEvent addLocalMonitorForEventsMatchingMask` | not started |
| S5 | Enumerate installed effects; apply one by match name | not started |

## S1 — how to run it

Build (non-elevated shell is fine; output goes to a writable dir):

    $env:AE_PLUGIN_BUILD_DIR="C:\AE_SDK\_build_out"
    MSBuild Win\pieFX.sln -p:Configuration=Debug -p:Platform=x64

Deploy: close AE fully (it locks the .aex), then copy
`C:\AE_SDK\_build_out\AEGP\pieFX.aex` into AE's own Plug-ins folder
(*not* MediaCore — that path is shared with Premiere, which has no AEGP host).

Test: open a comp, select a shape/solid layer, then
**Window > pieFX S1 (Anchor to Center)**. Pass = the anchor moves to the
layer's centre, an alert reports "moved N layer(s)", and one undo reverts it.

Also check: the menu item is greyed out when no comp is frontmost (that is the
UpdateMenu hook), and the alert is the *script's* return value, not a guess —
which is what proves the round trip rather than just the registration.

## S1 gotcha (cost one AE launch)

`Couldn't find main entry point for pieFX.aex (48 :: 72)` means the PiPL's
export name isn't in the DLL. The cause here: **Commando's `EntryPointFunc`
signature is stale.** It shows seven parameters (with `file_pathZ` / `res_pathZ`);
the real `AEGP_PluginInitFuncPrototype` in `AE_GeneralPlug.h:4169` takes five.

Because the header declares the entry point as `extern "C" AEGP_PluginInitFuncPrototype`,
a definition with a *different* signature is a legal C++ overload rather than
that function — so it compiles and links clean, and exports mangled:

    ?EntryPointFunc@@YAHPEAUSPBasicSuite@@HHPEBD1HPEAX@Z    (broken)
    EntryPointFunc                                          (correct)

`Persisto` has the correct five-param form; prefer it over `Commando` as the
model. Verify any AEGP build with:

    dumpbin /EXPORTS <plugin>.aex

## S1 outcome

Pass. The command appears under Window, greys out with no comp frontmost, the
hook fires, the script runs, and the alert carries the script's own return value
("moved 1 layer(s)") � a real round trip, not just registration.

Two fixes after the first successful run:

- **Blank error alert.** `AEGP_ExecuteScript` returns a non-NULL but *empty*
  error handle on success, so reporting on `handleH != NULL` fires an empty
  alert every time. Test the string's first byte, not the handle.
- **The layer jumped.** Position is measured *to* the anchor, so moving the
  anchor alone shifts the layer on screen. The script now pushes Position by the
  same delta, taken through the layer's Scale and Rotation, so the pixels stay
  put. This is the behaviour the real Anchor tool needs � worth having correct in
  the spike, since it is the whole reason the roadmap picks Anchor for the POC.
  Compensation is 2D only (Z Rotation for 3D layers); full 3D orientation is a
  POC problem, not a spike one.

---

## S2A - the measurement half of S2

S2 asks whether we can subclass the comp viewer's window proc. Before writing a
resolver we need to know what we are resolving, and the roadmap is explicit that
hardcoding a class name is what pins competing products to one AE version. So
S2 splits:

- **S2A (this):** arm a probe, sweep the cursor, write down every distinct window
  it passes through with its full ancestor chain and screen rect.
- **S2B:** write the resolver + subclass against that data, not against a guess.

**Window > pieFX S2A (Probe Window Under Cursor)** arms an 8-second sample.
Dismiss the alert, then sweep the cursor over: the comp viewer, the timeline, the
project panel, effect controls, a floating panel, and a second monitor if there
is one. A second alert reports the count when it finishes.

Log: `%TEMP%\pieFX_S2_probe.txt`.

Sampling runs in the AEGP idle hook, which is itself a datum - if the hook does
not fire while the mouse is moving, that constrains how S2B can work and is worth
knowing now rather than after the subclass is written.

What we are reading the log for:

1. Does the comp viewer have a **stable, distinctive class name**, or is it a
   generic one shared with every other panel? If generic, the resolver has to
   key on something else (rect, ancestor position, window text).
2. Is the viewer **rooted at the AE main HWND**? A floating or second-monitor
   panel that is a separate top-level window changes the walk.
3. Do the panels differ from each other at all, or do they only differ by rect?

### S2A outcome - PASS as a measurement, and it invalidates the planned approach

Probe ran, idle hook sampled fine, 22 distinct windows logged. Findings:

**1. There is no comp-viewer class name to resolve by.** Every AE panel - comp
viewer, timeline, project, effect controls, floating panels - is the same
window class:

    class="DroverLord - Window Class"

The window *text* only ever carries a structural role, never a panel identity:
`DroverLord - Frame Window`, `OS_ViewContainer`, `DroverLord - TabPanel Window`,
nested in that order. A comp viewer and a timeline are indistinguishable by
class, by text, and by shape of ancestor chain. So the roadmap's "hierarchy walk
that resolves by class name with fallbacks" **has nothing to key on**. This is
the single most important thing S2A tells us.

**2. The AE main window class is version-stamped.**

    class="AE_CApplication_26.3"

That is almost certainly the real reason competing products cap at "AE 2025+":
anyone matching that string exactly gets a product that breaks every release.
Never match it - `AEGP_GetMainHWND` hands us the HWND directly and is version
independent.

**3. Everything is rooted at the AE main HWND - including the second monitor.**
A panel dragged to the second display was still `rooted at main` (six levels
deep, through an extra unnamed DroverLord container). So there is one window
tree and one UI thread, which is exactly the precondition an in-process hook
needs. This is good news and it removes the multi-monitor worry from S2.

**4. CEP panels are a Chrome hierarchy wearing an AE panel as a hat.** The
MTAG/Declutter panels nest
`WC_PLUGPLUG_HTMLEXTENSION_CLASS_NAME` -> a GUID class -> `CefBrowserWindow` ->
`Chrome_WidgetWin_0` -> `Chrome_RenderWidgetHostHWND`. Mouse input over those
belongs to Chrome, not to AE. Any gesture must stand down over them - and it is
independent confirmation of the roadmap's "never touch CEP" call.

**5. The hierarchy is not even internally consistent.** One panel's parent was
`#32770` - the stock Win32 dialog class - rather than a DroverLord frame. More
evidence against resolving anything structurally.

### What this changes for S2B

Subclassing one resolved window is off the table: we cannot identify the window.
The in-process alternative is strictly better anyway - a **thread-local
`WH_MOUSE` hook**:

    SetWindowsHookEx(WH_MOUSE, proc, NULL, GetCurrentThreadId())

- Thread-local, not global: no OS permission prompt, no other process affected.
  We are already a DLL on AE's UI thread, which is the whole reason this is
  available to us.
- Sees mouse messages for *every* AE window, so no resolution problem at all.
- Version independent: no class names anywhere.
- It is the same "we're already inside the process" argument the roadmap makes
  for macOS local event monitors, which makes S2 and S4 the same shape.

The "which panel am I over?" question then gets answered by asking AE rather than
Windows. `AEGP_WindowType` (`AEGP_WindType_COMP`, `_TIMELINE`, ...) exists, but
is only *delivered* to the UpdateMenuHook - there is no getter. Whether caching
it there tracks panel focus closely enough is a real open question, and S2B
should measure it rather than assume it.

### S2B run 1 - the hook works; two defects found

25 presses across 7 distinct panels. The core claim holds:

- `SetWindowsHookEx(WH_MOUSE, ..., GetCurrentThreadId())` installed without
  complaint and saw **every** right press, in every panel, with no window
  resolution required. The S2A dead end is genuinely routed around.
- Detection latency was tight: 203-281ms against a 200ms threshold.
- Nothing was swallowed; `CallNextHookEx` on every path.

**Defect 1 - the still-hold backstop does not exist.** Two presses held for
**1687ms and 1703ms were logged as short clicks.** The mouse never moved, so
WM_MOUSEMOVE never fired, and the AEGP idle hook - the intended backstop - never
ran either. AE is in a modal drag loop for the duration of a press and that loop
does not pump AEGP idle time. So the first cut only ever detected *flicks*, and
a press-and-wait, which is the actual gesture, silently failed 2 times in 25.

Fixed by moving the clock to `SetTimer(NULL, 0, RM_HOLD_MS, HoldTimerProc)` - a
thread timer with a NULL hwnd, dispatched by `DispatchMessage`, which modal loops
still call. The move and idle paths are kept only so the log names which clock
won; "via idle" appearing again would mean this reasoning is wrong.

**Defect 2 - `AEGP_WindowType` never changed.** Every press in every panel -
timeline, project, the right-hand stack - logged `active panel = COMP`. The
cache is not tracking panel focus.

Two readings, and run 1 cannot separate them:
  a) UpdateMenuHook only refires on menu rebuilds, so the value was simply
     stale for the whole session; or
  b) right-clicking does not change AE's active panel at all, so COMP was
     *correct* the whole time and the test never actually changed focus.

Run 2 distinguishes them: LEFT-click into the timeline first, then right-hold.
If it still says COMP, the value is useless and panel identity needs another
answer entirely.

**Consolation prize if it is useless:** the press HWNDs were *stable and
distinct* per panel across the whole session (7 of them, e.g. `210EDE`,
`190978`, `1F0C8E`, `660C98`). We cannot NAME a panel, but we can tell panels
APART and recognise one we have seen before - which may be enough to learn the
comp viewer once and remember it.

### S2B run 2 - detection PASSES; timer fix confirmed

**29 holds, 29 detected, 0 misses, 203-218ms every time** against a 200ms
threshold. Run 1's two silent failures are gone.

`via idle` **never appears** in the log. That confirms the diagnosis rather than
just papering over it: AE's modal drag loop does not pump AEGP idle time, and a
thread timer does survive it. `via timer` and `via move` split the wins, which
is expected - whichever clock reaches the threshold first takes it - and the
move path can be deleted whenever we want, since the timer alone is sufficient.

Also confirmed: presses at negative X (second monitor) were seen normally, so
the single-UI-thread finding from S2A holds up under real input.

### `AEGP_WindowType` is unusable - closing that question

Run 2 still logged `active panel = COMP` for every press in every panel, across
6 distinct panel HWNDs and both monitors.

The explanation is structural, not a stale-cache accident: **UpdateMenuHook
fires when AE rebuilds its menus**, which happens when the user opens a menu -
not when panel focus changes. During ordinary mouse work no menu is rebuilt, so
the cached value can only ever be as fresh as the last time a menu was opened.
In this session that was the Window menu used to arm the watch, with a comp
active. The value is therefore correct-but-ancient by construction, and no
amount of caching discipline fixes it.

**Conclusion: AE cannot tell us which panel the cursor is over.** Combined with
S2A - Windows cannot either, because every panel shares one class - panel
identity has no direct answer from either side.

What we do have: **HWND identity is stable and distinct per panel** (run 2 saw 6
of them, consistent within the session). We can tell panels apart and recognise
a repeat, we just cannot name one. Two ways to exploit that later, both POC-time
design questions rather than capability questions:

1. **Don't gate on panel at all.** Summon on right-hold anywhere in AE and let
   each tool act on AE's *selection state*, which we can query properly
   (`AEGP_GetActiveItem`, selected layers). Simplest, and probably better UX.
2. **Learn the viewer once.** A click that changes AE's active item came from a
   viewer; remember that HWND. More precise, more machinery.

### S2 verdict

**PASS on the roadmap's stated claim** - the hold gesture is detectable
in-process, cross-panel, cross-monitor, version-independently, with no OS
permission and no window resolution.

Two things are NOT yet proven and should not be quietly folded into the pass:

- **Passthrough was not exercised in run 2** - the log records 0 short clicks,
  so the "normal right-click still works" half of S2 rests on run 1 only.
- **Swallowing is untested.** Detecting the gesture and *taking* it are separate
  claims. Suppressing the context menu on a hold while letting a short click
  through means deciding at button-down, before you know which it is. That is
  S2C, and it is the real remaining risk in the moat.

**Passthrough confirmed by observation (run 2 session):** context menus behaved
normally throughout. S2's stated claim is fully met.

**Note on gating (user observation):** FX Console appears to key its behaviour
off *selection state* - layer selected shows the effect search, otherwise the
other actions - rather than off which panel you are in. That is independent
support for option 1 above, and it is also a hint about why FX Console still
leans on a .jsx: selection state is exactly what ExtendScript can see cheaply.
It suggests panel identity may never need solving at all.

---

## S2C - can we TAKE the gesture, not just see it?

The open risk. Suppressing AE's context menu on a hold, while a short click still
raises it, means deciding at button-DOWN - before we know which gesture it is.

The decisive unknown is **when AE actually raises the menu**. If the menu comes
from `WM_RBUTTONUP`, we can let DOWN through untouched and swallow only the UP of
a press that already crossed the threshold; nothing needs synthesising, and a
short click is never even observed by us. If AE raises it on DOWN, that is off
the table and the only route left is swallow-everything-and-resynthesise, which
is far riskier.

`WM_CONTEXTMENU` is *sent*, not posted, so a WH_GETMESSAGE hook cannot see it.
WH_CALLWNDPROC can. So S2C is again two steps:

- **S2C-1 (trace):** a WH_CALLWNDPROC hook logging `WM_CONTEXTMENU`,
  `WM_ENTERMENULOOP`, `WM_INITMENUPOPUP`, `WM_EXITMENULOOP` with their offset in
  ms from the last right-button DOWN. That timing answers the question outright.
- **S2C-2 (swallow):** a separate toggle that returns non-zero from the WH_MOUSE
  hook for the `WM_RBUTTONUP` of a press that crossed the threshold.

Both ship in one build so a single AE session can run both. Known risk to watch
for in S2C-2: swallowing the UP means AE never sees the button released, which
could leave it holding mouse capture. The trace will show it.

### S2C outcome - PASS

**S2C-1: AE raises its menu on button-UP.** The timestamps settle it outright -
`WM_CONTEXTMENU` lands on exactly the same millisecond as the release, every
time:

    right button UP after 94ms ... [+  94ms] WM_CONTEXTMENU
    right button UP after 78ms ... [+  78ms] WM_CONTEXTMENU

Never on DOWN, never while `(button still DOWN)`. So the safe cut is available
and swallow-and-resynthesise is not needed.

**S2C-2: swallowing the UP suppresses the menu, and only for holds.** With the
experiment armed, 20 holds were swallowed and **not one produced a
WM_CONTEXTMENU**. Short clicks during the same armed period still produced
theirs normally (click #10: UP at 78ms, WM_CONTEXTMENU at +78ms). Normal
right-click is untouched because a short click never reaches the swallowing
branch at all - by construction, not by careful timing.

**The control.** Disarming mid-session and repeating the same gesture brings the
menu straight back:

    hold #23, UP after 2016ms ... [+2016ms] WM_CONTEXTMENU   (disarmed)

That is the measurement having a broken control rather than an assertion that
passes for the wrong reason - the suppression is demonstrably ours.

**No sign of stuck capture.** `WM_CAPTURECHANGED` follows swallowed holds, but it
follows ordinary short clicks just as often throughout the whole log, so it is
AE's normal housekeeping and not a symptom. AE releases capture on its own
despite never seeing our swallowed UP.

**Caveat worth carrying forward.** Not every panel routes through
`WM_CONTEXTMENU`. The big bottom panel (`200CA4`, the timeline) never emits one -
AE handles right-click there through its own path. Swallowing the UP still
prevents that, since AE's window proc never receives the message at all, but it
means `WM_CONTEXTMENU` is not a reliable universal signal and we should not build
anything that depends on seeing it.

## Phase 0 gate - S2 column

**S2: PASS**, including the part the roadmap deferred. The hold gesture is
detectable AND takeable, in-process, cross-panel, cross-monitor, version
independently, with no OS permission, no window resolution, and no damage to
normal right-click.

Remaining spikes: S3 (overlay window), S4 (macOS parity), S5 (effects
catalogue).

### S2C CORRECTION - it did NOT pass. Observation beats the trace.

User report after the run: **the context menus appeared on mouse-DOWN, and they
appeared every time - including while swallowing was armed.**

The trace said otherwise and the trace was misleading. The error was mine: I
treated *absence of `WM_CONTEXTMENU`* as *absence of a menu*. The log itself
contained the evidence against that reading and I did not weigh it - **many short
clicks produced no `WM_CONTEXTMENU` at all**, yet the user saw a menu on every
one of them. That alone proves AE's context menus are not driven by
`WM_CONTEXTMENU` in the general case.

What is actually true:

- **AE opens its context menu on button-DOWN, through its own internal path**,
  not via `DefWindowProc` turning an UP into `WM_CONTEXTMENU`.
- The `WM_CONTEXTMENU` messages we did see, timed exactly to the release, are
  some secondary or forwarded traffic - not the thing that opens the menu.
- Therefore swallowing the UP is far too late, and S2C-2's apparent success was
  an artefact of measuring the wrong signal.
- The "control" I was pleased about proves nothing either: it showed
  `WM_CONTEXTMENU` returning when disarmed, but that message was never the menu.

Lesson for the rest of this project, and it is the [[godrays-star-glint-corrections]]
lesson again: a message trace measures messages, not behaviour. Where the claim
is about what the user sees, the check has to be what the user sees.

Good news that survives: **AE felt completely normal throughout** - no stuck
capture, no dead panels, no broken drags, across ~20 swallowed UPs. Eating mouse
messages inside AE is safe. It just has to be the right message.

### S2D - swallow the DOWN and replay it

If the menu opens on DOWN then the DOWN is what must be swallowed, which puts us
back on the route S2C-1 was hoping to avoid:

- Swallow every right-button DOWN while armed.
- If the press crosses the threshold, it is our gesture - swallow the UP too and
  AE never learns a right-click happened.
- If it is released early, it was a normal right-click: **replay it** with
  `SendInput`, guarded by a pending-count so our own hook ignores the injected
  pair, then swallow the real UP.

The visible cost, and it is a real one: for a short right-click AE's menu now
appears on *release* instead of on press. Sub-200ms, but not nothing - it needs
judging by feel, not by log.

The thing to watch: whether a replayed click lands in the right place and opens
the same menu AE would have opened. `SendInput` uses the live cursor position, so
it should, but "should" is what S2C already cost us.

### S2D outcome - PASS, confirmed by eye

User report: **holding shows no menu; a regular click shows it.** That is the
claim, checked the only way it can be checked.

Log corroborates without being the evidence this time: 12 short clicks swallowed
and replayed, 10 holds swallowed outright, and the replay guard held - no
injected click was ever mistaken for a new gesture, so there is no recursion and
no double menu.

So the gesture is genuinely ours:

- swallow every right-button DOWN while armed;
- past 200ms it is a gesture - swallow the UP too, AE never learns a right-click
  happened;
- released early it was a click - `SendInput` it back and swallow the real UP.

Two things left open rather than closed, both for later:

- **The replayed click's menu appears on release, not on press.** Under 200ms,
  and it did not bother the user in testing, but this is a feel question and the
  MVP's month-of-daily-use is what should actually judge it.
- **Right-DRAG is untested.** While armed the DOWN is eaten, so any AE feature
  driven by right-press-and-drag would break. Worth a line in the V1 test matrix
  next to "never break normal right-click".

## Phase 0 gate - S2 column, final

**S2: PASS.** The hold gesture is detectable AND takeable: in-process,
cross-panel, cross-monitor, version independent, no OS permission, no window
resolution, and normal right-click intact.

Per the roadmap's gate table, S2 passing means the distinctive product is
available on Windows. S4 decides whether it is available on both.

Remaining: S3 (overlay window), S4 (macOS parity), S5 (effects catalogue).

---

## S3 - the overlay window

### Why this is not a Tauri spike

Tauri IS the plan for the product's overlay. It is not the plan for S3. The
roadmap puts the architecture decision under a heading that says **"lock before
POC, not before spikes"**, and Phase 0's whole point is answering questions
before any decision gets expensive. Reaching for Tauri now would:

- **confound the measurement** - if the overlay lost the z-order fight we would
  not know whether that was Windows or Tauri;
- **presuppose the decision** the spikes are meant to inform;
- cost a toolchain, for a spike the roadmap budgets in days.

A raw Win32 layered window answers both S3 questions immediately: if the OS
itself cannot hold a window above AE, no toolkit built on it can.

### S3A - in-process (this build)

`WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW` popup at the cursor,
per-pixel alpha via `UpdateLayeredWindow`, an antialiased ring painted by hand
into a premultiplied BGRA DIB. Antialiasing is deliberate - a hard-edged shape
would look identical under a colour key, so a soft edge is what proves the
per-pixel alpha path is really working.

It **deliberately takes focus** (`SetForegroundWindow`, no `WS_EX_NOACTIVATE`).
The roadmap's assumption is that layer selection is document state, not focus
state, and it says to test that explicitly rather than assume it. So the spike
counts the selection before showing, restores focus on close, counts again, and
reports both numbers. A measurement, not a vibe.

### S3B - out of process (not built yet)

The one thing S3A cannot cover: **a Tauri window would be in a different
process**, and cross-process windows can behave differently in z-order and focus
than one created by AE's own process. If S3A passes, S3B is a throwaway .exe
with the same layered window - an hour's work - and it is the version that
actually matches the planned architecture.

Doing A before B is still right: if the in-process window loses, the
out-of-process one has no chance, and we would have learned it for free.

### S3A outcome - PASS on both questions

- **Z-order: the ring sat above everything, every time** - comp viewer, timeline,
  a Chrome-hosted CEP panel, and the second monitor. This is precisely where the
  roadmap says FX Console's ScriptUI palette loses, so a plain `WS_EX_TOPMOST`
  layered window already beats the thing we are trying not to be.
- **Selection survived the focus round-trip**, measured before and after, with
  the overlay deliberately taking focus. The roadmap's load-bearing assumption -
  selection is document state, not focus state - **holds**. That removes the
  whole `WS_EX_NOACTIVATE` class of workarounds from the design.
- Per-pixel alpha works: soft edges, so it is genuinely `UpdateLayeredWindow`
  with premultiplied BGRA and not a colour key in disguise.

Also fixed during the run: the overlay appeared at the cursor, and the cursor is
on the menu item, so every run measured the same screen position. It now arms and
fires 4 seconds later wherever the cursor has been moved to. (The arm timer has
to start *after* `AEGP_ReportInfo` returns - it is modal, so the grace period
would otherwise burn down behind the alert.)

### S3B - the out-of-process case

Still open, and it is the one that matches the planned architecture. A Tauri
overlay lives in **another process**, which differs from S3A in a way that could
matter both times:

- **Z-order:** a TOPMOST window owned by a background process is not obviously
  the same fight as one owned by the foreground app.
- **Focus:** in-process, AE stays the active *application* throughout. Out of
  process, AE gets deactivated outright (`WM_ACTIVATEAPP`) - a stronger event
  than the one S3A proved selection survives.

So S3A passing does not settle S3B. The test: the plug-in counts the selection,
launches a throwaway .exe that draws the identical ring, waits for it to exit,
and counts again - the same measurement across a real process boundary.

### S3B outcome - PASS

The cyan ring sat above everything too, and **selection survived across a real
process switch** - not merely a focus change within AE, but AE being deactivated
as an application.

So the planned architecture is viable as written: a separate-process overlay can
hold z-order over AE and costs us nothing in document state. That is the specific
claim the Tauri decision rests on, and it is now measured rather than assumed.

**S3: PASS**, both halves.

## Phase 0 gate - status

| Spike | Outcome |
|---|---|
| S1 AEGP hello world | PASS |
| S2 hold gesture | PASS - detectable and takeable |
| S3 overlay window | PASS - in-process and out-of-process |
| S4 macOS parity | not started (needs a Mac) |
| S5 effects catalogue | not started |

Three of five, and the two that could have killed the product are both through.
S4 remains the one that decides *how much* product, per the roadmap's gate table.

### S5 outcome - PASS

**519 walked, 519 claimed. MATCH.** No sign of the partial or unstable list the
roadmap warns about. Third-party effects are all present and correct - Plugin
Everything, ProductionCrate, irrealix, Boris/Mocha, Keylight, Cineware, Mettle,
and the user's own `ags_utilities` plug-ins. Those are what a partial list would
drop first, and none of them are missing.

**S5B applied Gaussian Blur by match name.** The whole search path is proven:
catalogue -> match name -> key -> apply.

So the search segment is possible, and the roadmap's reasoning for the AEGP layer
being load-bearing holds up: this list has no clean ExtendScript equivalent.

### Three things in the catalogue that the search feature must handle

None of these are failures - they are the shape of the data, and the reason
dumping the list beat counting it.

**1. Match names are TRUNCATED AT 31 CHARACTERS.** `PF_MAX_EFFECT_NAME_LEN` is
31 (`AE_Effect.h:324`), and seven entries sit exactly at the cap with visible
truncation:

    PC_TransitionChromaticAberratio     (…Aberration)
    PC_TransitionRetroFilmEssential     (…Essentials)
    Mettle SkyBox Chromatic Aberrat     (…Aberrations)

This matters for persistence. A settings file that stores a *real* match name
copied from documentation will not match what this API returns. **Always
round-trip through the API's own strings**, and treat a 31-character match name
as possibly ambiguous - two effects sharing their first 31 characters would be
indistinguishable here.

**2. 50 entries are in `_Obsolete` / `Obsolete`,** and they collide with live
effects on DISPLAY name: "Levels", "Threshold", "Photo Filter", "Noise",
"Exposure", "Box Blur" all appear twice. A naive fuzzy search over display names
would happily offer the dead one. Filter on category.

**3. 107 entries have an EMPTY category** - `ADBE CM …`, `ADBE DE …` and
`Pseudo/ADBE …`. These are pseudo-effects and preset control rigs, not things a
user picks from an effects menu. Applying one directly is probably wrong. The
`Pseudo/` match-name prefix flags a subset; empty category flags the rest.

Consequence for the design: **display name is not a unique key** ("Animated Shape
Control" appears twice with different match names). Search on display name, but
carry the match name as identity and the category as the disambiguator.

## Phase 0 gate - final Windows status

| Spike | Outcome |
|---|---|
| S1 AEGP hello world | PASS |
| S2 hold gesture | PASS - detectable and takeable |
| S3 overlay window | PASS - in-process and out-of-process |
| S4 macOS parity | BLOCKED - needs a Mac |
| S5 effects catalogue | PASS |

Four of five. Every Windows-side question is answered, and none of them came back
"no". Per the roadmap's gate table, S2 passing means the full product is
available; S4 alone decides whether it is available on both platforms or whether
macOS falls back to hotkey summon.

## Porting map - what S4 actually costs

Not "just build". Roughly half the spike code is AEGP (portable) and half is
Win32 (not). By call site: ~85 AEGP suite calls, ~86 Win32 calls, in 1592 lines.

**Ports as-is - compile and run:**

| Spike | Why |
|---|---|
| S1 entry point, command/menu/update hooks, ExtendScript | pure AEGP |
| S5 catalogue enumeration + apply-by-match-name | pure AEGP |

`pieFX_PiPL.r` already declares `CodeMacIntel64` / `CodeMacARM64`, so the
PiPL needs nothing. What is missing is an Xcode project and an Info.plist - copy
the pattern from `AEGP/Persisto/Mac/`, which is the same sample we modelled the
Windows side on.

**Must be rewritten - it is Win32 all the way down:**

| Windows | macOS |
|---|---|
| `SetWindowsHookEx(WH_MOUSE, …, GetCurrentThreadId())` | `NSEvent addLocalMonitorForEventsMatchingMask:` |
| swallow by returning 1 from the hook | consume by returning `nil` from the monitor block |
| `SendInput` replay of a short click | `CGEventPost`, same idea |
| `SetTimer(NULL, …)` hold clock | `dispatch_after` / `NSTimer` |
| `CreateWindowEx` + `UpdateLayeredWindow` | borderless `NSWindow`, `opaque:NO`, clear background, high window level |
| S3B helper .exe | a small .app bundle |

**S2A (the window probe) is not needed at all.** It existed to find a window we
then proved we never have to find.

**The findings port better than the code - but each is a claim about AE, so each
needs re-checking on Mac before it is leaned on:**

- selection is document state, not focus state (very likely holds - it is AE's
  data model, not an OS behaviour);
- AE opens its context menu on mouse-DOWN (**recheck** - macOS menus are NSMenu
  and right-click semantics differ);
- nothing can identify an AE panel (probably the same, and irrelevant either way
  since the design no longer needs it);
- AE's modal drag loop starves the AEGP idle hook (the Mac equivalent is unknown,
  but a local event monitor does not depend on idle time, so this may not arise).

**The strategy ports even where the code does not.** Swallow the DOWN, replay a
short click, decide at release - that shape is the same on both platforms.

---

# macOS results (see MAC_RESULTS.md for the full account)

**S4 PASSES - detection and swallow.** 23/23 holds at 202-211ms, every one via
`dispatch_after`, so Cocoa's run loop does NOT starve the way AE's Windows modal
loop does and the `CFRunLoopTimer` contingency was never needed.

**The roadmap's gate table is now pass/pass: the full product, hold gesture on
both platforms.**

Three findings from the Mac bench that change what we thought:

**1. A local monitor does NOT see the whole press.** Watch-only mode logged 23
DOWNs and 1 UP. Once AE opens a context menu, NSMenu's tracking loop eats mouse
events before any local monitor sees them. Two consequences: it independently
confirms AE raises the menu on mouse-DOWN, since only an already-tracking menu
could eat the UP; and **swallow is not optional on macOS** - watching without
swallowing can never observe the end of a press.

**2. The permission premise was half wrong, and the half that was wrong was
load-bearing.** `pieFXMac.mm` justified the local monitor partly on "no
Accessibility prompt". Watching and swallowing are indeed free. But the
`CGEventPost` *replay* tripped `AXIsProcessTrusted` - and that prompt names
**After Effects**, not the plug-in, because a plug-in cannot hold its own TCC
entitlement. Shipping it would have meant every user granting Accessibility to
AE before a short right-click behaved normally, and users on managed Macs being
unable to at all.

Fixed by not synthesising anything: retain the original `NSEvent` when it is
swallowed and hand it back with `[NSApp postEvent:atStart:NO]`. Verified with
the permission **revoked** and `AXIsProcessTrusted()` reported in the log - no
prompt, and the menu still opened under the cursor. That last part is the half
no log can see; a silent discard would have produced an identical log.

This is a better answer than the Windows one, and worth back-porting in spirit:
a re-posted event carries its own true window, view and location, so the
coordinate maths - and the multi-monitor failure mode it carried - simply
disappear.

**3. `CountSelection` on Mac was measuring nothing** (`AEGP_GetFirstProjItem`
with a NULL project handle errors immediately) and returned 0 unconditionally,
so S3's first run reported "0 before, 0 after" and passed for the wrong reason.
A measurement that cannot fail is not a measurement - the same lesson S2C taught
on Windows, in a different disguise.

## Two things to carry into the POC

**The 300ms replay window is a real, if small, behavioural limit.** It replaced a
counter-based guard that would have desynced given finding 1. But it means a
genuine right-press starting within 300ms of a replayed short click is passed
through un-gestured. Fine for a spike; worth a deliberate decision before the
gesture ships, since fast repeated right-clicks are a real thing people do.

**The S5 encoding note needs a correction.** MAC_RESULTS suggests moving to "the
Unicode accessors" - **there are none.** `AEGP_GetEffectName` /
`AEGP_GetEffectCategory` have no Unicode variant, not even in `AEGP_EffectSuite5`
(only `AEGP_ReportInfoUnicode` exists, for dialogs). The fix is to *decode* the
legacy single-byte text with the system encoding, not to switch API.

Related lead spotted while checking: `AEGP_EffectSuite5` has
**`AEGP_GetIsInternalEffect`**, which may be the principled way to filter the 107
uncategorised pseudo-effects the Windows S5 dump found, rather than keying on an
empty category string.
