# pieFX — handoff

If you are picking this up cold, read this file, then `poc/README.md`,
`poc/SETTINGS.md` and `ARCHITECTURE.md`. `SPIKES.md` is the Phase 0 record and
is still the reason several obvious-looking approaches are known to fail.
Everything below is true as of 2026-09-01.

## One-paragraph state

Phase 0 (capability spikes) is complete and **the POC is complete and verified
live**: right-hold summons a wheel under the cursor, a flick picks a slot, and
release fires a real action in After Effects. Since then the product has grown a
**hexagon wheel with one level of drill-down**, an **action model** that can bind
a slot to AE menu commands / user scripts / effects, and a **native executor
table** for them. That work is MVP-shaped, so the project is no longer "the POC"
— but it is not an MVP either, because settings have no UI and several things are
coded rather than proven. The table below is the honest line between the two.

## What is actually proven

Observed working, either live in AE or against the real overlay binary driven by
the offline harness.

| | How it was proven |
|---|---|
| Hold gesture: detect, swallow, replay; normal right-click intact | many live runs, both monitors |
| Overlay: z-order over AE, spans all monitors, click-through | live |
| Two-way pipe, one per direction, with a ready handshake | harness + live |
| Hexagon wheel: drill-down, arming, category defaults | harness, 5 gestures |
| `script-snippet` executor (Master Null) | **live in AE** |
| `ae-command` / `script-file` / `effect` / `anchor-grid` executors | wired and covered by the self-test; run it to confirm |
| Second AE instance gets its own pipes | harness, custom names on the command line |
| Errors reach the user as a non-modal toast | harness |

**Run `Window ▸ pieFX Self-Test (Executors)` after any install.** It fires one of
every executor kind with a layer selected and reports what it attempted; three of
the five must be judged by eye. That command exists because those paths were
"code, not facts" for too long.

## What is NOT proven, and what is missing

- **The AE command ids are the biggest unknown.** `poc/overlay/src/ae-commands-2025.json`
  was hand-tested by another developer against AE 2025; we run 2026. Checking the
  first draft of the defaults against it already caught three wrong guesses. A
  wrong-but-valid id fires the *wrong menu item*, which is worse than failing, so
  a settings **test-fire** button is a requirement, not a nicety.
- **Settings have no UI.** The format is designed (`poc/SETTINGS.md`), and
  `load_settings` / `save_settings` exist in Rust, but nothing calls them: there
  is no settings file, and the slot tree is the `DEFAULTS` constant in
  `hexwheel.js`. `armMode` likewise defaults to `"center"` in code.
- **The Effects search widget is a mock.** It draws; it fires nothing. S5 found
  519 installed effects, so its real form is a filter field over the catalogue.
- **No per-slot context gating.** Nothing greys out when an action cannot apply.
  "New Solid" needs no selection, "Master Null" does, and the schema cannot say
  so yet. This wants a `requires` field.
- **A `script-snippet` needs its script already loaded.** `_mn.addMasterNull(...)`
  only works once `ag_masterNull.jsx` has been run, because AE shares one
  ExtendScript namespace and the global has to exist. The intended fix is lazy
  bootstrap — an action declaring the global it needs and the file to load — plus
  a one-line guard in the user's script so loading it headless does not pop its
  palette.
- **macOS is untouched since the rename.** The Mac tree has not been rebuilt.

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
3. **Both were found by an offline harness, not by AE.** `scratchpad/pipe_test*.ps1`
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

## The next three things, in order

1. **Run the self-test in AE** and settle the four unconfirmed executors —
   especially whether the 2025 command ids hold on 2026.
2. **Per-slot context gating** (`requires`), so slots grey out honestly.
3. **The settings UI**, as a clickable wheel, with a test-fire button per binding.

Then: script bootstrap, the real Effects search, and the macOS port.
