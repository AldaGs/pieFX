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

const canvas = document.getElementById("wheel");
const ctx = canvas.getContext("2d");

// --- geometry -------------------------------------------------------------
const R = 54;
const GAP = 7;
const SPACING = Math.sqrt(3) * R + GAP;
const DEAD = R * 0.92;
const DIRS = [-90, -30, 30, 90, 150, 210]; // N, NE, SE, S, SW, NW

// --- the menu -------------------------------------------------------------
// kind: "verb"   fires on release
//       "ring"   opens six more hexagons
//       "widget" opens tool-specific UI (NOT hexagons)
//
// `def` on a ring/widget is its DEFAULT action: flick to it and release without
// drilling in, and this fires. So a category costs one flick for the common
// case and two only when you want a variant — the 80% case stays as fast as a
// verb. Slots are positional (0..5 = N,NE,SE,S,SW,NW); null is a deliberate
// hole so adding an item later does not move the others.
// An `action` is one of six kinds — see SETTINGS.md. A slot with `slots` is a
// ring; it may ALSO carry an action, which is its default (fired when you flick
// to it and release without drilling in).
//
//   builtin        { name: "anchor-grid" | "effect-search" }
//   ae-command     { name: "Add to Render Queue", id: 2161 }  name first, id is fallback
//   script-snippet { code: "_mn.addMasterNull(false)" }
//   script-file    { path: "D:/…/ag_masterNull.jsx" }
//   effect         { matchName: "ADBE Gaussian Blur 2" }
const DEFAULTS = {
  version: 1,
  gesture: { holdMs: 200, armMode: "center" },
  wheel: {
    slots: [
      // N
      {
        label: "Effects",
        action: { kind: "builtin", name: "effect-search" },
        widget: "search",
      },
      // NE — from ag_masterNull.jsx. Its two orthogonal axes (placement and
      // parenting) are modifier chords there; a ring replaces the chord with a
      // direction. The script exposes `_mn`, so every variant is one snippet.
      {
        label: "Master Null",
        action: { kind: "script-snippet", code: "_mn.addMasterNull(false)" },
        slots: [
          {
            label: "Comp Center",
            action: { kind: "script-snippet", code: "_mn.addMasterNull(true)" },
          },
          {
            label: "Area Center",
            action: {
              kind: "script-snippet",
              code: "_mn.addMasterNull(false,{useArea:true,skipParenting:false,forceReparent:false})",
            },
          },
          null,
          {
            label: "No Parenting",
            action: {
              kind: "script-snippet",
              code: "_mn.addMasterNull(false,{useArea:false,skipParenting:true,forceReparent:false})",
            },
          },
          {
            label: "Force Reparent",
            action: {
              kind: "script-snippet",
              code: "_mn.addMasterNull(false,{useArea:false,skipParenting:false,forceReparent:true})",
            },
          },
          null,
        ],
      },
      // SE
      {
        label: "Create",
        slots: [
          // Every name here was resolved by app.findMenuCommandId on AE 2026;
          // the ids are what it returned, kept only as a locale fallback.
          // Two of them corrected a guess taken from the command map: Solid is
          // 3000 ("Solid...") not 2038 ("Solid"), and Text is 7034, which the
          // map does not contain at all.
          { label: "Solid", action: { kind: "ae-command", name: "Solid...", id: 3000 } },
          { label: "Null", action: { kind: "ae-command", name: "Null Object", id: 2767 } },
          {
            label: "Adjustment Layer",
            action: { kind: "ae-command", name: "Adjustment Layer", id: 2279 },
          },
          { label: "Text", action: { kind: "ae-command", name: "Text", id: 7034 } },
          { label: "Light", action: { kind: "ae-command", name: "Light...", id: 2563 } },
          { label: "Camera", action: { kind: "ae-command", name: "Camera...", id: 2564 } },
        ],
      },
      // S
      {
        label: "Queue Comp to Render",
        action: { kind: "ae-command", name: "Add to Render Queue", id: 2161 },
      },
      // SW — was "More Actions" (a pager). Same content as a NAMED category, so
      // positions stay stable; paging would break the muscle memory the whole
      // design depends on.
      {
        label: "Layer",
        slots: [
          { label: "Pre-comp", action: { kind: "ae-command", name: "Pre-compose...", id: 2071 } },
          // "Split + Dup" is back. The command map labels 2158
          // "DuplicatePreserveFile", which is why it was written off as having
          // no menu id — but findMenuCommandId("Split Layer") returns 2158.
          // The map's names are not to be trusted; AE's own lookup is.
          { label: "Split + Dup", action: { kind: "ae-command", name: "Split Layer", id: 2158 } },
          {
            // Name still unresolved — "Save Frame As..." returns 0. Likely a
            // submenu whose leaf carries the command. Runs by id meanwhile.
            label: "Save Frame as PNG",
            action: { kind: "ae-command", id: 2233 },
          },
          {
            // Name unresolved too, but 3819 is PROVEN: the self-test really did
            // centre the layer. Runs by id until the spelling is found.
            label: "Center in Comp",
            action: { kind: "ae-command", id: 3819 },
          },
          null,
          null,
        ],
      },
      // NW
      {
        label: "Anchor Master",
        action: { kind: "builtin", name: "anchor-grid", cell: 4 },
        widget: "anchor",
      },
    ],
  },
};

