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
import { R, SPACING, DEAD, DIRS, bindDraw, DEFAULT_ACCENT } from "./hexdraw.js";
import { sendFire, setInstallDir } from "./actions.js";

const canvas = document.getElementById("wheel");
const ctx = canvas.getContext("2d");

const D = bindDraw(ctx);
const {
  C, hotPalette, noise, roundRect, glassHex, drawHex, glassPanel,
} = D;

// How far out a level-2 child arms, for armMode "distance": just outside the
// CENTRE hexagon. Once a category is open the six hexagons on screen are its
// children, and the parent has moved to the middle - so the moment you are
// clear of the middle you are on a child, and the wheel should say so.
//
// It was originally the far edge of the child hexagon instead, which measured
// out at 147px: you had to overshoot the very thing you were aiming at before
// it lit up, and it felt exactly as wrong as that sounds.
//
// The price is stated where it is paid, in `release()`: this leaves about four
// pixels between the dead zone and the arming radius, so a category's DEFAULT
// action is no longer reachable by flicking to it and letting go. Under
// "distance" a flick into a category lands on the child in that direction.
// "center" and "exit" still reach defaults the old way.
const ARM_DIST = R;

// The wheel's highlight colour. A slot may override it; see `accent` in
// SETTINGS.md.
let ACCENT = DEFAULT_ACCENT;

// Overall size. Applied as a canvas TRANSFORM at draw time and as a division on
// the incoming cursor, rather than by scaling R and every constant derived from
// it: the geometry here is load-bearing and thoroughly measured, and one
// multiply at each boundary cannot get any of it subtly wrong. Everything
// inside - hexagons, the anchor grid, the search panel, the dead zone, the
// arming radius - is in one unscaled design space and stays there.
let SCALE = 1;

// Screen point -> design space. The summon centre is the fixed point, so the
// wheel grows and shrinks around the cursor that called it.
function unscale(x, y) {
  if (SCALE === 1) return [x, y];
  return [S.cx + (x - S.cx) / SCALE, S.cy + (y - S.cy) / SCALE];
}

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
  ACCENT = (SETTINGS.appearance && SETTINGS.appearance.accent) || DEFAULT_ACCENT;
  const sc = SETTINGS.appearance && SETTINGS.appearance.scale;
  // Clamped, because a zero or a negative would divide the gesture into
  // nonsense and a huge one would put the hexagons off every monitor.
  SCALE = typeof sc === "number" && isFinite(sc) ? Math.max(0.5, Math.min(2.5, sc)) : 1;
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
  // Read from DEFAULTS rather than repeated as a literal - a second copy of the
  // shipping default is a second thing to forget to change, and the browser
  // preview (which never calls applySettings) would go on testing the old rule
  // long after the wheel had stopped using it.
  armMode: SETTINGS.gesture.armMode,
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
  const acc = (S.parent && S.parent.accent) || ACCENT;
  const P = hotPalette(acc);
  for (let i = 0; i < 9; i++) {
    const r = anchorCellRect(i);
    const hot = S.hot === i;
    const dead = !S.armed || !canFire(S.parent);

    ctx.save();
    ctx.shadowColor = hot ? P.glow : "rgba(0,0,0,0.55)";
    ctx.shadowBlur = hot ? 28 : 14;
    ctx.shadowOffsetY = 3;
    roundRect(r.x, r.y, r.w, r.h, A_CORNER);
    const g = ctx.createLinearGradient(r.x, r.y, r.x, r.y + r.h);
    g.addColorStop(0, dead ? C.deadTop : hot ? P.hotTop : C.glassTop);
    g.addColorStop(1, dead ? C.deadBot : hot ? P.hotBot : C.glassBot);
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
    ctx.strokeStyle = hot ? P.accent : rim;
    ctx.lineWidth = hot ? 2 : 1.3;
    ctx.stroke();
    ctx.restore();

    const dx = r.x + (r.col / 2) * r.w;
    const dy = r.y + (r.row / 2) * r.h;
    ctx.beginPath();
    ctx.arc(dx, dy, hot ? 5 : 3.4, 0, Math.PI * 2);
    ctx.fillStyle = dead ? C.inkDead : hot ? P.inkHot : C.ink;
    ctx.fill();
  }

  ctx.font = "600 13px system-ui, 'Segoe UI', sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillStyle = C.ink;
  ctx.fillText("Anchor Master", S.cx, S.cy + A_SPAN / 2 + 22);
}

// --- search widget --------------------------------------------------------
// Effects cannot be a ring: S5 found 519 installed effects. Nor can its level 2
// be a filter field, because NOTHING IN pieFX CAN TAKE A KEYSTROKE — the
// overlay is click-through and unfocused, the plug-in hooks the mouse only, and
// the gesture is a press-and-hold. So the panel is not an input: it is the
// recents list, which is the part of a search that needs no typing, and a
// release opens the real focused window (search.js) for everything else.
const W_W = 340;
// The panel is sized to what is IN it, not to a fixed rectangle. With the fake
// search box gone and a recents list that is empty on a fresh install, a fixed
// height was mostly dead grey glass sitting over the user's comp.
const W_PAD = 40; // caption above the rows
const W_ROW = 30;
function widgetHeight(rowCount) {
  return W_PAD + Math.max(1, rowCount) * W_ROW + 14;
}

