# pieFX

A cursor-anchored radial menu for After Effects. Right-press and hold, and a
hexagon wheel appears under the pointer; flick to a slot, release to fire.

**Status: shipping on Windows, built and running on macOS.** Windows has a
one-click installer, the wheel fires real actions on both platforms, and the main
thing standing between macOS and a release is an Intel Mac to test on.

A slot can fire any of six things, and all six have been watched working from the
gesture inside After Effects:

| | |
|---|---|
| **AE menu command** | anything on AE's own menus, by name, via `app.executeCommand` |
| **ExtendScript snippet** | a line of your own script |
| **ExtendScript file** | a `.jsx`, loaded once and cached |
| **effect by match name** | applied to the selection |
| **preset** | from AE's own catalogue and yours — one undo takes it back |
| **built-in** | the 3x3 anchor grid, and the current frame onto the clipboard |

Plus one level of drill-down, per-slot greying when a slot needs a selection or a
comp it does not have, a settings window that rebinds any of it, and the search
below.

## The search

Release the gesture on `Effects` and a focused window opens under the cursor. It
searches three things in one list — **installed effects**, **animation presets**,
and **After Effects' own menu commands** — and applies on Enter.

After Effects 26.2 shipped **Quick Apply**, which searches the same three things
from `Ctrl+Enter`. So this is no longer the only search box in AE, and it does not
try to win on what it can reach. It is reached by a **right-press where the
cursor already is** rather than by a chord, and it can be taught three things a
dialog cannot:

**Values in the query.** `gaussian 40` applies Gaussian Blur and sets it.
`gaussian blur=40` names the property — matched as a substring, so you do not
need AE's exact wording. `levels 0.2 0.8` fills two in order. The property is
looked up **by name against the live effect**, never by index, so it works for
third-party plug-ins nobody has catalogued, and there is no list of supported
effects: any effect with a settable numeric property takes one. The row tells you
what it parsed before you commit, and a row that cannot take a value says
`ignores 40` instead of swallowing it.

**Stacking.** `Shift+Enter` applies and keeps the window up with an empty field,
so Levels then Curves then Glow is one summon instead of three. Each is still its
own undo.

**Macros.** A macro is a named list of steps, and you do not write one — you
record it. Stack the steps with Shift+Enter, type a name, press `Ctrl+S`. The
steps are known to work because they just ran. Applying the macro runs the whole
list inside **one undo group**, so one Ctrl+Z takes all of it back.
`Shift+Delete` on a macro removes it. They live in `%APPDATA%\pieFX\macros.json`,
which is a plain file you can copy to another machine.

Smaller things: recents are the list when you have typed nothing, so the common
case needs no typing at all; effects can be found by match name (`ADBE Gauss`);
and AE's 50-odd `_Obsolete` entries — which collide with live effects on names
like *Levels* and *Box Blur* — are hidden behind a checkbox rather than silently
offered.

Menu commands are searched but not browsed: there are hundreds, and an
alphabetical dump of AE's menu bar is not what you want to see the instant a
gesture ends. Type a letter and they are there. They also rank below any effect
or preset that matched, because this is an effect search that happens to know the
menu.

## Un-precompose, and a Duplicate that copies what it nests

Two verbs AE does not have, on the wheel by default. Both are ExtendScript that
ships in `scripts/ag_compTools.jsx` and loads on first use, so a slot of your own
can call them too.

**Un-precompose** (`Layer` ring, next to Pre-comp) takes the selected precomp
layer apart: the layers inside it come up into this comp, in order, in their
place. What makes it a real inverse rather than a paste is the transform. A layer
inside the precomp has coordinates in the *inner* comp's space, and its position
out here is `P + R*S*(p - A)` — the precomp layer's position, rotation, scale and
anchor point. That is also exactly what AE computes for a layer **parented** to
something with that transform. So the extracted layers are parented to a null
carrying the precomp layer's transform, keyframes and all, and their own values
are never touched. Animation stays animated; nothing is baked.

