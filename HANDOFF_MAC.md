# pieFX — the macOS handoff

If you are picking up the macOS port cold, read this file, then `MAC_PORT.md`
(the plan, with each step's outcome folded back into it) and `MAC_RESULTS.md`
(the measurements, in the order they were made). `HANDOFF.md` is the Windows
product's handoff and is still the authority on everything not platform
specific. Everything below is true as of 2026-09-03.

**Start here: the hand-check in AE.** The `%APPDATA%` decision has been taken
and the three jobs it blocked are written, but every one of them was proven by
harness rather than in After Effects. Two five-minute checks with AE open —
the settings/catalogue round trip, and the second-display gesture — are what
stands between "builds and passes" and "works", and they are the first section
under "What to do next".

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
settings and copy-frame are written on the back of it. **They have not been
run in AE.** Presets remain stubbed, deliberately and last.

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

## What is NOT proven

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

**One** function is still stubbed. It logs
`not implemented on macOS yet (MAC_PORT step 5)` rather than failing quietly.

| stub | user-visible effect | blocked on |
|---|---|---|
| `WritePresets` | no animation presets in the search | the macOS Documents equivalent, and the shipped-presets root inside the AE install |

Returning 0 is honest here in a way the clipboard stub could not be: the
catalogue is a LIST, so an empty one degrades to "no presets found" rather than
to a silent wrong answer. The effects half of the same file is populated.

The other three — `ReadSettings`, `WriteEffectCatalogue` and
`CopyFrameToClipboard` — are written. Note that they are no longer behind
`#else // AE_OS_WIN` at all: each turned out to have exactly ONE line that was
genuinely platform-specific, so the seam moved down to that line
(`PieFX_ConfigPath`, `PieFX_LegacyToUtf8`, `PngFileToClipboard`) and the bodies
are now shared between the two platforms. Do not go looking for a macOS copy of
them.

## What to do next

### 1. The hand-check in AE — do this first

Three things are written, harnessed and never run inside After Effects. None
of them takes long, and each one is a place where a passing harness can still
be wrong about the real thing.

```bash
./Mac/build_product.sh --install     # AE must be quit; asks for sudo
```

Then, with AE open:

- **Settings round trip.** Open `pieFX Settings` from the Window menu, change
  the hold to something obviously different (say 600ms), save, restart AE, and
  check `$TMPDIR/pieFX_poc.txt` for
  `settings: /Users/…/Library/Application Support/pieFX/settings.json -> armOnLaunch=… holdMs=600`.
  Then feel it: the wheel should need a noticeably longer press.
- **The catalogue.** Arm pieFX and look for `effects: wrote N entries (AE
  claims N)` in the same log — **with no `*** MISMATCH ***`** — then open the
  effect search and confirm the list is populated and the accented Spanish
  names are correct rather than mojibake. This is the one place the encoding
  work can still be wrong: the harness proved the conversion, not that AE's
  bytes are the ones the conversion expects.
- **Copy-frame.** Fire it from the wheel, then paste into something. Preview
  and Photoshop are the two worth trying; a transparent comp is the
  interesting case, because it is the one Windows cannot do properly.

**The second-display gesture check** belongs here too: right-hold on the
second screen and confirm the wheel appears under the cursor. It is the last
coordinate question outstanding.

### 2. Presets

The remaining stub, and the plan always had it last: the fiddliest of the four
for the least payoff. It needs the macOS equivalent of a localised, possibly
redirected Documents folder, plus the shipped-presets root inside the AE
install — which on macOS is inside the `.app`, not beside it.

The path half is now much cheaper than it was: `PieFX_ConfigBase` and
`PieFX_ConfigPath` exist in `poc/native/mac/pieFX_paths.cpp` and the Documents
lookup can sit beside them. The Windows side uses `SHGetFolderPath` rather
than `%USERPROFILE%\Documents` for a reason recorded at the call site — the
author's folder is redirected to OneDrive AND localised — and the macOS
equivalent (`NSSearchPathForDirectoriesInDomains` with
`NSDocumentDirectory`) is the same kind of answer for the same kind of reason.

`WriteEffectCatalogue` already calls `WritePresets` and already writes the
`presets` array around whatever it returns, so this is a self-contained
function with a defined contract and a working harness path around it.

### Also outstanding

- **Distribution.** Untouched, and it has grown two facts: `macos-private-api`
  is required for a transparent window and rules out the App Store, and the
  overlay currently runs as a **bare executable, not a bundled `.app`**, with
  its accessory activation policy set in code rather than by `LSUIElement`.
  Whether the shipped form is a bundle, and whether bundling disturbs any of the
  four window properties measured in step 1, is untested.
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
  pieFX_paths.{h,cpp}       ~/Library/Application Support/pieFX, and mkdir -p
  pieFX_text.{h,cpp}        legacy AE text -> UTF-8 (there are no Unicode accessors)
  pieFX_clipboard.{h,mm}    a PNG onto NSPasteboard; three formats become one
  fifo_test.cpp             transport harness, plug-in side
  gesture_test.mm           the gesture in a host that is not AE
  paths_test.cpp            the two-sided path agreement, and the encoding
  clipboard_test.mm         the clipboard, alpha included
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
