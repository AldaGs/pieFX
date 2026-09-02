// pieFX — the settings window.
//
// The screen IS a wheel. Not a list of dropdowns that describes one: the point
// of a radial menu is that positions become muscle memory, and a configurator
// laid out as a table teaches the wrong shape. So this draws the real thing,
// with the real renderer (hexdraw.js) and the real model (menu.js), and puts an
// inspector beside it.
//
// Three rules it inherits from the format, and enforces:
//   - slots are POSITIONAL and a hole is real. "Clear slot" leaves a null, it
//     never compacts, because compacting would move hexagons the user's hand
//     already knows.
//   - depth is capped at 2. A category inside a category is refused.
//   - a binding is not trustworthy until it has been watched working, so every
//     action has a TEST-FIRE button that goes down the same pipe the gesture
//     uses.

import { DEFAULTS, compile, parseSettings, cloneSettings, settingsError } from "./menu.js";
import { R, bindDraw, slotPosAt } from "./hexdraw.js";
import { sendFire, setInstallDir } from "./actions.js";

const T = window.__TAURI__;
const canvas = document.getElementById("wheel");
const ctx = canvas.getContext("2d");
const D = bindDraw(ctx);

// Bigger than the live wheel — this one is clicked, not flicked, and it is the
// subject of the window rather than an overlay on someone's footage.
const BIG = R * 1.55;
const BIG_SPACING = Math.sqrt(3) * BIG + 14;

// --- state ----------------------------------------------------------------
const state = {
  settings: cloneSettings(DEFAULTS),
  saved: "", // the JSON as it is on disk, for the dirty check
  path: [], // [] at the top ring, [i] inside category i
  sel: -1, // selected slot 0..5 within the current ring, or -1
  hover: -1,
};

function ensure6(a) {
  const out = Array.isArray(a) ? a.slice(0, 6) : [];
  while (out.length < 6) out.push(null);
  return out;
}

// The slot array currently on screen.
function ring() {
  state.settings.wheel.slots = ensure6(state.settings.wheel.slots);
  if (state.path.length === 0) return state.settings.wheel.slots;
  const parent = state.settings.wheel.slots[state.path[0]];
  parent.slots = ensure6(parent.slots);
  return parent.slots;
}

function parentSlot() {
  return state.path.length ? state.settings.wheel.slots[state.path[0]] : null;
}

function selected() {
  return state.sel >= 0 ? ring()[state.sel] : null;
}

function serialise() {
  return JSON.stringify(state.settings, null, 2);
}

function dirty() {
  return serialise() !== state.saved;
}

// --- canvas ---------------------------------------------------------------
function layout() {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { w, h, cx: w / 2, cy: h / 2 };
}

function centre() {
  return { cx: canvas.clientWidth / 2, cy: canvas.clientHeight / 2 };
}

// A hexagon's inradius is r·√3/2; that is the honest hit shape, and close
// enough to the drawn one that a click never lands on the wrong hexagon.
function hitSlot(mx, my, g) {
  for (let i = 0; i < 6; i++) {
    const [x, y] = slotPosAt(g.cx, g.cy, i, BIG_SPACING);
    if (Math.hypot(mx - x, my - y) <= BIG * 0.866) return i;
  }
  return -1;
}

function hitCentre(mx, my, g) {
  return Math.hypot(mx - g.cx, my - g.cy) <= BIG * 0.72;
}

// The empty position. Drawn, not omitted: a hole is part of the layout, and
// seeing it is what makes "slots are positional" obvious rather than a rule in
// a document.
function drawHole(x, y, hot) {
  ctx.save();
  ctx.globalAlpha = hot ? 0.55 : 0.3;
  D.glassHex(x, y, BIG, "dead");
  ctx.restore();

  ctx.save();
  ctx.globalAlpha = hot ? 0.9 : 0.45;
  ctx.strokeStyle = "rgba(235,238,245,0.8)";
  ctx.lineCap = "round";
  ctx.lineWidth = 2;
  const k = 12;
  ctx.beginPath();
  ctx.moveTo(x - k, y);
  ctx.lineTo(x + k, y);
  ctx.moveTo(x, y - k);
  ctx.lineTo(x, y + k);
  ctx.stroke();
  ctx.restore();
}

