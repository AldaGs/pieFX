// pieFX — the effect search.
//
// WHY THIS IS A WINDOW AND NOT A PANEL ON THE WHEEL. A filter field needs a
// keyboard and nothing in pieFX can receive a keystroke: the overlay window is
// created click-through and unfocused, the plug-in installs WH_MOUSE and no
// keyboard hook, and the gesture is a press-and-hold, so even with a hook the
// user would be typing one-handed with the right button down. Releasing on
// `Effects` therefore opens a real focused window — the same machinery as the
// settings window, `raise()` included, which is the only input path this
// project has already watched working inside After Effects. The cost is
// honest: the gesture ends, and this begins.
//
// The catalogue comes from a file the plug-in writes once per session
// (`%APPDATA%\pieFX\effects.json`). Nothing is enumerated here.
//
// It holds THREE kinds of thing, and the window deliberately does not separate
// them: installed effects, animation presets (.ffx), and AE's own MENU
// COMMANDS. AE's own Effects & Presets panel lists effects and presets in one
// tree, so a search that offered only effects would answer half of what the
// user came to ask; commands are here because AE 26.2's own Quick Apply put
// them in the same box, and a search that cannot run "Precompose" sends the
// user to Adobe's dialog for half their day. They differ in ways the code has
// to keep straight and the UI mostly should not:
//
//   identity   an effect is its MATCH NAME, a preset is its PATH, a command is
//              its numeric ID
//   applying   an effect goes through AEGP_ApplyEffect, a preset through the
//              scripting DOM's layer.applyPreset, a command through
//              app.executeCommand - three different fire kinds
//   grouping   an effect's category comes from AE, a preset's is the folder it
//              was found in (the only grouping a .ffx file has), and a command
//              has none, so it is given one
//
// COMMANDS COME FROM A FILE, NOT FROM AE, and that is the sharp edge. The
// settings window already reads `ae-commands-2025.json`, and its comment there
// is the warning worth repeating: the names in it are INTERNAL IDENTIFIERS,
// not the display strings `findMenuCommandId` resolves. So a command row
// cannot be fired by name the way a hand-typed binding can — it fires by id,
// and the id is a hand-tested constant for AE 2025 that has already been
// wrong three times (see actions.js). That is why the id is ON SCREEN in the
// row rather than hidden behind the display name: it is the identity, it is
// the thing that can be wrong, and a user who fires the wrong command needs to
// be able to see why. Until the plug-in dumps the live menu from the running
// AE, this list is a convenience with a known error bar, and the UI says so.

import { sendFire } from "./actions.js";
import { compileAction } from "./compile.js";

const T = window.__TAURI__;
const qEl = document.getElementById("q");
const listEl = document.getElementById("list");
const noteEl = document.getElementById("note");
const countEl = document.getElementById("count");
const hiddenEl = document.getElementById("showHidden");
const stackEl = document.getElementById("stack");

const MAX_ROWS = 60;
const MAX_RECENTS = 8;

const state = {
  all: [], // every entry the plug-in walked
  recents: [], // match names, most recent first
  rows: [], // what is on screen: {head} separators and effect entries
  sel: 0,
  walked: 0,
  claimed: 0,
  presets: 0,
  commands: 0,
  applied: 0, // how many have been fired in this one summon - see apply(stay)
  params: [], // parsed off the END of the query - see parseQuery
};

// --- the catalogue's three sharp edges -------------------------------------
// Recorded in SPIKES.md and repeated here because they are filtering rules,
// not trivia:
//   - 50 entries live in `_Obsolete`/`Obsolete` and COLLIDE with live effects
//     on display name ("Levels", "Noise", "Box Blur" each appear twice).
//     Offering the dead one is a silent wrong answer.
//   - 107 have an empty category: pseudo-effects and preset control rigs, not
//     things anyone picks from a menu. Empty category and the `Pseudo/` match
//     prefix are the two flags.
//   - match names truncate at 31 characters, so the string the API returns is
//     the identity — never one retyped from documentation.
// Both groups are hidden by default and reachable behind a checkbox, because
// "hidden" is a judgement about what is useful and the user may disagree.
function isObsolete(e) {
  const c = (e.category || "").toLowerCase();
  return c === "_obsolete" || c === "obsolete";
}

function isInternal(e) {
  return !e.category || /^Pseudo\//.test(e.match || "");
}

