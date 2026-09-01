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
const MENU = {
  kind: "ring",
  children: [
    { label: "Effects", kind: "widget", widget: "search", def: "Effect search" },
    {
      // From ag_masterNull.jsx. Two orthogonal axes there — placement and
      // parenting — both driven by modifier chords. A ring replaces the chord
      // with a direction. Default = anchor average + parent roots (the 80%).
      label: "Master Null",
      kind: "ring",
      def: "Master Null",
      children: [
        { label: "Comp Center", kind: "verb" }, // N   (was secondary-click)
        { label: "Area Center", kind: "verb" }, // NE  (was Shift)
        null, // SE
        { label: "No Parenting", kind: "verb" }, // S   (was Alt)
        { label: "Force Reparent", kind: "verb" }, // SW  (was Ctrl/Cmd)
        null, // NW
      ],
    },
    {
      label: "Create",
      kind: "ring",
      children: [
        { label: "Solid", kind: "verb" },
        { label: "Null", kind: "verb" },
        { label: "Adjustment Layer", kind: "verb" },
        { label: "Text", kind: "verb" },
        { label: "Light", kind: "verb" },
        { label: "Camera", kind: "verb" },
      ],
    },
    { label: "Queue Comp to Render", kind: "verb" },
    {
      // Was "More Actions" (a pager). Same content as a NAMED category, so
      // positions stay stable — paging would break the muscle memory the whole
      // design depends on.
      label: "Layer",
      kind: "ring",
      children: [
        { label: "Pre-comp", kind: "verb" },
        { label: "Split + Dup", kind: "verb" },
        { label: "Save Frame as PNG", kind: "verb" },
        { label: "Center in Comp", kind: "verb" },
        null,
        null,
      ],
    },
    { label: "Anchor Master", kind: "widget", widget: "anchor", def: "Anchor to center" },
  ],
};

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
  armMode: "center", // "center" | "exit"  — press M to switch
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

  drawHud();
  requestAnimationFrame(draw);
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

function release() {
  if (!S.visible) return;
  let fired = null;

  if (S.node.widget === "anchor") {
    if (S.armed && S.hot >= 0) fired = `Anchor → cell ${S.hot}`;
    else if (!S.armed && S.parent && S.parent.def) fired = S.parent.def;
  } else if (S.armed && S.hot >= 0) {
    const inertSlot = S.armMode === "exit" && S.hot === S.entrySector;
    const node = (S.node.children || [])[S.hot];
    if (node && node.kind === "verb" && !inertSlot)
      fired = (S.parent ? S.parent.label + " → " : "") + node.label;
  } else if (!S.armed && S.parent && S.parent.def) {
    // Flicked to a category and let go without drilling: the common case.
    fired = S.parent.def;
  }

  if (fired) {
    S.lastFired = fired;
    if (window.__PIEFX_LOCAL__) console.log("FIRE:", fired);
    if (window.__TAURI__ && window.__TAURI__.core)
      window.__TAURI__.core.invoke("report_choice", { choice: fired }).catch(() => {});
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
  window.__TAURI__.core
    .invoke("overlay_origin")
    .then((o) => {
      originX = o[0];
      originY = o[1];
    })
    .catch(() => {});

  window.__TAURI__.event.listen("piefx", (e) => {
    let m;
    try {
      m = JSON.parse(e.payload);
    } catch (_) {
      return;
    }
    if (m.type === "summon") summon(...toLocal(m.x, m.y));
    else if (m.type === "cursor") move(...toLocal(m.x, m.y));
    else if (m.type === "release") release();
    else if (m.type === "cancel") S.visible = false;
  });
} else {
  window.__PIEFX_LOCAL__ = true;
  window.__PIEFX__ = { S, summon, move, release, MENU };
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
