# pieFX — architecture (LOCKED 2026-09-01)

Locked at the end of Phase 0, before any POC code. Every spike that this
architecture depended on has passed, so these are decisions, not proposals.
Change them only with a reason written down here.

## The shape: two processes

**1. The native plug-in — the AEGP (C++ / Objective-C++).**
Registers the summon. Owns input capture, the effects catalogue, and all talking
to After Effects. Runs *on AE's UI thread* — a subclass/hook and a local event
monitor are both in-process — which is why there is no thread-marshalling
problem and no OS permission prompt. Target: small. It holds no UI.

Responsibilities:
- Detect the hold gesture and take it (Windows `WH_MOUSE` hook; macOS
  `addLocalMonitorForEventsMatchingMask:`). Proven in S2/S4.
- Read AE state the wheel needs: the active comp and its **selection**
  (`AEGP_GetActiveItem` + the comp selection collection). The design gates on
  selection state, never on panel identity — S2A proved nothing can name a panel.
- Enumerate the installed-effects catalogue and apply by match name
  (`AEGP_GetNextInstalledEffect` … `AEGP_ApplyEffect`). Proven in S5.
- Run tool actions. Hand-written ExtendScript for anything the script API does
  better (S1's anchor move); direct AEGP suites otherwise.
- Launch the overlay, tell it where the cursor is and what context applies, and
  receive back the chosen action.

**2. The overlay + settings — web tech in a Tauri shell (Rust + native webview).**
All the UI: the wheel, the search bar, the settings screen. Transparent,
undecorated, always-on-top windows are first-class config in Tauri rather than
hand-rolled twice in Direct2D and Cocoa. S3 proved a separate-process topmost
overlay wins AE's z-order **and** does not cost the layer selection (selection is
document state, not focus state), so this split is viable exactly as drawn.

## Why this split (unchanged from the roadmap, now evidence-backed)

- **The UI problem is an interaction-design problem, not a C++ one.** The wheel
  belongs in React, not Direct2D.
- **Process isolation: the UI cannot crash AE.** S3B ran the overlay out of
  process against real AE with no ill effect.
- **We never touch CEP or UXP.** CEP 12 is the last major CEP; UXP in AE lags.
  A settings panel in our own window never faces that rewrite. S2A's probe also
  showed CEP panels are a Chrome hierarchy we must stand clear of anyway.
- **One codebase for wheel and settings** — one design system, one state model.

## IPC: local socket / named pipe

Sub-millisecond against a ~600 ms interaction budget; not a bottleneck. The
native side is the server (it exists first and owns the AE connection); the
overlay connects on launch. Payloads are small JSON messages:

- native → overlay: summon at `(x, y)`, plus context (has-selection, layer
  count, comp present) so the wheel can grey out inapplicable tools.
- overlay → native: the chosen segment/action, or cancel.

No shared memory, no embedding a webview in-process. If IPC latency ever shows,
revisit — but S-nothing suggested it will.

## Deliberately deferred (still deferred)

- **ExtendScript generation.** Hand-write ES3 for now — no `let`, no arrows, no
  `JSON` without a polyfill; debugging generated ES3 is worse than writing it.
- **Which tools live on the wheel.** An MVP question, not an architecture one.
- **Settings persistence format.** Roadmap says human-readable JSON on disk;
  confirm at MVP, not now.

## What the spikes changed versus the roadmap's original sketch

- The roadmap planned to **resolve the comp-viewer window and subclass it**.
  Dead — nothing can identify an AE panel. Replaced by an in-process input hook
  that needs no window resolution, and by gating tools on selection state.
- The gesture is **taken by swallowing the button-DOWN and replaying short
  clicks**, because AE opens its context menu on DOWN. (Windows: `SendInput`.
  macOS: re-post the original `NSEvent` — never `CGEventPost`, which demands
  Accessibility granted to *After Effects*.)
- Multi-monitor and focus worries are **retired**: one AE window tree on one UI
  thread, and selection survives any focus round-trip.

## Consequences to honour in POC code

- The native plug-in must never block AE's UI thread waiting on the overlay.
  Fire the summon, keep processing, act on the reply when it lands.
- Tool actions run on the UI thread (where the hook already is), so no marshalling
  — but keep them short, or AE stutters.
- Selection is read at summon time and is the only context a tool may assume.
- Nothing keys on window class, panel identity, or `AEGP_WindowType`.

## The idea from the user worth carrying into UX (not architecture)

FX Console likely keeps its `.jsx` because it switches behaviour on context: a
layer selected → offer effect search; nothing selected → offer other actions.
pieFX already has the better primitive for that — it reads real selection state
natively at summon time — so the *wheel's contents* can be context-dependent
without a script round-trip. This is a wheel-design note; it does not change the
two-process split.
