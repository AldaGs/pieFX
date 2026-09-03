# pieFX — handoff

If you are picking this up cold, read this file, then `poc/README.md`,
`poc/SETTINGS.md` and `ARCHITECTURE.md`. **For the macOS port, read
`HANDOFF_MAC.md`** — the product builds and runs there too now, and the
platform seam it describes is why this file's Windows detail still reads as it
always did. `SPIKES.md` is the Phase 0 record and
is still the reason several obvious-looking approaches are known to fail.
Everything below is true as of 2026-09-02.

## One-paragraph state

Phase 0 (capability spikes) is complete, the POC is complete, and the product now
runs **end to end in After Effects with nothing started by hand**: arm it from the
Window menu, right-press and hold, and a hexagon wheel appears under the cursor
with one level of drill-down. Releasing on a slot fires a real action — an AE menu
command, a user's ExtendScript, an effect by match name, the built-in anchor
grid, or the current frame onto the clipboard. **All six executor kinds are
proven live from the gesture**, and so are the settings window and
auto-arm-on-launch. The Effects search is no longer a mock and no longer
unproven: releasing on `Effects` opens a focused window with a real filter over
the plug-in's own catalogue, and Enter applies the effect - **watched working in
the user's own AE**, catalogue file and all. `Comp > Copy to Clipboard` is
watched too, pasted out of AE into another app. The tables below are the
honest line between what has been watched working and what has merely been
written.

## What is actually proven

Observed working, either live in AE or against the real overlay binary driven by
the offline harness (`poc/pipe_test.ps1`).

| | How it was proven |
|---|---|
| Hold gesture: detect, swallow, replay; normal right-click intact | many live runs, both monitors |
| Overlay: z-order over AE, spans all monitors, click-through | live |
| Overlay auto-launches beside the .aex; no manual start | live |
| Two pipes (one per direction) + ready handshake | harness + live |
| Second AE instance gets its own pid-suffixed pipes | harness, non-default names |
| Hexagon wheel: drill-down, arming, category defaults | harness + live |
| `script-snippet` (Master Null, all variants) | **live** |
| `script-file` | **live** — self-test writes a .jsx and confirms it ran |
| `effect` by match name | **live** — Gaussian Blur appeared |
| `builtin` anchor grid via the overlay action path | **live** — anchor recentred |
| `ae-command` **from the gesture**, via `app.executeCommand` | **live** — Null and Adjustment Layer appeared |
| Menu names resolved by `findMenuCommandId` | **live** — corrected three ids the map had wrong |
| Error toast, anchored below the summon | harness |
| `Comp > Save Frame as PNG` (saveFrameToPng at 1:1, save dialog) | **live** |
| `Comp > Comp Settings` (2007) and `Comp > Queue to Render` (2161) | **live** |
| `Create > Comp` (`New Composition...`, 2000) | **live** |
| `Layer > Pre-comp` (2071) and `Layer > Split + Dup` (2158) | **live** |
| Dark-glass palette | **live**, and the user's call |
| `Create` (Solid/Null/Adjustment/Light/Camera), `Layer > Center in Comp` | **live** |
| Anchor grid on an ANIMATED layer: keyframes rewritten, no jump | **live** |
| Script bootstrap: Master Null fires on the FIRST flick, no palette | **live** |
| Per-slot `requires` greying | **live** — greys correctly with nothing selected |
| Overlay dies with AE, and AE quits clean | **live** — quit message first, then the backstops |
| The settings window, end to end in AE | **live** — the user's own session |
| The settings window opens IN FRONT of AE | **live** — after the `raise()` fix |
| A settings file changes what the wheel fires | harness — a rebound `S` fired the rebound command |
| The whole harness still passes after the frontend was split into modules | harness |
| The plug-in's walk writes `effects.json` inside AE | **live** — 522 walked, 522 claimed |
| The preset walk finds AE's own presets and the user's | **live** — 621 shipped + 1 in Documents |
| A preset APPLIED from the search (`layer.applyPreset`) | **live** — applied from the window, and ONE undo takes it back |
| The Effects search: window opens in front of AE, filters, Enter applies | **live** — the user's own session |
| `layerCount` counts LAYERS, not selected things | **live** — `layers=1` with a layer's properties selected |
| `copy-frame`: the frame reaches the Windows clipboard and pastes | **live** — pasted out of AE |
| `copy-frame` reports the frame the TIMELINE shows | **live** — checked at several playhead positions |
| `Comp > Copy to Clipboard` sends the right builtin down the pipe | harness — the clipboard half is native and needs AE |
| Releasing on `Effects` opens the search window, and nothing crosses the pipe | harness — a real window, raised, and the overlay still answers `quit` |
| The search window filters, ranks and keyboard-navigates a catalogue | harness fixture + browser preview |