// The renderer wants `kind`/`children`; the settings file speaks
// `action`/`slots`. Compile one into the other so the on-disk format stays the
// document and the runtime shape stays convenient.
function compile(slot) {
  if (!slot) return null;
  const node = {
    label: slot.label,
    action: slot.action || null,
    widget: slot.widget || (slot.action && slot.action.kind === "builtin" ? slot.action.name : null),
  };
  if (slot.slots) {
    node.kind = "ring";
    node.children = slot.slots.map(compile);
    node.def = slot.action ? slot.label : null;
  } else if (slot.widget) {
    node.kind = "widget";
    node.def = slot.action ? slot.label : null;
  } else {
    node.kind = "verb";
  }
  return node;
}

let SETTINGS = DEFAULTS;
let MENU = { kind: "ring", children: DEFAULTS.wheel.slots.map(compile) };

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

// --- palette --------------------------------------------------------------
// Their mockup colours, with the label ink deepened for contrast over footage;
// the bright magenta is kept as the ACCENT for the active slot.
const C = {
  glassTop: "rgba(198,216,240,0.90)",
  glassBot: "rgba(150,176,212,0.86)",
  hotTop: "rgba(232,244,255,0.97)",
  hotBot: "rgba(186,214,248,0.95)",
  deadTop: "rgba(150,158,172,0.42)",
  deadBot: "rgba(120,128,142,0.38)",
  ink: "#4A2A6B",
  inkHot: "#8E1FA8",
  inkDead: "rgba(80,80,95,0.55)",
  accent: "#C74FD6",
};

// --- film grain: a real glass cue, and it hides canvas banding -------------
const noise = (() => {
  const c = document.createElement("canvas");
  c.width = c.height = 64;
  const x = c.getContext("2d");
  const d = x.createImageData(64, 64);
  for (let i = 0; i < d.data.length; i += 4) {
    const v = 120 + Math.random() * 135;
    d.data[i] = d.data[i + 1] = d.data[i + 2] = v;
    d.data[i + 3] = 255;
  }
  x.putImageData(d, 0, 0);
  return ctx.createPattern(c, "repeat");
})();

// --- primitives -----------------------------------------------------------
// Rounded-corner hexagon. Each corner is an arc of radius CORNER, walked via
// arcTo with the EDGE MIDPOINTS as the control targets — the midpoint is half a
// side away (27px) while a 20px corner only consumes 20/tan(60) = 11.5px of it,
// so the radius can never overrun the available edge and collapse a corner.
const CORNER = 20;

