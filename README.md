# pieFX

A cursor-anchored radial menu for After Effects. Press and hold summons a wheel
under the pointer; flick to a segment, release to fire.

**Status: working on Windows.** Right-press and hold inside After Effects and a
hexagon wheel appears under the cursor; flick to a slot and release to fire. A
slot can run an AE menu command, a snippet of your own ExtendScript, an effect by
match name, or a built-in tool such as the 3x3 anchor grid. One level of
drill-down. Settings have no UI yet, and macOS is not built.

The product lives in `poc/` — start with `HANDOFF.md`, then `poc/README.md` and
`poc/SETTINGS.md`. The files at the top level are the frozen Phase 0 spikes, kept
as reference.

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

    Win/                  the Windows installer: pieFX.iss + build_installer.ps1
    Mac/                  the macOS build, package and sign scripts

    poc/native/           THE PRODUCT plug-in (pieFX.cpp/.h, Win/, mac/)
    poc/overlay/          the Tauri overlay: the wheel, settings, effect search
    poc/scripts/          ExtendScript that ships with pieFX

    _archive/phase0-spike-win/   The Windows Phase 0 spike, retired but kept.
                          S1, S2 (hook + swallow), S3, S5 in one self-contained
                          file, plus S3B_Overlay.cpp (the S3 overlay from a
                          SEPARATE process) and Win/pieFX_spike.sln. It is the
                          floor: the smallest thing that loads as an AEGP and
                          hooks the mouse. Read its README before using it —
                          it used to build pieFX.aex over the product's.

    pieFXMac.mm      macOS spikes: S1, S3, S4, S5  (Mac/pieFXMac.xcodeproj
    pieFXMac_PiPL.r  builds them; they make pieFXMac.plugin, not pieFX.plugin,
                     which is why they did not need archiving)

    SPIKES.md             The spike log. The actual deliverable of Phase 0.
    MAC_SESSION.md        Ordered checklist for the first macOS session.
    MAC_RESULTS.md        What that session found.

## Building

Needs the After Effects SDK. Drop this directory into
`Examples/Template/` inside your SDK copy — the project files use SDK-relative
include paths and expect that depth.

**Windows.** Set `AE_PLUGIN_BUILD_DIR` to a writable directory, then build
`poc/native/Win/pieFX.sln` (`Debug|x64`). Output lands in
`$(AE_PLUGIN_BUILD_DIR)\AEGP\pieFX.aex`. Close After Effects first — it locks
the loaded `.aex`.

To make the thing other people install, `.\Winuild_installer.ps1` produces
`Win/build/pieFX-<version>-win-setup.exe`. It finds every After Effects version
in the registry, installs all three payload items together into
`<AE>\Support Files\Plug-ins\pieFX\`, refuses to run while AE is open, and
registers an uninstaller. It needs Inno Setup 6
(`winget install --id JRSoftware.InnoSetup`). **The installer is unsigned**, so
a downloaded copy trips SmartScreen — see HANDOFF.md before shipping it.

The archived spike under `_archive/` builds `pieFX_spike.aex` from its own
solution. The two used to share a filename, which is a mistake worth not
repeating: check AE's Window menu after installing. The product is
`pieFX (Show/Hide)` and `pieFX Settings`; the spike is `pieFX S1 (Anchor to
Center)` and friends.

Install to After Effects' **own** `Support Files/Plug-ins/` directory, *not* the
shared MediaCore path used for effect plugins: MediaCore is shared with Premiere,
which has no AEGP host.

A clean build proves nothing about loading. Verify the export:

    dumpbin /EXPORTS pieFX.aex

It must show a bare `EntryPointFunc`. A mangled name means the entry point
signature drifted — it takes five parameters, and the `Commando` SDK sample
still shows a stale seven-parameter form that compiles and links silently.
Model AEGP work on `Persisto` instead.

**macOS.** `./Mac/build_product.sh [--install]` builds the product; the Phase 0
spikes have their own Xcode project via `./Mac/build_and_install.sh`. See
`HANDOFF_MAC.md`.

## License

Open source, free. License not yet chosen — see the roadmap.
