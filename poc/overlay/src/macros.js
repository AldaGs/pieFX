// pieFX — macros: a named list of steps, recorded from a stack.
//
// A macro is the n-step case of the sequence the compiler already builds for a
// parameterised effect, so there is no new fire path here and no new executor:
// `compile()` turns the steps into one snippet inside one undo group, and one
// Ctrl+Z takes the whole macro back. This file is only the STORAGE and the
// SHAPE.
//
// WHERE THEY COME FROM. Not a JSON file someone hand-writes and not an editor
// nobody asked for: a macro is recorded from a stack. Shift+Enter already
// applies without dismissing, so by the time three things are on the layer the
// user has ALREADY performed the macro — naming it is the only thing left, and
// the steps are known to work because they just ran. An authoring UI would have
// to be discovered, learnt and kept correct; this costs one keystroke and can
// only produce macros that are real workflows.
//
// WHY A THIRD FILE. `settings.json` is written WHOLE by the settings window, so
// a second writer would be a lost update. `recents.json` is churn — rewritten
// on every application, capped at eight, and worth nothing if lost. A macro is
// something a person deliberately made and expects to find next year, so it
// does not share a file with the thing that is rewritten every time an effect
// is applied.

const T = window.__TAURI__;

function invoke(cmd, args) {
  if (!(T && T.core)) return Promise.reject(new Error("not running under Tauri"));
  return T.core.invoke(cmd, args || {});
}

// A step is exactly what the compiler takes, and NOTHING ELSE is let through.
// A macros file is a file on disk: it can be hand-edited, copied between
// machines, or written by a version of pieFX that is not this one. Validating
// on READ means a bad entry is dropped at the door rather than becoming a
// generated-source surprise much later.
function cleanStep(s) {
  if (!s || typeof s !== "object") return null;
  if (s.t === "command") {
    const id = Number(s.cmdId);
    return Number.isFinite(id) && id >= 0 ? { t: "command", cmdId: id } : null;
  }
  if (typeof s.id !== "string" || !s.id) return null;
  if (s.t === "preset") return { t: "preset", id: s.id };
  if (s.t === "effect") {
    const params = (Array.isArray(s.params) ? s.params : [])
      .map((p) => {
        const v = Number(p && p.value);
        if (!Number.isFinite(v)) return null;
        return p.name ? { name: String(p.name).toLowerCase(), value: v } : { value: v };
      })
      .filter(Boolean);
    return { t: "effect", id: s.id, params };
  }
  return null; // including "builtin", which cannot go in a snippet at all
}

function cleanMacro(m) {
  if (!m || typeof m.name !== "string" || !m.name.trim()) return null;
  const steps = (Array.isArray(m.steps) ? m.steps : []).map(cleanStep).filter(Boolean);
  if (!steps.length) return null;
  return { name: m.name.trim(), steps };
}

export function loadMacros() {
  return invoke("load_macros").then(
    (txt) => {
      try {
        const a = JSON.parse((txt || "[]").replace(/^﻿/, ""));
        return (Array.isArray(a) ? a : []).map(cleanMacro).filter(Boolean);
      } catch (e) {
        // A macros file that will not parse is NOT an empty macros list, and
        // the difference matters enough to say out loud rather than silently
        // start the user again from nothing. The caller surfaces it.
        throw new Error("the macros file could not be read: " + e);
      }
    },
    () => []
  );
}

export function saveMacros(list) {
  return invoke("save_macros", { json: JSON.stringify(list) });
}

// The step a just-applied row becomes. It is derived from the row rather than
// from the fired action, because an action is already compiled and a step has
// to stay editable and re-compilable.
export function stepFor(entry, params) {
  if (entry.t === "command") return { t: "command", cmdId: entry.cmdId };
  if (entry.t === "preset") return { t: "preset", id: entry.id };
  if (entry.t === "effect") return { t: "effect", id: entry.id, params: params || [] };
  return null; // a macro inside a macro: see search.js, where it is flattened
}
