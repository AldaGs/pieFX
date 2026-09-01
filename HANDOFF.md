# pieFX — handoff

If you are picking this up cold, read this file, then `SPIKES.md`. Everything
below is true as of 2026-09-01.

## One-paragraph state

pieFX is a cursor-anchored radial menu for After Effects (AEGP plugin, not a
pixel effect). **Phase 0 — the capability spikes — is complete. All five pass on
both Windows and macOS.** Nothing that could kill the product remains unknown.
There is no product yet: the code in this repo is throwaway spike code whose only
job was to answer "is this buildable at all?" before any of it got expensive.
The next phase is the POC (one wheel segment: Anchor).

## The gate is pass/pass

The roadmap's Phase 0 gate turns on S2 (Windows gesture) and S4 (macOS gesture).
Both pass, so per the roadmap: **the full product — hold gesture on both
platforms**, not the hotkey fallback.

| Spike | Question | Windows | macOS |
|---|---|---|---|
| S1 | AEGP loads, registers a command, dispatches ExtendScript | pass | pass |
| S2/S4 | Right-hold is detectable AND takeable, right-click intact | pass | pass |
| S3 | Transparent overlay stays above AE; selection survives focus | pass | pass |
| S5 | Enumerate all installed effects; apply by match name | pass (519/519) | pass (454/454) |

## The findings that constrain every design decision from here

These are *why* the code looks the way it does. Do not re-litigate them without
re-reading `SPIKES.md` — several obvious-looking alternatives are already known
to fail.

1. **Nothing can identify which AE panel the cursor is over.** Windows: every
   panel shares the window class `DroverLord - Window Class`. Both platforms:
   `AEGP_WindowType` only refreshes when AE rebuilds its menus, so it is stale by
   construction. **Consequence: the design gates on AE's *selection state*
   (`AEGP_GetActiveItem`, the comp's selection collection), never on panel
   identity.** This killed the roadmap's original "resolve the viewer window by
   class name" plan.

2. **AE opens its context menu on mouse-DOWN.** So the gesture is taken by
   swallowing the DOWN, and — if the press turns out to be a short click —
   replaying it so the menu still appears. Swallowing the UP does nothing
   (learned the hard way; a message trace lied and a user's eyes corrected it).

3. **Layer selection is document state, not focus state.** The overlay can take
   focus like a normal window, even from a separate process. No `NOACTIVATE`
   tricks; the planned separate-process (Tauri) overlay is viable.

## Platform mechanics (what actually differs)

| | Windows | macOS |
|---|---|---|
| Capture | thread-local `SetWindowsHookEx(WH_MOUSE, …, GetCurrentThreadId())` | `addLocalMonitorForEventsMatchingMask:` |
| Swallow | return 1 from the hook | return `nil` from the monitor block |
| Replay a short click | `SendInput` (synthesised) | **re-post the ORIGINAL `NSEvent`** via `[NSApp postEvent:atStart:NO]` |
| Hold clock | `SetTimer(NULL, …)` (AE's modal loop starves the idle hook) | `dispatch_after` on the main queue (no starvation) |
| Overlay | `WS_EX_LAYERED|TOPMOST` + `UpdateLayeredWindow` | borderless `NSWindow`, `NSStatusWindowLevel`, per-pixel alpha |

**The single most important macOS lesson:** never synthesise input with
`CGEventPost` from an AE plugin. It is gated on `AXIsProcessTrusted`, and the
permission prompt names *After Effects*, not the plugin — a plugin cannot hold
its own TCC entitlement. Re-posting the original `NSEvent` needs no permission
and carries its own true window/view/location, so all coordinate maths
disappears. (Windows still synthesises via `SendInput` and owns that risk.)

macOS also revealed that **a local monitor cannot see the whole press once a menu
is tracking** — NSMenu's tracking loop eats the mouse-UP. So on macOS swallowing
is mandatory, not optional: without it you never observe the end of a press.

## Repo layout

    README.md            Public-facing overview
    HANDOFF.md           This file
    SPIKES.md            The full spike log — the real deliverable of Phase 0
    MAC_SESSION.md       The pre-written macOS checklist (now executed)
    MAC_RESULTS.md       What the macOS bench actually found

    RadialMenu.cpp/.h    Windows spikes S1,S2,S3,S5
    RadialMenu_PiPL.r
    Win/                 Visual Studio project
    S3B_Overlay.cpp      Throwaway .exe: S3 overlay from a separate process

    RadialMenuMac.mm     macOS spikes S1,S3,S4,S5 (built + passing)
    RadialMenuMac_PiPL.r
    Mac/                 Xcode project, Info.plist, build_and_install.sh

## Building (fast reminder — full detail in README)

- **Windows:** set `AE_PLUGIN_BUILD_DIR`, build `Win/RadialMenu.sln` Debug|x64,
  install to AE's own `Support Files/Plug-ins/` (NOT MediaCore). Verify with
  `dumpbin /EXPORTS` that `EntryPointFunc` is un-mangled.
- **macOS:** `./Mac/build_and_install.sh` (quit AE first; needs sudo to copy).
- Both: the entry point takes FIVE parameters. The `Commando` SDK sample shows a
  stale seven-param form that compiles and links silently but exports mangled and
  fails to load. Model AEGP work on `Persisto`.

## What Phase 0 deliberately did NOT settle — the POC's opening questions

- **Naming.** The product is "pieFX" but the code still says `RadialMenu`
  everywhere (match names, menu strings, filenames). Renaming is cheapest now,
  before the POC hardens any of it. **Decision pending.**
- **Architecture lock.** The roadmap wants two processes: the AEGP plugin (C++,
  owns input + catalogue + ExtendScript) and a Tauri overlay/settings app
  (Rust + webview), talking over a local socket / named pipe. S3B proved the
  separate-process overlay works. Lock this before writing POC code.
- **The 300ms replay window (macOS).** A genuine right-press within 300ms of a
  replayed short click passes through un-gestured. Fine for a spike; decide
  deliberately before ship, since fast repeated right-clicks are real.
- **Effect-name encoding (S5).** On a non-English AE, `AEGP_GetEffectName` /
  `AEGP_GetEffectCategory` return legacy single-byte text and there is **no
  Unicode variant** (not even in `AEGP_EffectSuite5`). Must be *decoded*, not
  swapped for another API, before those names reach a menu.
- **Pseudo-effect filtering (S5).** 107 catalogue entries have an empty category
  (pseudo-effects, preset rigs). `AEGP_EffectSuite5::AEGP_GetIsInternalEffect`
  is likely the principled filter, rather than testing for an empty string.

## The POC, when it starts

Per the roadmap: one segment only — **Anchor**, the 3×3 grid. Pure math, no
dialogs, instantly verifiable, and genuinely annoying in stock AE. The S1 anchor
script already does the hard part (moving the anchor while compensating Position
through the layer's Scale/Rotation so the layer does not jump — 2D only so far;
full 3D orientation is a POC problem). Windows only for the POC; Mac comes at
MVP. Hardcoded layout, no settings, no persistence.
