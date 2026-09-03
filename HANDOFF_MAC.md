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
| **launch and teardown from inside the bundle** | `fifo_test` run beside the `.app`: it picks the `.app`, connects, and the watchdog still fires |

## What is NOT proven

- **APPLYING a preset from the wheel.** The catalogue is confirmed in AE — the
  list matches — but the `.ffx` path crossing to ExtendScript and coming back
  as an applied preset has not been watched on macOS. Reading the list and
  firing one are different code paths and only the first is settled.
- **The settings round trip.** That `ReadSettings` honours a `holdMs` written
  by the settings window has never been watched. It is the last thing in step 5
  with no observation behind it at all.
- **Windows, at all, since this port began.** See "What to do next".
- **Click-through, since the overlay was bundled.** It is the one of the four
  window properties `CGWindowList` cannot see — `overlay_probe.swift` has
  always said so — and it is set by the same call in both layouts, so there is
  no reason to expect a change. No reason to expect is not a measurement. One
  right-hold over an AE panel answers it.
- **The wheel summoned on a second display from the real gesture.** The window
  move is measured and the driver script summons there correctly, but nobody has
  right-held on the second screen in AE. This is a five-minute check and it is
  the last coordinate question outstanding.
- **`ag_masterNull.jsx` from the wheel.** It ships as of the latest build but has
  not been fired from a slot on macOS.
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

### 2. A Windows build

The largest un-repaid debt in the project, and it has been growing all through
step 5. **None of the Windows code changed in this port has been compiled**,
because there is no Windows toolchain on this machine. What changed there:
`PieFX_ConfigPath`, `PieFX_LegacyToUtf8`, `FrameFileSize`, `PngIsComplete` and
the `WaitForFrameFile` rewrite, `EmitPreset`, and the
`DirOpen`/`DirNext`/`DirClose` iterator that `WalkPresetFolder` and
`WritePresets` were rebuilt on.

Every one of those was a deliberate choice to share a body rather than
duplicate it, and each is defensible on its own. The accumulated risk is not:
the Windows product is SHIPPING, and it currently ships on code that has only
ever been compiled for macOS. A build — even one that only compiles and loads —
should come before the next Windows release.

Two of those changes are also FIXES for Windows, which is the other half of the
argument: `WaitForFrameFile`'s timing guess and the effect-name encoding are
both latent there.

### Also outstanding

- **`.center()` on Windows.** The settings and search windows still call it
  there, and it centers on the primary monitor on Windows too — so the bug the
  hand-check found on macOS is latent on the shipping platform. It was left
  alone rather than fixed blind: there is no Windows toolchain on this machine,
  and per-monitor DPI makes the Windows version of this arithmetic its own
  question. It is the fourth defect in this port that turned out not to be
  about macOS.
- **A 15s freeze on a failed copy-frame.** `WaitForFrameFile` runs on AE's UI
  thread, and its budget went from 4s to 15s when the check became exact. That
  is only reached when AE never finishes writing the frame — but when it is
  reached, After Effects is unresponsive for it. Making copy-frame asynchronous
  is the real answer and is more than this was worth today.
- **Distribution, the rest of it.** The bundling question is ANSWERED — the
  overlay ships as `pieFX-overlay.app` inside the plug-in and nothing measurable
  changed; see `MAC_RESULTS.md`. What is left:
  - **Signing and notarization.** Two nested binaries, and the inner one must be
    signed before the outer, with the outer signature staying valid over it.
    Nothing here is signed at all yet.
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
