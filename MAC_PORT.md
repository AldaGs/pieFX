# pieFX — the macOS port

What it would take, written from the Windows product as it stands and from
`MAC_RESULTS.md`, which is the record of the one macOS bench session. Read that
file first: it answers the research questions, and this one is only about the
plumbing that follows.

**The headline, and it cuts both ways.** Phase 0 passed on macOS — including the
half everyone expects to fail — but **no line of the product has ever run
there.** The Mac tree is four Phase 0 spikes in `pieFXMac.mm`, frozen the same
way `pieFX.cpp` at the repo root is frozen on Windows. `poc/native` has no macOS
build at all.

## Already answered, and it is the expensive half

From the bench, on Apple silicon, macOS 26.1, AE 2026:

- **The hold gesture works, swallow included**, through
  `addLocalMonitorForEventsMatchingMask`. 23 holds out of 23 detected.
- **It needs no Accessibility permission**, once the replay re-posts the
  ORIGINAL `NSEvent` (`[NSApp postEvent:atStart:NO]`, DOWN before UP) instead of
  synthesising one with `CGEventPost`. That distinction is the whole difference
  between a product anyone can install and one that demands a TCC grant on
  After Effects itself.
- **`dispatch_after` survives a press.** Cocoa has no equivalent of Windows'
  modal drag loop starving the idle hook.
- **Enumeration works** — 454 walked, 454 claimed.
- **The plug-in loads**, and `Mac/pieFXMac.xcodeproj` plus
  `Mac/build_and_install.sh` already build, verify and install it.

Every one of those is a question that could have ended the port. None of them
did.

## The one real unknown: the Tauri overlay — ANSWERED

**All four properties hold. See the step 1 section of `MAC_RESULTS.md` for the
measurements.** The overlay strategy survives on macOS. Nothing below is wasted
work, and step 1 is done.

It cost one design change and one prerequisite for the plug-in, both of which
this page had no way to anticipate:

- **The window does not span the displays — it MOVES to the one the cursor is
  on.** "Displays have separate Spaces" is on by default and clips a window to
  a single screen. The window server ACCEPTS a spanning frame and then renders
  it on one screen, so this fails silently in the direction of looking fine.
- **macOS coordinates are POINTS, top-left origin**, not physical px. That is
  what `NSEvent` gives the plug-in, and the only space that is coherent across
  a mixed-DPI desktop. The plug-in must send points and convert nothing. This
  is settled BEFORE the transport is written, and the table below assumes it.

Two smaller ones, also in `MAC_RESULTS.md`: transparency needs Tauri's
`macos-private-api` feature (which forecloses the App Store), and the overlay
had to be made an **accessory** app — `"focus": false` governs the window,
while what was stealing the foreground from AE was the application.

The fifth property, the one this page called known-NOT-portable, went exactly as
predicted: `raise()` now calls `NSApp activateIgnoringOtherApps:` alongside the
`#[cfg(windows)]` `AttachThreadInput` dance. An accessory app has to ask, which
is the mirror of Windows making you borrow the foreground thread's input queue.
Confirmed by hand — the settings window takes the first keystroke.

## The Windows-only surface in the plug-in

`poc/native/pieFX.cpp` is ~2,600 lines. Everything that talks to **AEGP** —
every executor, the effects walk, the selection context, the anchor script, both
self-tests — is portable untouched. What is not:

| Windows | macOS replacement | State |
|---|---|---|
| `CreateNamedPipe` / `ConnectNamedPipe` (7 calls) | `mkfifo` pair, or Unix domain sockets | **written and proven** in `poc/native/mac/pieFX_fifo.cpp`, against the real overlay with no AE |
| `SetWindowsHookEx(WH_MOUSE)` | `addLocalMonitorForEventsMatchingMask` | **written and proven** in `pieFXMac.mm` |
| `SetTimer` / `KillTimer` | `dispatch_after` on the main queue | **written and proven** |
| `SendInput` replay | `[NSApp postEvent:atStart:NO]`, DOWN then UP | **written and proven** |
| `CreateProcess` + job object + `--owner-pid` watchdog | `NSTask` / `posix_spawn`, `kqueue` on the parent pid | to write; the `quit` MESSAGE path is already portable |
| Clipboard: `OpenClipboard` + WIC + three formats (~230 lines) | `NSPasteboard` + `NSPasteboardTypePNG` | to write, and it gets SMALLER |
| `GetTempPath`, `%APPDATA%` | `$TMPDIR`, `~/Library/Application Support/pieFX` | to write; the overlay's `dlog` already falls back to `TMPDIR`, the rest of the overlay's `APPDATA` paths do not |
| Screen coordinates in physical px | **points, top-left origin**, straight from `NSEvent` | settled by step 1; the overlay expects points and divides by nothing |

The clipboard is the one place the port makes the code shorter. Three formats
exist on Windows because `CF_DIB` cannot express alpha; `NSPasteboard` takes the
PNG bytes as they are, so the WIC decode, the DIB construction and the
force-opaque fallback all disappear.