function hexPath(x, y, r, round) {
  const rad = round === undefined ? CORNER : round;
  const p = [];
  for (let k = 0; k < 6; k++) {
    const a = (k * 60 * Math.PI) / 180;
    p.push([x + r * Math.cos(a), y + r * Math.sin(a)]);
  }
  ctx.beginPath();
  ctx.moveTo((p[5][0] + p[0][0]) / 2, (p[5][1] + p[0][1]) / 2);
  for (let i = 0; i < 6; i++) {
    const cur = p[i];
    const nxt = p[(i + 1) % 6];
    const mx = (cur[0] + nxt[0]) / 2;
    const my = (cur[1] + nxt[1]) / 2;
    ctx.arcTo(cur[0], cur[1], mx, my, rad);
    ctx.lineTo(mx, my);
  }
  ctx.closePath();
}

function roundRect(x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
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

function wrap(text, maxW) {
  const words = String(text).split(" ");
  const lines = [];
  let line = "";
  for (const w of words) {
    const t = line ? line + " " + w : w;
    if (ctx.measureText(t).width > maxW && line) {
      lines.push(line);
      line = w;
    } else line = t;
  }
  if (line) lines.push(line);
  return lines;
}

// --- the glass panel ------------------------------------------------------
// Simulated, not a real backdrop blur: a transparent Tauri window has nothing
// behind it that CSS can blur (AE is another window entirely). The cues that
// actually read as glass over moving footage are the rim light and the depth,
// not the blur — so those are what this spends its pixels on.
function glassHex(x, y, r, mode) {
  const hot = mode === "hot";
  const dead = mode === "dead";

  ctx.save();
  ctx.shadowColor = hot ? "rgba(90,30,120,0.50)" : "rgba(0,0,0,0.45)";
  ctx.shadowBlur = hot ? 34 : 18;
  ctx.shadowOffsetY = hot ? 5 : 3;
  hexPath(x, y, r);
  const g = ctx.createLinearGradient(x, y - r, x, y + r);
  g.addColorStop(0, dead ? C.deadTop : hot ? C.hotTop : C.glassTop);
  g.addColorStop(1, dead ? C.deadBot : hot ? C.hotBot : C.glassBot);
  ctx.fillStyle = g;
  ctx.fill();
  ctx.restore();

  // grain, clipped to the shape
  ctx.save();
  hexPath(x, y, r);
  ctx.clip();
  ctx.globalAlpha = dead ? 0.03 : 0.07;
  ctx.fillStyle = noise;
  ctx.fillRect(x - r, y - r, r * 2, r * 2);
  ctx.restore();

  // rim light: bright along the top edges, fading to nothing at the bottom
  ctx.save();
  hexPath(x, y, r);
  const rim = ctx.createLinearGradient(x, y - r, x, y + r);
  rim.addColorStop(0, dead ? "rgba(255,255,255,0.22)" : "rgba(255,255,255,0.85)");
  rim.addColorStop(0.45, "rgba(255,255,255,0.16)");
  rim.addColorStop(1, "rgba(255,255,255,0.04)");
  ctx.strokeStyle = rim;
  ctx.lineWidth = 1.4;
  ctx.stroke();
  ctx.restore();

  if (hot) {
    ctx.save();
    hexPath(x, y, r * 0.995);
    ctx.strokeStyle = C.accent;
    ctx.lineWidth = 2;
    ctx.globalAlpha = 0.9;
    ctx.stroke();
    ctx.restore();
  }
}

// --- micro-glyphs ---------------------------------------------------------
// The anchor grid's dot language, carried onto the hexagons: a category shows a
// miniature of what is inside it, so you can see which directions are populated
// BEFORE you drill. Informative, not decorative.
function dot(x, y, r, filled, col) {
  ctx.beginPath();
  ctx.arc(x, y, r, 0, Math.PI * 2);
  if (filled) {
    ctx.fillStyle = col;
    ctx.fill();
  } else {
    ctx.strokeStyle = col;
    ctx.lineWidth = 1;
    ctx.globalAlpha = 0.5;
    ctx.stroke();
    ctx.globalAlpha = 1;
  }
}

function glyphRing(x, y, children, col) {
  const sp = 7.2;
  dot(x, y, 1.7, false, col);
  for (let i = 0; i < 6; i++) {
    const a = (DIRS[i] * Math.PI) / 180;
    dot(x + sp * Math.cos(a), y + sp * Math.sin(a), 2.3, !!children[i], col);
  }
}

function glyphGrid(x, y, col) {
  const sp = 6;
  for (let r = 0; r < 3; r++)
    for (let c = 0; c < 3; c++)
      dot(x + (c - 1) * sp, y + (r - 1) * sp, 1.9, r === 1 && c === 1, col);
}

function glyphSearch(x, y, col) {
  ctx.save();
  ctx.strokeStyle = col;
  ctx.lineWidth = 1.6;
  ctx.beginPath();
  ctx.arc(x - 1, y - 1, 5.2, 0, Math.PI * 2);
  ctx.stroke();
  ctx.beginPath();
  ctx.moveTo(x + 2.8, y + 2.8);
  ctx.lineTo(x + 7, y + 7);
  ctx.stroke();
  ctx.restore();
}

function glyphVerb(x, y, col) {
  ctx.save();
  ctx.globalAlpha = 0.55;
  ctx.beginPath();
  ctx.arc(x, y, 4.4, 0, Math.PI * 2);
  ctx.strokeStyle = col;
  ctx.lineWidth = 1.4;
  ctx.stroke();
  ctx.restore();
}

function drawHex(x, y, node, mode) {
  glassHex(x, y, R, mode);
  const dead = mode === "dead";
  const col = dead ? C.inkDead : mode === "hot" ? C.inkHot : C.ink;

  if (node.kind === "ring") glyphRing(x, y - 20, node.children || [], col);
  else if (node.widget === "search") glyphSearch(x, y - 20, col);
  else if (node.kind === "widget") glyphGrid(x, y - 20, col);
  else glyphVerb(x, y - 20, col);

  ctx.font = "600 13.5px system-ui, 'Segoe UI', sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillStyle = col;
  const lines = wrap(node.label, R * 1.5);
  const lh = 15.5;
  lines.forEach((ln, k) =>
    ctx.fillText(ln, x, y + 12 + (k - (lines.length - 1) / 2) * lh)
  );
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
    const dead = !S.armed;

    ctx.save();
    ctx.shadowColor = hot ? "rgba(90,30,120,0.5)" : "rgba(0,0,0,0.45)";
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
    rim.addColorStop(0, "rgba(255,255,255,0.8)");
    rim.addColorStop(1, "rgba(255,255,255,0.05)");
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

function glassPanel(x, y, w, h, r) {
  ctx.save();
  ctx.shadowColor = "rgba(0,0,0,0.5)";
  ctx.shadowBlur = 30;
  ctx.shadowOffsetY = 6;
  roundRect(x, y, w, h, r);
  const g = ctx.createLinearGradient(x, y, x, y + h);
  g.addColorStop(0, C.glassTop);
  g.addColorStop(1, C.glassBot);
  ctx.fillStyle = g;
  ctx.fill();
  ctx.restore();

  ctx.save();
  roundRect(x, y, w, h, r);
  ctx.clip();
  ctx.globalAlpha = 0.06;
  ctx.fillStyle = noise;
  ctx.fillRect(x, y, w, h);
  ctx.restore();

  ctx.save();
  roundRect(x, y, w, h, r);
  const rim = ctx.createLinearGradient(x, y, x, y + h);
  rim.addColorStop(0, "rgba(255,255,255,0.85)");
  rim.addColorStop(1, "rgba(255,255,255,0.05)");
  ctx.strokeStyle = rim;
  ctx.lineWidth = 1.4;
  ctx.stroke();
  ctx.restore();
}

function drawSearchWidget() {
  const x = S.cx - W_W / 2;
  const y = S.cy - W_H / 2;
  glassPanel(x, y, W_W, W_H, 16);

  // filter field
  ctx.save();
  roundRect(x + 14, y + 14, W_W - 28, 34, 9);
  ctx.fillStyle = "rgba(255,255,255,0.35)";
  ctx.fill();
  ctx.strokeStyle = "rgba(255,255,255,0.55)";
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
      ctx.fillStyle = "rgba(255,255,255,0.42)";
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
        ctx.strokeStyle = "rgba(255,255,255,0.95)";
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
        const inert = !S.armed || (S.armMode === "exit" && i === S.entrySector);
        drawHex(x, y, node, inert ? "dead" : S.hot === i ? "hot" : "idle");
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

  ctx.fillStyle = TOAST.level === "error" ? "#B3261E" : C.ink;
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
    "release on a category without drilling = its default action",
    S.lastFired ? `fired: ${S.lastFired}` : "",
  ].forEach((l, i) => ctx.fillText(l, 16, 14 + i * 16));
}
requestAnimationFrame(draw);

// --- gesture state machine ------------------------------------------------
function summon(x, y) {
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
    if (node && (node.kind === "ring" || node.kind === "widget")) drillInto(node, sec);
  }
}

