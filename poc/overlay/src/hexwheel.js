// pieFX — hexagon wheel prototype (Behavior B: drill-down).
//
// Purpose: FEEL THE STROKE. The whole bet of the design is that a second flick
// inside one unbroken press is nearly free. That cannot be judged from a
// screenshot, so this runs standalone in a browser with a real right-press as
// well as under the plug-in.
//
// Geometry: flat-top hexagons (flat edge top and bottom, vertices left/right),
// which is what makes a vertical stack of three share horizontal edges — the
// arrangement in the Base.png mockup. The six neighbours therefore sit at
// N, NE, SE, S, SW, NW, i.e. six 60-degree sectors.
//
// Distance does not matter, only direction: past the dead zone, a sector stays
// highlighted however far out the cursor goes. That is deliberate — a fast flick
// overshoots, and punishing overshoot would defeat the point.

const canvas = document.getElementById("wheel");
const ctx = canvas.getContext("2d");

// --- geometry -------------------------------------------------------------
const R = 52; // hex circumradius (centre -> vertex)
const GAP = 6;
const SPACING = Math.sqrt(3) * R + GAP; // distance between adjacent hex centres
const DEAD = R * 0.92; // centre dead zone radius

// Sector centres in screen angles (0 = right, +y = down): N, NE, SE, S, SW, NW
const DIRS = [-90, -30, 30, 90, 150, 210];

// --- the menu -------------------------------------------------------------
// kind: "verb"   fires on release
//       "ring"   opens six more hexagons
//       "widget" opens a tool-specific UI (NOT hexagons)
// Slots are positional: index 0..5 = N, NE, SE, S, SW, NW. null = a hole, kept
// so that adding an item later does not move the others.
const MENU = {
  kind: "ring",
  children: [
    { label: "Effects", kind: "widget", widget: "search" },
    { label: "Master Null", kind: "verb" },
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
      // Was "More Actions" (a pager). Same content, but as a NAMED category:
      // drill-down instead of a page flip, so positions stay stable.
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
    { label: "Anchor Master", kind: "widget", widget: "anchor" },
  ],
};

// --- state ----------------------------------------------------------------
// armMode "center": level 2 is inert until the cursor returns through the
//                   centre dead zone (release in centre = cancel).
// armMode "exit":   level 2 is live immediately EXCEPT the sector you arrived
//                   in, which stays inert until you leave it once.
const S = {
  visible: false,
  cx: 0,
  cy: 0,
  px: 0,
  py: 0,
  node: MENU, // ring currently displayed
  parent: null, // parent node when drilled in (shown in the centre)
  level: 1,
  armed: true, // level 1 is armed from the start
  entrySector: -1, // sector we drilled from (for "exit" mode)
  hot: -1, // highlighted sector, or -1
  hotCenter: false,
  armMode: "center",
  lastFired: "",
  lastFiredAt: 0,
};

// --- helpers --------------------------------------------------------------
function hexPath(x, y, r) {
  ctx.beginPath();
  for (let k = 0; k < 6; k++) {
    const a = (k * 60 * Math.PI) / 180;
    const vx = x + r * Math.cos(a);
    const vy = y + r * Math.sin(a);
    if (k === 0) ctx.moveTo(vx, vy);
    else ctx.lineTo(vx, vy);
  }
  ctx.closePath();
}

function slotPos(i) {
  const a = (DIRS[i] * Math.PI) / 180;
  return [S.cx + SPACING * Math.cos(a), S.cy + SPACING * Math.sin(a)];
}

