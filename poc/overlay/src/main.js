// pieFX overlay — the Anchor wheel.
//
// A pure renderer. The native plug-in owns all input and, from M2 on, drives
// this over a named pipe: summon(x,y,ctx) → cursor(x,y)* → release. The window
// is click-through, so the wheel never receives OS mouse events itself.
//
// Milestone 1: no pipe yet. On load we summon once at screen centre so the
// static 3x3 grid is visible under `npm run tauri dev`.

const canvas = document.getElementById("wheel");
const ctx = canvas.getContext("2d");

// --- wheel geometry -------------------------------------------------------
const CELL = 76; // px, side of one grid cell
const GAP = 8; // px, gap between cells
const SPAN = 3 * CELL + 2 * GAP; // full grid side
const HALF = SPAN / 2;
const RADIUS = 10; // rounded-corner radius

// --- state (mutated by the driver, read by the render loop) ---------------
const state = {
  visible: false,
  cx: 0, // summon centre, screen px (window-local)
  cy: 0,
  px: 0, // last known cursor, window-local px
  py: 0,
  hasSelection: true,
  cell: -1, // highlighted cell 0..8, or -1
};

// Screen (col,row) → the anchor fraction that cell represents on the layer.
// Purely for the little in-cell dot hint; the real math lives native-side.
function cellRect(index) {
  const row = Math.floor(index / 3);
  const col = index % 3;
  const gx = state.cx - HALF + col * (CELL + GAP);
  const gy = state.cy - HALF + row * (CELL + GAP);
  return { x: gx, y: gy, w: CELL, h: CELL, row, col };
}

// Which cell is the point over? -1 if none.
function hitTest(px, py) {
  for (let i = 0; i < 9; i++) {
    const r = cellRect(i);
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

// --- render loop ----------------------------------------------------------
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

  if (state.visible) {
    // soft backdrop behind the grid so it reads over any comp
    ctx.save();
    ctx.shadowColor = "rgba(0,0,0,0.45)";
    ctx.shadowBlur = 28;
    roundRect(state.cx - HALF - 12, state.cy - HALF - 12, SPAN + 24, SPAN + 24, 18);
    ctx.fillStyle = "rgba(24,26,32,0.62)";
    ctx.fill();
    ctx.restore();

    for (let i = 0; i < 9; i++) {
      const r = cellRect(i);
      const active = i === state.cell;

      roundRect(r.x, r.y, r.w, r.h, RADIUS);
      if (!state.hasSelection) {
        ctx.fillStyle = "rgba(120,124,134,0.18)";
      } else if (active) {
        ctx.fillStyle = "rgba(90,170,255,0.85)";
      } else {
        ctx.fillStyle = "rgba(220,224,232,0.16)";
      }
      ctx.fill();

      ctx.lineWidth = active ? 2 : 1;
      ctx.strokeStyle = active
        ? "rgba(150,205,255,0.95)"
        : "rgba(230,234,242,0.28)";
      ctx.stroke();

      // dot marking which corner/edge/centre this cell anchors to
      const dotX = r.x + (r.col / 2) * r.w;
      const dotY = r.y + (r.row / 2) * r.h;
      ctx.beginPath();
      ctx.arc(dotX, dotY, active ? 4 : 3, 0, Math.PI * 2);
      ctx.fillStyle = active ? "#ffffff" : "rgba(235,238,245,0.55)";
      ctx.fill();
    }

    if (!state.hasSelection) {
      ctx.fillStyle = "rgba(235,238,245,0.75)";
      ctx.font = "13px system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.fillText("select a layer", state.cx, state.cy + HALF + 24);
    }
  }

  requestAnimationFrame(draw);
}
requestAnimationFrame(draw);

// --- driver API -----------------------------------------------------------
// The native plug-in is the single source of truth for the highlighted cell
// (it has the mouse hook and the same geometry). We just render what it says.
const wheel = {
  summon(x, y, hasSelection) {
    state.cx = x;
    state.cy = y;
    state.px = x;
    state.py = y;
    state.hasSelection = hasSelection !== false;
    state.cell = -1;
    state.visible = true;
  },
  cursor(x, y, cell) {
    state.px = x;
    state.py = y;
    state.cell = state.hasSelection ? cell : -1;
  },
  hide() {
    state.visible = false;
    state.cell = -1;
  },
};
window.wheel = wheel;

// --- coordinate mapping ---------------------------------------------------
// The plug-in sends physical screen px. The window sits at the virtual-desktop
// origin (which may be negative), so window-local = (screen - origin). Then
// divide by devicePixelRatio because the canvas draws in CSS px.
let originX = 0;
let originY = 0;

async function loadOrigin() {
  try {
    const o = await window.__TAURI__.core.invoke("overlay_origin");
    originX = o[0];
    originY = o[1];
  } catch (_) {
    /* browser preview or older runtime: leave origin at 0,0 */
  }
}

function toLocal(x, y) {
  const dpr = window.devicePixelRatio || 1;
  return [(x - originX) / dpr, (y - originY) / dpr];
}

// --- pipe messages, forwarded by the Rust side as `piefx` events ----------
function handle(msg) {
  switch (msg.type) {
    case "summon": {
      const [lx, ly] = toLocal(msg.x, msg.y);
      wheel.summon(lx, ly, msg.hasSelection);
      break;
    }
    case "cursor": {
      const [lx, ly] = toLocal(msg.x, msg.y);
      wheel.cursor(lx, ly, typeof msg.cell === "number" ? msg.cell : -1);
      break;
    }
    case "release":
    case "cancel":
      wheel.hide();
      break;
  }
}

if (window.__TAURI__ && window.__TAURI__.event) {
  loadOrigin();
  window.__TAURI__.event.listen("piefx", (e) => {
    try {
      handle(JSON.parse(e.payload));
    } catch (_) {
      /* ignore malformed line */
    }
  });
} else {
  // Not under Tauri (plain browser preview): show a static demo grid so the
  // layout can be eyeballed without the plug-in. Wait for layout so the
  // viewport size is real (innerWidth is 0 before first layout).
  window.addEventListener("load", () => {
    wheel.summon(window.innerWidth / 2, window.innerHeight / 2, true);
  });
}
