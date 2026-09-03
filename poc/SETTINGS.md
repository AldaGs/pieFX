# pieFX — settings and the action model

Written before the settings UI exists, because the UI is the easy half. The hard
half is deciding **what a hexagon can be bound to**, and that decision reaches
into the native plug-in, the pipe protocol, and the on-disk format all at once.

## The one abstraction

A hexagon is a **slot**. A slot has a label and an **action**. Everything the
product can ever do is one of six action kinds:

| Kind | Payload | Executed by |
|---|---|---|
| `builtin` | `name` (`anchor-grid`, `effect-search`, `copy-frame`), plus per-tool args | pieFX's own native code and custom widget |
| `ae-command` | `name` (preferred), `id` (fallback) | `app.executeCommand(findMenuCommandId(name))` |
| `script-snippet` | ExtendScript source | `AEGP_ExecuteScript` |
| `script-file` | path to a `.jsx` | `AEGP_ExecuteScript` wrapping `$.evalFile` |
| `effect` | `matchName` | S5's catalogue lookup + `AEGP_ApplyEffect` |
| `preset` | `path` | an animation preset (.ffx), applied with `layer.applyPreset` |
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

**The bootstrap.** "A snippet needs its script loaded first" was true and
unhelpful: the first Master Null of every session toasted `_mn is undefined` and
the second worked, which reads as a flaky wheel. So an action may declare what
it needs:

    "action": {
      "kind":  "script-snippet",
      "code":  "_mn.addMasterNull(false)",
      "needs": { "global": "_mn", "file": "ag_masterNull.jsx" }
    }

The overlay wraps the snippet in a loader that runs the file **only when the
global is absent**, and the whole thing crosses the pipe base64-encoded as
before — the native side stays a dumb executor that knows nothing about scripts.

`file` takes three forms, in order of how portable they are:

- **relative** (`scripts/ag_masterNull.jsx`) — resolved against pieFX's install
  directory, the folder holding the `.aex` and the overlay. This is the form for
  a script that **ships with the product**: `poc/scripts/` is copied beside the
  plug-in at install time, so the binding works on any machine without anyone
  editing it.
- **bare** (`ag_masterNull.jsx`) — searched in AE's own script folders
  (`Scripts/ScriptUI Panels` first, then `Scripts`, under every version
  directory in the user data folder), for a script the user already has.
- **absolute** (`D:/…/ag_masterNull.jsx`) — one machine's layout, and the thing
  the other two exist to avoid.

`$.global.__pieFXHeadless` is set for the duration of the call. That is the flag
a script author guards their UI with —

    if (!$.global.__pieFXHeadless) showUI(thisObj);

— so that loading a panel script to reach its functions does not also pop its
palette in the middle of a gesture. Scripts without the guard still work; they
just show their window the first time.

The settings UI should offer both kinds and steer script authors toward
snippets, with `needs` presented as part of the snippet form rather than as an
advanced option — a snippet without it is the flaky case.

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

## `copy-frame`: the current frame on the clipboard

`Comp > Copy to Clipboard`. The sibling of `Save Frame as PNG` - same frame,
same forced 1:1 resolution - with no file dialog and no file for the user to
manage.

It is a **builtin** rather than a snippet because the work is split across the
two things neither side can do alone: ExtendScript can write a PNG
(`saveFrameToPng`) and can do nothing at all with the clipboard, and the
plug-in owns the clipboard but would have to take on the whole render API to
produce a frame. So the frame goes out through `%TEMP%\pieFX_clipboard_frame.png`
and comes straight back in as pixels, decoded with WIC. The file is left there
and overwritten by the next copy.

**Three clipboard formats go on, and the reason is alpha.** A comp frame can be
transparent and the classic `CF_DIB` cannot say so:

    "PNG"      the original file bytes. What Photoshop, Chrome, Figma and Slack
               reach for first, and exactly the frame AE wrote.
    CF_DIBV5   32bpp with a real alpha mask, for consumers that read it.
    CF_DIB     the same pixels forced opaque - the universal fallback. A
               transparent frame pasted through this one shows whatever was
               behind the alpha, usually black. That is the format, not a bug.

The toast reports `Copied Frame 1234 from <Comp>`, and the frame number is the
one the TIMELINE shows: `displayStartFrame` is added, because a comp that starts
at 1001 must not report frame 0 - a number the user cannot find in their own
timeline is worse than no number.

