# pieFX

A cursor-anchored radial menu for After Effects. Press and hold summons a wheel
under the pointer; flick to a segment, release to fire.

**Status: Phase 0 — capability spikes.** There is no product here yet. This
repository currently holds throwaway code whose only job is to answer, cheaply,
whether the thing is buildable at all before any of it becomes expensive.

## Where it stands

| Spike | Question | Outcome |
|---|---|---|
| S1 | Does an AEGP plugin load, register a menu command, and dispatch? | **PASS** |
| S2 | Can we detect right-button-held-past-200ms, and *take* it, without breaking normal right-click? | **PASS** |
| S3 | Can a transparent overlay stay above AE without costing the layer selection? | **PASS** |
| S4 | Same as S2, on macOS | **not run — needs a Mac** |
| S5 | Can we enumerate every installed effect and apply one by match name? | **PASS** (519/519) |

`SPIKES.md` is the full log: what each spike measured, what it found, and — in
two cases — what it got wrong first and how that was caught. Worth reading before
changing anything, because several obvious-looking approaches are already known
to fail.

The three findings that shaped everything:

- **Nothing can identify an AE panel.** Every panel shares one window class on
  Windows, and `AEGP_WindowType` only updates when AE rebuilds its menus. So the
  design gates on AE's *selection state*, never on which panel the cursor is over.
- **AE opens its context menu on mouse-DOWN.** The gesture is taken by swallowing
  the DOWN and replaying short clicks with `SendInput`.
- **Layer selection is document state, not focus state.** The overlay can take
  focus like a normal window, even from another process.

## Layout

    RadialMenu.cpp        Windows spikes: S1, S2 (hook + swallow), S3, S5
    RadialMenu.h
    RadialMenu_PiPL.r
    Win/                  Visual Studio project
    S3B_Overlay.cpp       Throwaway .exe: the S3 overlay from a separate process

    RadialMenuMac.mm      macOS spikes: S1, S3, S4, S5  (NEVER COMPILED)
    RadialMenuMac_PiPL.r
    Mac/                  Info.plist  (no Xcode project yet — see MAC_SESSION.md)

    SPIKES.md             The spike log. The actual deliverable of Phase 0.
    MAC_SESSION.md        Ordered checklist for the first macOS session.

## Building

Needs the After Effects SDK. Drop this directory into
`Examples/Template/` inside your SDK copy — the project files use SDK-relative
include paths and expect that depth.

**Windows.** Set `AE_PLUGIN_BUILD_DIR` to a writable directory, then build
`Win/RadialMenu.sln` (`Debug|x64`). Output lands in `$(AE_PLUGIN_BUILD_DIR)\AEGP\`.
Close After Effects first — it locks the loaded `.aex`.

Install to After Effects' **own** `Support Files/Plug-ins/` directory, *not* the
shared MediaCore path used for effect plugins: MediaCore is shared with Premiere,
which has no AEGP host.

A clean build proves nothing about loading. Verify the export:

    dumpbin /EXPORTS RadialMenu.aex

It must show a bare `EntryPointFunc`. A mangled name means the entry point
signature drifted — it takes five parameters, and the `Commando` SDK sample
still shows a stale seven-parameter form that compiles and links silently.
Model AEGP work on `Persisto` instead.

**macOS.** Not yet buildable — the Xcode project has to be created first.
`MAC_SESSION.md` has the recipe and the test order.

## License

Open source, free. License not yet chosen — see the roadmap.
