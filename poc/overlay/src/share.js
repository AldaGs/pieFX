// pieFX — the share format: backups, and single buttons or categories that
// travel between machines.
//
// Why a wrapper rather than "just save the JSON":
//
//   - A settings.json and an exported button are both objects with a `label`
//     in them somewhere, and telling them apart by shape is guesswork that
//     fails on the interesting cases. The envelope says which it is.
//   - A file from a stranger's machine is UNTRUSTED INPUT, and the settings
//     window is the one place a bad tree can be adopted without anyone
//     noticing until the next flick. Everything that comes in goes through
//     sanitise, which keeps the keys this build knows and drops the rest.
//   - Depth is capped at 2 by the wheel, and nothing else enforces it on
//     import. A category exported from the top ring, dropped into a slot that
//     is already inside a category, would be depth 3 — a tree the renderer
//     draws and the gesture cannot reach. That is refused by name, not
//     silently flattened, because flattening loses bindings.
//
// The globals travel with a BACKUP and never with a shared button. Adopting a
// colleague's snippet should not adopt their hold time.

import { cloneSettings, parseSettings, settingsError } from "./menu.js";

export const SHARE_FORMAT = 1;

// The action kinds the settings window offers, and the fields each one owns.
// Anything not listed is dropped on import — including fields a FUTURE build
// might add, which is the honest trade: a file from a newer pieFX loses what
// this one cannot honour, rather than carrying it into a wheel that will not
// act on it.
const ACTION_FIELDS = {
  "ae-command": ["name", "id"],
  "script-snippet": ["code", "needs"],
  "script-file": ["path"],
  effect: ["matchName"],
  preset: ["path"],
  builtin: ["name", "cell"],
};

function str(v) {
  return typeof v === "string" ? v : undefined;
}

function sanitiseAction(raw) {
  if (!raw || typeof raw !== "object") return undefined;
  const kind = str(raw.kind);
  if (!kind || !ACTION_FIELDS[kind]) return undefined;
  const out = { kind };
  for (const k of ACTION_FIELDS[kind]) {
    const v = raw[k];
    if (v === undefined || v === null) continue;
    if (k === "id" || k === "cell") {
      const n = parseInt(v, 10);
      if (Number.isFinite(n)) out[k] = n;
    } else if (k === "needs") {
      // `needs` is the load-on-demand pair, and only those two strings.
      const n = {};
      if (str(v.global)) n.global = v.global;
      if (str(v.file)) n.file = v.file;
      if (n.global || n.file) out.needs = n;
    } else if (str(v) !== undefined) {
      out[k] = v;
    }
  }
  return out;
}

// A hole is a hole. `null` survives sanitising unchanged, because a share file
// that compacted its empty positions would move every hexagon after them.
function sanitiseSlot(raw, depth) {
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) return null;
  const out = {};
  out.label = str(raw.label) || "Imported";
  if ("requires" in raw) {
    // null means "needs nothing", which is a real value and not an absence.
    if (raw.requires === null) out.requires = null;
    else if (str(raw.requires)) out.requires = raw.requires;
  }
  const a = sanitiseAction(raw.action);
  if (a) out.action = a;
  if (str(raw.accent)) out.accent = raw.accent;
  if (str(raw.widget)) out.widget = raw.widget;
  if (Array.isArray(raw.slots)) {
    if (depth >= 1) throw new Error("that file nests categories deeper than the wheel goes");
    const kids = raw.slots.slice(0, 6).map((c) => sanitiseSlot(c, depth + 1));
    while (kids.length < 6) kids.push(null);
    out.slots = kids;
  }
  return out;
}

export function isCategory(slot) {
  return !!(slot && Array.isArray(slot.slots));
}

function stamp() {
  return new Date().toISOString();
}

// --- packing --------------------------------------------------------------

export function packSettings(settings) {
  return {
    piefx: "settings",
    format: SHARE_FORMAT,
    exported: stamp(),
    settings: cloneSettings(settings),
  };
}

// One hexagon, or one category and its six. No globals — see the header.
export function packSlot(slot) {
  return {
    piefx: "slot",
    format: SHARE_FORMAT,
    exported: stamp(),
    kind: isCategory(slot) ? "category" : "button",
    label: slot.label || "",
    slot: sanitiseSlot(slot, 0),
  };
}

export function serialise(obj) {
  return JSON.stringify(obj, null, 2) + "\n";
}

// A filename someone can read in a Downloads folder six months later.
export function shareFileName(kind, label) {
  const safe = String(label || "")
    .replace(/[^A-Za-z0-9 _-]+/g, "")
    .trim()
    .replace(/\s+/g, "-")
    .slice(0, 40);
  const day = stamp().slice(0, 10);
  if (kind === "settings") return `pieFX-backup-${day}.piefx.json`;
  return `pieFX-${safe || kind}.piefx.json`;
}

// --- unpacking ------------------------------------------------------------
//
// Returns { type: "settings", settings } or { type: "slot", slot, category }.
// Throws an Error whose message is shown to the user verbatim, so every throw
// here is worded as something a person can act on.
export function unpack(text) {
  const clean = String(text || "").replace(/^\uFEFF/, "").trim();
  if (!clean) throw new Error("the file is empty");

  let raw;
  try {
    raw = JSON.parse(clean);
  } catch (e) {
    throw new Error("not valid JSON (" + ((e && e.message) || e) + ")");
  }
  if (!raw || typeof raw !== "object") throw new Error("not a pieFX file");

  // A format from the future is refused rather than guessed at. The envelope
  // exists so this can be said plainly instead of failing halfway through an
  // import that has already replaced something.
  if (raw.format !== undefined && Number(raw.format) > SHARE_FORMAT) {
    throw new Error("made by a newer pieFX (format " + raw.format + ")");
  }

  if (raw.piefx === "slot" || (raw.slot && typeof raw.slot === "object")) {
    const slot = sanitiseSlot(raw.slot, 0);
    if (!slot) throw new Error("that file has no hexagon in it");
    return { type: "slot", slot, category: isCategory(slot) };
  }

  // A backup, or — deliberately — a plain settings.json copied off another
  // machine. Both restore the same way, and refusing the second would mean
  // telling users their own settings file is not a pieFX file.
  const body = raw.piefx === "settings" && raw.settings ? raw.settings : raw;
  if (body && body.wheel && Array.isArray(body.wheel.slots)) {
    // Through the SAME parser the wheel uses, so a restored file cannot be
    // one the wheel would reject — and its coercions (arm mode, hold time)
    // apply here exactly as they do on load.
    const settings = parseSettings(JSON.stringify(body));
    const bad = settingsError();
    if (bad) throw new Error("the wheel could not read it (" + bad + ")");
    const slots = settings.wheel.slots.slice(0, 6).map((c) => sanitiseSlot(c, 0));
    while (slots.length < 6) slots.push(null);
    settings.wheel.slots = slots;
    return { type: "settings", settings };
  }

  // A bare slot object, pasted or hand-written. Accepted last, so it can never
  // shadow one of the two real shapes above.
  if (str(raw.label) && (raw.action || Array.isArray(raw.slots))) {
    return { type: "slot", slot: sanitiseSlot(raw, 0), category: isCategory(raw) };
  }

  throw new Error("not a pieFX backup or button");
}