Two menu commands to run after any install, both under `Window`:
**pieFX Self-Test (Executors)** fires one of every action kind, and
**pieFX Self-Test (AE Commands)** resolves candidate menu names against the
running AE and compares the two dispatch paths. They exist because these things
were "code, not facts" for too long.

## What is NOT proven, and what is missing

- **The menu-name table is closed.** `New Composition...` resolves at 2000 and
  fires (watched live); the bare `New Composition` does not resolve.
  `Composition Settings...` is 2007 and `Composition...` is 3203, both unused so
  far except the first, which is now `Comp > Comp Settings`. `Save Frame As` resolves at 2233 without
  the ellipsis the menu shows, and `Center In View` at 3819 — **the capital I is
  the whole difference**; `Center in View` returns 0. AE's menu strings are not
  case-normalised and `findMenuCommandId` does not forgive. Also resolved and
  unused so far: `Duplicate` 2080, `Fit to Comp` 2156,
  `Center Anchor Point in Layer Content` 10312, `File...` 2003,
  `Photoshop Layers...` 5002.
- **`(5027 :: 12)`** appeared once after the AE Commands probe and is
  unattributed. Possibly `executeCommand(2263)`, the id that fires and does
  nothing. Watch for it.
- **The settings UI works in After Effects**, watched by the user: the window
  opens from `Window > pieFX Settings` and the editing, test-fire and save all
  do what they claim. `DEFAULTS` in `menu.js` is now only the fallback for a
  machine with no settings file.
- The settings window opening BEHIND After Effects is **fixed and confirmed
  live**. Windows refuses `SetForegroundWindow` to a process that does not own
  the foreground, and the foreground belonged to AE — the click went to AE's
  menu, not to the overlay; `.focused(true)` asks for the same privilege and is
  refused just as quietly. `raise()` in `lib.rs` bounces through always-on-top
  (a **z-order** change, which needs no foreground rights and so cannot be
  refused) and then does the `AttachThreadInput` dance for the input focus that
  z-order does not give.
- **The attempt to reproduce that bug offline FAILED, and the failure is still
  worth knowing.** A script that starts the overlay, gives another app the
  foreground and then asks for the settings window passes *against the
  pre-fix binary too* — so it never reproduced the condition and proves
  nothing about the fix. The likely reason: an overlay launched by the test a
  moment earlier still holds foreground rights inherited from its launcher,
  while the real overlay was launched by AE at startup and those rights have
  long expired by the time anyone clicks the menu. A test that passes before
  and after the fix is worse than no test, so it was not kept. Confirming this
  one needs a real AE.
- **`copy-frame` fired NOTHING on its first live run, and the reason is the
  rule.** The kind was added to the enum, to the wire parser and to the
  self-test, and never to `ExecuteAction`'s switch - so the message crossed the
  pipe, parsed cleanly, queued, and fell out of `default: break;` with no toast
  and no log line. The default arm of a dispatch switch is where silence hides;
  it now names the kind and toasts. Nothing offline could have caught it (the
  harness ends at the pipe) but **probe 6 of the executor self-test would have**,
  and it existed - it was written in the same commit and never run.
- **`copy-frame` is watched live and pastes.** `Comp > Copy to Clipboard`
  writes the frame to `%TEMP%\pieFX_clipboard_frame.png` through
  `saveFrameToPng` at 1:1, decodes it with WIC and puts three formats on the
  clipboard ("PNG", CF_DIBV5, CF_DIB - see SETTINGS.md for why three). Watched:
  the toast, the log, and a successful paste out of AE.
