// pieFX — hexagon wheel prototype (Behavior B: drill-down).
//
// Purpose: FEEL THE STROKE. The whole bet of the design is that a second flick
// inside one unbroken press is nearly free. Runs standalone in a browser with a
// real right-press, and also accepts the plug-in's pipe messages.
//
// Geometry: flat-top hexagons (flat edge top and bottom, vertices left/right),
// which is what makes a vertical stack of three share horizontal edges — the
// arrangement in the mockups. The six neighbours sit at N, NE, SE, S, SW, NW.
//
// Distance does not matter, only direction: past the dead zone a sector stays
// highlighted however far out the cursor goes, so a fast flick that overshoots
// still lands.

import { compileMenu, parseSettings, settingsError } from "./menu.js";
import { R, SPACING, DEAD, DIRS, bindDraw } from "./hexdraw.js";
import { sendFire, setInstallDir } from "./actions.js";

const canvas = document.getElementById("wheel");
const ctx = canvas.getContext("2d");

const D = bindDraw(ctx);
const {
  C, noise, roundRect, glassHex, drawHex, glassPanel,
} = D;

// The settings file is the source of truth; DEFAULTS is only the fallback for a
// machine that has never opened the settings window. Loaded asynchronously at
// startup and RE-loaded whenever the settings window saves, so a change is live
// on the very next flick without restarting AE.
let SETTINGS = parseSettings("");
let MENU = compileMenu(SETTINGS);

function applySettings(text) {
  SETTINGS = parseSettings(text);
  const bad = settingsError();
  if (bad) {
    // Loud, because the alternative is a wheel that quietly is not the one the
    // user configured.
    if (window.__PIEFX_SAY__) window.__PIEFX_SAY__("settings REJECTED: " + bad);
    toast("error", "settings file unreadable - using defaults");
  }
  MENU = compileMenu(SETTINGS);
  if (SETTINGS.gesture && SETTINGS.gesture.armMode) S.armMode = SETTINGS.gesture.armMode;
  // A summon in flight keeps the tree it started with: swapping the menu out
  // from under a press would move hexagons while the cursor is travelling.
  if (!S.visible) S.node = MENU;
}


// --- AE command map -------------------------------------------------------
// id -> name, for the settings picker. NAMING AID ONLY: the id stays the key
// (see SETTINGS.md). Two caveats the file itself carries:
//   - it is version-stamped (2025), and ids are mostly but not provably stable
//     across AE versions, so a binding wants a test-fire before it is trusted;
//   - the names are internal identifiers ("AddtoRenderQueue"), NOT AE's display
//     strings, so app.findMenuCommandId cannot validate them.
// Negative ids are effects; prefer the `effect` action kind for those, since
// apply-by-match-name is the documented path and does not depend on menus.
const AE_COMMANDS = {};

async function loadCommandMap() {
  try {
    const r = await fetch("ae-commands-2025.json");
    Object.assign(AE_COMMANDS, await r.json());
  } catch (_) {
    /* picker falls back to raw ids */
  }
}
loadCommandMap();

function commandName(id) {
  return AE_COMMANDS[String(id)] || `#${id}`;
}
window.pieFXCommands = { AE_COMMANDS, commandName };

// --- state ----------------------------------------------------------------
const S = {
  visible: false,
  cx: 0,
  cy: 0,
  px: 0,
  py: 0,
  node: MENU,
  parent: null,
  level: 1,
  armed: true,
  entrySector: -1,
  hot: -1,
  // "center" is the shipping default: level 2 stays inert until the cursor
  // passes back through the middle, which doubles as the cancel gesture. "exit"
  // is faster but needs a leave-and-return to pick the child lying in the
  // parent's own direction. Exposed as a user setting, not a constant.
  armMode: "center",
  lastFired: "",
  t0: 0,
};

// --- context gating -------------------------------------------------------
// What AE looked like at the moment of the summon, as reported by the plug-in.
// Defaults are permissive: standalone in a browser there is no AE to ask, and
// a wheel that greys everything teaches nothing.
const CTX = { hasSelection: true, hasComp: true, layerCount: 0 };