## Animation presets: found by walking, applied by script

They are in the search because AE's own Effects & Presets panel puts them
there, and a search that offered only effects answered half the question. They
are not effects in any other sense, and three differences drive the code:

- **No enumeration.** There is no AEGP call that lists presets; AE's panel finds
  them by SCANNING FOLDERS and so does the plug-in. Two roots: the shipped
  `Support Files/Presets` and every `Documents/Adobe/After Effects*/User
  Presets`. The first is found by **climbing** from the plug-in's own module
  path until an ancestor holds both a `Presets` and a `Plug-ins` folder - that
  pair is what identifies Support Files. It climbs rather than counting levels
  because **a plug-in may be installed in a SUBFOLDER of `Plug-ins`** (the
  author's is in `Plug-ins\AGS\`), and the folder is the user's to organise. A
  plug-in loaded from somewhere else entirely finds no Presets, logs that, and
  offers effects only.
- **Documents is not `%USERPROFILE%\Documents`.** It can be redirected to
  OneDrive and localised - on the author's machine it is
  `C:\Users\aldai\OneDrive\Documentos`. `SHGetFolderPath(CSIDL_PERSONAL)`
  follows both; a hand-built path finds nothing and reports it as "you have
  none".
- **No AEGP apply.** `applyPreset` is a LAYER method in the scripting DOM, so
  the `preset` kind runs a snippet rather than calling a suite (watched live).
  It opens one undo group, because a preset can add half a dozen effects and
  keyframes and the user who dislikes the result wants ONE undo - that last
  part is reasoned, not yet watched.

Identity is the **path**, the way an effect's is its match name. The category is
the folder it was found in - the only grouping a .ffx file carries - which is
also what makes typing "transitions" find the transitions.

## The effect search: two files beside the settings

`builtin` / `effect-search` is the one action kind that never reaches the
plug-in. It opens a **focused, undecorated window** in the overlay process -
no title bar, no close or minimise button, not in the taskbar, dismissed by
Enter, Escape or clicking away - because a search field needs a keyboard and
nothing in the gesture can take one: the overlay is
click-through and unfocused, the plug-in hooks `WH_MOUSE` only, and the gesture
is a press-and-hold. Applying the effect the user picks is an ordinary
`effect` fire from that window, so nothing new crosses the pipe.

Two files sit beside `settings.json` in `%APPDATA%\pieFX\`:

    effects.json   WRITTEN BY THE PLUG-IN, read by the overlay. The installed
                   catalogue AND the animation presets, both built once per
                   session on the first idle after arming:
                   { "effects": [ { "name", "match", "category" }, ... ],
                     "presets": [ { "name", "path", "category" }, ... ],
                     "walked": 519, "claimed": 519 }
                   `walked` vs `claimed` is the honesty check S5 used - a
                   disagreement means the enumeration is incomplete.
                   Everything walked is written, obsolete and uncategorised
                   entries included: filtering is the search UI's decision,
                   and the plug-in owes it the API's own strings unedited.

    recents.json   WRITTEN BY THE SEARCH WINDOW, read by it and by the wheel.
                   A plain array of match names, most recent first. Its own
                   file rather than a key in settings.json, because the
                   settings window writes that file WHOLE - two windows of one
                   process saving the same file is a lost update waiting to
                   happen.

`--effects <path>` overrides where the catalogue is read from, and
`--effects none` means there is none. It exists for the same reason
`--settings none` does: the harness drives the search against a known fixture
(`src/effects-sample.json`, which carries one of each of the catalogue's sharp
edges) rather than against whatever the developer has installed.

**Display name is not a unique key.** Match names truncate at 31 characters, 50
entries live in `_Obsolete` and collide with live effects on display name, and
107 have no category at all. Search on the display name, carry the **match
name** as identity, show the category to disambiguate.

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
      "gesture": { "holdMs": 200, "armMode": "distance", "armOnLaunch": true },
      "appearance": { "accent": "#C74FD6", "scale": 1 },
      "wheel": {
        "slots": [ <slot>|null × 6 ]
      }
    }

    <slot> = {
      "label":    "Master Null",
      "requires": "selection",             // or "comp", or absent/null for none
      "accent":   "#E8A33D",               // optional: this hexagon's highlight
      "action":   { "kind": "script-snippet", "code": "_mn.addMasterNull(false)" },
      "slots":    [ <slot>|null × 6 ]      // present only when it is a ring
    }

Notes that are load-bearing rather than cosmetic:

- **Slots are positional and `null` is meaningful.** A hole keeps its direction
  reserved, so adding an item later does not move the ones already in the user's
  muscle memory. Never compact the array.
- **A ring may also carry an `action`** — that is its *default*, fired when the
  user flicks to it and releases without drilling in. This is what keeps the
  common case one flick.
- **`armMode`** is a setting, and the default is now `"distance"`. The three:

  - **`distance`** — a child is live once the stroke is clear of the **centre
    hexagon** (`ARM_DIST = R`). Once a category is open, the six hexagons on
    screen are its children and the parent has moved to the middle, so the
    moment you are out of the middle you are on a child.

    It was first written as the far edge of the *child* hexagon, 147px, which
    meant overshooting the very thing you were aiming at before it lit up. It
    felt as wrong as that sounds and lasted one session.

    **The cost, stated plainly: a category's DEFAULT action is not reachable
    under this rule.** The band that would fire it is `DEAD` (49.7px) to
    `ARM_DIST` (54px) — about four pixels — so in practice a flick into a
    category lands on the child in that direction. Defaults still work under
    `center` and `exit`. Anything that must stay reachable therefore needs to be
    a child slot as well as a default: in the shipped tree that is
    **`Master Null`'s plain variant**, which exists only as the category default
    and so is currently unreachable under `distance`.
  - **`center`** — level 2 stays inert until the cursor passes back through the
    middle. Was the default.
  - **`exit`** — armed at once, but the child lying in the direction you arrived
    from stays inert until you leave that sector and return.

  `distance` replaced `center` because **neither of the other two can select the
  child that lies in its parent's own direction in one stroke** — the case that
  sent a user back to report `Create > Adjustment Layer` as broken. `center`
  holds every child inert until the cursor comes back to the middle; `exit`
  holds that specific child inert on purpose. Distance also leaves the one-flick
  default intact, because releasing anywhere ON the category hexagon is still
  inside the radius.

- **`appearance.scale`** multiplies the whole wheel — hexagons, spacing, the
  anchor grid, the search panel, and every distance the gesture measures.
  Clamped to 0.5–2.5. It is applied as a canvas transform at draw time and as a
  division on the incoming cursor, NOT by scaling `R` and the dozen constants
  derived from it: that geometry is load-bearing and thoroughly measured, and
  one multiply at each boundary cannot get any of it subtly wrong. Everything
  inside lives in one unscaled design space and stays there. The toast is
  deliberately outside it — a message about a failure is not part of the wheel
  and has to stay legible at the smallest setting.

- **The highlight colour is a setting**, `appearance.accent`, and a slot may
  override it with its own `accent`. The shipped purple's hot gradient and hot
  ink were tuned by eye and are the user's call, so they are not recomputed from
  the accent: they are kept verbatim for the default and hue-rotated (with
  saturation scaled) for anything else. Default in, default out, bit for bit.

- **"Not armed yet" is drawn differently from "cannot apply".** They used to be
  the same grey, which is a lie with consequences — a hexagon one movement away
  from working looked exactly like one that could never work, so the wheel
  appeared to refuse an action it was perfectly willing to perform. There are
  now four states: `idle`, `hot`, `dead` (cannot fire) and `pending` (can fire,
  not armed), plus a line under the wheel saying what the stroke is waiting
  for.
- **A resolved name is not a correct name.** `Create > Solid` carries an id and
  no name, alone among the menu bindings, because `"Solid..."` resolves — to
  3000, which makes a solid in the **project** and never puts it in the comp.
  `findMenuCommandId` proves an item by that name exists, not that it is the one
  you meant, and After Effects has more than one. Prefer the name; drop it the
  moment behaviour disagrees with it.
- **`requires` is what makes the wheel honest.** `"selection"` needs one or more
  selected layers, `"comp"` needs a comp as the active item; absent means the
  action needs nothing. The plug-in sends both facts (`hasSelection`, `hasComp`)
  with every summon, and the overlay draws a slot that cannot apply dead and
  refuses to fire it — firing into nothing and reporting success is the failure
  mode this exists to prevent. A child with no `requires` **inherits** its
  category's, and an explicit `null` overrides that back to nothing: that is how
  `Create > Comp` stays live with no comp open while its five siblings do not.
  A category is drawn dead only when nothing inside it is live, so a ring whose
  default cannot apply still opens to the children that can.
- **Depth is capped at 2.** The format could nest forever; the UI must not let
  it. Marking-menu accuracy degrades badly past two levels.

## Settings UI

Built, and it **is** a wheel: a large, clickable version of the real thing with
an inspector beside it. Self-documenting, and it teaches the layout while you
configure it — which a list of dropdowns never would.

`Window ▸ pieFX Settings` (`overlay/src/settings.html` + `settings.js`). It is a
**second window in the overlay's own process**, not a second process: it must
read and write the same file the wheel is using, and two processes racing on one
settings file is a bug waiting for the first person who leaves the configurator
open. The plug-in only sends `{"type":"settings"}` down the events pipe, arming
first if it has to — the overlay does not exist while disarmed, so a settings
item that did nothing when off would look broken.

The wheel, the settings window and the firing path are **one implementation**:
`menu.js` holds the slot tree and the compile step, `hexdraw.js` the glass
renderer, `actions.js` the action-to-message translation. A configurator drawn
by a second renderer would drift from the thing being configured, and the copy
that drifted would be the one the user is looking at.

What it enforces, because these are the rules that are easy to write down and
easy to break:

- **A hole is drawn.** An empty position renders as a hexagon with a `+`, and
  "Clear slot" writes `null` in place rather than compacting. Seeing the hole is
  what makes "slots are positional" obvious instead of a rule in a document.
- **Depth is capped at 2.** "Make a category" is simply absent at level 2.
- **`needs` is part of the snippet form**, not an advanced option, because a
  snippet without it is the flaky case.
- **A category's action is labelled `Default action`**, with the explanation
  beside the field rather than at the foot of the panel. It read as "this
  hexagon runs Add to Render Queue" when what it means is "this fires only if
  you let go without picking a child", and a user duly reported the screen as
  contradicting the wheel.
- **Test-fire, per binding.** It goes through `sendFire` — the same function the
  gesture calls, down the same pipe — because a test through a second code path
  tests the wrong thing. This is the practical answer to a command map that has
  been wrong three times, and to `findMenuCommandId` proving only that *a* menu
  item by that name exists.

Saving broadcasts `piefx-settings`, and the live wheel reloads. Without that a
rebound hexagon would keep firing the old action until AE restarted, which reads
as "settings do not work" rather than as "settings are cached". A summon in
flight keeps the tree it started with: swapping the menu out from under a press
would move hexagons while the cursor is still travelling to one.

Global settings sit along the bottom: hold threshold, arm mode, and **arm on
launch**.

### The two settings the plug-in reads for itself

`gesture.holdMs` and `gesture.armOnLaunch` are read by the NATIVE side, from
`%APPDATA%\pieFX\settings.json`, on the first idle after AE starts. They have to
be: the hold is measured in the mouse hook before the overlay is involved at
all, and `armOnLaunch` has to be honoured *before the overlay exists*.

It is a hand scan for two scalars, not a parser — `FindKey` finds the key and
reads the next token. A real JSON parser in the plug-in would be a second
implementation of a format that already has one, and everything else in the file
belongs to the overlay. `holdMs` is **clamped to 80–2000ms**: a zero would summon
the wheel on every right-click and take AE's context menu away entirely, which is
a setting nobody could undo without editing the file by hand.

`armOnLaunch` ships **on**, so an install is "restart AE and flick" rather than
"remember to arm it first". The cost is real and worth stating: a global mouse
hook and a spawned overlay process now happen at every AE launch with no click to
attribute them to, and a missing `pieFX-overlay.exe` fails silently there instead
of loudly at a menu click. `Window ▸ pieFX` remains the manual toggle and the way
out.

## Deliberately not decided yet

- **Per-context wheels.** The plug-in reads selection state natively at summon,
  so the wheel's contents *can* vary by context. Doing so costs muscle memory,
  so the rule is: keep the skeleton stable and only grey the slots that genuinely
  cannot apply. The greying half is now `requires` (above); *swapping* contents
  by context remains deliberately undecided.
- **Sharing / importing presets.** Obvious once the format is stable.
- **macOS paths.** `~/Library/Application Support/pieFX/` when the port happens.
