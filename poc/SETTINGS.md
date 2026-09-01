# pieFX — settings and the action model

Written before the settings UI exists, because the UI is the easy half. The hard
half is deciding **what a hexagon can be bound to**, and that decision reaches
into the native plug-in, the pipe protocol, and the on-disk format all at once.

## The one abstraction

A hexagon is a **slot**. A slot has a label and an **action**. Everything the
product can ever do is one of six action kinds:

| Kind | Payload | Executed by |
|---|---|---|
| `builtin` | `name` (`anchor-grid`, `effect-search`), plus per-tool args | pieFX's own native code and custom widget |
| `ae-command` | `id` (numeric), `name` (display only) | `AEGP_DoCommand(id)` |
| `script-snippet` | ExtendScript source | `AEGP_ExecuteScript` |
| `script-file` | path to a `.jsx` | `AEGP_ExecuteScript` wrapping `$.evalFile` |
| `effect` | `matchName` | S5's catalogue lookup + `AEGP_ApplyEffect` |
| `ring` | six child slots | nothing — it is a container |

Every kind was already proven in Phase 0 except `ae-command`, and
`AEGP_DoCommand(AEGP_Command)` exists in `AE_GeneralPlug.h:2897` with
`AEGP_Command` being a plain `A_long`. So this is assembly, not new risk.

Adding a seventh kind later must never touch the gesture code — that is the test
of whether this abstraction is holding.

## AE menu commands: store the ID, never the name

There is **no API to enumerate** After Effects' menu commands. What exists is the
ExtendScript pair `app.findMenuCommandId(name)` → numeric id, and natively
`AEGP_DoCommand(id)`.

So the binding is resolved **once, at bind time**, in settings: the user picks or
types the menu item as it appears in *their* AE, we resolve it to a number, and
we persist **the number**. The name is kept for display only and is never the
key.

This is the S5 lesson in a different costume: round-trip through the API's own
values rather than persisting a human-readable string. If the name were the key,
a user switching AE to another language would break every binding they own. The
numeric id survives a language change, survives most version changes, and is
faster at fire time because it needs no script round-trip at all.

A stored id that no longer resolves is a real case (an older AE, a removed
feature). Those slots grey out at summon, reusing the context gating that already
exists rather than failing at fire time.

## User scripts: snippet and file are different, and snippet matters more

Both kinds exist because they solve different problems.

**`script-file`** runs a `.jsx` by path. It is the obvious one and the less
useful one. Most real scripts do work *at load*: `ag_masterNull.jsx` ends with
`showUI(thisObj)`, so binding the file would pop its palette on every fire. That
is not what anyone wants from a wheel.

**`script-snippet`** is a line of ExtendScript, and it is the right binding for a
power user's existing library. After Effects shares **one ExtendScript namespace**
across every running script, so a script that has been loaded — from the Scripts
menu, from a panel, from a `script-file` binding — leaves its globals reachable.
`ag_masterNull.jsx` deliberately exposes exactly one:

    var _mn = (function(thisObj){ … return { addMasterNull : addMasterNull … }; })(this);

which means a hexagon can be bound to

    _mn.addMasterNull(false)          // anchor average
    _mn.addMasterNull(true)           // comp centre

and the whole variant space of that script is reachable without pieFX knowing
anything about it. `addMasterNull` already defaults its `mods` argument when
there is no click behind it, which is precisely the call shape a wheel makes.

The settings UI should therefore offer both and steer script authors toward
snippets, with the note that a snippet needs its script loaded first.

**Undo discipline.** A snippet that throws inside an open undo group wedges AE's
undo stack until some other script happens to close one. `ag_masterNull.jsx`
guards this with `try/finally` and that is the pattern to require: pieFX opens no
undo group around a user snippet, because the snippet may legitimately want to
own several, and a nested group we opened could be left dangling by a throw we
cannot see.

## Wire format: base64 for anything free-text

The native side hand-rolls its JSON (`sprintf_s`) and hand-rolls its parsing,
which is fine for fixed keys and numbers. It is **not** fine for arbitrary user
script, which contains quotes, backslashes and newlines — writing a correct JSON
string unescaper by hand is exactly the kind of thing that works until a user
writes `"` in a string literal.

So every free-text payload crosses the pipe **base64-encoded**:

    {"type":"fire","kind":"ae-command","id":2359}
    {"type":"fire","kind":"script-snippet","b64":"X21uLmFkZE1hc3Rlck51bGwoZmFsc2Up"}
    {"type":"fire","kind":"script-file","b64":"RDovLi4uL2FnX21hc3Rlck51bGwuanN4"}
    {"type":"fire","kind":"effect","b64":"QURCRSBHYXVzc2lhbiBCbHVyIDI="}
    {"type":"fire","kind":"builtin","name":"anchor-grid","cell":4}

Base64 is a safe alphabet, so the parser only ever scans for a closing quote. The
decoder is ~30 lines and cannot be surprised by user input.

`script-file` paths are additionally normalised to forward slashes before being
wrapped in `$.evalFile("…")` — ExtendScript accepts them, and it removes the
backslash-escaping question entirely.

## Ownership: overlay decides, native executes

The overlay already owns the wheel geometry and hit-testing (it draws them), so
it also owns the **slot tree** and the **settings file**. The native plug-in is a
dumb executor with a switch on `kind`.

    native  → overlay   summon / cursor / release / cancel        (as today)
    overlay → native    fire  { action descriptor }               (new)

Native reads the pipe on its **background thread**, parses to a small fixed
`PieAction` queue under the existing critical section, and `IdleHook` drains and
executes it on AE's UI thread. This reuses the deferred-action pattern the POC
already established for the anchor move: nothing AEGP is ever called from inside
the mouse hook or off the UI thread.

## On disk

`%APPDATA%\pieFX\settings.json`, human-readable per the roadmap. Shape:

    {
      "version": 1,
      "gesture": { "holdMs": 200, "armMode": "center" },
      "wheel": {
        "slots": [ <slot>|null × 6 ]
      }
    }

    <slot> = {
      "label":  "Master Null",
      "action": { "kind": "script-snippet", "code": "_mn.addMasterNull(false)" },
      "slots":  [ <slot>|null × 6 ]        // present only when it is a ring
    }

Notes that are load-bearing rather than cosmetic:

- **Slots are positional and `null` is meaningful.** A hole keeps its direction
  reserved, so adding an item later does not move the ones already in the user's
  muscle memory. Never compact the array.
- **A ring may also carry an `action`** — that is its *default*, fired when the
  user flicks to it and releases without drilling in. This is what keeps the
  common case one flick.
- **`armMode`** is a setting, defaulting to `"center"`. `"exit"` is faster but
  needs a leave-and-return to pick the child lying in the parent's own direction.
- **Depth is capped at 2.** The format could nest forever; the UI must not let
  it. Marking-menu accuracy degrades badly past two levels.

## Settings UI (not built yet)

The settings screen should **be a wheel**: a large, clickable version of the real
thing, with an inspector beside it for the selected slot (label, action kind,
kind-specific fields). It is self-documenting, and it teaches the layout while
the user configures it — which a list of dropdowns never would.

Global settings (hold threshold, arm mode, theme) sit alongside it.

## Deliberately not decided yet

- **Per-context wheels.** The plug-in reads selection state natively at summon,
  so the wheel's contents *can* vary by context. Doing so costs muscle memory,
  so the rule should be: keep the skeleton stable and only grey or swap the slots
  that genuinely cannot apply. Not implemented.
- **Sharing / importing presets.** Obvious once the format is stable.
- **macOS paths.** `~/Library/Application Support/pieFX/` when the port happens.