- **Its second failure was a check in the wrong place, and that is the reusable
  part.** The script asked `f.exists` the instant `saveFrameToPng` returned and
  reported "AE wrote no frame" - a question that cannot tell "AE failed" from
  "the bytes are not on disk yet", so the only answer it ever gave was the one
  that stopped everything. The existence check now belongs to the side that has
  to open the file anyway: the plug-in deletes the previous copy first, then
  waits for the same non-zero size twice in a row. **A test whose failure mode
  is indistinguishable from the thing it is testing for is not a test** - the
  same shape as the settings-window bug that passed against the pre-fix binary.
- **The frame number is right**, watched across several playhead positions in
  one session. `Math.round(c.time/c.frameDuration) + displayStartFrame` is
  therefore measured, not merely reasoned about - the first run reported
  `Frame 0` and proved nothing, because a formula that is wrong in every term
  still prints 0 at the start of a comp. The one case still untried is a comp
  whose first frame is NOT 0, which is the only thing `displayStartFrame`
  contributes.
- **Animation presets are in the search, and it works end to end in AE.** AE's own
  Effects & Presets panel lists effects and presets together, so a search that
  offered only effects answered half the question. Presets have no AEGP
  enumeration and no AEGP apply: the plug-in FINDS them by walking folders (the
  shipped `Support Files/Presets`, located from the plug-in's own module path,
  plus every `Documents/Adobe/After Effects*/User Presets`) and APPLIES them
  through the scripting DOM's `layer.applyPreset`, under one undo group because
  a preset can add half a dozen effects at once. Watched: 621 shipped presets
  plus one saved in Documents, in one list with the effects, and one of them
  applied to a layer from the window, and a single Ctrl+Z taking the whole
  preset back off - which is what the undo group is for and the last thing in
  this feature that was reasoned rather than seen.
