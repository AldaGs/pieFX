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

## The one real unknown: the Tauri overlay

**S3 proved a hand-built `NSWindow` can sit above AE at
`NSStatusWindowLevel`. It did not prove Tauri can.** The entire product UI lives
in that process — the wheel, the settings window, the search window — and it has
never been launched on a Mac.

Four separate properties have to hold, and only the first is obviously portable:

1. a transparent, borderless window,
2. **click-through** (`set_ignore_cursor_events`),
3. spanning **all displays**, positioned in one virtual-desktop coordinate space,
4. **above After Effects**, without stealing focus from it.

And a fifth that is known NOT to be portable: `raise()` in `lib.rs` is the
`AttachThreadInput` dance, `#[cfg(windows)]` and Windows-only by nature. macOS
has the mirror-image problem with a different answer —
`NSApp activateIgnoringOtherApps:` — and its own rules about App Nap and Spaces
on top. The settings and search windows both depend on it.

**Measure this before writing anything else.** Build the existing Tauri app on
the Mac, run it with no plug-in at all, and see whether a click-through window
sits over AE across two displays. If it does not, the overlay strategy needs
rethinking on that platform and every other task on this page is wasted work.

## The Windows-only surface in the plug-in

`poc/native/pieFX.cpp` is ~2,600 lines. Everything that talks to **AEGP** —
every executor, the effects walk, the selection context, the anchor script, both
self-tests — is portable untouched. What is not:

| Windows | macOS replacement | State |
|---|---|---|
| `CreateNamedPipe` / `ConnectNamedPipe` (7 calls) | `mkfifo` pair, or Unix domain sockets | to write |
| `SetWindowsHookEx(WH_MOUSE)` | `addLocalMonitorForEventsMatchingMask` | **written and proven** in `pieFXMac.mm` |
| `SetTimer` / `KillTimer` | `dispatch_after` on the main queue | **written and proven** |
| `SendInput` replay | `[NSApp postEvent:atStart:NO]`, DOWN then UP | **written and proven** |
| `CreateProcess` + job object + `--owner-pid` watchdog | `NSTask` / `posix_spawn`, `kqueue` on the parent pid | to write; the `quit` MESSAGE path is already portable |
| Clipboard: `OpenClipboard` + WIC + three formats (~230 lines) | `NSPasteboard` + `NSPasteboardTypePNG` | to write, and it gets SMALLER |
| `GetTempPath`, `%APPDATA%` | `$TMPDIR`, `~/Library/Application Support/pieFX` | to write, both sides |

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

`poc/pipe_test.ps1` is PowerShell driving .NET named pipes. **There is no Mac
equivalent, and the Mac needs one first, not last.**

Every transport bug this project has had — the freeze, the startup race, the
swapped `--tx`/`--rx` flags — was caught by that harness rather than by After
Effects. A macOS port introduces a brand-new transport, which is precisely the
category of bug it catches. Writing the FIFO harness before the FIFO transport
is the difference between finding those in a shell and finding them in a
session with AE open.

## Suggested order

1. **The Tauri overlay on macOS**: click-through, all displays, above AE,
   without taking focus. A measurement, not a build. Everything else is
   conditional on it.
2. **The offline harness**, driving FIFOs, before there is anything to drive.
3. **Transport** in the plug-in, then **launch and lifetime**.
4. **The gesture**, moved out of the frozen spike into the product plug-in. This
   is proven code changing address, not new work.
5. **Paths, clipboard, Unicode accessors.**
6. **Localisation of menu ids** — its own investigation.

Steps 3 to 5 are mechanical. Step 1 can still change the design. Step 6 is the
one most likely to be bigger than it looks.

## What this page does NOT cover

Distribution. A plug-in that launches a helper `.app` runs into code signing,
notarization and quarantine, and none of that has been looked at. It does not
block development — a locally built, locally signed binary runs fine on the
machine that built it — but it is not nothing, and it is not in the estimate
above.