function selectionRing(x, y) {
  ctx.save();
  D.hexPath(x, y, BIG + 7, 26 * (BIG / R));
  ctx.strokeStyle = "#C74FD6";
  ctx.lineWidth = 2;
  ctx.setLineDash([6, 5]);
  ctx.stroke();
  ctx.restore();
}

function draw() {
  const g = layout();
  ctx.clearRect(0, 0, g.w, g.h);

  const slots = ring();
  const parent = parentSlot();

  // Centre: the way back up when we are inside a category, and the wheel's own
  // inert middle when we are not.
  if (parent) {
    D.drawHex(g.cx, g.cy, compile(parent, null), "hot", BIG * 0.82);
  } else {
    ctx.save();
    ctx.globalAlpha = 0.5;
    D.glassHex(g.cx, g.cy, BIG * 0.82, "dead");
    ctx.restore();
    ctx.font = "600 13px system-ui, 'Segoe UI', sans-serif";
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillStyle = "rgba(228,232,240,0.45)";
    ctx.fillText("cancel", g.cx, g.cy);
  }

  for (let i = 0; i < 6; i++) {
    const [x, y] = slotPosAt(g.cx, g.cy, i, BIG_SPACING);
    const slot = slots[i];
    if (!slot) {
      drawHole(x, y, state.hover === i || state.sel === i);
    } else {
      const node = compile(slot, parent ? parent.requires || null : null);
      const mode = state.sel === i || state.hover === i ? "hot" : "idle";
      D.drawHex(x, y, node, mode, BIG);
    }
    if (state.sel === i) selectionRing(x, y);
  }

  requestAnimationFrame(draw);
}
requestAnimationFrame(draw);

// --- canvas input ---------------------------------------------------------
function mouse(e) {
  const r = canvas.getBoundingClientRect();
  return [e.clientX - r.left, e.clientY - r.top];
}

canvas.addEventListener("mousemove", (e) => {
  const [mx, my] = mouse(e);
  const g = centre();
  state.hover = hitSlot(mx, my, g);
  canvas.style.cursor =
    state.hover >= 0 || (state.path.length && hitCentre(mx, my, g)) ? "pointer" : "default";
});
canvas.addEventListener("mouseleave", () => {
  state.hover = -1;
});

canvas.addEventListener("click", (e) => {
  const [mx, my] = mouse(e);
  const g = centre();
  if (hitCentre(mx, my, g)) {
    goUp();
    return;
  }
  state.sel = hitSlot(mx, my, g);
  syncInspector();
});

canvas.addEventListener("dblclick", (e) => {
  const [mx, my] = mouse(e);
  const i = hitSlot(mx, my, centre());
  if (i < 0) return;
  const slot = ring()[i];
  if (slot && slot.slots && state.path.length === 0) drillInto(i);
});

function drillInto(i) {
  state.path = [i];
  state.sel = -1;
  syncInspector();
  syncCrumbs();
}

function goUp() {
  if (!state.path.length) return;
  const was = state.path[0];
  state.path = [];
  state.sel = was;
  syncInspector();
  syncCrumbs();
}

// --- the AE command map, as naming help only ------------------------------
// The names in it are internal identifiers, NOT the display strings
// findMenuCommandId wants, so this cannot validate a binding — it can only
// remind you what an id was called. Test-fire is the validation.
fetch("ae-commands-2025.json")
  .then((r) => r.json())
  .then((map) => {
    const dl = document.getElementById("cmdnames");
    const seen = new Set();
    for (const id of Object.keys(map)) {
      if (Number(id) < 0) continue; // negative ids are effects; use the effect kind
      const n = map[id];
      if (!n || seen.has(n)) continue;
      seen.add(n);
      const o = document.createElement("option");
      o.value = n;
      dl.appendChild(o);
    }
  })
  .catch(() => {
    /* no map: the field is still free text, which is what actually binds */
  });