function ctxOk(requires) {
  if (!requires) return true;
  if (requires === "selection") return CTX.hasSelection;
  if (requires === "comp") return CTX.hasComp;
  return true; // unknown requirement: never silently disable a binding
}

// Can this node's OWN action fire? (For a category that is its default action,
// the one fired by flicking to it and releasing without drilling in.)
function canFire(node) {
  return !!(node && node.action && ctxOk(node.requires));
}

// Is the hexagon live at all? A category stays live while anything inside it
// is — greying a whole ring because its default cannot apply would hide the
// children that still can.
function isLive(node) {
  if (!node) return false;
  if (canFire(node)) return true;
  if (node.kind === "ring") return (node.children || []).some(isLive);
  if (node.kind === "widget") return false;
  return false;
}



function slotPos(i) {
  const a = (DIRS[i] * Math.PI) / 180;
  return [S.cx + SPACING * Math.cos(a), S.cy + SPACING * Math.sin(a)];
}

function sectorFor(px, py) {
  const dx = px - S.cx;
  const dy = py - S.cy;
  if (Math.hypot(dx, dy) < DEAD) return -1;
  const deg = (Math.atan2(dy, dx) * 180) / Math.PI;
  return ((Math.floor((deg + 120) / 60) % 6) + 6) % 6;
}


// --- anchor widget --------------------------------------------------------
// Corner radius kept in the same proportion as the hexagons' 20/54, so the two
// level-2 surfaces read as the same material.
const A_CORNER = 18;
const A_CELL = 56;
const A_GAP = 7;
const A_SPAN = 3 * A_CELL + 2 * A_GAP;

function anchorCellRect(i) {
  const row = (i / 3) | 0;
  const col = i % 3;
  return {
    x: S.cx - A_SPAN / 2 + col * (A_CELL + A_GAP),
    y: S.cy - A_SPAN / 2 + row * (A_CELL + A_GAP),
    w: A_CELL,
    h: A_CELL,
    row,
    col,
  };
}

function anchorCellFor(px, py) {
  for (let i = 0; i < 9; i++) {
    const r = anchorCellRect(i);
    if (px >= r.x && px <= r.x + r.w && py >= r.y && py <= r.y + r.h) return i;
  }
  return -1;
}

function drawAnchorWidget() {
  for (let i = 0; i < 9; i++) {
    const r = anchorCellRect(i);
    const hot = S.hot === i;
    const dead = !S.armed || !canFire(S.parent);

    ctx.save();
    ctx.shadowColor = hot ? "rgba(199,79,214,0.45)" : "rgba(0,0,0,0.55)";
    ctx.shadowBlur = hot ? 28 : 14;
    ctx.shadowOffsetY = 3;
    roundRect(r.x, r.y, r.w, r.h, A_CORNER);
    const g = ctx.createLinearGradient(r.x, r.y, r.x, r.y + r.h);
    g.addColorStop(0, dead ? C.deadTop : hot ? C.hotTop : C.glassTop);
    g.addColorStop(1, dead ? C.deadBot : hot ? C.hotBot : C.glassBot);
    ctx.fillStyle = g;
    ctx.fill();
    ctx.restore();

    ctx.save();
    roundRect(r.x, r.y, r.w, r.h, A_CORNER);
    ctx.clip();
    ctx.globalAlpha = 0.06;
    ctx.fillStyle = noise;
    ctx.fillRect(r.x, r.y, r.w, r.h);
    ctx.restore();

    ctx.save();
    roundRect(r.x, r.y, r.w, r.h, A_CORNER);
    const rim = ctx.createLinearGradient(r.x, r.y, r.x, r.y + r.h);
    rim.addColorStop(0, "rgba(255,255,255,0.38)");
    rim.addColorStop(1, "rgba(255,255,255,0.04)");
    ctx.strokeStyle = hot ? C.accent : rim;
    ctx.lineWidth = hot ? 2 : 1.3;
    ctx.stroke();
    ctx.restore();

    const dx = r.x + (r.col / 2) * r.w;
    const dy = r.y + (r.row / 2) * r.h;
    ctx.beginPath();
    ctx.arc(dx, dy, hot ? 5 : 3.4, 0, Math.PI * 2);
    ctx.fillStyle = dead ? C.inkDead : hot ? C.inkHot : C.ink;
    ctx.fill();
  }

  ctx.font = "600 13px system-ui, 'Segoe UI', sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillStyle = C.ink;
  ctx.fillText("Anchor Master", S.cx, S.cy + A_SPAN / 2 + 22);
}

