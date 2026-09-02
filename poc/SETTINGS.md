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
| `ae-command` | `name` (preferred), `id` (fallback) | `app.executeCommand(findMenuCommandId(name))` |
| `script-snippet` | ExtendScript source | `AEGP_ExecuteScript` |
| `script-file` | path to a `.jsx` | `AEGP_ExecuteScript` wrapping `$.evalFile` |
| `effect` | `matchName` | S5's catalogue lookup + `AEGP_ApplyEffect` |
| `ring` | six child slots | nothing — it is a container |

Every kind is now proven live in After Effects. `ae-command` was the one that
fought back, and the two sections below record why.

Adding a seventh kind later must never touch the gesture code — that is the test
of whether this abstraction is holding.

## AE menu commands: resolve by NAME, keep the id as a fallback

**This reverses an earlier decision in this file, and the reason is worth
keeping.** The original rule was "store the id, never the name", on the grounds
that names are localised and `app.findMenuCommandId` could not validate the names
we had. That second half was only true because the names in the *command map* are
internal identifiers (`AddtoRenderQueue`), not the strings AE puts in its menus.

Given the real menu name, `findMenuCommandId` resolves it against the running
After Effects and returns 0 when it does not exist. That is **bind-time
validation, which ids never offered** — and it is the exact protection needed
against a map that has already produced three wrong entries and two duplicate
names, and whose ids are only as good as one developer's testing on AE 2025.

An id can only ever be trusted. A wrong-but-valid id fires *some other command*
silently, which is strictly worse than failing.

So a binding carries a `name` and, optionally, a cached `id`:

    { "kind": "ae-command", "name": "Add to Render Queue", "id": 2161 }

The executor prefers the name, falls back to the id, and reports
`NO SUCH MENU COMMAND` as a toast when the name does not resolve. Names being
localised is the remaining cost, and the id fallback is what covers it.

## Menu commands must go through ExtendScript

`AEGP_DoCommand` is not usable for this. Measured both ways: every id fired
through it from a **menu command** worked, and every id fired through it from the
**idle hook** — where the gesture path lands — silently did nothing and still
returned `A_Err_NONE`.

That is the S2B finding again. `UpdateMenuHook` fires when AE **rebuilds its
menus**, so command enable-state is only current just after a menu interaction;
during idle AE treats the command as not-enabled and drops it. There is no API to
force a rebuild.

`app.executeCommand` is a different dispatch path and is already proven to work
from idle — it is how Master Null fires. The generated snippet brackets the call
with the comp's layer count and the render queue's item count, so a command that
returns cleanly and changes nothing says so.

### Names measured on AE 2026 (findMenuCommandId)

Asking AE directly beat the map three times, which is the argument for name
binding in one table:

| Menu name | AE returns | the map said |
|---|---|---|
| `Solid...` | **3000** | 2038 is `Solid`, 3000 is `Solid...` — the guess took the wrong twin |
| `Text` | **7034** | 2836; **7034 is not in the map at all** |
| `Split Layer` | **2158** | labels 2158 `DuplicatePreserveFile`, so this was written off as having no id |
| `Pre-compose...` | **2071** | id right, but `Precompose...` does **not** resolve — the hyphen matters |
| `Null Object`, `Adjustment Layer`, `Add to Render Queue`, `Light...`, `Camera...`, `Duplicate` | 2767, 2279, 2161, 2563, 2564, 2080 | agreed |
| `Center Anchor Point in Layer Content` | **10312** | agreed — useful for the Anchor tool |

Not resolving yet, and running by id meanwhile: **`Save Frame As...`** (a
*submenu* in AE, so the command is probably its leaf) and **`Center in View`**
(3819 is proven to work, so only the spelling is missing).

`2263`, the map's second `AdjustmentLayer`, does nothing. `2279` is the live one.

### The command map

`overlay/src/ae-commands-2025.json` is an id → name table (909 entries: 613 menu
commands, 296 effects) brought over from another project. It is now a **hint for
the settings picker only** — not the binding key — because bindings resolve by
name. Three things about it are load-bearing:

- **It is version-stamped, and the stamp is the point.** It was captured from AE
  2025; we target 2026. Ids are mostly stable across versions but not provably
  so, which is why the settings UI needs a **test-fire** button: the user
  confirms a binding does what they expect before committing it. That is the
  practical safety net, because —
- **the names are internal identifiers, not display strings.** `AddtoRenderQueue`
  is not what the menu says, so `app.findMenuCommandId` cannot round-trip these
  and cannot validate them for us.
- **Names are not unique.** `AdjustmentLayer` appears at both 2263 and 2279,
  `File` at four different ids. The id disambiguates; the name never can. One
  more reason a *map* name cannot be the key. A real MENU name can, because AE
  resolves it itself.

Checking the first draft of the defaults against this map caught three wrong
guesses — `SaveFrameAs` is 2233 (not 2104, which is `File`), `CenterInView` is
3819, and **Split Layer has no menu id in the map at all**, which means the
"Split + Dup" slot from the mockups is a script action waiting to be written
rather than a menu command. Guessing ids is exactly as unreliable as it looks.

Also worth noting for the Anchor tool: **10312 `CenterAnchor`** exists as a menu
command, which may make part of the anchor grid reachable without ExtendScript.

Negative ids are effects. Prefer the `effect` action kind for those —
apply-by-match-name is the documented path and does not depend on menu structure.

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