// --- inspector ------------------------------------------------------------
const el = (id) => document.getElementById(id);
const KINDS = ["ae-command", "script-snippet", "script-file", "effect", "builtin"];
const DIRNAMES = ["N", "NE", "SE", "S", "SW", "NW"];

function kindOf(slot) {
  return slot && slot.action ? slot.action.kind : "";
}

function syncCrumbs() {
  const c = el("crumbs");
  c.innerHTML = "";
  const parent = parentSlot();
  if (!parent) {
    const here = document.createElement("span");
    here.className = "here";
    here.textContent = "Wheel";
    c.appendChild(here);
    return;
  }
  const b = document.createElement("button");
  b.textContent = "◂ Wheel";
  b.onclick = goUp;
  c.appendChild(b);
  const sep = document.createElement("span");
  sep.className = "sep";
  sep.textContent = "▸";
  c.appendChild(sep);
  const here = document.createElement("span");
  here.className = "here";
  here.textContent = parent.label || "(unnamed)";
  c.appendChild(here);
}

function syncInspector() {
  const slot = selected();
  el("empty").hidden = state.sel >= 0;
  el("form").hidden = state.sel < 0;
  el("dirty").hidden = !dirty();
  el("testresult").textContent = "";
  el("testresult").className = "testresult";
  if (state.sel < 0) return;

  const dirName = DIRNAMES[state.sel];
  el("slotTitle").textContent = slot
    ? `${slot.label || "(unnamed)"} — ${dirName}`
    : `Empty — ${dirName}`;

  el("f_label").value = slot ? slot.label || "" : "";
  el("f_requires").value = !slot
    ? "inherit"
    : !("requires" in slot)
    ? "inherit"
    : slot.requires === null
    ? "none"
    : slot.requires;

  const kind = kindOf(slot);
  el("f_kind").value = kind;
  for (const k of KINDS) el("k_" + k).hidden = k !== kind;

  const a = (slot && slot.action) || {};
  el("f_cmd_name").value = a.name && kind === "ae-command" ? a.name : "";
  el("f_cmd_id").value = a.id === undefined ? "" : a.id;
  el("f_code").value = a.code || "";
  el("f_needs_global").value = (a.needs && a.needs.global) || "";
  el("f_needs_file").value = (a.needs && a.needs.file) || "";
  el("f_path").value = a.path || "";
  el("f_match").value = a.matchName || "";
  el("f_builtin").value = kind === "builtin" ? a.name || "anchor-grid" : "anchor-grid";
  el("f_cell").value = a.cell === undefined ? "" : a.cell;
  // A cell is a position in the anchor GRID. The effect search has no grid, so
  // offering the field there invites a value that means nothing.
  const gridded = kind === "builtin" && el("f_builtin").value === "anchor-grid";
  el("cellfield").hidden = !gridded;
  el("cellnote").hidden = !gridded;

  const isRing = !!(slot && slot.slots);
  const canRing = state.path.length === 0; // depth is capped at 2
  el("makering").hidden = isRing || !canRing || !slot;
  el("openring").hidden = !isRing;
  el("ringnote").hidden = !isRing;
  el("clear").disabled = !slot;
  el("testfire").disabled = !(slot && slot.action);
}

// Every edit writes straight into the settings object and redraws. There is no
// separate "apply": the wheel on the left IS the preview, and a preview you
// have to ask for is one nobody looks at.
function edit(fn) {
  if (state.sel < 0) return;
  const slots = ring();
  if (!slots[state.sel]) slots[state.sel] = { label: "New" };
  fn(slots[state.sel], slots);
  syncInspector();
  syncCrumbs();
}

