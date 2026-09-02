# pieFX — handoff

If you are picking this up cold, read this file, then `poc/README.md`,
`poc/SETTINGS.md` and `ARCHITECTURE.md`. `SPIKES.md` is the Phase 0 record and
is still the reason several obvious-looking approaches are known to fail.
Everything below is true as of 2026-09-01.

## One-paragraph state

Phase 0 (capability spikes) is complete, the POC is complete, and the product now
runs **end to end in After Effects with nothing started by hand**: arm it from the
Window menu, right-press and hold, and a hexagon wheel appears under the cursor
with one level of drill-down. Releasing on a slot fires a real action — an AE menu
command, a user's ExtendScript, an effect by match name, or the built-in anchor
grid. **All five executor kinds are proven live from the gesture.** What is
missing is not capability but polish: settings have no UI, the Effects search is a
mock, and nothing greys out when an action cannot apply. The tables below are the
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
| Overlay dies with AE (death hook, plus an `--owner-pid` watchdog) | **not yet** — written, built, unwatched |

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
- **Settings have no UI.** The format is designed (`poc/SETTINGS.md`) and
  `load_settings` / `save_settings` exist in Rust, but nothing calls them: there
  is no settings file, and the slot tree is the `DEFAULTS` constant in
  `hexwheel.js`. `armMode` likewise defaults to `"center"` in code.
- **The Effects search widget is a mock.** It draws and fires nothing. S5 found
  519 installed effects, so its real form is a filter field over the catalogue.
- **Per-slot context gating is still not watched in AE.** The schema now
  has `requires: "selection" | "comp"`, the plug-in sends `hasComp` alongside
  `hasSelection`, and the overlay draws un-appliable slots dead, refuses to fire
  them and says why in a toast. Verified only by a headless harness over
  `hexwheel.js`, plus the browser HUD (`S` and `C` fake the context). Not seen
  live, and the native side has not been recompiled since.
- **A `script-snippet` needs its script already loaded.** `_mn.addMasterNull(...)`
  works only once `ag_masterNull.jsx` has been run, because AE shares one
  ExtendScript namespace. Intended fix: an action declares the global it needs
  and the file to load, plus a one-line guard in the user's script
  (`if (!$.global.__pieFXHeadless) showUI(thisObj);`) so a headless load does not
  pop its palette.
- **macOS is untouched since the rename.** The Mac tree has not been rebuilt.
- **The gesture is always armed once toggled on.** No per-panel gating, and
  right-DRAG inside AE is still untested (S2D left that open).

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
4. **All were found by an offline harness, not by AE.** `scratchpad/pipe_test*.ps1`
   mimics the plug-in with .NET named pipes and drives the real overlay binary.
   It should have existed before the first build was handed over. Prefer it to
   burning an AE session.

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
    poc/overlay/         the Tauri app (src/hexwheel.js is the wheel)

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

1. **Confirm the `requires` gating live.** The code is in and `SETTINGS.md`
   carries the rule; what is missing is one AE session. Rebuild the native
   plug-in first — `hasComp` is new in the summon message. With nothing
   selected the wheel should still appear, with `Create` and `Queue Comp to
   Render` live and the rest dead, and releasing on a dead slot should toast
   what is missing rather than fire.
2. **The settings UI**, as a large clickable wheel with an inspector: label,
   action kind, kind-specific fields, and a **test-fire button per binding** —
   which is the practical safety net for menu ids and names alike. Wire it to
   `load_settings` / `save_settings`, which already exist and are unused.

`Save Frame as PNG` no longer goes through the Render Queue: it is a snippet
that opens a save dialog and calls `saveFrameToPng` at 1:1, restoring the comp's
resolutionFactor afterwards. Unwatched in AE.

After that: the real Effects search (filter over the S5 catalogue), script
bootstrap, then the macOS port. `ARCHITECTURE.md` is still accurate; the Mac side
needs the two-pipe transport and the `ready` handshake replicated.

**Before handing over any build, run `poc/pipe_test.ps1`.** It drives the real
overlay binary with no AE involved and has caught every transport bug in this
project — the freeze, the startup race, and the swapped launch flags.