// Free text crosses the pipe base64-encoded: the native side hand-rolls its JSON
// parsing, and a correct unescaper for arbitrary user script is exactly the kind
// of thing that works until someone puts a quote in a string literal.
function b64(s) {
  return btoa(unescape(encodeURIComponent(s)));
}

function sendFire(action, cell) {
  if (!action) return;
  let m = null;
  switch (action.kind) {
    case "ae-command":
      // Name first, id as fallback. findMenuCommandId resolves the name against
      // the running AE, so a binding is checkable at the moment it is made — an
      // id can only be trusted, and the hand-tested map has already produced
      // three wrong entries and two duplicate names.
      m = { type: "fire", kind: "ae-command" };
      if (action.name) m.b64 = b64(action.name);
      if (action.id) m.id = action.id;
      break;
    case "script-snippet":
      m = { type: "fire", kind: "script-snippet", b64: b64(action.code) };
      break;
    case "script-file":
      m = { type: "fire", kind: "script-file", b64: b64(action.path) };
      break;
    case "effect":
      m = { type: "fire", kind: "effect", b64: b64(action.matchName) };
      break;
    case "builtin":
      m = {
        type: "fire",
        kind: "builtin",
        name: action.name,
        cell: cell === undefined || cell === null ? (action.cell ?? -1) : cell,
      };
      break;
    default:
      return;
  }
  if (window.__PIEFX_LOCAL__) console.log("FIRE:", m);
  if (window.__TAURI__ && window.__TAURI__.core)
    window.__TAURI__.core
      .invoke("fire_action", { json: JSON.stringify(m) })
      .catch((e) => window.__PIEFX_SAY__ && window.__PIEFX_SAY__("fire FAILED " + e));
}

