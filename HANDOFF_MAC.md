# pieFX — the macOS handoff

If you are picking up the macOS port cold, read this file, then `MAC_PORT.md`
(the plan, with each step's outcome folded back into it) and `MAC_RESULTS.md`
(the measurements, in the order they were made). `HANDOFF.md` is the Windows
product's handoff and is still the authority on everything not platform
specific. Everything below is true as of 2026-09-03.

**Start here: the `%APPDATA%` decision.** It is the first section under "What
to do next", it blocks three of the four remaining jobs, and it is the one
piece of work that cannot be done correctly on one side of the wire alone.

## One-paragraph state

The port is through the risky half. **The product plug-in builds, loads in
After Effects on macOS, and the wheel works from the real gesture**: right-press
and hold, the hexagon appears under the cursor, and releasing on a slot fires an
AE menu command for real — watched working on the author's machine, which runs a
Spanish AE. Everything Phase 0 could not answer has been answered by
measurement: the overlay strategy survives, the transport is proven outside AE
by its own harness, launch and lifetime are proven, and the gesture is the
Phase 0 spike moved into a module. What is NOT yet true is the second half of
the feature set — the effect search, saved settings, presets and copy-frame are
all stubbed, all of them loudly, and all of them behind one decision about where
files live.

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

Four functions log `not implemented on macOS yet (MAC_PORT step 5)` rather than
failing quietly. They are all in `poc/native/pieFX.cpp`, each behind
`#else // AE_OS_WIN` with the reasoning at the site.

| stub | user-visible effect | blocked on |
|---|---|---|
| `ReadSettings` | settings never load; built-in defaults are used | the path decision |
| `WriteEffectCatalogue` | **the Effects search has no catalogue** | the path decision + encoding |
| `WritePresets` | no animation presets in the search | Documents equivalent |
| `CopyFrameToClipboard` | reports to the user and does nothing | nothing — it is ready to write |

## What to do next

### 1. The `%APPDATA%` decision — do this first

Three files live in `%APPDATA%\pieFX\` on Windows, and **both processes touch
them**, which is why this cannot be decided in the plug-in alone:

| file | written by | read by |
|---|---|---|
| `settings.json` | the overlay's settings window | the plug-in (`ReadSettings`) and the overlay |
| `effects.json` | the plug-in (`WriteEffectCatalogue`) | the overlay's search window |
| `recents.json` | the overlay's search window | the overlay |

The documented macOS answer is `~/Library/Application Support/pieFX/`. What has
to be settled in one go:

- **The location**, on both sides. `poc/native/pieFX.cpp` (two `%APPDATA%`
  sites) and `poc/overlay/src-tauri/src/lib.rs` (`settings_path`,
  `recents_path`, `effects_path` — all three currently return `None` on macOS,
  so settings, recents and the catalogue are disabled there).
- **The relative paths.** `PIEFX_SETTINGS_REL` and `PIEFX_EFFECTS_REL` in
  `poc/native/pieFX.h` are backslash-shaped (`"pieFX\\settings.json"`). They
  need a separator that is right on both platforms.
- **Directory creation.** The Windows side calls `CreateDirectoryA` because the
  settings window may never have run. The macOS side needs the same, and
  `~/Library/Application Support` may not have a `pieFX` in it.

`--settings <path>` and `--effects <path>` already override the defaults on the
overlay, and `--settings none` disables both plus recents. Every harness in the
project runs that way, so **the harnesses will not catch a wrong default** —
this needs checking by hand once, in AE.

### 2. The effect catalogue, and the encoding

`WriteEffectCatalogue` comes back once the path exists. It also needs the
conversion below, because those names go straight into `effects.json` and
`JSON.parse` on invalid UTF-8 is not a graceful failure.

**There are no Unicode accessors.** `AEGP_EffectSuite5` is the newest and both
`AEGP_GetEffectName` and `AEGP_GetEffectCategory` still take `A_char *`. Two
earlier documents claimed otherwise and have been corrected; do not go looking.

So the conversion is ours, at the one point the strings are written out:

- macOS: `CFStringCreateWithCString` with `CFStringGetSystemEncoding()`, then
  read back as UTF-8.
- Windows: `MultiByteToWideChar(CP_ACP)` then `WideCharToMultiByte(CP_UTF8)`.
- On failure, fall back to `AEGP_GetEffectMatchName`, which the SDK header marks
  `UTF8!!` and which is a stable, non-localised identifier.

**This is not a macOS job.** The same bug is latent on Windows under any
non-Latin locale; the Spanish Mac merely got there first.

### 3. The clipboard

Self-contained, blocked on nothing, and the one place the port makes the code
*smaller*. Three formats exist on Windows because `CF_DIB` cannot express alpha;
`NSPasteboard` takes the PNG bytes as they are, so the WIC decode, the DIB
construction and the force-opaque fallback all disappear. ~230 lines becoming
perhaps 40.

The frame still arrives as a PNG on disk from ExtendScript — that half is
already portable and already works.

### 4. Presets

Needs the macOS equivalent of a localised, possibly redirected Documents folder,
plus the shipped-presets root inside the AE install. The fiddliest of the four
for the least payoff, so it goes last.

### Also outstanding

- **The second-display gesture check** (above).
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
  fifo_test.cpp             transport harness, plug-in side
  gesture_test.mm           the gesture in a host that is not AE
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

Everything else that failed to compile failed over **vocabulary** — `BOOL`,
`MAX_PATH`, `sprintf_s`, `ZeroMemory`, `CRITICAL_SECTION` — and that is
translated once in `pieFX_compat.h`. The point is that the ~300 lines which
merely SPEAK Windows stay byte-identical on both platforms. The Windows product
is shipping; a port is not a licence to churn it. The whole partition was 337
added lines against 10 deleted.

`pieFX_compat.h` does not try to emulate Windows. It covers what `pieFX.cpp`
uses and stops. Anything with a real decision in it went to a module instead.