When the mapping is the identity — the ordinary "pre-comp, then change your
mind" — there is no null at all. Parenting inside the precomp is restored by
index, timing is shifted by the precomp layer's start and clipped to its bar, and
a layer that was scrolled off the end of the precomp does not become visible out
here.

It **asks before it runs**, because there is exactly one decision in it that
cannot be guessed: a precomp containing precomps either stops at the first level
or goes all the way down, and both are things people mean. The dialog counts the
nested precomps it found and offers to remove emptied comps from the project.
Bind `_ct.unPrecompose({deep: true, ask: false})` to a slot if you always mean
one of them.

What cannot come up is **said out loud** rather than dropped. Effects, masks,
opacity, a blending mode, a track matte, time remapping and time stretch are
properties of the precomp *layer* with no counterpart inside it — there is no
arrangement of extracted layers that reproduces a blur applied to the comp as a
whole. Each one that applied is named in one alert after the work is committed.

**Duplicate** (`Comp` ring) is not Edit > Duplicate. AE's duplicate leaves the
copy **sharing** every precomp inside it, so editing a precomp in the "copy"
edits the original — which is the surprise this removes. The nested comps are
duplicated too and the copy is re-pointed at them, all the way down. A comp used
twice inside the original is duplicated once and used twice inside the copy, so
the sharing that was deliberate survives and the sharing that was an accident of
duplication does not. The one thing it cannot fix is an expression that names a
comp by string: those still point at the originals, and it counts them and says
so.

Both are one undo group each.

Neither has been watched working inside After Effects yet — they are written
against the scripting DOM and syntax-checked, which is not the same thing as
proven, and this repo's rule is that the difference gets said.

## Installing