// A preset is never hidden: they are all things a person deliberately made or
// Adobe deliberately shipped, and there is no _Obsolete folder among them.
function usable(e) {
  if (!e || !e.id) return false;
  if (e.t === "preset") return true;
  // A command is never hidden either. The obsolete/internal judgement is about
  // AE's effect catalogue; the command map has no equivalent, and what it does
  // have - entries that may be plain wrong - is not something a checkbox can
  // sort out.
  if (e.t === "command") return true;
  return !isObsolete(e) && !isInternal(e);
}

// --- loading ---------------------------------------------------------------
function invoke(cmd, args) {
  if (!(T && T.core)) return Promise.reject(new Error("not running under Tauri"));
  return T.core.invoke(cmd, args || {});
}

function loadCatalogue() {
  return invoke("load_effects").catch(() =>
    // Browser preview: there is no plug-in and no APPDATA, so fall back to the
    // fixture the harness uses. One file, two consumers — a preview that shows
    // a different list from the one the tests assert on is a preview of
    // nothing.
    fetch("effects-sample.json").then((r) => r.text())
  ).then((txt) => {
    // A missing file is not an empty catalogue, and the difference matters:
    // one means "pieFX has not been armed in this AE session yet", the other
    // would mean "no effects are installed", which is never true.
    if (!txt) {
      state.all = [];
      return;
    }
    let doc;
    try {
      doc = JSON.parse(txt.replace(/^﻿/, ""));
    } catch (e) {
      state.all = [];
      state.parseError = String(e);
      return;
    }
    // One flat list, tagged. Everything downstream - filtering, ranking,
    // recents, the row renderer - works on `id`, so the only places that care
    // which kind a row is are the ones that must: the type pill and the fire.
    const fx = (Array.isArray(doc.effects) ? doc.effects : []).map((e) => ({
      t: "effect",
      id: e.match,
      name: e.name,
      match: e.match,
      category: e.category,
    }));
    const ps = (Array.isArray(doc.presets) ? doc.presets : []).map((e) => ({
      t: "preset",
      id: e.path,
      name: e.name,
      category: e.category,
    }));

    state.all = fx.concat(ps);
    state.walked = doc.walked | 0;
    state.claimed = doc.claimed | 0;
    state.presets = ps.length;
  });
}

// The command map. Read with `fetch` rather than a Tauri command because it is
// a file this project SHIPS, next to the HTML, not one the plug-in writes into
// APPDATA - the settings window reads it exactly this way, and two readers of
// one shipped file should not need two mechanisms.
//
// Two entries are dropped rather than shown:
//   - negative ids, which are EFFECTS wearing a command id. The effect kind
//     applies those properly, by match name, and offering the same effect twice
//     under two fire paths is a way to find out later which one was wrong.
//   - duplicate ids, which the map does carry.
// Duplicate NAMES are kept, because the id distinguishes them and the id is on
// screen. Hiding one of a pair would be guessing which one the user wanted.
function prettyCommand(raw) {
  // "NewComposition" -> "New Composition", "RAMPreview" -> "RAM Preview". The
  // raw identifier stays searchable: someone who knows it should be able to
  // type it.
  return String(raw)
    .replace(/([a-z0-9])([A-Z])/g, "$1 $2")
    .replace(/([A-Z]+)([A-Z][a-z])/g, "$1 $2")
    .trim();
}

function loadCommands() {
  return fetch("ae-commands-2025.json")
    .then((r) => r.json())
    .then((map) => {
      const seen = new Set();
      const out = [];
      for (const key of Object.keys(map || {})) {
        const n = Number(key);
        if (!Number.isFinite(n) || n < 0) continue;
        if (seen.has(n)) continue;
        const raw = map[key];
        if (!raw) continue;
        seen.add(n);
        out.push({
          t: "command",
          id: String(n),
          cmdId: n,
          name: prettyCommand(raw),
          match: String(raw),
          category: "Menu command",
        });
      }
      state.commands = out.length;
      state.all = state.all.concat(out);
    })
    .catch(() => {
      // No map: the window is still a working effect search, which is what it
      // was before commands existed. Silence is right here - there is nothing
      // the user can do about a missing shipped file from this window.
      state.commands = 0;
    });
}

// Recents are {t, id} now that a recent can be a preset. The old file was a
// bare array of match names, so a string read back is MIGRATED rather than
// dropped: someone's recents list is not worth resetting over a format change,
// and a match name is unambiguously an effect.
const RECENT_KINDS = ["preset", "command"];