// --- search widget --------------------------------------------------------
// Effects cannot be a ring: S5 found 519 installed effects. Its level 2 is a
// filter field, which is the other reason "widget" exists as a kind. Static
// mock for now — the real one is fed by the plug-in's catalogue.
const SEARCH_MOCK = ["Gaussian Blur", "Glow", "Curves", "Fill", "Displacement Map"];
const W_W = 360;
const W_H = 232;


function drawSearchWidget() {
  const x = S.cx - W_W / 2;
  const y = S.cy - W_H / 2;
  glassPanel(x, y, W_W, W_H, 16);

  // filter field
  ctx.save();
  roundRect(x + 14, y + 14, W_W - 28, 34, 9);
  ctx.fillStyle = "rgba(12,13,16,0.45)";
  ctx.fill();
  ctx.strokeStyle = "rgba(255,255,255,0.16)";
  ctx.lineWidth = 1;
  ctx.stroke();
  ctx.restore();

  ctx.font = "600 14px system-ui, 'Segoe UI', sans-serif";
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  ctx.fillStyle = C.ink;
  ctx.fillText("blur", x + 26, y + 31);
  ctx.fillStyle = C.accent;
  ctx.fillRect(x + 26 + ctx.measureText("blur").width + 2, y + 22, 1.5, 18);

  SEARCH_MOCK.forEach((name, i) => {
    const ry = y + 58 + i * 33;
    if (i === 0) {
      ctx.save();
      roundRect(x + 14, ry, W_W - 28, 29, 8);
      ctx.fillStyle = "rgba(199,79,214,0.16)";
      ctx.fill();
      ctx.strokeStyle = C.accent;
      ctx.lineWidth = 1.4;
      ctx.stroke();
      ctx.restore();
    }
    ctx.font = "500 13.5px system-ui, 'Segoe UI', sans-serif";
    ctx.fillStyle = i === 0 ? C.inkHot : C.ink;
    ctx.fillText(name, x + 26, ry + 15);
  });

  ctx.font = "600 13px system-ui, 'Segoe UI', sans-serif";
  ctx.textAlign = "center";
  ctx.fillStyle = C.ink;
  ctx.fillText("Effects", S.cx, y + W_H + 20);
}

