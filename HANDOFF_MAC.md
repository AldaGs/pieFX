# pieFX — the macOS handoff

If you are picking up the macOS port cold, read this file, then `MAC_PORT.md`
(the plan, with each step's outcome folded back into it) and `MAC_RESULTS.md`
(the measurements, in the order they were made). `HANDOFF.md` is the Windows
product's handoff and is still the authority on everything not platform
specific. Everything below is true as of 2026-09-03.

**Start here: finish the hand-check in AE.** The first pass through it found
two real bugs — a truncated frame on the clipboard, and the settings and search
windows opening on the wrong display. Both are fixed, and **both fixes are now
confirmed in After Effects on the author's machine.** What that pass did NOT
reach is the settings round trip, the effect catalogue, and the second-display
gesture. It is the first section under "What to do next", and its record so far
is two bugs in the first two features it touched — which is the best argument
available for finishing it.

## One-paragraph state

The port is through the risky half. **The product plug-in builds, loads in
After Effects on macOS, and the wheel works from the real gesture**: right-press
and hold, the hexagon appears under the cursor, and releasing on a slot fires an
AE menu command for real — watched working on the author's machine, which runs a
Spanish AE. Everything Phase 0 could not answer has been answered by
measurement: the overlay strategy survives, the transport is proven outside AE
by its own harness, launch and lifetime are proven, and the gesture is the
Phase 0 spike moved into a module. The second half of the
feature set was blocked behind one decision about where files live; that
decision is now taken on both sides of the wire, and the effect search, saved
settings and copy-frame are written on the back of it. Presets came last, as
planned, and are in: 621 shipped presets found against the real install.
**Nothing is stubbed on macOS any more.**

## What is actually proven

Watched working in AE, or measured by a harness against the real binaries.

| | how it was proven |
|---|---|
| the plug-in loads and arms | in AE, from the Window menu |
| the right-hold gesture, swallow and replay | in AE; also `pieFX_gesture_test` in a host that is not AE |
| the wheel draws under the cursor | in AE |
| `ae-command` slots fire | in AE, on a Spanish install, via the id fallback |
| the overlay above AE, click-through, no focus theft | `Mac/overlay_probe.swift` + by eye with AE open |
| per-summon move between displays | `overlay_probe` on a 2x and a 1x screen; by eye for the wheel position |
| the FIFO transport, both directions | `poc/pipe_test.py` (drives the overlay) and `fifo_test` (drives the plug-in side) |
| bounded write, SIGPIPE, re-accept, quit | `fifo_test`, 23 assertions |
| launch, the process group, the kqueue watchdog | `fifo_test` |
| the `.jsx` snippets ship in the bundle | `build_product.sh`, layout checked |
| the two processes agree on where files live | `pieFX_paths_test`, plus the overlay started on NO arguments finding a file written by hand at the agreed path |
| legacy effect names become UTF-8, both ways | `pieFX_paths_test`, against real MacRoman bytes |
| a frame reaches the clipboard WITH its alpha | `pieFX_clipboard_test`, read back as PNG data and as an `NSImage` |
| a half-written PNG is refused, not pasted | `pieFX_clipboard_test`, against a truncated 1600x1200 |
| **copy-frame pastes the WHOLE frame** | in AE, on the 6656x2270 comp that originally failed |
| **settings and search open on the cursor’s display** | `CGWindowListCopyWindowInfo` from outside the process, then in AE on the second display |
| **the effect catalogue, with its accents** | in AE: 456 of 456, valid UTF-8, 189 non-ASCII entries correct |
| **the preset walk** | `pieFX_presets_test` against the real AE install: 621 found, every path opens — and the list checked against AE's own by hand |
| **the overlay as a bundled `.app`** | `overlay_probe` before and after: level, alpha, bounds and z-order all unchanged; LaunchServices reports `type="UIElement"` |
| **all four window properties, bundled** | in AE: click-through, no Dock icon, no focus theft — the three a probe cannot reach |
| **applying a preset from the wheel** | in AE |
| **an unsigned, QUARANTINED plug-in loads** | in AE, with the download attribute set by hand — so signing is not a gate |
| **`holdMs` from settings.json reaches the gesture** | in AE, by feel, and the log states the armed threshold |
| **the bundle signs and verifies** | ad-hoc, on every build, inside out |
| **`ag_masterNull.jsx` from a slot** | in AE |
| **the wheel on both displays, from the real gesture** | in AE, on a 2x and a 1x screen |
| **launch and teardown from inside the bundle** | `fifo_test` run beside the `.app`: it picks the `.app`, connects, and the watchdog still fires |

## What is NOT proven

- **Intel, and an English AE.** The binary is universal — both halves of it now,
  the overlay included — but only arm64 has ever run, and every observation in
  this port is from an es_ES install, so the ENGLISH path is the untested one.
  Both are one machine away and neither can be checked here.
- **Windows, RUN, since this port began.** It now COMPILES there (Debug and
  Release, x64, no warnings), and the overlay harness passes against a
  Windows build carrying the placement fix — but no .aex from this port has
  been loaded into a running After Effects on Windows. See "What to do next".
- **Anything on an English macOS AE.** Every observation here is from an es_ES
  install. That is mostly an asset — see the traps below — but it does mean the
  English path on macOS is the untested one, which is an unusual way round.
- **Intel.** The binary is universal; only arm64 has ever run.

## Stubbed, and where

**Nothing.** All four functions that logged
`not implemented on macOS yet (MAC_PORT step 5)` are written:
`ReadSettings`, `WriteEffectCatalogue`, `CopyFrameToClipboard` and
`WritePresets`.

None of them ended up behind `#else // AE_OS_WIN`, and that is the shape of the
whole step. Each was split whole, and each turned out to contain one or two
genuinely platform-specific LINES — the config path, the encoding, the
pasteboard call, and listing a directory. The seams moved down to those lines
(`PieFX_ConfigPath`, `PieFX_LegacyToUtf8`, `PngFileToClipboard`, and
`DirOpen`/`DirNext`/`DirClose`) and the bodies are shared. Do not go looking for
a macOS copy of any of them.

## What to do next

### 1. The hand-check in AE — do this first

The first pass through this list found a real bug in each of the two features
it reached. That is the argument for the rest of it: a passing harness is weak
evidence about a feature nobody has used.

**Both halves have to be reinstalled**, and the overlay is easy to forget —
`build_product.sh` copies whatever binary is in `target/release` into the
bundle, so an overlay built after the last plug-in build is not in there.

```bash
cd poc/overlay/src-tauri && cargo build --release && cd -
./Mac/build_product.sh --install     # AE must be quit; asks for sudo
```

Then, with AE open:

- **Settings round trip.** Open `pieFX Settings` from the Window menu, change
  the hold to something obviously different (say 600ms), save, restart AE, and
  check `$TMPDIR/pieFX_poc.txt` for
  `settings: /Users/…/Library/Application Support/pieFX/settings.json -> armOnLaunch=… holdMs=600`.
  Then feel it: the wheel should need a noticeably longer press.
- ~~**The catalogue.**~~ **DONE**: 456 entries, AE claims 456, no mismatch,
  valid UTF-8, and 189 non-ASCII names correct (`Simulación`, `Corrección de
  color`). The encoding work is settled on the install that motivated it.
- **Presets, which are new and unwatched in AE.** `pieFX_presets_test` finds
  621 against the real install with every path openable, but nothing has fired
  one from the wheel. Arm pieFX and look for
  `presets: 621 under /Applications/Adobe After Effects 2026/Presets` in
  `$TMPDIR/pieFX_poc.txt`, then search for one and apply it. **Type an accented
  name** — `caída` is a good one — because 136 of AE's preset names are stored
  decomposed and were unfindable before this build normalised them.
- ~~**Copy-frame, on a BIG comp.**~~ **DONE, and it was the one that already
  failed:** a 6656x2270 frame pasted as 6656x804, because the wait for AE to
  finish writing was a timing guess and a truncated PNG keeps its full
  advertised dimensions. Retried on that exact comp and correct. The log says
  `copy-frame: complete PNG after Nms, N bytes` on success and names the byte
  count on failure, so "AE never finished" and "AE wrote nothing" are no longer
  the same message. Then paste — Preview and Photoshop are both worth trying,
  and a transparent comp is the interesting case, because it is the one Windows
  cannot do properly.
- ~~**The two windows, on the second display.**~~ **DONE**, and also a bug
  that had already failed once: settings and search now open on the screen
  under the cursor, watched working on the second display. Settings
  is placed only when it is first created, so a settings window you dragged
  somewhere stays there; search is placed on every summon, because it has no
  title bar and cannot be dragged at all.

**The second-display gesture check** belongs here too: right-hold on the
second screen and confirm the wheel appears under the cursor. It is the last
coordinate question outstanding.

### 2. ~~A Windows build~~ — DONE, and clean

The largest un-repaid debt in the project, paid on 2026-09-03 on the Windows
machine. **`poc/native/Win/pieFX.sln`** — the PRODUCT project; see the trap
below — builds **Debug and Release, x64, with no warnings and no errors**, and
it is built `/WX`, so those are hard zeroes. Every shared body this port rewrote —  `PieFX_ConfigPath`,
`PieFX_LegacyToUtf8`, `FrameFileSize`, `PngIsComplete` and the
`WaitForFrameFile` rewrite, `EmitPreset`, and the `DirOpen`/`DirNext`/`DirClose`
iterator that `WalkPresetFolder` and `WritePresets` were rebuilt on — has now
been compiled by MSVC as well as clang. It compiled on the first attempt; the
sharing was sound.

**The trap, and it bit once.** There are TWO Windows projects in this tree and
they write to the SAME output, `C:\AE_SDK\_build_out\AEGP\pieFX.aex`:

- `Win/pieFX.sln`, which compiles the ROOT `pieFX.cpp` — the Phase 0 SPIKE, 1592
  lines, whose Window menu is `pieFX S1 (Anchor to Center)`, `S2A`, `S2B` …
- `poc/native/Win/pieFX.sln`, which compiles `poc/native/pieFX.cpp` — the
  PRODUCT, 3622 lines, whose Window menu is `pieFX (Show/Hide)` and
  `pieFX Settings`.

Whichever was built LAST is the .aex on disk, and the first attempt at this
build was the spike one — which compiles just as cleanly and installs just as
happily, and is only caught by opening AE's Window menu. Build the product one.
The .aex was verified afterwards by reading its strings: `pieFX (Show/Hide)`
present, no `Anchor to Center`, no `pieFX S1`. (`Self-Test` is absent on purpose
— `PIEFX_SHOW_SELFTESTS` is 0.)

What that does NOT cover: the .aex was built, not installed and not loaded into
a running AE, and none of those bodies has been EXERCISED on Windows since the
port. Compiling is the floor, not the proof. The two changes that are also
Windows fixes — `WaitForFrameFile`'s timing guess and the effect-name encoding —
are still unwatched there, and `copy-frame` plus the preset walk are the two
things to run first on the next Windows session.

### Also outstanding

- ~~**`.center()` on Windows.**~~ **DONE**, on the Windows machine and not
  blind. `win_place_on_cursor_screen` is the mirror of the macOS function and
  both windows now use it; `.center()` is gone from both platforms, and both
  build `.visible(false)` so the window is placed before it is seen. Windows is
  the easier half — the virtual desktop is one coordinate space in physical
  pixels, so nothing is divided by a per-monitor scale — but it has a trap macOS
  does not: crossing to a monitor with a different scale factor RESIZES the
  window, which invalidates the centring just computed. The function measures
  the size again after the move and re-centres once. Watched working in the
  harness (the search window placed on the cursor's monitor, log line and all)
  on a SINGLE-monitor machine; the second-display case is exactly as unproven on
  Windows as it was on macOS before the hand-check, and wants the same hand-check.
  The mixed-DPI re-centre is reasoned, not measured.
- **A 15s freeze on a failed copy-frame.** `WaitForFrameFile` runs on AE's UI
  thread, and its budget went from 4s to 15s when the check became exact. That
  is only reached when AE never finishes writing the frame — but when it is
  reached, After Effects is unresponsive for it. Making copy-frame asynchronous
  is the real answer and is more than this was worth today.
- **Distribution, the rest of it.** The bundling question is ANSWERED — the
  overlay ships as `pieFX-overlay.app` inside the plug-in and nothing measurable
  changed; see `MAC_RESULTS.md`. What is left:
  - **Signing and notarization.** NOT a gate — measured. AE ships
    `com.apple.security.cs.disable-library-validation`, so an ad-hoc-signed
    plug-in loads into its hardened runtime, and a QUARANTINED plug-in was
    tested by hand and loaded normally. Signing buys Gatekeeper's approval of
    the downloaded container, nothing more.
    `Mac/sign_product.sh` is written and waiting for a certificate; there are
    none in this keychain, so **it has never been run** — read its header
    before trusting it. Note that `codesign`, `notarytool` and `stapler` are
    macOS-only, so this can only ever be done on a Mac.
  - **An installer.** Today it is `sudo cp -R`. A `.pkg` has to cope with AE
    being open, with several AE versions side by side, and with uninstalling.
  - `macos-private-api` is required for a transparent window and rules out the
    App Store. Settled rather than open, but it belongs here.
## Traps — things already paid for once

- **A local NSEvent monitor cannot be tested from outside its own app.** That is
  the same property that makes it need no Accessibility grant. `gesture_test.mm`
  supplies an app, and its window carries a context menu on purpose: a short
  right-click must still open it, a held one must not. Neither half shows up in
  a log that only records what the monitor saw.
- **`poll()` does not wake when a FIFO's last writer closes.** Measured. The
  Windows disconnect signal (`ReadFile` returning 0) has no equivalent, so
  liveness is asked directly: `O_WRONLY` fails with `ENXIO` when there is no
  reader.
- **`SIGPIPE` kills the process**, and the process is After Effects. Ignored
  process-wide, with the reasoning at the call site.
- **macOS will not render one window across two displays.** The window server
  ACCEPTS a spanning frame and then draws it on one screen — so this fails in
  the direction of looking fine. The overlay moves per summon instead.
- **Tauri's monitor geometry is not one coordinate space** on a mixed-DPI
  desktop: each monitor's position is its points times ITS OWN scale. Divide by
  each monitor's own scale to get points, which do tile.
- **macOS coordinates are POINTS, top-left origin.** `NSEvent` gives points, so
  the plug-in converts nothing; `overlay_origin` returns a third value telling
  the frontend whether to divide by `devicePixelRatio`, and the JS does not
  sniff the platform.
- **`findMenuCommandId` follows the UI language.** No English name resolves on a
  localised AE. **But the ids do not vary by language** — on es_ES every Spanish
  spelling resolved to exactly the id already shipped. Id rot is a per-VERSION
  question, answerable once per AE release by whoever has an English install,
  and not answerable at runtime at all: `AEGP_CommandSuite1` cannot tell you
  what a command is called.
- **`%ld` against a 32-bit `A_long`** reads eight bytes from a four-byte slot on
  arm64. Harmless where `long` is 32 bits, which is why Windows never saw it.
  Cross-compiling is its own kind of test.
- **A localised AE has now exposed two defects that are not about
  localisation.** Neither was a macOS bug; both were latent everywhere. Keep
  testing on it.

## The tools, and how to run them

```bash
# the plug-in (AE must be quit for --install; it asks for sudo)
./Mac/build_product.sh
./Mac/build_product.sh --install

# the overlay (Rust only; Node is NOT needed — frontendDist is static)
cd poc/overlay/src-tauri && cargo build --release

# the two transport harnesses, no AE involved
python3 poc/pipe_test.py                                    # drives the OVERLAY
./poc/native/mac/build_fifo_test.sh                         # drives the PLUG-IN side
poc/overlay/src-tauri/target/release/pieFX_fifo_test

# the step-5 harnesses, no AE involved
./poc/native/mac/build_paths_test.sh                        # paths + the encoding
poc/overlay/src-tauri/target/release/pieFX_paths_test
./poc/native/mac/build_clipboard_test.sh                    # REPLACES your clipboard
poc/overlay/src-tauri/target/release/pieFX_clipboard_test
./poc/native/mac/build_presets_test.sh                     # needs AE INSTALLED, not running
poc/overlay/src-tauri/target/release/pieFX_presets_test

# the wheel's own settings logic, in JavaScriptCore (no Node in this project)
swift poc/overlay/test/armmode_test.swift

# the icon set, rebuilt from the SVG and recoloured to the wheel's accent
./icon/make_icons.sh

# both slices of the overlay, then a zip for someone else's machine
rustup target add x86_64-apple-darwin
(cd poc/overlay/src-tauri && cargo build --release --target x86_64-apple-darwin)
./Mac/build_product.sh && ./Mac/package.sh

# what came back from their machine
./Mac/report.sh

# the gesture, in a host that is not AE
./poc/native/mac/build_gesture_test.sh
poc/overlay/src-tauri/target/release/pieFX_gesture_test

# measurements
swift Mac/overlay_probe.swift     # levels, bounds, z-order vs AE, focus
swift Mac/span_test.swift         # the Spaces question
./Mac/overlay_drive.sh            # drive the overlay by hand: s / c / r / g / q
```

In AE: **File > Scripts > Run Script File…** and pick
`poc/scripts/ag_localeProbe.jsx` to re-check the localisation position on any
install.

Logs: `$TMPDIR/pieFX_poc.txt` (plug-in) and `$TMPDIR/piefx_overlay.log`
(overlay). The overlay is a windowed process with no console, so the second one
is the only way to tell "the page never loaded" from "the page loaded but the
action never fired".

## Repo layout, macOS additions

```
icon/pieFX LOGO.svg         the master artwork; ICON.png is a 600px export
icon/make_icons.sh          the icon set, recoloured to hexdraw.js's accent
icon/render_svg.swift       SVG -> transparent PNG (qlmanage flattens; see MAC_RESULTS)
Mac/build_product.sh        builds + installs the PRODUCT plug-in (not Xcode)
Mac/sign_product.sh         sign + notarize + staple — WRITTEN BUT NEVER RUN
Mac/package.sh              a zip somebody else can install, + the report script
Mac/report.sh               diagnostics from a machine that is not this one
Mac/overlay_probe.swift     window levels, bounds, z-order, focus
Mac/span_test.swift         can one window span two displays (no)
Mac/overlay_drive.sh        drive the overlay by hand, no plug-in
Mac/pieFXMac.xcodeproj      the Phase 0 SPIKE only; not the product
poc/native/mac/
  pieFX_compat.h            the Win32 VOCABULARY, so ~300 lines stay identical
  pieFX_fifo.{h,cpp}        the transport
  pieFX_launch.{h,cpp}      launch, process group, teardown
  pieFX_gesture.{h,mm}      the right-hold gesture
  pieFX_paths.{h,mm}        ~/Library/Application Support/pieFX, Documents, mkdir -p
  pieFX_text.{h,cpp}        AE and filesystem text -> canonical (NFC) UTF-8
  pieFX_clipboard.{h,mm}    a PNG onto NSPasteboard; three formats become one
  fifo_test.cpp             transport harness, plug-in side
  gesture_test.mm           the gesture in a host that is not AE
  paths_test.cpp            the two-sided path agreement, and the encoding
  clipboard_test.mm         the clipboard, alpha included
  presets_test.cpp          the preset walk vs the real AE install; #includes pieFX.cpp
poc/pipe_test.py            transport harness, overlay side
poc/scripts/ag_localeProbe.jsx
```

`Mac/build_product.sh` is deliberately not an Xcode project: a `.plugin` is a
directory with a binary, an Info.plist, a PkgInfo and a Rez'd PiPL, and twenty
lines of shell is easier to read and to review than a `pbxproj`.

## How the platform seam works

`poc/native/pieFX.cpp` is one file for both platforms. Everything that talks to
AEGP is shared and untouched. Five regions are `#ifdef AE_OS_WIN` with a macOS
branch that delegates to the modules above: the transport, the overlay launch,
the gesture, the clipboard, the paths.

**Step 5 made three of those regions smaller, and that is the lesson in it.**
`ReadSettings`, `WriteEffectCatalogue` and `CopyFrameToClipboard` were each
split whole, and each turned out to contain exactly one genuinely
platform-specific LINE — the config path, the encoding, the pasteboard call.
Everything around those lines was stdio, an AEGP walk, and ExtendScript. So the
seam was pushed down to the line, the bodies rejoined, and what would have been
a duplicate JSON parser and a duplicate 90-line catalogue walker never got
written.

The rule that produced that: split at the smallest thing that genuinely
differs, and be suspicious of a `#ifdef` whose two branches would say mostly
the same thing. A stub is a fine placeholder for a function nobody has ported;
it is a bad guess at where the platform boundary actually is.

Everything else that failed to compile failed over **vocabulary** — `BOOL`,
`MAX_PATH`, `sprintf_s`, `ZeroMemory`, `CRITICAL_SECTION` — and that is
translated once in `pieFX_compat.h`. The point is that the ~300 lines which
merely SPEAK Windows stay byte-identical on both platforms. The Windows product
is shipping; a port is not a licence to churn it. The whole partition was 337
added lines against 10 deleted.

`pieFX_compat.h` does not try to emulate Windows. It covers what `pieFX.cpp`
uses and stops. Anything with a real decision in it went to a module instead.
