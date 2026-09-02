# pieFX — handoff

If you are picking this up cold, read this file, then `poc/README.md`,
`poc/SETTINGS.md` and `ARCHITECTURE.md`. `SPIKES.md` is the Phase 0 record and
is still the reason several obvious-looking approaches are known to fail.
Everything below is true as of 2026-09-02.

## One-paragraph state

Phase 0 (capability spikes) is complete, the POC is complete, and the product now
runs **end to end in After Effects with nothing started by hand**: arm it from the
Window menu, right-press and hold, and a hexagon wheel appears under the cursor
with one level of drill-down. Releasing on a slot fires a real action — an AE menu
command, a user's ExtendScript, an effect by match name, or the built-in anchor
grid. **All five executor kinds are proven live from the gesture.** A settings
window and auto-arm-on-launch are now written but **have not been watched inside
AE** — that is the first thing the next session should do. The remaining known
gap is the Effects search, which is still a mock. The tables below are the
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
| A settings file changes what the wheel fires | harness — a rebound `S` fired the rebound command |
| The whole harness still passes after the frontend was split into modules | harness |

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
- **The one fault found in that first AE session was the window opening BEHIND
  After Effects**, which reads as it not opening at all. The cause is that
  Windows refuses `SetForegroundWindow` to a process that does not own the
  foreground, and the foreground belongs to AE — the click went to AE's menu,
  not to the overlay. `.focused(true)` at build time asks for the same
  privilege and is refused just as quietly. The fix is `raise()` in `lib.rs`:
  a bounce through always-on-top (a **z-order** change, which needs no
  foreground rights and is therefore the part that cannot be refused),
  followed by the `AttachThreadInput` dance for the input focus that z-order
  does not give. **Written, and NOT yet confirmed in AE.**
- **The attempt to reproduce that bug offline FAILED, and the failure is worth
  knowing.** A script that starts the overlay, gives another app the
  foreground and then asks for the settings window passes *against the
  pre-fix binary too* — so it never reproduced the condition and proves
  nothing about the fix. The likely reason: an overlay launched by the test a
  moment earlier still holds foreground rights inherited from its launcher,
  while the real overlay was launched by AE at startup and those rights have
  long expired by the time anyone clicks the menu. A test that passes before
  and after the fix is worse than no test, so it was not kept. Confirming this
  one needs a real AE.
- **The Effects search widget is a mock.** It draws and fires nothing. S5 found
  519 installed effects, so its real form is a filter field over the catalogue.
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
- **Arming rule `center`** — level 2 stays inert until the cursor passes back
  through the middle, which doubles as cancel. To become a user setting.
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

    pieFX.cpp/.h         FROZEN Phase 0 spikes (S1,S2,S3,S5) — reference only
    Win/                 spike VS project
    pieFXMac.mm, Mac/    macOS spikes

    poc/README.md        Build + run + verification steps
    poc/SETTINGS.md      The action model and settings format
    poc/native/          the product plug-in (pieFX.cpp/.h, Win/)
    poc/scripts/         ExtendScript that SHIPS with pieFX; install beside the
                         .aex. ag_masterNull.jsx is a vendored fork of the
                         author's, with the __pieFXHeadless guard added.
    poc/overlay/         the Tauri app. src/hexwheel.js is the live wheel and
                         src/settings.js the settings window; the two share
                         menu.js (the slot tree), hexdraw.js (the glass
                         renderer) and actions.js (firing), so what you
                         configure is drawn and fired by the same code that
                         draws and fires the real thing.

## Building

**Native:** set `AE_PLUGIN_BUILD_DIR`, build `poc/native/Win/pieFX.sln`
`Debug|x64`, verify with `dumpbin /EXPORTS` that `EntryPointFunc` is **un-mangled**
(the entry point takes FIVE parameters; `Commando` shows a stale seven-param form
that links silently and fails to load — model on `Persisto`). Install to AE's own
`Support Files/Plug-ins/`, not MediaCore.

**Overlay:** `cd poc/overlay/src-tauri && cargo build --release` →
`pieFX-overlay.exe`. Copy it **beside the .aex** and the plug-in launches it
automatically; otherwise `npm run tauri dev` works and the plug-in's launch
no-ops.

## Next steps for a fresh session

Roughly in order. The first two are cheap and close open measurements; the third
is the real remaining feature work.

1. **Confirm the settings window now opens IN FRONT of After Effects**, and
   that the first keystroke goes to it rather than to AE. If it still lands
   behind, the next move is not another guess: it is to read
   `%TEMP%\piefx_overlay.log`, which now says whether the window was raised
   and whether its HWND was even obtainable. The escalation after that is to
   leave the window always-on-top for its whole life — correct for what is
   effectively a modeless dialog of AE's, and only avoided because a settings
   window that floats over everything forever is its own annoyance.

`Save Frame as PNG` no longer goes through the Render Queue: it is a snippet
that opens a save dialog and calls `saveFrameToPng` at 1:1, restoring the comp's
resolutionFactor afterwards. Unwatched in AE.

After that: the real Effects search (filter over the S5 catalogue), then the
macOS port. `ARCHITECTURE.md` is still accurate; the Mac side
needs the two-pipe transport and the `ready` handshake replicated.

**Before handing over any build, run `poc/pipe_test.ps1`.** It drives the real
overlay binary with no AE involved and has caught every transport bug in this
project — the freeze, the startup race, and the swapped launch flags.

It passes the overlay `--settings none`, which pins it to the built-in
`DEFAULTS`. That flag was added the moment the wheel started reading a settings
file, because everything the harness asserts is a default binding: without it,
the harness would pass or fail according to whatever the developer happened to
have configured, and the failure would read as a transport bug — the one thing
the harness exists to catch. A test that depends on the tester's own settings
is not a test.