// --- render ---------------------------------------------------------------
function draw() {
  const dpr = window.devicePixelRatio || 1;
  const w = window.innerWidth;
  const h = window.innerHeight;
  if (canvas.width !== w * dpr || canvas.height !== h * dpr) {
    canvas.width = w * dpr;
    canvas.height = h * dpr;
  }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  if (S.visible) {
    if (S.node.widget === "anchor") {
      drawAnchorWidget();
    } else if (S.node.widget === "search") {
      drawSearchWidget();
    } else {
      // Centre: empty and neutral at level 1 (release here = cancel), the
      // parent breadcrumb once drilled in.
      if (S.parent) {
        drawHex(S.cx, S.cy, S.parent, "hot");
      } else {
        glassHex(S.cx, S.cy, R, "dead");
        ctx.save();
        ctx.globalAlpha = 0.42;
        ctx.strokeStyle = "rgba(235,238,245,0.85)";
        ctx.lineCap = "round";
        ctx.lineWidth = 2;
        const k = 9;
        ctx.beginPath();
        ctx.moveTo(S.cx - k, S.cy - k);
        ctx.lineTo(S.cx + k, S.cy + k);
        ctx.moveTo(S.cx + k, S.cy - k);
        ctx.lineTo(S.cx - k, S.cy + k);
        ctx.stroke();
        ctx.restore();
      }

      const kids = S.node.children || [];
      for (let i = 0; i < 6; i++) {
        const node = kids[i];
        if (!node) continue;
        const [x, y] = slotPos(i);
        const inert =
          !S.armed || (S.armMode === "exit" && i === S.entrySector) || !isLive(node);
        drawHex(x, y, node, inert ? "dead" : S.hot === i ? "hot" : "idle");
      }

      // Why the greyed ones are greyed. One line, under the wheel, only when
      // something is actually unavailable.
      const missing = !CTX.hasComp
        ? "no comp open"
        : !CTX.hasSelection && kids.some((n) => n && !isLive(n))
        ? "select a layer"
        : "";
      if (missing) {
        ctx.font = "600 13px system-ui, 'Segoe UI', sans-serif";
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        ctx.fillStyle = "rgba(235,238,245,0.78)";
        ctx.fillText(missing, S.cx, S.cy + SPACING + R + 14);
      }
    }
  }

  // A throw anywhere above used to abort before the next frame was scheduled,
  // which silently killed the render loop and froze the overlay for the rest of
  // the session with no clue why. (An undefined constant in drawToast did
  // exactly that.) One bad frame should cost one frame.
  try {
    drawToast();
    drawHud();
  } catch (e) {
    if (window.__PIEFX_SAY__) window.__PIEFX_SAY__("draw error: " + e);
    else console.error("draw error:", e);
  }
  requestAnimationFrame(draw);
}

// --- toast ----------------------------------------------------------------
// The plug-in's only non-modal way to tell the user something failed. A modal
// AEGP_ReportInfo mid-gesture would be worse than the failure it reports.
const TOAST = { text: "", until: 0, level: "error" };

function drawToast() {
  const left = TOAST.until - Date.now();
  if (left <= 0) return;

  const w = window.innerWidth;
  const h = window.innerHeight;
  ctx.font = "600 14px system-ui, 'Segoe UI', sans-serif";
  const bw = Math.min(ctx.measureText(TOAST.text).width + 44, 520);
  const bh = 44;

  // Anchored to the last summon, NOT to the window. innerWidth here is the
  // whole virtual desktop, so a window-centred toast lands near the seam
  // between monitors — which is how the first real error went unseen while
  // the user was working on the other screen. The wheel is cursor-anchored
  // for the same reason.
  const ax = S.cx || w / 2;
  const ay = S.cy || h / 2;
  const x = Math.max(12, Math.min(w - bw - 12, ax - bw / 2));
  const y = Math.max(12, Math.min(h - bh - 12, ay + SPACING + R + 26));

  // fade the last 400ms so it leaves rather than blinks out
  ctx.save();
  ctx.globalAlpha = Math.min(1, left / 400);
  glassPanel(x, y, bw, bh, 12);

  ctx.fillStyle = TOAST.level === "error" ? "#FF6B5E" : C.accent;
  ctx.fillRect(x + 1, y + 10, 3, bh - 20);

  ctx.fillStyle = C.ink;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText(TOAST.text, x + bw / 2, y + bh / 2);
  ctx.restore();
}

function toast(level, text) {
  TOAST.level = level;
  TOAST.text = text;
  TOAST.until = Date.now() + 3200;
}

function drawHud() {
  if (!window.__PIEFX_LOCAL__) return;
  ctx.font = "12px ui-monospace, Consolas, monospace";
  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  ctx.fillStyle = "rgba(220,225,235,0.72)";
  [
    "hexwheel — right-press and hold 200ms, flick, release",
    `arm mode: ${S.armMode}   (M to switch)`,
    `ctx: selection=${CTX.hasSelection} comp=${CTX.hasComp}   (S / C to toggle)`,
    "release on a category without drilling = its default action",
    S.lastFired ? `fired: ${S.lastFired}` : "",
  ].forEach((l, i) => ctx.fillText(l, 16, 14 + i * 16));
}
requestAnimationFrame(draw);