**Windows.** Download the installer and run it with After Effects closed. It
finds your After Effects versions in the registry, installs into each one's own
`Plug-ins\pieFX\` folder, and registers an uninstaller. One UAC prompt — After
Effects only ever loads plug-ins from under `C:\Program Files`, so there is no
unprivileged install to offer.

It is **unsigned** — pieFX is free, and a code-signing certificate costs more
per year than the project takes in — so a downloaded copy trips SmartScreen:
click *More info*, then *Run anyway*. See `docs/HANDOFF.md` for the decision.

Then start AE and turn it on: **Window > pieFX (Show/Hide)**. It stays armed
across launches.

**macOS.** `sudo cp -R pieFX.plugin "/Applications/Adobe After Effects 2026/Plug-ins/"`,
for now. A `.pkg` is not written yet. Signing is genuinely optional there and
that was measured, not assumed — see `docs/MAC_RESULTS.md`.

## The three findings that shaped everything

- **Nothing can identify an AE panel.** Every panel shares one window class on
  Windows, and `AEGP_WindowType` only updates when AE rebuilds its menus. So the
  design gates on AE's *selection state*, never on which panel the cursor is over.
- **AE opens its context menu on mouse-DOWN.** The gesture is taken by swallowing
  the DOWN and replaying short clicks with `SendInput`. On macOS the same problem
  has a different shape — a tracking `NSMenu` eats the mouse-UP — and a different
  answer.
- **Layer selection is document state, not focus state.** The overlay can take
  focus like a normal window, even from another process. That is the whole reason
  the settings window and the effect search can exist at all.

`docs/SPIKES.md` is the full log of what was measured to get there, including the
approaches that are known **not** to work. Worth reading before changing anything.

## Layout

    README.md             this file — the only .md at the root
    LICENSE               MIT, plus the note on the Adobe SDK
    docs/                 everything else; docs/README.md is the index

    poc/native/           THE PRODUCT plug-in (pieFX.cpp/.h, Win/, mac/)
    poc/overlay/          the Tauri overlay: the wheel, settings, effect search
    poc/scripts/          ExtendScript that ships with pieFX
                          (ag_masterNull.jsx, ag_compTools.jsx)
    poc/README.md         build + run + verify, by hand
    poc/SETTINGS.md       the action model and the settings file format

    Win/                  the Windows installer: pieFX.iss + build_installer.ps1
    Mac/                  the macOS build, package and sign scripts

    pieFXMac.mm           macOS Phase 0 spikes: S1, S3, S4, S5. Mac/pieFXMac.xcodeproj
    pieFXMac_PiPL.r       builds them; they produce pieFXMac.plugin, not
                          pieFX.plugin, which is why they did not need archiving

    _archive/             retired, kept on purpose
      phase0-spike-win/   the Windows Phase 0 spike: S1, S2 (hook + swallow), S3,
                          S5 in one self-contained file, plus S3B_Overlay.cpp and
                          Win/pieFX_spike.sln. It is the FLOOR — the smallest
                          thing that loads as an AEGP and hooks the mouse — and
                          it is what to reach for when the question is "is this
                          us, or is this AE?". Read its README first: it used to
                          build pieFX.aex, over the top of the product's.

## Building

Needs the After Effects SDK. Drop this directory into `Examples/Template/` inside
your SDK copy — the project files use SDK-relative include paths and expect that
depth.

**Windows.** Set `AE_PLUGIN_BUILD_DIR` to a writable directory, then build
`poc/native/Win/pieFX.sln` (`Release|x64`, or Debug). Output lands in
`$(AE_PLUGIN_BUILD_DIR)\AEGP\pieFX.aex`. Close After Effects first — it locks the
loaded `.aex`. The overlay is a separate binary:

    cd poc/overlay/src-tauri && cargo build --release

To make the thing other people install:

    .\Win\build_installer.ps1 -Version 0.1.0 -Rebuild

producing `Win/build/pieFX-<version>-win-setup.exe`. It needs Inno Setup 6
(`winget install --id JRSoftware.InnoSetup`). It stages all three payload items —
the `.aex`, the overlay `.exe`, and `scripts/` — and refuses to package a build
whose `.aex` is the Phase 0 spike rather than the product, which is a mistake
that has been made once.

A clean build proves nothing about loading. Verify the export:

    dumpbin /EXPORTS pieFX.aex

It must show a bare `EntryPointFunc`. A mangled name means the entry point
signature drifted — it takes five parameters, and the `Commando` SDK sample still
shows a stale seven-parameter form that compiles and links silently. Model AEGP
work on `Persisto` instead.

Install to After Effects' **own** `Support Files/Plug-ins/`, *not* the shared
MediaCore path used for effect plug-ins: MediaCore is shared with Premiere, which
has no AEGP host.

**macOS.** `./Mac/build_product.sh [--install]` builds the product;
`./Mac/package.sh` makes a zip somebody else can install. The Phase 0 spikes have
their own Xcode project via `./Mac/build_and_install.sh`. See
`docs/HANDOFF_MAC.md`.

## Known gaps

- **Intel macOS is untested.** The binaries are universal, both halves, but only
  arm64 has ever run.
- **Every macOS observation is from a Spanish AE.** That has been an asset — it
  exposed two defects that were not about localisation at all — but the English
  path on macOS is the untested one, which is an unusual way round.
- **The Windows installer is unsigned, and will stay that way.** pieFX is free
  and does not earn the ~$200-600/yr a certificate costs, so a downloaded copy
  trips SmartScreen once: *More info*, then *Run anyway*. A `winget` package is
  the planned way to skip that path entirely.
- **The newest search features have not been run inside After Effects yet.**
  Values in the query, stacking, macros and menu-command firing are all driven
  and asserted by a harness, and the generated ExtendScript has been read but not
  executed. Menu command ids in particular come from a hand-tested map for AE
  2025 that has already been wrong three times, which is why every command row
  shows its id.
- **A failed `copy-frame` freezes AE for up to 15 seconds.** It waits on AE's UI
  thread. Only reached when AE never finishes writing the frame; making it
  asynchronous is the real answer.

## License

**MIT** — see `LICENSE`. pieFX is free, and there is nothing to buy.

That licence covers the pieFX source only. The After Effects SDK it builds
against is Adobe's, under Adobe's own terms; none of it is included here or
redistributed, which is why the build instructions tell you to bring your own
copy.