- **The shipped-presets root is found by CLIMBING, not by counting levels**, and
  the first version counted. It went two folders up from the .aex on the
  assumption that a plug-in sits directly in `Plug-ins` - and the author's sits
  in `Plug-ins\AGS\`, so it looked in `Plug-ins\Presets`, found nothing, and
  reported zero shipped presets. **The user-presets half of the same walk
  worked**, which is what made the report diagnostic rather than "presets do not
  work": one half working narrows a fault to what the halves do not share. It
  now climbs until an ancestor holds both `Presets` and `Plug-ins` - the pair
  that identifies Support Files - and logs the module path and every folder it
  ruled out.
- **Documents is not `%USERPROFILE%\Documents`.** Measured while writing that
  walk: on the author's machine it is `C:\Users\aldai\OneDrive\Documentos` -
  redirected to OneDrive AND localised. `SHGetFolderPath(CSIDL_PERSONAL)`
  follows both; the hand-built path would have found zero user presets and
  reported that as "you have none", which is the same silent-wrong-answer shape
  as the BOM and the swapped pipe flags.
- **The Effects search works end to end in AE**, watched by the user: the
  catalogue file appears, the window comes to the front, the filter finds
  effects and Enter applies one. The multi-layer refusal and the
  clear-on-dismiss both came out of that session.
- Still unmeasured about it: whether `walked` equals `claimed` on a real
  machine (the file records both; nobody has read them back), and how the
  window behaves on a second monitor - it is `.center()`ed on the primary.
- Per-slot context gating is **done and watched live**: `requires` is in the
  schema, the plug-in sends `hasComp` alongside `hasSelection`, and the greying
  reads correctly with nothing selected.
- **Script bootstrap is done and watched live.** An action declares
  `needs: { global, file }` and the overlay wraps the snippet in a loader that
  runs the file only when the global is missing. `ag_masterNull.jsx` now **ships
  with the product** in `poc/scripts/`, carrying the guard
  `if (!$.global.__pieFXHeadless) { showUI(thisObj); }` so a headless load does
  not pop its palette mid-gesture. Relative `file` paths resolve against the
  install directory, so **the `scripts` folder must be copied beside the .aex**
  along with the overlay exe. The vendored copy is a FORK of the author's
  original in Dropbox: changes there do not flow here by themselves.
- `Create > Solid` at id **2038** is confirmed live: the solid lands in the comp.
- **macOS is untouched since the rename.** The Mac tree has not been rebuilt.
- **The gesture is always armed once toggled on.** No per-panel gating, and
  right-DRAG inside AE is still untested (S2D left that open).
- **Auto-arm is written and unwatched.** `gesture.armOnLaunch` ships ON: the
  plug-in reads it (and `gesture.holdMs`) from
  `%APPDATA%\pieFX\settings.json` on the FIRST IDLE and arms silently. Not
  from `EntryPointFunc` - the mouse hook binds to the calling thread and the
  overlay is a process launch, neither of which belongs in the middle of AE
  loading its plug-ins. The failure mode to watch for is the mirror of the old
  one: a broken settings file or a missing `pieFX-overlay.exe` now fails
  silently at launch instead of loudly at a click, and only the log says so.

## The bugs that cost real sessions, and what they taught

Worth reading before changing the transport or the launch path.

1. **AE froze on the first right-hold.** The pipe was made duplex but left
   synchronous, and Windows serialises I/O on a handle opened without
   `FILE_FLAG_OVERLAPPED`: the reader thread parked in `ReadFile`, so the first
   summon's `WriteFile` — from **AE's UI thread** — queued behind it forever.
   Fixed by splitting the directions into two pipes. `ARCHITECTURE.md` already
   said never to block the UI thread; the rule was written down and broken
   anyway.
2. **The first summon after arming was lost.** The pipes connected before the
   webview had registered its listener. Fixed with the `ready` handshake the
   architecture specified but the code never had.
3. **The auto-launched overlay never connected, while a hand-started one did.**
   The plug-in passed `--tx`/`--rx`, and *tx from the plug-in's side is rx from
   the overlay's*: both agreed on the words and disagreed on the meaning, so the
   launched overlay opened the write-only pipe for reading. A hand-started one
   used the defaults and worked, which hid it. Channels are now named for what
   flows through them — `--events` and `--actions` — which has no perspective to
   get backwards.
5. **The overlay survived being terminated, and the order was why.** Three
   attempts failed — a death-hook terminate, an `--owner-pid` watchdog, a job
   object — because all three were ways of *killing* a process that was stuck in
   a synchronous `ReadFile` on a pipe whose server the same hook had just torn
   down. A thread in a kernel I/O that never completes makes a process un-dead,
   and `TerminateProcess` returns success on one. The fix was to ask first:
   send `{"type":"quit"}` while the pipe is still whole, and let it leave under
   its own power. Each failed attempt was kept as a backstop; none of them is
   the plan. **Two lessons.** Log what an operation ACHIEVED, not that it was
   attempted — "terminate ok" printed as success while the process sat in Task
   Manager. And when a fix fails, the next move is the measurement that
   distinguishes the remaining theories, not the next fix: the one that cracked
   this was "which process is still in Task Manager", which cost nothing.

6. **All were found by an offline harness, not by AE.** `scratchpad/pipe_test*.ps1`
   mimics the plug-in with .NET named pipes and drives the real overlay binary.
   It should have existed before the first build was handed over. Prefer it to
   burning an AE session.

7. **A settings file the wheel could not read was ignored in SILENCE.** The
   first hand-written `settings.json` did nothing, and the wheel gave no sign
   it had even been read: `parseSettings` caught the exception and quietly
   returned `DEFAULTS`. The cause was a **UTF-8 BOM** — PowerShell's
   `Set-Content -Encoding utf8` writes one and `JSON.parse` rejects the string
   outright. The BOM is now stripped, but the silence was the worse half: a
   parse failure is now reported (a toast on the wheel, a line in the settings
   window) instead of swallowed, because a file that is read and rejected
   without a word is indistinguishable from one that is working. And the sharp
   edge underneath: the **native** side scans the same file with `strstr` and
   is unbothered by a BOM, so `holdMs` and `armOnLaunch` were being honoured
   from a file the overlay had thrown away. Two halves of one product
   disagreeing about whether a file exists is the actual bug — and the same
   shape as the `--tx`/`--rx` mix-up above.

Also live: the overlay writes `%TEMP%\piefx_overlay.log` and the plug-in writes
`%TEMP%\pieFX_poc.txt`. Together they show which side stopped.

## Design decisions that are settled

- **Behavior B (drill-down), not paging.** A pager breaks positional constancy,
  which is the entire value of a radial menu. "More Actions" became a named
  `Layer` category instead.
- **Depth is capped at 2.** Marking-menu accuracy degrades badly past two levels.
- **Slots are positional; `null` is a real hole.** Never compact the array, or
  adding an item moves the ones already in the user's hands.
- **A category may carry a default action**, fired when you flick to it and
  release without drilling. That is what keeps the common case one flick.
- **Arming rule `distance`** — a child is live once the stroke is clear of the
  CENTRE hexagon, because once a category is open the six hexagons on screen
  are its children and the parent has moved to the middle. It replaced `center`
  (still available, along with `exit`) because neither of the other two can
  reach the child lying in its parent's own direction in a single stroke, which
  is how the rule came to be reported as a bug. The centre still cancels.
- **Category defaults are unreachable under `distance`**, and that is a
  consequence, not an oversight: the band that fires one is the four pixels
  between the dead zone and the arming radius. A default that must stay
  reachable has to exist as a child slot too. **`Master Null`'s plain variant
  is the one case in the shipped tree that does not**, and it has two empty
  slots (SE, NW) waiting for it — an open decision, because filling a hole
  moves nothing but is still the user's menu.
- **The overlay owns geometry and hit-testing; native is a dumb executor.** The
  native side has no opinion about which slot is under the cursor; having both
  decide is what made every gesture fire twice, once.
- **Menu commands persist the numeric id, never the name.** Names are localised,
  are not unique, and are not AE's display strings anyway.
- **Free text crosses the pipe base64-encoded.** A hand-rolled JSON unescaper for
  arbitrary user script is a bug waiting for its first quote character.
- **Settings do not live on the wheel.** The centre hex is cancel; putting
  settings there would open a window on every aborted gesture. They belong in
  `Window ▸ pieFX Settings`, and the settings screen should *be* a clickable
  wheel.

## Repo layout

    README.md            Public-facing overview
    HANDOFF.md           This file
    SPIKES.md            The Phase 0 log — still the record of what cannot work
    ARCHITECTURE.md      The locked two-process design
    MAC_SESSION.md       Pre-written macOS checklist (executed once)
    MAC_RESULTS.md       What the macOS bench found
    MAC_PORT.md          What porting the PRODUCT would take, and in what order

    pieFX.cpp/.h         FROZEN Phase 0 spikes (S1,S2,S3,S5) — reference only
    Win/                 spike VS project
    pieFXMac.mm, Mac/    macOS spikes

    poc/README.md        Build + run + verification steps
    poc/SETTINGS.md      The action model and settings format
    poc/native/          the product plug-in (pieFX.cpp/.h, Win/)
    poc/scripts/         ExtendScript that SHIPS with pieFX; install beside the
                         .aex. ag_masterNull.jsx is a vendored fork of the
                         author's, with the __pieFXHeadless guard added.
    poc/overlay/         the Tauri app. src/hexwheel.js is the live wheel,
                         src/settings.js the settings window and src/search.js
                         the effect search (with its fixture,
                         src/effects-sample.json); they share
                         menu.js (the slot tree), hexdraw.js (the glass
                         renderer) and actions.js (firing), so what you
                         configure is drawn and fired by the same code that
                         draws and fires the real thing.

## Building

**Native:** set `AE_PLUGIN_BUILD_DIR`, build `poc/native/Win/pieFX.sln`
`Debug|x64`. **That path, and not the one at the repo root.** `Win/pieFX.sln`
still exists and still builds the Phase 0 SPIKE (the root `pieFX.cpp`), and both
projects link to the SAME `pieFX.aex` — so the wrong one compiles cleanly,
installs happily, and is only caught by opening AE's Window menu and finding
`pieFX S1 (Anchor to Center)` where `pieFX (Show/Hide)` should be. It has
already cost one session. Verify the built .aex if in any doubt: it must contain
the string `pieFX (Show/Hide)` and must not contain `Anchor to Center`.

Then verify with `dumpbin /EXPORTS` that `EntryPointFunc` is **un-mangled**
(the entry point takes FIVE parameters; `Commando` shows a stale seven-param form
that links silently and fails to load — model on `Persisto`). Install to AE's own
`Support Files/Plug-ins/`, not MediaCore.

**Overlay:** `cd poc/overlay/src-tauri && cargo build --release` →
`pieFX-overlay.exe`. Copy it **beside the .aex** and the plug-in launches it
automatically; otherwise `npm run tauri dev` works and the plug-in's launch
no-ops.

## The Effects search — what was built, and what is not yet true

The input model was chosen with the user before anything was drawn: **release to
open a focused window**, of the three candidates the previous handoff laid out.
The reason it won is the reason it was recommended — it is the only one whose
machinery this project had already watched working inside AE — and the reason
the other two lost is worth keeping: nothing in pieFX can receive a keystroke.
The overlay window is created click-through and `focus: false`, the plug-in
installs `WH_MOUSE` and no keyboard hook, and the gesture is a press-and-hold,
so a filter field on the wheel would need a new input path AND one-handed
typing. That has not changed; it has been designed around.

### The shape it took

    plug-in    WriteEffectCatalogue() — S5A's walk, ported out of the frozen
               spike, writing %APPDATA%\pieFX\effects.json once per session on
               the first idle AFTER arming. Everything walked is written,
               obsolete and uncategorised included: filtering is the UI's call.
               `walked` and `claimed` both go in the file, because S5's real
               assertion was never the count, it was whether the two agree.

    wheel      The `search` widget no longer draws a fake query and five
               hard-coded results, and it no longer draws a SEARCH BOX either:
               a field with a border invites typing, and the wheel cannot take
               a keystroke, so the box was a picture of a control that does
               not exist. It draws "Release to search…" as plain text over the
               RECENTS list, sized to what is in it. With more than one layer
               selected the caption turns red and says so, and the release
               refuses instead of opening: the plug-in applies through
               AEGP_GetActiveLayer, which returns a layer only when exactly
               one is selected, so a search opened there would take a query
               and an Enter and then apply nothing. `layerCount` is DISTINCT
               LAYERS, not collection items - the comp selection also holds
               masks, effects, properties and keyframes, every one of which
               carries its containing layer, so counting items reported three
               layers for one layer with a property and a keyframe clicked.
               The miscount was reported from a live run; the DISTINCT-layer
               walk that replaces it is unwatched, and the summon log line
               (`layers=`) is where to check it. Nothing in the panel is
               highlighted, because a hot row
               would promise that releasing fires that row and releasing opens
               a window. Releasing outside the dead zone opens it; the centre
               still cancels, so an aborted gesture never puts a window in
               front of AE.

    window     search.html / search.js / search.css — a third window in the
               overlay process, raised exactly like the settings window but
               with NO TITLE BAR and out of the taskbar: settings is a place
               you go and stay, this is a palette whose whole lifetime is
               "type, Enter, gone", and a close button, a minimise button and
               a title are three ways to interact with that. Substring filter
               over display name then match name, prefix matches ranked first,
               arrows to walk. Three ways out and all of them go through one
               `hide_search` command: Enter (after the effect is applied),
               Escape, and the focus going anywhere else. Hidden rather than
               closed, so the next summon is instant - and the query is
               cleared ON THE WAY OUT, because a warm window that comes back
               holding the last search costs more than it saved.

    recents    %APPDATA%\pieFX
ecents.json, its own file rather than a key in
               settings.json: the settings window writes that file whole, and
               two windows of one process saving the same file is a lost
               update waiting to happen. The wheel re-reads it on each summon.

`--effects <path>` / `--effects none` mirror `--settings`, and the harness uses
them: `overlay/src/effects-sample.json` is a nine-entry fixture carrying one of
each of the catalogue's sharp edges (a display name that collides across
categories, an `_Obsolete` entry, two internal ones, a match name at the
31-character cap), so a filter that ignores them fails in the harness instead of
in front of a user. It doubles as the browser preview's data.

### The bug this cost, which is a rule about Tauri commands

The search release fired nothing, silently, and the overlay then ignored `quit`
— the exact signature of the un-dead overlay this project has already paid for
once. A **sync `#[tauri::command]` runs on the main thread**, so building a
webview window inside one asks the event loop to do work from inside its own IPC
handler, and `run_on_main_thread` called from the main thread waits for a turn
that never comes. Either way the process goes deaf: no window, no error, no
reply to the next message. `open_search` is `async` and posts the build to the
main thread; `open_settings` was the same shape and is fixed with it — it had
never been invoked from JS, so nothing had found it. **The harness found this,
not AE**, and only because the assertion was "the log says a window was built"
rather than "nothing came down the pipe" — silence was also what the mock did.