function release() {
  if (!S.visible) return;
  let action = null;
  let cell = null;
  let label = null;

  if (S.node.widget === "anchor") {
    if (S.armed && S.hot >= 0) {
      action = S.parent && S.parent.action;
      cell = S.hot;
      label = `Anchor → cell ${S.hot}`;
    } else if (!S.armed && S.parent && S.parent.action) {
      action = S.parent.action;
      label = S.parent.label;
    }
  } else if (S.armed && S.hot >= 0) {
    const inertSlot = S.armMode === "exit" && S.hot === S.entrySector;
    const node = (S.node.children || [])[S.hot];
    if (node && node.kind === "verb" && !inertSlot) {
      action = node.action;
      label = (S.parent ? S.parent.label + " → " : "") + node.label;
    }
  } else if (!S.armed && S.parent && S.parent.action) {
    // Flicked to a category and let go without drilling: the common case.
    action = S.parent.action;
    label = S.parent.label;
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
        say("summon screen=" + m.x + "," + m.y + " local=" + l[0] + "," + l[1]);
        summon(l[0], l[1]);
      } else if (m.type === "cursor") {
        const l = toLocal(m.x, m.y);
        move(l[0], l[1]);
        say("cursor local=" + l[0] + "," + l[1] + " hot=" + S.hot + " node=" + S.node.label);
      } else if (m.type === "release") {
        say("release: armed=" + S.armed + " hot=" + S.hot);
        release();
      } else if (m.type === "cancel") S.visible = false;
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
  window.__PIEFX__ = { S, summon, move, release, toast, MENU };
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
  });
}
