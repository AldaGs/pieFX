# pieFX POC — the Anchor wheel (Windows)

First real product code. Wires three Phase-0 spikes into one live loop:
right-hold (S2D) → 3×3 anchor wheel under the cursor (S3, now Tauri) → flick +
release → anchor snaps to the chosen cell, layer does not jump (S1).

Scope is deliberately narrow: **one segment (Anchor), Windows only, hardcoded
layout, no settings, no persistence.** macOS and everything else come at MVP.

## Two processes

    poc/
      native/    the AEGP plug-in — owns input, AE access, the pipe SERVER
        pieFX.cpp / .h / _PiPL.r
        Win/     VS project (paths are SDK-relative, two levels deeper than the
                 frozen spike's Win/, so ..\..\..\ became ..\..\..\..\..\)
      overlay/   the Tauri v2 app — the wheel UI, the pipe CLIENT
        src/          vanilla canvas renderer (main.js)
        src-tauri/    Rust: transparent click-through window + pipe client

The native side owns **all** input and is the single source of truth for which
cell is highlighted/fired. The overlay is a pure renderer, made click-through
(`set_ignore_cursor_events`), so it never fights for the mouse while the right
button is held — the one S2+S3 combination Phase 0 never exercised.

IPC: named pipe `\\.\pipe\pieFX`, newline-delimited JSON, native→overlay only:
`summon` / `cursor` (with the authoritative `cell`) / `release` / `cancel`.

**Geometry contract:** `PIEFX_CELL` / `PIEFX_GAP` in `native/pieFX.h` MUST match
`CELL` / `GAP` in `overlay/src/main.js`, or the cell the plug-in fires won't be
the one the overlay highlights.

## Build

**Native** (VS 17/18, AE SDK):

    $env:AE_PLUGIN_BUILD_DIR="C:\AE_SDK\_build_out"
    MSBuild poc\native\Win\pieFX.sln -p:Configuration=Debug -p:Platform=x64

Output: `%AE_PLUGIN_BUILD_DIR%\AEGP\pieFX.aex`. Verify the export is un-mangled:

    dumpbin /EXPORTS pieFX.aex        # must show a bare  EntryPointFunc

Install: close AE, copy `pieFX.aex` into AE's **own**
`Support Files\Plug-ins\` (not MediaCore).

**Overlay** (Rust + Node, WebView2):

    cd poc\overlay
    npm install
    npm run tauri dev                 # dev: runs the overlay, retries the pipe

    # or a self-contained release exe, for the plug-in to auto-launch:
    cd src-tauri && cargo build --release
    #   -> target\release\pieFX-overlay.exe

**Auto-launch.** When armed, the plug-in looks for `pieFX-overlay.exe` **beside
the .aex** and launches it (best-effort). So copy the release exe into the same
folder the `.aex` lives in (e.g. `...\Support Files\Plug-ins\AGS\`). During dev
you can skip that and just run `npm run tauri dev` — the plug-in's launch simply
no-ops when the exe is not found, and the running dev overlay connects anyway.

**Bundled scripts.** `poc/scripts/` travels with the product: copy it beside the
`.aex` too, so the folder holds `pieFX.aex`, `pieFX-overlay.exe` and
a `scripts` folder. A binding whose `needs.file` is relative
(`scripts/ag_masterNull.jsx`) is resolved against that directory, which is how a
snippet can depend on a script without depending on where one machine keeps it.

The overlay window spans the **whole virtual desktop** (all monitors) and is
positioned at the virtual-desktop origin; the frontend converts the plug-in's
physical screen coords to window-local using that origin (and divides by
`devicePixelRatio`). So the wheel draws on whichever monitor the cursor is on.

## Run / verify (end-to-end, by eye — the only honest check)

1. Build + install the `.aex` (AE closed). Start AE, open a comp, select one
   shape/solid layer.
2. Start the overlay: `npm run tauri dev` in `poc/overlay`. A fullscreen
   transparent click-through window sits over everything; nothing is visible yet.
3. In AE: **Window ▸ pieFX POC (Anchor Wheel: OFF/ON)** to arm. This creates the
   pipe and installs the mouse hook. (The overlay connects within ~0.5 s; the
   native log records "overlay connected".)
4. **Right-press and hold** past 200 ms → the 3×3 wheel appears under the cursor.
   **Flick to a corner cell** and **release** → the layer's anchor snaps to that
   corner of its source rect and **the layer does not jump** (position is
   compensated). One **Undo** reverts it.
5. **Guardrails:** a quick **right-click** (no hold) still opens AE's normal
   context menu. With **nothing selected**, the wheel greys out and fires nothing.

Native log: `%TEMP%\pieFX_poc.txt` (summon / cursor / fire / pipe events).

## Status of the milestones — ALL VERIFIED LIVE (2026-09-01)

| # | Milestone | State |
|---|---|---|
| 1 | Overlay standalone (transparent grid) | **PASS** — on top across BOTH monitors, behaved |
| 2 | Pipe handshake | **PASS** — "overlay connected" |
| 3 | Summon on right-hold | **PASS** — carries real selection context |
| 4 | Cursor → highlight | **PASS** |
| 5 | Release → anchor fires | **PASS** — all 9 cells, correct fractions, `moved 1 layer(s)` |
| 6 | Guardrails (grey / right-click / undo) | **PASS** — nothing selected → no fire |

The full loop works end to end and the S2-hook + S3-overlay intersection Phase 0
never exercised is now proven on real AE, across two monitors. Log of the first
run: `%TEMP%\pieFX_poc.txt`.

## Edges from the first live run

- **Multi-monitor draw (FIXED).** The overlay originally spanned only the primary
  monitor, so a summon on a second display often fell outside the canvas and did
  not draw. The window now spans the whole virtual desktop and the frontend
  translates by the virtual-desktop origin — see the note under Build.
- **Press released over a non-AE window (FIXED).** The thread-local `WH_MOUSE`
  hook only sees AE's thread, so an UP that lands outside AE was never observed:
  `S_rdownB` stayed set and the wheel was left showing. `IdleHook` now backstops
  it — once the press has ended, if we still think the button is down but
  `GetAsyncKeyState(VK_RBUTTON)` says it is up, it cancels and hides the wheel.

- **Stale selection between summon and the deferred fire (OPEN — MVP).** The wheel
  summons on cached selection context, but the anchor move runs later in IdleHook.
  If the layer is deselected in that gap, the wheel showed *enabled* yet the script
  no-ops (`no layer selected`). The ExtendScript's own guard catches it, so no
  harm — but for MVP the fire path should re-check selection, or the summon should
  hold a layer reference rather than a boolean.