function normaliseRecent(r) {
  if (typeof r === "string") return { t: "effect", id: r };
  if (r && typeof r.id === "string") {
    const t = RECENT_KINDS.includes(r.t) ? r.t : "effect";
    return t === "command" ? { t, id: r.id, name: r.name } : { t, id: r.id };
  }
  return null;
}

function loadRecents() {
  return invoke("load_recents").then(
    (txt) => {
      try {
        const a = JSON.parse((txt || "[]").replace(/^﻿/, ""));
        state.recents = (Array.isArray(a) ? a : []).map(normaliseRecent).filter(Boolean);
      } catch (e) {
        state.recents = [];
      }
    },
    () => {
      state.recents = [];
    }
  );
}

function rememberUsed(entry) {
  const r = { t: entry.t, id: entry.id };
  // A command recent carries its NAME as well as its identity, which no other
  // kind needs to. The wheel's recents panel turns an identity into something
  // readable on its own ("ADBE Gaussian Blur 2" -> "Gaussian Blur", a preset
  // path -> its filename) precisely so it does not have to carry a copy of the
  // catalogue - but a command id is "2071", and no amount of trimming makes
  // that a word. The name is three characters of file per entry and the only
  // way the panel can say "Precompose".
  if (entry.t === "command") r.name = entry.name;

  state.recents = [r]
    .concat(state.recents.filter((x) => !(x.t === r.t && x.id === r.id)))
    .slice(0, MAX_RECENTS);
  // Best effort. A recents file that cannot be written must not stop an effect
  // from being applied — the applying is the feature.
  invoke("save_recents", { json: JSON.stringify(state.recents) }).catch(() => {});
}

// --- the query, which is not only a search term -------------------------
// "gaussian 40" is a search for "gaussian" and an instruction to set 40. The
// parameters are taken off the END, one token at a time, and the first token
// that does not look like one stops the walk — so "3 d layer" and "1 up" are
// searches, not a search for "3 d" with a parameter, and "box blur2" keeps its
// 2 because "blur2" is not a number.
//
// Two forms, and the named one is the one to prefer:
//
//   gaussian blur=40     named:      matched as a substring of the property
//                                    name against the LIVE effect
//   gaussian 40          positional: the first settable numeric property
//   levels 0.2 0.8       positional, in order
//
// Positional is a guess and is documented as one; named is not. Neither is a
// whitelist: every effect with a settable numeric property can take one, which
// is nearly all of them, and the ones that cannot say so in a toast rather than
// failing to apply.
const PARAM_TOKEN = /^(?:([a-z][a-z0-9 _-]*?)=)?(-?(?:\d+\.?\d*|\.\d+))$/i;

function parseQuery(raw) {
  const tokens = raw.trim().split(/\s+/).filter(Boolean);
  const params = [];

  while (tokens.length > 1) {
    const m = PARAM_TOKEN.exec(tokens[tokens.length - 1]);
    if (!m) break;
    tokens.pop();
    params.unshift(m[1] ? { name: m[1].toLowerCase(), value: Number(m[2]) } : { value: Number(m[2]) });
  }

  return { text: tokens.join(" "), params };
}

function paramLabel(params) {
  return params.map((p) => (p.name ? p.name + " = " + p.value : String(p.value))).join(", ");
}

// --- matching --------------------------------------------------------------
// Substring, case-insensitive, on the display name first and the match name
// second, so that typing "ADBE Gauss" finds what typing "gaussian" does. A
// name that STARTS with the query outranks one that merely contains it, which
// is what makes "blur" put "Blur (Fast)" above "Bilateral Blur".
function score(e, q) {
  const n = (e.name || "").toLowerCase();
  const m = (e.match || "").toLowerCase();
  const c = (e.category || "").toLowerCase();
  if (n === q) return 0;
  if (n.startsWith(q)) return 1;
  if (n.includes(q)) return 2;
  if (m.includes(q)) return 3;
  // Category last, and it is here for PRESETS: their names are written for a
  // folder ("Fade In"), so the folder is half of what identifies them, and
  // typing "transitions" should find the transitions. It also makes "menu
  // command" list every command, which is the only way to browse them.
  if (c.includes(q)) return 4;
  return -1;
}

function pool() {
  return hiddenEl.checked ? state.all.filter((e) => e && e.id) : state.all.filter(usable);
}