// --- gesture state machine ------------------------------------------------
function summon(x, y, ctxMsg) {
  if (ctxMsg) {
    CTX.hasSelection = ctxMsg.hasSelection !== false;
    // Older plug-in builds send no hasComp; a selection can only exist inside
    // a comp, so infer rather than grey the whole wheel.
    CTX.hasComp =
      typeof ctxMsg.hasComp === "boolean" ? ctxMsg.hasComp : CTX.hasSelection;
    CTX.layerCount = ctxMsg.layerCount || 0;
  }
  S.cx = x;
  S.cy = y;
  S.px = x;
  S.py = y;
  S.node = MENU;
  S.parent = null;
  S.level = 1;
  S.armed = true;
  S.entrySector = -1;
  S.hot = -1;
  S.visible = true;
  S.t0 = Date.now();
}

function drillInto(node, fromSector) {
  S.parent = node;
  S.node = node;
  S.level = 2;
  S.entrySector = fromSector;
  S.armed = S.armMode === "exit";
  S.hot = -1;
}

function move(x, y) {
  if (!S.visible) return;
  S.px = x;
  S.py = y;

  if (S.node.widget === "anchor") {
    if (!S.armed && Math.hypot(x - S.cx, y - S.cy) < DEAD) S.armed = true;
    S.hot = S.armed ? anchorCellFor(x, y) : -1;
    return;
  }

  const sec = sectorFor(x, y);

  if (S.level === 2 && !S.armed) {
    if (S.armMode === "center" && sec === -1) S.armed = true;
    if (S.armMode === "exit" && sec !== S.entrySector && sec !== -1) S.armed = true;
  }

  S.hot = sec;

  // Entering a category drills straight in, so the stroke never has to pause.
  if (S.level === 1 && sec >= 0) {
    const node = (S.node.children || [])[sec];
    // A category with nothing live inside it is not worth drilling into: the
    // level-2 ring would be six dead hexagons and the way out is the centre.
    if (node && (node.kind === "ring" || node.kind === "widget") && isLive(node))
      drillInto(node, sec);
  }
}

function release() {
  if (!S.visible) return;
  let action = null;
  let cell = null;
  let label = null;
  let unavailable = null;

  // Every branch below is guarded by canFire(): the grey is a promise, and a
  // slot that looks dead must not fire if the cursor happens to be over it.
  if (S.node.widget === "anchor") {
    if (S.armed && S.hot >= 0 && canFire(S.parent)) {
      action = S.parent.action;
      cell = S.hot;
      label = `Anchor → cell ${S.hot}`;
    } else if (!S.armed && canFire(S.parent)) {
      action = S.parent.action;
      label = S.parent.label;
    }
  } else if (S.armed && S.hot >= 0) {
    const inertSlot = S.armMode === "exit" && S.hot === S.entrySector;
    const node = (S.node.children || [])[S.hot];
    if (node && node.kind === "verb" && !inertSlot && canFire(node)) {
      action = node.action;
      label = (S.parent ? S.parent.label + " → " : "") + node.label;
    } else if (node && node.kind === "verb" && !inertSlot && !isLive(node)) {
      unavailable = node;
    }
  } else if (!S.armed && S.parent) {
    // Flicked to a category and let go without drilling: the common case.
    if (canFire(S.parent)) {
      action = S.parent.action;
      label = S.parent.label;
    } else if (S.parent.action) {
      unavailable = S.parent;
    }
  }

  if (unavailable) {
    toast(
      "error",
      `${unavailable.label} needs ${
        unavailable.requires === "comp" ? "a comp open" : "a selected layer"
      }`
    );
  }

  if (action) {
    S.lastFired = label;
    sendFire(action, cell);
  }
  S.visible = false;
  S.hot = -1;
}

// --- input ----------------------------------------------------------------
let originX = 0;
let originY = 0;

function toLocal(x, y) {
  const dpr = window.devicePixelRatio || 1;
  return [(x - originX) / dpr, (y - originY) / dpr];
}

