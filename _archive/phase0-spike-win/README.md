# Phase 0 — the Windows spike plug-in, archived

This is the plug-in that answered "can this be built at all?" on Windows. It is
**not the product** and must never be mistaken for it again. The product is
`poc/native/pieFX.cpp` and `poc/native/Win/pieFX.sln`.

Kept, not deleted, and deliberately so: it is a good tool for recognising the
floor. It is the smallest thing that loads into After Effects as an AEGP, hooks
the mouse, swallows a right-hold, throws a layered window over AE and walks the
effect catalogue — four capabilities, one file, no pipes, no overlay process, no
settings. When something in the product stops working and the question becomes
"is this us, or is this AE?", this is where you ask it. `SPIKES.md` is the log
it produced.

## What is in here

| | |
|---|---|
| `pieFX_spike.cpp` / `.h` | S1, S2 (hook + swallow), S3, S5. 1592 lines, self-contained. |
| `pieFX_spike_PiPL.r` | its PiPL |
| `S3B_Overlay.cpp` | throwaway .exe — the S3 overlay from a SEPARATE process, which is the case that mattered |
| `Win/pieFX_spike.sln` | the Visual Studio project |

Its Window menu is the way to identify it on sight: `pieFX S1 (Anchor to
Center)`, `pieFX S2A (Probe Window Under Cursor)`, `S2B`, `S2C`, `S3A`, `S3B`,
`S5A`, `S5B`. The product's is `pieFX (Show/Hide)` and `pieFX Settings`.

## Why the names all changed

Everything here used to be called `pieFX` and it emitted **`pieFX.aex` into the
same output folder as the product** — `$(AE_PLUGIN_BUILD_DIR)\AEGP\`. Whichever
project built last won. The spike compiles just as cleanly as the product and
installs just as happily, so nothing about the build says which one you got; the
only symptom is AE's Window menu, and that is how it was eventually caught,
after a session spent believing a spike .aex was the product.

So the target is now `pieFX_spike.aex`. The two cannot collide.

## Building it

It still builds, and that was checked when it was archived (Release, x64, clean).

    _archive/phase0-spike-win/Win/pieFX_spike.sln   Release|x64  ->  pieFX_spike.aex

The paths inside the project are SDK-relative and were re-based for this folder's
depth. If you move this directory again, they all move with it.