el("f_label").addEventListener("input", (e) => {
  const v = e.target.value;
  edit((s) => {
    s.label = v;
  });
});

el("f_requires").addEventListener("change", (e) => {
  const v = e.target.value;
  edit((s) => {
    if (v === "inherit") delete s.requires;
    else if (v === "none") s.requires = null;
    else s.requires = v;
  });
});

el("f_kind").addEventListener("change", (e) => {
  const k = e.target.value;
  edit((s) => {
    if (!k) {
      delete s.action;
      return;
    }
    // Keep whatever the old action had that the new kind also uses, so
    // switching kinds by accident does not silently eat a snippet.
    const old = s.action || {};
    s.action = { kind: k };
    if (k === "ae-command") {
      if (old.name) s.action.name = old.name;
      if (old.id) s.action.id = old.id;
    } else if (k === "script-snippet") {
      s.action.code = old.code || "";
      if (old.needs) s.action.needs = old.needs;
    } else if (k === "script-file") {
      s.action.path = old.path || "";
    } else if (k === "effect") {
      s.action.matchName = old.matchName || "";
    } else if (k === "builtin") {
      s.action.name = "anchor-grid";
      s.action.cell = 4;
    }
  });
});

function bindAction(id, apply) {
  el(id).addEventListener("input", (e) => {
    const v = e.target.value;
    edit((s) => {
      if (!s.action) return;
      apply(s.action, v);
    });
  });
}

bindAction("f_cmd_name", (a, v) => {
  if (v) a.name = v;
  else delete a.name;
});
bindAction("f_cmd_id", (a, v) => {
  const n = parseInt(v, 10);
  if (Number.isFinite(n)) a.id = n;
  else delete a.id;
});
bindAction("f_code", (a, v) => {
  a.code = v;
});
bindAction("f_path", (a, v) => {
  a.path = v;
});
bindAction("f_match", (a, v) => {
  a.matchName = v;
});
bindAction("f_cell", (a, v) => {
  const n = parseInt(v, 10);
  a.cell = Number.isFinite(n) ? n : -1;
});
el("f_builtin").addEventListener("change", (e) => {
  const v = e.target.value;
  edit((s) => {
    if (!s.action) return;
    s.action.name = v;
    if (v === "anchor-grid") {
      if (s.action.cell === undefined) s.action.cell = 4;
    } else {
      delete s.action.cell;
    }
  });
});

// `needs` is part of the snippet form, not an advanced option, because a
// snippet WITHOUT it is the flaky case — the one that fails on the first flick
// of every session and works on the second.
function bindNeeds(id, key) {
  el(id).addEventListener("input", (e) => {
    const v = e.target.value.trim();
    edit((s) => {
      if (!s.action) return;
      const n = s.action.needs || {};
      if (v) n[key] = v;
      else delete n[key];
      if (n.global || n.file) s.action.needs = n;
      else delete s.action.needs;
    });
  });
}
bindNeeds("f_needs_global", "global");
bindNeeds("f_needs_file", "file");

el("makering").addEventListener("click", () => {
  edit((s) => {
    if (!s.slots) s.slots = ensure6(null);
  });
});

el("openring").addEventListener("click", () => {
  if (state.path.length === 0 && state.sel >= 0) drillInto(state.sel);
});

// A hole, not a gap. The array is never compacted — see the header.
el("clear").addEventListener("click", () => {
  if (state.sel < 0) return;
  const slots = ring();
  const s = slots[state.sel];
  if (!s) return;
  const kids = s.slots ? s.slots.filter(Boolean).length : 0;
  if (kids && !confirm(`"${s.label}" holds ${kids} items. Clear all of it?`)) return;
  slots[state.sel] = null;
  syncInspector();
});