// Commands are SEARCHED, never BROWSED, and that is a measured decision rather
// than a taste. There are 613 of them against nine effects in the fixture and a
// few hundred in a real install, so an alphabetical list of everything opens on
// "1", "1 Up", "2 Up", "3 D Layer" - the map's internal identifiers, in a heap,
// burying the effects the window exists to apply. Worse, the no-query list is
// the ZERO-TYPING case: it is what a user sees when they release the gesture
// and have not asked for anything yet, and it should show the things they
// apply, not a dump of AE's menu bar. Type a letter and the commands are there.
function browsable(e) {
  return e.t !== "command";
}

// A command sorts AFTER every effect and preset that matched, whatever it
// scored. Not a fudge factor - a rule, because the fudge factor was tried and
// the number was arbitrary: this is an effect search that also knows the menu,
// so anything applicable outranks anything runnable. Measured on the fixture,
// "blur" scored the command `Blur` (#3698) an exact-name 0 and put it above
// all three blur EFFECTS, which is the wrong answer to the commonest query in
// the window. Nothing is lost at the bottom of the list: "precompose" and
// "new comp" match no effect, so their commands are still the first row.
function rank(e) {
  return e.t === "command" ? 1 : 0;
}

function byRecent(r) {
  return state.all.find((e) => e.t === r.t && e.id === r.id);
}

function buildRows() {
  const raw = qEl.value.trim();
  const parsed = parseQuery(raw);
  state.params = parsed.params;
  let q = parsed.text.toLowerCase();
  const rows = [];

  // A parse that finds NOTHING is a parse that was wrong. "levels 2" is far
  // more likely to be someone looking for an effect with a 2 in its name than
  // a request to set a parameter on nothing, so the whole raw string gets a
  // second chance before the window says "Nothing matches".
  if (q && state.params.length) {
    const hit = pool().some((e) => score(e, q) >= 0);
    if (!hit) {
      state.params = [];
      q = raw.toLowerCase();
    }
  }

  if (!q) {
    // No query: recents ARE the answer. This is the zero-typing common case,
    // and it is why the window is bearable at all.
    const rec = state.recents.map(byRecent).filter(Boolean);
    if (rec.length) {
      rows.push({ head: "Recent" });
      rec.forEach((e) => rows.push(e));
      rows.push({ head: "All effects and presets" });
    }
    pool()
      .filter(browsable)
      .sort((a, b) => (a.name || "").localeCompare(b.name || ""))
      .slice(0, MAX_ROWS)
      .forEach((e) => rows.push(e));
  } else {
    pool()
      .map((e) => ({ e, s: score(e, q) }))
      .filter((r) => r.s >= 0)
      .sort(
        (a, b) =>
          rank(a.e) - rank(b.e) ||
          a.s - b.s ||
          (a.e.name || "").localeCompare(b.e.name || "")
      )
      .slice(0, MAX_ROWS)
      .forEach((r) => rows.push(r.e));
  }

  state.rows = rows;
  state.sel = rows.findIndex((r) => !r.head);
}