### What is left

The three questions this list opened with - does the catalogue file appear, does
the window come to the front, does Enter apply - are all answered YES, live.
What is left is smaller and mostly unmeasured rather than unbuilt.

1. ~~`walked` vs `claimed`~~ **CLOSED: 522 walked, 522 claimed** on the
   author's AE 2026. The Phase 0 spike walked 519 on the same platform and the
   macOS bench walked 454, so the number tracks what is installed rather than
   being a constant - which is the point of writing both into the file.
2. **Recents are unproven end to end** — written by the window, read by the
   wheel, and the panel drawing them has never been seen with anything in it.
3. **A recent is not fireable from the wheel.** The panel lists them; it does
   not hit-test them. Making the first recent a one-flick re-apply is the
   obvious next move and is a real design decision, not a tidy-up: it needs
   hit-testing inside the panel and it changes what a release on `Effects`
   means depending on where the cursor is.
4. **Nothing has been done about the 31-character truncation on the way IN.**
   The window only ever hands back a match name the API itself produced, which
   is the rule, but a hand-typed `effect` binding in the settings window can
   still name a string AE will never return.

## Next steps for a fresh session

Roughly in order. Both are measurements that only a running After Effects can
make; the code they are measuring is written and passes the harness.

1. **Watch the new `distance` arming rule in After Effects.** It is measured
   against the state machine and passes the harness, but the thing it is meant
   to fix is a feeling in the hand, and only a real flick can report on that.
   Two questions in particular: does the one-flick category default still land
   where you expect (releasing ON the hexagon), and does pushing past it to a
   child feel like one motion or like two? An existing
   `%APPDATA%\pieFX\settings.json` still says whatever it was saved with, so
   a machine that has used the settings window is still on `center` until the
   dropdown is changed or the defaults are reset.