// Filled from `recents.json`, which the search window writes. Refreshed on each
// summon rather than watched: a summon is user-paced, one read is free, and a
// list that is one flick stale is the one thing this panel must not be.
let RECENTS = [];

function refreshRecents() {
  if (!(window.__TAURI__ && window.__TAURI__.core)) return;
  window.__TAURI__.core.invoke("load_recents").then(
    (txt) => {
      try {
        const a = JSON.parse((txt || "[]").replace(/^﻿/, ""));
        RECENTS = Array.isArray(a) ? a.slice(0, 5) : [];
      } catch (e) {
        RECENTS = [];
      }
    },
    () => {}
  );
}

// The catalogue carries the display names; the panel only has match names. AE's
// match names are readable enough to be worth showing as-is rather than holding
// a second copy of a 519-entry list in the wheel just to prettify five lines:
// "ADBE Gaussian Blur 2" reads as Gaussian Blur once the vendor tag is off.
function shortMatch(m) {
  return String(m)
    .replace(/^ADBE\s+/, "")
    .replace(/\s+\d+$/, "");
}

function drawSearchWidget() {
  const rows = RECENTS.length ? RECENTS.map(shortMatch) : ["No recent effects yet"];
  const h = widgetHeight(rows.length);
  const x = S.cx - W_W / 2;
  const y = S.cy - h / 2;
  glassPanel(x, y, W_W, h, 16);

  // NO SEARCH BOX IS DRAWN HERE. There used to be one, and it was the single
  // most misleading thing on the wheel: a field with a border invites typing,
  // and the wheel cannot take a keystroke — the box was a picture of a control
  // that does not exist. What the panel says instead is what the release will
  // actually do, in plain text.
  // The caption is the honest one for the state you are in. With two layers
  // selected the release will refuse, and it says so BEFORE you let go rather
  // than as a toast afterwards - the plug-in applies through
  // AEGP_GetActiveLayer, which needs exactly one.
  const tooMany = CTX.layerCount > 1;
  ctx.font = "600 14px system-ui, 'Segoe UI', sans-serif";
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  ctx.fillStyle = tooMany ? "#FF6B5E" : hotPalette(ACCENT).accent;
  ctx.fillText(
    tooMany ? `Select a single layer (${CTX.layerCount} selected)` : "Release to search…",
    x + 22,
    y + 28
  );

  // Nothing here is highlighted, deliberately. A hot row would say "release
  // fires this", and release opens the window; the rows are what you will find
  // waiting in it, not slots. Making one of them fireable is a real idea and a
  // separate one — it needs hit-testing inside the panel.
  ctx.save();
  ctx.globalAlpha = RECENTS.length ? 0.82 : 0.45;
  rows.forEach((name, i) => {
    const ry = y + W_PAD - 6 + i * W_ROW;
    ctx.font = "500 13.5px system-ui, 'Segoe UI', sans-serif";
    ctx.fillStyle = C.ink;
    ctx.fillText(name, x + 22, ry + 15);
  });
  ctx.restore();

  ctx.font = "600 13px system-ui, 'Segoe UI', sans-serif";
  ctx.textAlign = "center";
  ctx.fillStyle = C.ink;
  ctx.fillText("Effects", S.cx, y + h + 20);
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
    // Everything below is drawn in design space; the transform does the sizing.
    // It is scaled about the summon centre so the wheel grows around the cursor
    // rather than drifting away from it.
    ctx.save();
    if (SCALE !== 1) {
      ctx.translate(S.cx, S.cy);
      ctx.scale(SCALE, SCALE);
      ctx.translate(-S.cx, -S.cy);
    }
    if (S.node.widget === "anchor") {
      drawAnchorWidget();
    } else if (S.node.widget === "search") {
      drawSearchWidget();
    } else {
      // Centre: empty and neutral at level 1 (release here = cancel), the
      // parent breadcrumb once drilled in.
      if (S.parent) {
        drawHex(S.cx, S.cy, S.parent, "hot", undefined, ACCENT);
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
        // Three different reasons a hexagon is not selectable right now, and
        // only ONE of them is permanent. Drawing them alike is what made the
        // wheel look like it was refusing an action it would happily perform.
        const cannot = !isLive(node);
        const notYet =
          !cannot && (!S.armed || (S.armMode === "exit" && i === S.entrySector));
        const mode = cannot ? "dead" : notYet ? "pending" : S.hot === i ? "hot" : "idle";
        drawHex(x, y, node, mode, undefined, ACCENT);
      }

      // Why the greyed ones are greyed. One line, under the wheel, only when
      // something is actually unavailable - or, at level 2, what the stroke is
      // still waiting for. The second case is the one that used to be silent.
      const waiting =
        S.parent && !S.armed
          ? S.armMode === "distance"
            ? "keep going to pick one"
            : S.armMode === "center"
            ? "back through the centre to pick one"
            : ""
          : "";
      const missing = waiting
        ? waiting
        : !CTX.hasComp
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
    // The toast is NOT scaled: it is a message about a failure, not part of the
    // wheel, and it has to stay legible at the smallest setting.
    ctx.restore();
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
  const y = Math.max(12, Math.min(h - bh - 12, ay + (SPACING + R + 26) * SCALE));

  // fade the last 400ms so it leaves rather than blinks out
  ctx.save();
  ctx.globalAlpha = Math.min(1, left / 400);
  glassPanel(x, y, bw, bh, 12);

  ctx.fillStyle = TOAST.level === "error" ? "#FF6B5E" : hotPalette(ACCENT).accent;
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
  refreshRecents();
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

function move(sx, sy) {
  if (!S.visible) return;
  const [x, y] = unscale(sx, sy);
  S.px = x;
  S.py = y;

  if (S.node.widget === "anchor") {
    if (!S.armed && Math.hypot(x - S.cx, y - S.cy) < DEAD) S.armed = true;
    S.hot = S.armed ? anchorCellFor(x, y) : -1;
    return;
  }

  const sec = sectorFor(x, y);
  const dist = Math.hypot(x - S.cx, y - S.cy);

  // ORDER MATTERS, and it did not used to. Drill FIRST, then arm, then set the
  // hot slot - because a fast flick crosses the whole wheel in ONE cursor
  // message, and that single message has to leave the state machine somewhere
  // a release can act on. Under the old order `S.hot = sec` ran before the
  // drill and `drillInto` then blanked it, so a one-message flick released
  // with nothing under the cursor and fired nothing at all.
  //
  // Entering a category drills straight in, so the stroke never has to pause.
  if (S.level === 1 && sec >= 0) {
    const node = (S.node.children || [])[sec];
    // A category with nothing live inside it is not worth drilling into: the
    // level-2 ring would be six dead hexagons and the way out is the centre.
    if (node && (node.kind === "ring" || node.kind === "widget") && isLive(node))
      drillInto(node, sec);
  }

  // "distance" is CONTINUOUS, unlike the other two: cross ARM_DIST and the
  // children are live, come back inside it and the category's own default is
  // what you are holding again. Depth is the radius, and it is reversible,
  // which is what makes it readable without instructions.
  if (S.level === 2 && S.node.widget !== "anchor") {
    if (S.armMode === "distance") S.armed = dist > ARM_DIST;
    else if (!S.armed) {
      if (S.armMode === "center" && sec === -1) S.armed = true;
      if (S.armMode === "exit" && sec !== S.entrySector && sec !== -1) S.armed = true;
    }
  }

  // A widget hit-tests for itself - the anchor grid's cells are not sectors -
  // so it keeps the -1 that drillInto gave it until the next message, which is
  // the first one the widget branch above handles.
  S.hot = S.node.widget === "anchor" ? -1 : sec;
}

function release() {
  if (!S.visible) return;
  let action = null;
  let cell = null;
  let label = null;
  let unavailable = null;

  // Every branch below is guarded by canFire(): the grey is a promise, and a
  // slot that looks dead must not fire if the cursor happens to be over it.
  if (S.node.widget === "search") {
    // The search widget has nothing to hit-test: it is a panel, and what a
    // release on it means is "open the search window". The dead zone still
    // cancels, so an aborted gesture does not put a window in front of AE —
    // which is the whole reason settings were kept off the wheel too.
    if (S.hot >= 0 && CTX.layerCount > 1) {
      // Refused BEFORE the window opens, not after the user has typed. The
      // plug-in applies an effect through AEGP_GetActiveLayer, which returns a
      // layer only when exactly one is selected - so with two selected the
      // search would take a query, take an Enter, and then quietly apply
      // nothing. Saying so at the gesture is the whole difference.
      toast("error", `select a single layer (${CTX.layerCount} selected)`);
    } else if (S.hot >= 0 && canFire(S.parent)) {
      action = S.parent.action;
      label = S.parent.label;
    } else if (S.hot >= 0 && S.parent.action) {
      unavailable = S.parent;
    }
  } else if (S.node.widget === "anchor") {
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
  } else if (!S.armed && S.parent && S.hot >= 0) {
    // Flicked to a category and let go without drilling: the common case.
    // `S.hot >= 0` keeps the centre meaning cancel. Under "center" arming the
    // point was moot - entering the middle armed you - but under "distance"
    // the middle is inside ARM_DIST, so without this a release there would
    // fire the default instead of cancelling.
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
  // applySettings is exported so the browser preview can drive the WHOLE
  // settings path - accent, scale, arm mode, the slot tree - and not just the
  // state machine. Everything a real settings.json can change is reachable
  // here without an After Effects.
  window.__PIEFX__ = {
    S, CTX, summon, move, release, toast, MENU, isLive, canFire, applySettings,
    scale: () => SCALE,
  };
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