// --- rendering -------------------------------------------------------------
function render() {
  listEl.innerHTML = "";

  if (state.parseError) {
    show(`The effects catalogue could not be read: ${state.parseError}`);
  } else if (!state.all.length) {
    // Said out loud rather than shown as an empty list, because this project
    // has already lost a session to a file that was read, rejected, and never
    // mentioned.
    show(
      "No catalogue yet. The plug-in writes one shortly after pieFX arms inside " +
        "After Effects — open AE with pieFX armed, then reopen this window."
    );
  } else if (!state.rows.length) {
    show("Nothing matches.");
  } else {
    noteEl.hidden = true;
  }

  state.rows.forEach((r, i) => {
    const li = document.createElement("li");
    if (r.head) {
      li.className = "head";
      li.textContent = r.head;
    } else {
      li.className = i === state.sel ? "sel" : "";
      const n = document.createElement("span");
      n.className = "name";
      n.textContent = r.name || r.id;

      // What the middle of the row says is per-kind, because what identifies a
      // row is per-kind:
      //   effect    the match name, which IS the identity and is worth showing
      //   preset    a KIND pill - the identity is a long absolute path, which
      //             tells a reader nothing at a glance
      //   command   both. The pill because a command is not an effect and
      //             firing one is not undoable the way applying an effect is,
      //             and the id because the id is the identity AND the thing
      //             that can be wrong in a shipped, hand-tested map.
      const parts = [n];
      if (r.t !== "effect") {
        const k = document.createElement("span");
        k.className = "kind";
        k.textContent = r.t;
        parts.push(k);
      }
      if (r.t !== "preset") {
        const m = document.createElement("span");
        m.className = "match";
        m.textContent = r.t === "command" ? "#" + r.cmdId : r.match;
        parts.push(m);
      }
      // What the parameter will DO, on the row it will do it to. This is the
      // only confirmation available before Enter: the window holds no property
      // list (the catalogue has none), so it cannot promise "Blurriness = 40"
      // for an effect nobody has applied yet - it can only say what it parsed,
      // and let the script say in a toast if that did not land. A row that
      // CANNOT take one says so instead, which is the more important half:
      // typing "precompose 40" and watching the 40 vanish silently is the
      // failure this chip exists to prevent.
      if (state.params.length) {
        const pm = document.createElement("span");
        pm.className = r.t === "effect" ? "param" : "param off";
        pm.textContent =
          r.t === "effect" ? paramLabel(state.params) : "ignores " + paramLabel(state.params);
        parts.push(pm);
      }

      const c = document.createElement("span");
      c.className = "cat";
      c.textContent = r.category || "(internal)";
      parts.push(c);
      li.append(...parts);
      li.addEventListener("mousedown", (ev) => {
        ev.preventDefault(); // keep the focus in the field
        state.sel = i;
        render();
        apply(ev.shiftKey);
      });
    }
    listEl.appendChild(li);
  });

  const shown = state.rows.filter((r) => !r.head).length;
  const effects = state.all.length - state.presets - state.commands;
  // `pool()` is the searchable set; the browsable set is smaller. Counting
  // against the searchable one is right: "60 of 623" is a statement about what
  // typing can reach, which is what the number is for.
  countEl.textContent = state.all.length
    ? `${shown} of ${pool().length} shown · ${effects} effects, ` +
      `${state.presets} presets, ${state.commands} commands` +
      (state.walked && state.claimed && state.walked !== state.claimed
        ? ` · AE claimed ${state.claimed}`
        : "")
    : "";

  // The stack counter. Only on screen once something has been applied without
  // dismissing, because until then it would be a zero explaining nothing.
  stackEl.hidden = state.applied === 0;
  stackEl.textContent = state.applied === 1 ? "1 applied" : `${state.applied} applied`;

  const sel = listEl.querySelector(".sel");
  if (sel) sel.scrollIntoView({ block: "nearest" });
}

function show(msg) {
  noteEl.hidden = false;
  noteEl.textContent = msg;
}

// --- acting ----------------------------------------------------------------
function moveSel(d) {
  let i = state.sel;
  for (let n = 0; n < state.rows.length; n++) {
    i += d;
    if (i < 0 || i >= state.rows.length) return;
    if (!state.rows[i].head) {
      state.sel = i;
      render();
      return;
    }
  }
}

function actionFor(e) {
  // Three kinds, three firing paths, and the difference is not cosmetic: an
  // effect is applied by AEGP from its match name, a preset by the scripting
  // DOM from its file, a command by app.executeCommand from its id. All three
  // reuse a path that already exists rather than inventing a fourth.
  //
  // The command carries NO name, deliberately. `sendFire` prefers a name when
  // it has one, because a name can be resolved against the running AE and an
  // id can only be trusted - but the names in `ae-commands-2025.json` are
  // internal identifiers rather than the display strings findMenuCommandId
  // wants, so sending one would ask AE to resolve a string it has never heard
  // of. The id is all this map can honestly offer.
  if (e.t === "preset") return { kind: "preset", path: e.id };
  if (e.t === "command") return { kind: "ae-command", id: e.cmdId };

  // A parameterised effect is a SEQUENCE - add the effect, then set the
  // property - so it compiles to one snippet and one undo group. Without
  // parameters it stays on the AEGP path it has always used: that path is
  // proven live, it is one message instead of a kilobyte of generated source,
  // and the commonest thing this window does should not start going through a
  // compiler on the day parameters shipped.
  if (state.params.length) {
    return compileAction([{ t: "effect", id: e.id, params: state.params }], "pieFX: " + e.name);
  }
  return { kind: "effect", matchName: e.id };
}