// --- test-fire ------------------------------------------------------------
// The whole reason this button exists: AE menu ids are only as good as one
// developer's testing on one version, findMenuCommandId proves a name exists
// but not that it is the one you meant, and the command map has already been
// wrong three times. Watching it work is the only real validation.
el("testfire").addEventListener("click", () => {
  const slot = selected();
  const out = el("testresult");
  if (!slot || !slot.action) return;
  out.className = "testresult";
  out.textContent = "firing…";
  sendFire(slot.action, slot.action.cell).then(
    () => {
      out.className = "testresult ok";
      out.textContent = "sent — check After Effects";
    },
    (e) => {
      out.className = "testresult bad";
      out.textContent = "failed: " + e;
    }
  );
});

// --- global settings ------------------------------------------------------
el("g_hold").addEventListener("input", (e) => {
  const n = parseInt(e.target.value, 10);
  if (Number.isFinite(n)) state.settings.gesture.holdMs = n;
  el("dirty").hidden = !dirty();
});
el("g_armmode").addEventListener("change", (e) => {
  state.settings.gesture.armMode = e.target.value;
  el("dirty").hidden = !dirty();
});
el("g_autoarm").addEventListener("change", (e) => {
  state.settings.gesture.armOnLaunch = e.target.checked;
  el("dirty").hidden = !dirty();
});

function syncGlobals() {
  const g = state.settings.gesture || {};
  el("g_hold").value = g.holdMs === undefined ? 200 : g.holdMs;
  el("g_armmode").value = g.armMode || "center";
  el("g_autoarm").checked = g.armOnLaunch !== false;
}

// --- load / save ----------------------------------------------------------
function adopt(text) {
  state.settings = parseSettings(text);
  const bad = settingsError();
  state.settings.wheel.slots = ensure6(state.settings.wheel.slots);
  // Deliberately NOT marked as saved when the file was rejected: leaving it
  // dirty means the Save button is live, and saving is exactly what repairs a
  // file the wheel cannot read.
  state.saved = bad ? "" : serialise();
  state.path = [];
  state.sel = -1;
  syncGlobals();
  syncCrumbs();
  syncInspector();
  if (bad) {
    el("where").textContent =
      "settings file unreadable (" + bad + ") - showing defaults; Save overwrites it";
  }
}

el("save").addEventListener("click", () => {
  const json = serialise();
  if (!T) {
    el("where").textContent = "preview — nothing to save to";
    return;
  }
  // The Rust side broadcasts after writing, so the LIVE wheel reloads on the
  // next flick. Without that a rebound hexagon would keep firing the old
  // action, which reads as "settings do not work".
  T.core.invoke("save_settings", { json }).then(
    () => {
      state.saved = json;
      el("dirty").hidden = true;
      el("where").textContent = "saved — the wheel is using it now";
    },
    (e) => {
      el("where").textContent = "save failed: " + e;
    }
  );
});

el("revert").addEventListener("click", () => {
  if (dirty() && !confirm("Discard your unsaved changes?")) return;
  if (T) T.core.invoke("load_settings").then(adopt);
  else adopt("");
});

el("reset").addEventListener("click", () => {
  if (!confirm("Replace the whole wheel with the shipped defaults?")) return;
  state.settings = cloneSettings(DEFAULTS);
  state.path = [];
  state.sel = -1;
  syncGlobals();
  syncCrumbs();
  syncInspector();
});

window.addEventListener("beforeunload", (e) => {
  if (dirty()) {
    e.preventDefault();
    e.returnValue = "";
  }
});

if (T) {
  T.core.invoke("overlay_dir").then(setInstallDir).catch(() => {});
  T.core.invoke("load_settings").then(adopt, () => adopt(""));
  el("where").textContent = "%APPDATA%\\pieFX\\settings.json";
} else {
  // Browser preview: no plug-in, no file. The wheel still edits, and test-fire
  // reports honestly that there is nothing to fire into.
  adopt("");
  el("where").textContent = "preview — no After Effects connected";
}