if (window.__TAURI__ && window.__TAURI__.event) {
  const T = window.__TAURI__;
  const say = (m) => {
    try {
      T.core.invoke("dbg", { msg: String(m) });
    } catch (_) {}
  };
  window.__PIEFX_SAY__ = say;
  // Anything thrown at module scope would silently kill the listener
  // registration below, which looks identical to "the wheel is broken".
  window.addEventListener("error", (e) => say("ERROR " + e.message));

  say("module loaded, keys=" + Object.keys(T).join(","));

  T.core
    .invoke("overlay_dir")
    .then((d) => {
      setInstallDir(d);
      say("install dir " + d);
    })
    .catch((e) => say("overlay_dir FAILED " + e));

  const reload = () =>
    T.core
      .invoke("load_settings")
      .then((txt) => {
        applySettings(txt);
        say("settings loaded, " + (txt ? txt.length + " bytes" : "none -> DEFAULTS"));
      })
      .catch((e) => say("load_settings FAILED " + e));
  reload();
  // The settings window saves, the Rust side broadcasts, and the wheel picks
  // the change up without an AE restart.
  T.event.listen("piefx-settings", reload).catch((e) => say("settings listen FAILED " + e));

  T.core
    .invoke("overlay_origin")
    .then((o) => {
      originX = o[0];
      originY = o[1];
      say("origin " + originX + "," + originY + " dpr=" + window.devicePixelRatio);
    })
    .catch((e) => say("origin FAILED " + e));

  T.event
    .listen("piefx", (e) => {
      let m;
      try {
        m = JSON.parse(e.payload);
      } catch (_) {
        return;
      }
      if (m.type === "summon") {
        const l = toLocal(m.x, m.y);
        say(
          "summon screen=" + m.x + "," + m.y + " local=" + l[0] + "," + l[1] +
            " hasSel=" + m.hasSelection + " hasComp=" + m.hasComp
        );
        summon(l[0], l[1], m);
      } else if (m.type === "cursor") {
        const l = toLocal(m.x, m.y);
        move(l[0], l[1]);
        say("cursor local=" + l[0] + "," + l[1] + " hot=" + S.hot + " node=" + S.node.label);
      } else if (m.type === "release") {
        say("release: armed=" + S.armed + " hot=" + S.hot);
        release();
      } else if (m.type === "cancel") S.visible = false;
      else if (m.type === "quit") {
        // AE is going. Leave now, while the pipe is still whole — see the
        // plug-in's death hook for why the order matters.
        say("quit received");
        T.core.invoke("quit_overlay");
      }
      else if (m.type === "toast") {
        say("toast " + m.level + ": " + m.text);
        toast(m.level, m.text);
      }
    })
    .then(() => {
      say("listener registered");
      // Only now may the pipes be opened: the plug-in treats a completed
      // connect as "the overlay can be driven", so anything sent before this
      // would be dropped on the floor.
      T.core.invoke("frontend_ready");
    })
    .catch((e) => say("listen FAILED " + e));
} else {
  window.__PIEFX_LOCAL__ = true;
  window.__PIEFX__ = { S, CTX, summon, move, release, toast, MENU, isLive, canFire };
  let holdTimer = null;
  let downAt = null;

  addEventListener("contextmenu", (e) => e.preventDefault());
  addEventListener("mousedown", (e) => {
    if (e.button !== 2) return;
    downAt = { x: e.clientX, y: e.clientY };
    holdTimer = setTimeout(() => summon(downAt.x, downAt.y), 200);
  });
  addEventListener("mousemove", (e) => move(e.clientX, e.clientY));
  addEventListener("mouseup", (e) => {
    if (e.button !== 2) return;
    clearTimeout(holdTimer);
    release();
  });
  addEventListener("keydown", (e) => {
    if (e.key === "m" || e.key === "M")
      S.armMode = S.armMode === "center" ? "exit" : "center";
    // Fake the AE context so the `requires` greying is testable with no AE.
    if (e.key === "s" || e.key === "S") CTX.hasSelection = !CTX.hasSelection;
    if (e.key === "c" || e.key === "C") {
      CTX.hasComp = !CTX.hasComp;
      if (!CTX.hasComp) CTX.hasSelection = false;
    }
  });
}