// Direction -> sector. Distance is ignored beyond the dead zone (see header).
function sectorFor(px, py) {
  const dx = px - S.cx;
  const dy = py - S.cy;
  if (Math.hypot(dx, dy) < DEAD) return -1; // centre
  let deg = (Math.atan2(dy, dx) * 180) / Math.PI;
  let idx = Math.floor((deg + 120) / 60);
  return ((idx % 6) + 6) % 6;
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

// --- anchor widget (3x3) --------------------------------------------------
// Directionally identical to an 8-way ring plus a centre, which is why it can
// live at level 2 without breaking the stroke model.
const A_CELL = 54;
const A_GAP = 6;
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

function roundRect(x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

// --- palette (from the mockups) -------------------------------------------
const FILL = "rgba(169,194,227,0.94)";
const FILL_HOT = "rgba(212,232,252,0.98)";
const FILL_DEAD = "rgba(150,162,178,0.45)";
const INK = "#B845C8";
const INK_DEAD = "rgba(120,110,130,0.75)";

// --- render ---------------------------------------------------------------
function drawHex(x, y, label, hot, inert, isCategory) {
  ctx.save();
  ctx.shadowColor = "rgba(0,0,0,0.40)";
  ctx.shadowBlur = hot ? 26 : 14;
  ctx.shadowOffsetY = 2;
  hexPath(x, y, R);
  ctx.fillStyle = inert ? FILL_DEAD : hot ? FILL_HOT : FILL;
  ctx.fill();
  ctx.restore();

  ctx.font = "bold 14px system-ui, 'Segoe UI', sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillStyle = inert ? INK_DEAD : INK;
  const lines = wrap(label, R * 1.55);
  const lh = 17;
  lines.forEach((ln, k) =>
    ctx.fillText(ln, x, y + (k - (lines.length - 1) / 2) * lh)
  );

  // A category is marked, so you can see it drills without reading it.
  if (isCategory && !inert) {
    ctx.beginPath();
    ctx.arc(x, y + R * 0.62, 3.2, 0, Math.PI * 2);
    ctx.fillStyle = "rgba(184,69,200,0.85)";
    ctx.fill();
  }
}

function drawAnchorWidget() {
  for (let i = 0; i < 9; i++) {
    const r = anchorCellRect(i);
    const hot = S.hot === i;
    const inert = !S.armed;
    ctx.save();
    ctx.shadowColor = "rgba(0,0,0,0.4)";
    ctx.shadowBlur = hot ? 22 : 10;
    roundRect(r.x, r.y, r.w, r.h, 10);
    ctx.fillStyle = inert ? FILL_DEAD : hot ? FILL_HOT : FILL;
    ctx.fill();
    ctx.restore();

    const dotX = r.x + (r.col / 2) * r.w;
    const dotY = r.y + (r.row / 2) * r.h;
    ctx.beginPath();
    ctx.arc(dotX, dotY, hot ? 4.5 : 3, 0, Math.PI * 2);
    ctx.fillStyle = hot ? "#8E2FA0" : "rgba(140,90,150,0.7)";
    ctx.fill();
  }
  ctx.font = "bold 13px system-ui, 'Segoe UI', sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillStyle = INK;
  ctx.fillText("Anchor Master", S.cx, S.cy + A_SPAN / 2 + 20);
}

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
    } else {
      // centre hex: empty at level 1, the parent breadcrumb once drilled in
      if (S.parent) {
        drawHex(S.cx, S.cy, S.parent.label, false, false, false);
      } else {
        ctx.save();
        ctx.shadowColor = "rgba(0,0,0,0.30)";
        ctx.shadowBlur = 12;
        hexPath(S.cx, S.cy, R);
        ctx.fillStyle = "rgba(169,194,227,0.55)";
        ctx.fill();
        ctx.restore();
      }

      const kids = S.node.children || [];
      for (let i = 0; i < 6; i++) {
        const node = kids[i];
        if (!node) continue;
        const [x, y] = slotPos(i);
        const inert = !S.armed || (S.armMode === "exit" && i === S.entrySector);
        drawHex(
          x,
          y,
          node.label,
          S.hot === i && !inert,
          inert,
          node.kind === "ring" || node.kind === "widget"
        );
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
  ctx.fillStyle = "rgba(220,225,235,0.75)";
  const lines = [
    "hexwheel prototype — right-press and hold 200ms, flick, release",
    `arm mode: ${S.armMode}   (press M to switch)`,
    S.armMode === "center"
      ? "  level 2 inert until you pass back through the centre"
      : "  level 2 live except the slot you arrived in",
    S.lastFired ? `fired: ${S.lastFired}` : "",
  ];
  lines.forEach((l, i) => ctx.fillText(l, 16, 14 + i * 16));
}
requestAnimationFrame(draw);

// --- the gesture state machine -------------------------------------------
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
}

function drillInto(node, fromSector) {
  S.parent = node;
  S.node = node;
  S.level = 2;
  S.entrySector = fromSector;
  // In "center" mode nothing is live until the cursor comes back through the
  // middle; in "exit" mode everything is live except where we came from.
  S.armed = S.armMode === "exit";
  S.hot = -1;
}

function move(x, y) {
  if (!S.visible) return;
  S.px = x;
  S.py = y;

  if (S.node.widget === "anchor") {
    const c = anchorCellFor(x, y);
    // Entering the centre arms the level; releasing there commits it.
    if (!S.armed && Math.hypot(x - S.cx, y - S.cy) < DEAD) S.armed = true;
    S.hot = S.armed ? c : -1;
    return;
  }

  const sec = sectorFor(x, y);

  if (S.level === 2 && !S.armed) {
    if (S.armMode === "center" && sec === -1) S.armed = true;
    if (S.armMode === "exit" && sec !== S.entrySector && sec !== -1) S.armed = true;
  }

  S.hot = sec;
  S.hotCenter = sec === -1;

  // Level 1: entering a category slot drills straight in, so the stroke never
  // has to pause.
  if (S.level === 1 && sec >= 0) {
    const node = (S.node.children || [])[sec];
    if (node && (node.kind === "ring" || node.kind === "widget")) {
      drillInto(node, sec);
    }
  }
}

function release() {
  if (!S.visible) return;
  let fired = null;

  if (S.node.widget === "anchor") {
    if (S.armed && S.hot >= 0) fired = `Anchor → cell ${S.hot}`;
  } else if (S.armed && S.hot >= 0) {
    const inertSlot = S.armMode === "exit" && S.hot === S.entrySector;
    const node = (S.node.children || [])[S.hot];
    if (node && node.kind === "verb" && !inertSlot) {
      fired = (S.parent ? S.parent.label + " → " : "") + node.label;
    }
  }

  if (fired) {
    S.lastFired = fired;
    S.lastFiredAt = Date.now();
    if (window.__PIEFX_LOCAL__) console.log("FIRE:", fired);
    if (window.__TAURI__ && window.__TAURI__.core) {
      window.__TAURI__.core.invoke("report_choice", { choice: fired }).catch(() => {});
    }
  }
  S.visible = false;
  S.hot = -1;
}

// --- input: the plug-in (pipe) or a real mouse (browser) ------------------
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
    if (m.type === "summon") {
      const [x, y] = toLocal(m.x, m.y);
      summon(x, y);
    } else if (m.type === "cursor") {
      const [x, y] = toLocal(m.x, m.y);
      move(x, y);
    } else if (m.type === "release") {
      release();
    } else if (m.type === "cancel") {
      S.visible = false;
    }
  });
} else {
  // Browser: drive it with a real right-press so the stroke can be felt without
  // rebuilding the plug-in.
  window.__PIEFX_LOCAL__ = true;
  window.__PIEFX__ = { S, summon, move, release }; // for poking at in devtools
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
    if (e.key === "m" || e.key === "M") {
      S.armMode = S.armMode === "center" ? "exit" : "center";
    }
  });
}