`Save Frame as PNG` no longer goes through the Render Queue: it is a snippet
that opens a save dialog and calls `saveFrameToPng` at 1:1, restoring the comp's
resolutionFactor afterwards. Unwatched in AE.

2. **Watch the Effects search in AE**, in the order its section above gives:
   the catalogue file first (it is the piece everything else stands on), then
   the window's z-order, then the apply. Nothing else in the feature can be
   judged until the walk has been seen producing a file.

After that: the macOS port. `ARCHITECTURE.md` is still accurate; the Mac side
needs the two-pipe transport and the `ready` handshake replicated.

**Before handing over any build, run `poc/pipe_test.ps1`.** It drives the real
overlay binary with no AE involved and has caught every transport bug in this
project — the freeze, the startup race, and the swapped launch flags.

It does not exercise category defaults at all: under the shipped `distance`
rule there is no reachable band to aim at, and the two rules that do reach them
(`center`, `exit`) are never selected here. That is a real coverage gap and the
cheapest way to close it is a second overlay started with `--settings <a file
pinning armMode>`, which would also be the first test of the `--settings <path>`
branch.

It passes the overlay `--settings none`, which pins it to the built-in
`DEFAULTS`. That flag was added the moment the wheel started reading a settings
file, because everything the harness asserts is a default binding: without it,
the harness would pass or fail according to whatever the developer happened to
have configured, and the failure would read as a transport bug — the one thing
the harness exists to catch. A test that depends on the tester's own settings
is not a test.