### The transport, and why FIFOs are the cheap answer

The overlay opens both channels with a plain `File::open` on a path:

```rust
std::fs::OpenOptions::new().read(true).open(&events_name)
std::fs::OpenOptions::new().write(true).open(&actions_name)
```

A **`mkfifo` pair keeps that code unchanged** — same calls, different path — so
the whole transport job lands on the plug-in side. Two things come with FIFO
semantics and neither is optional:

- `open()` blocks until the other end appears. That is fine, and it is already
  on its own thread, but it is exactly the shape of the freeze that bug #1 in
  `HANDOFF.md` was: **never on the UI thread.**
- A write with no reader raises **`SIGPIPE`**, which kills the process by
  default. It has to be ignored explicitly. On Windows the same condition is
  just an error return, so nothing in the current code anticipates it.

Unix domain sockets give real connect/disconnect events and no `SIGPIPE`, at the
cost of a genuine branch in `pipe_client`. Start with FIFOs; the socket is the
fallback if the handshake turns out to need more than the app-level `ready`
message already provides.

## Two sharp edges the bench flagged that matter MORE now

Both were noted as harmless for a spike. Neither is harmless for the product.

1. **Localised AE returns legacy text, not UTF-8.**
   `AEGP_GetEffectName` and `AEGP_GetEffectCategory` returned single-byte
   extended ASCII on the bench's Spanish AE. Those strings now go straight into
   `effects.json` and onto the search window, where they will be mojibake or
   rejected outright — `JSON.parse` on invalid UTF-8 is not a graceful failure.
   **The Unicode accessors are a prerequisite for the search on a localised
   Mac**, not a polish item.
2. **Menu command names are localised too.** The entire `ae-command` table was
   resolved against an English AE, and `findMenuCommandId("Add to Render
   Queue")` returns 0 on a Spanish one. The id fallback would carry the whole
   wheel — which is the exact situation the name-first design exists to avoid,
   because an id is the thing that silently rots into some other command. This
   is its own investigation, and it is probably bigger than it looks.

## The thing that would otherwise get skipped

`poc/pipe_test.ps1` is PowerShell driving .NET named pipes. **There is now a Mac
equivalent — `poc/pipe_test.py` — and it was written first, not last.** It
passes end to end against the overlay with no plug-in and no AE: same
assertions, same strokes, same order, so a divergence between the platforms
shows up as a divergence in the test.

Every transport bug this project has had — the freeze, the startup race, the
swapped `--tx`/`--rx` flags — was caught by that harness rather than by After
Effects. A macOS port introduces a brand-new transport, which is precisely the
category of bug it catches. Writing the FIFO harness before the FIFO transport
is the difference between finding those in a shell and finding them in a
session with AE open.

It has already earned it once, on something other than transport: the same
stroke selected anchor cell 4 on the Retina and cell 0 on the 1x display, which
is what the mixed-DPI coordinate bug looked like from the outside.

## Suggested order

1. ~~**The Tauri overlay on macOS**~~ — **DONE.** All four properties measured
   and holding; see `MAC_RESULTS.md`. The design changed once: it moves between
   screens instead of spanning them.
2. ~~**The offline harness**, driving FIFOs~~ — **DONE**, `poc/pipe_test.py`,
   written before the transport and passing.
3. **Transport** in the plug-in — **DONE** and proven offline
   (`poc/native/mac/fifo_test.cpp`) — then **launch and lifetime**. ← next.
   Send POINTS, top-left origin, from `NSEvent`; the overlay converts nothing.
   Lifetime has one finding waiting for it already: a child of the overlay
   appears to outlive it holding the events FIFO open, which is what the
   Windows job object exists to prevent. See `MAC_RESULTS.md`.
4. **The gesture**, moved out of the frozen spike into the product plug-in. This
   is proven code changing address, not new work.
5. **Paths, clipboard, Unicode accessors.**
6. **Localisation of menu ids** — its own investigation.

Steps 3 to 5 are mechanical. Step 1 DID change the design, in the one way it
was most likely to. Step 6 is still the one most likely to be bigger than it
looks.

## What this page does NOT cover

Distribution. A plug-in that launches a helper `.app` runs into code signing,
notarization and quarantine, and none of that has been looked at. It does not
block development — a locally built, locally signed binary runs fine on the
machine that built it — but it is not nothing, and it is not in the estimate
above.

Step 1 added one fact to it: the overlay cannot be transparent on macOS without
Tauri's `macos-private-api` feature, which rules out the App Store. That is no
loss for a helper binary shipped beside a plug-in, but it is a door now closed,
and better known here than discovered at the end.

It also added a question that is not answered: the overlay currently runs as a
bare executable, not a bundled `.app`. That is what made it a regular,
activatable application in the first place, and the accessory activation policy
is set in code rather than by an `LSUIElement` key in a bundle. Whether the
shipped form is a bundle — and whether bundling changes any of the four
properties measured here — has not been tested.