// `stay` is Shift+Enter: fire, and keep the window up with an empty field so
// the next one can be typed straight away. Levels, then Curves, then Glow is
// one summon instead of three, and three summons is three gestures and three
// round trips through the foreground.
//
// It does NOT batch: each one is fired as it is entered, so each is its own
// undo, exactly as if it had been applied and the window reopened. Holding
// them to send together would be a different feature (a macro) and a different
// undo story, and guessing at it here would make Shift+Enter mean something
// the user cannot see.
function apply(stay) {
  const e = state.rows[state.sel];
  if (!e || e.head) return;

  // Compiling can fail - too many steps for one action is the real case - and
  // it fails HERE, before anything crosses the pipe, so the window can say so
  // instead of the toast having to.
  let action;
  try {
    action = actionFor(e);
  } catch (err) {
    show("Could not apply: " + err.message);
    return;
  }

  sendFire(action).then(
    () => {
      rememberUsed(e);
      if (!stay) {
        dismiss();
        return;
      }
      state.applied += 1;
      // Cleared rather than left selected: the next thing in a stack is a
      // different thing, and a field still holding "levels" is a field to
      // empty by hand. Recents have just moved this row to the top, so the
      // empty list is now the stack so far, newest first, which is the right
      // thing to be looking at mid-stack.
      qEl.value = "";
      loadRecents().then(() => {
        buildRows();
        render();
        qEl.focus();
      });
    },
    (err) => show("Could not apply: " + err)
  );
}

// Hidden, not closed: the next summon should be instant, and rebuilding the
// window would also mean asking Windows for the foreground again. The window
// has no title bar and so no close button — Enter, Escape and clicking away
// are the three ways out, and all three land here.
//
// The query is cleared ON THE WAY OUT, not only on the way back in. `freshen()`
// clears it too, but that runs off an event from the Rust side, and a window
// that comes back still holding the last search is a window you have to empty
// by hand before you can use it — which is most of the time you saved by
// keeping it warm. Clearing here means the field is empty the moment it is
// hidden, whatever happens to the event.
function dismiss() {
  qEl.value = "";
  state.applied = 0;
  buildRows();
  render();
  invoke("hide_search").catch(() => {});
}

// --- wiring ----------------------------------------------------------------
qEl.addEventListener("input", () => {
  buildRows();
  render();
});

document.addEventListener("keydown", (ev) => {
  if (ev.key === "Escape") {
    ev.preventDefault();
    dismiss();
  } else if (ev.key === "ArrowDown") {
    ev.preventDefault();
    moveSel(1);
  } else if (ev.key === "ArrowUp") {
    ev.preventDefault();
    moveSel(-1);
  } else if (ev.key === "Enter") {
    ev.preventDefault();
    apply(ev.shiftKey);
  }
});

hiddenEl.addEventListener("change", () => {
  buildRows();
  render();
  qEl.focus();
});

// Re-shown rather than rebuilt, so the field has to be cleared and refocused
// by hand — a window that opens still holding the last search is a window that
// looks broken.
function freshen() {
  qEl.value = "";
  state.applied = 0;
  document.dispatchEvent(new Event("piefx-shown"));
  // loadCommands appends to state.all, so it has to run AFTER loadCatalogue
  // replaces it - a Promise.all over the two would race and drop the commands
  // about half the time, which is the kind of bug that only shows up on a slow
  // disk in front of someone else.
  return loadCatalogue()
    .then(() => Promise.all([loadCommands(), loadRecents()]))
    .then(() => {
    buildRows();
    render();
    qEl.focus();
    qEl.select();
  });
}

if (T && T.event) {
  T.event.listen("piefx-search-shown", freshen).catch(() => {});
}

// Clicking away dismisses it. A window with no title bar that stays up after
// you have gone back to After Effects is litter on top of the comp, and there
// is no close button to get rid of it with.
//
// The guard matters: `freshen()` refocuses the field, and Windows delivers a
// focus-lost for the moment BEFORE a newly shown window takes the foreground.
// Without it the window hides itself on the way in, which looks exactly like a
// release that never opened it.
if (T && T.window && T.window.getCurrentWindow) {
  let shownAt = Date.now();
  const armAfter = 400;
  document.addEventListener("piefx-shown", () => (shownAt = Date.now()));
  T.window
    .getCurrentWindow()
    .onFocusChanged(({ payload: focused }) => {
      if (!focused && Date.now() - shownAt > armAfter) dismiss();
    })
    .catch(() => {});
}

freshen();
