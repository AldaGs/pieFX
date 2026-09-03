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

import { sendFire } from "./actions.js";

const T = window.__TAURI__;
const qEl = document.getElementById("q");
const listEl = document.getElementById("list");
const noteEl = document.getElementById("note");
const countEl = document.getElementById("count");
const hiddenEl = document.getElementById("showHidden");

const MAX_ROWS = 60;
const MAX_RECENTS = 8;

const state = {
  all: [], // every entry the plug-in walked
  recents: [], // match names, most recent first
  rows: [], // what is on screen: {head} separators and effect entries
  sel: 0,
  walked: 0,
  claimed: 0,
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

function usable(e) {
  return e && e.match && !isObsolete(e) && !isInternal(e);
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
    state.all = Array.isArray(doc.effects) ? doc.effects : [];
    state.walked = doc.walked | 0;
    state.claimed = doc.claimed | 0;
  });
}

function loadRecents() {
  return invoke("load_recents").then(
    (txt) => {
      try {
        const a = JSON.parse((txt || "[]").replace(/^﻿/, ""));
        state.recents = Array.isArray(a) ? a.filter((s) => typeof s === "string") : [];
      } catch (e) {
        state.recents = [];
      }
    },
    () => {
      state.recents = [];
    }
  );
}

function rememberUsed(match) {
  state.recents = [match].concat(state.recents.filter((m) => m !== match)).slice(0, MAX_RECENTS);
  // Best effort. A recents file that cannot be written must not stop an effect
  // from being applied — the applying is the feature.
  invoke("save_recents", { json: JSON.stringify(state.recents) }).catch(() => {});
}

// --- matching --------------------------------------------------------------
// Substring, case-insensitive, on the display name first and the match name
// second, so that typing "ADBE Gauss" finds what typing "gaussian" does. A
// name that STARTS with the query outranks one that merely contains it, which
// is what makes "blur" put "Blur (Fast)" above "Bilateral Blur".
function score(e, q) {
  const n = (e.name || "").toLowerCase();
  const m = (e.match || "").toLowerCase();
  if (n === q) return 0;
  if (n.startsWith(q)) return 1;
  if (n.includes(q)) return 2;
  if (m.includes(q)) return 3;
  return -1;
}

function pool() {
  return hiddenEl.checked ? state.all.filter((e) => e && e.match) : state.all.filter(usable);
}

function byMatch(m) {
  return state.all.find((e) => e.match === m);
}

function buildRows() {
  const q = qEl.value.trim().toLowerCase();
  const rows = [];

  if (!q) {
    // No query: recents ARE the answer. This is the zero-typing common case,
    // and it is why the window is bearable at all.
    const rec = state.recents.map(byMatch).filter(Boolean);
    if (rec.length) {
      rows.push({ head: "Recent" });
      rec.forEach((e) => rows.push(e));
      rows.push({ head: "All effects" });
    }
    pool()
      .slice()
      .sort((a, b) => (a.name || "").localeCompare(b.name || ""))
      .slice(0, MAX_ROWS)
      .forEach((e) => rows.push(e));
  } else {
    pool()
      .map((e) => ({ e, s: score(e, q) }))
      .filter((r) => r.s >= 0)
      .sort((a, b) => a.s - b.s || (a.e.name || "").localeCompare(b.e.name || ""))
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
      "No effects catalogue yet. The plug-in writes one shortly after pieFX arms " +
        "inside After Effects — open AE with pieFX armed, then reopen this window."
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
      n.textContent = r.name || r.match;
      const m = document.createElement("span");
      m.className = "match";
      m.textContent = r.match;
      const c = document.createElement("span");
      c.className = "cat";
      c.textContent = r.category || "(internal)";
      li.append(n, m, c);
      li.addEventListener("mousedown", (ev) => {
        ev.preventDefault(); // keep the focus in the field
        state.sel = i;
        render();
        apply();
      });
    }
    listEl.appendChild(li);
  });

  const shown = state.rows.filter((r) => !r.head).length;
  countEl.textContent = state.all.length
    ? `${shown} of ${pool().length} shown · ${state.all.length} installed` +
      (state.walked && state.claimed && state.walked !== state.claimed
        ? ` · AE claimed ${state.claimed}`
        : "")
    : "";

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

function apply() {
  const e = state.rows[state.sel];
  if (!e || e.head) return;
  // The match name is the identity and it goes over the pipe base64-encoded,
  // exactly as a bound `effect` slot does — this reuses the firing path that
  // has already been watched putting Gaussian Blur on a layer.
  sendFire({ kind: "effect", matchName: e.match }).then(
    () => {
      rememberUsed(e.match);
      dismiss();
    },
    (err) => show("Could not apply: " + err)
  );
}

function dismiss() {
  // Hidden, not closed: the next summon should be instant, and rebuilding the
  // window would also mean asking Windows for the foreground again.
  if (T && T.window && T.window.getCurrentWindow) T.window.getCurrentWindow().hide();
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
    apply();
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
  return Promise.all([loadCatalogue(), loadRecents()]).then(() => {
    buildRows();
    render();
    qEl.focus();
    qEl.select();
  });
}

if (T && T.event) {
  T.event.listen("piefx-search-shown", freshen).catch(() => {});
}

freshen();
