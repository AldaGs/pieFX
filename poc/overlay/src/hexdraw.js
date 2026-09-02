// pieFX — the glass renderer: palette, hexagon primitives, glyphs, panels.
//
// Extracted from hexwheel.js so the SETTINGS WINDOW draws the wheel with the
// same code the gesture draws it with. That is the whole point of a settings
// screen that IS a wheel: if the configurator's hexagons were a second
// implementation, what you configured would stop being what you flicked at.
//
// Everything here is bound to one 2D context by `bindDraw(ctx)`. Geometry
// constants are module-level because the geometry is the design, not a
// per-canvas choice.

// --- geometry -------------------------------------------------------------
export const R = 54;
export const GAP = 7;
export const SPACING = Math.sqrt(3) * R + GAP;
export const DEAD = R * 0.92;
export const DIRS = [-90, -30, 30, 90, 150, 210]; // N, NE, SE, S, SW, NW
export const CORNER = 20;

// Where slot `i` sits around a centre, at whatever spacing the caller wants.
// The live wheel and the settings wheel differ only in scale.
export function slotPosAt(cx, cy, i, spacing) {
  const a = (DIRS[i] * Math.PI) / 180;
  const s = spacing === undefined ? SPACING : spacing;
  return [cx + s * Math.cos(a), cy + s * Math.sin(a)];
}

// --- accent ---------------------------------------------------------------
// The highlight colour is a setting now, globally and per hexagon, so the hot
// state can no longer be four hand-picked constants.
//
// It is NOT recomputed from scratch either. The shipped purple's hot gradient
// and hot ink were tuned by eye against real footage and are the user's call;
// a formula that "derives" them would quietly change the default. So the
// shipped values stay exactly as they are, and any other accent is the same
// values ROTATED to the new hue (with saturation scaled, so a desaturated
// accent gets a desaturated glow rather than a purple one). Default in, default
// out, bit for bit.
export const DEFAULT_ACCENT = "#C74FD6";

const BASE = {
  hotTop: "rgba(86,74,104,0.95)",
  hotBot: "rgba(52,44,66,0.94)",
  inkHot: "#F2C8FA",
  glow: "rgba(199,79,214,0.45)",
};

function parseColor(c) {
  const hex = /^#([0-9a-f]{6})$/i.exec(c.trim());
  if (hex) {
    const n = parseInt(hex[1], 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255, 1];
  }
  const m = /rgba?\(([^)]+)\)/i.exec(c);
  if (!m) return [255, 255, 255, 1];
  const p = m[1].split(",").map((v) => parseFloat(v));
  return [p[0], p[1], p[2], p.length > 3 ? p[3] : 1];
}

function rgbToHsl(r, g, b) {
  r /= 255;
  g /= 255;
  b /= 255;
  const mx = Math.max(r, g, b);
  const mn = Math.min(r, g, b);
  const l = (mx + mn) / 2;
  const d = mx - mn;
  if (!d) return [0, 0, l];
  const s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
  let h;
  if (mx === r) h = ((g - b) / d + (g < b ? 6 : 0)) / 6;
  else if (mx === g) h = ((b - r) / d + 2) / 6;
  else h = ((r - g) / d + 4) / 6;
  return [h, s, l];
}

function hslToRgb(h, s, l) {
  if (!s) {
    const v = Math.round(l * 255);
    return [v, v, v];
  }
  const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
  const p = 2 * l - q;
  const f = (t) => {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1 / 6) return p + (q - p) * 6 * t;
    if (t < 1 / 2) return q;
    if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
    return p;
  };
  return [
    Math.round(f(h + 1 / 3) * 255),
    Math.round(f(h) * 255),
    Math.round(f(h - 1 / 3) * 255),
  ];
}

// Re-hue one of the BASE colours towards `accent`, keeping its alpha and its
// tuned lightness.
function shift(color, dh, sMul) {
  const [r, g, b, a] = parseColor(color);
  const [h, sat, l] = rgbToHsl(r, g, b);
  const h2 = (((h + dh) % 1) + 1) % 1;
  const s2 = Math.max(0, Math.min(1, sat * sMul));
  const [r2, g2, b2] = hslToRgb(h2, s2, l);
  return `rgba(${r2},${g2},${b2},${a})`;
}

const paletteCache = new Map();

// The hot half of the palette for one accent. Cached: this runs inside the
// render loop, once per hexagon per frame.
export function hotPalette(accent) {
  const key = accent || DEFAULT_ACCENT;
  const hit = paletteCache.get(key);
  if (hit) return hit;

  let pal;
  if (key.toLowerCase() === DEFAULT_ACCENT.toLowerCase()) {
    pal = { accent: DEFAULT_ACCENT, ...BASE };
  } else {
    const [dr, dg, db] = parseColor(DEFAULT_ACCENT);
    const [nr, ng, nb] = parseColor(key);
    const [dh, ds] = rgbToHsl(dr, dg, db);
    const [nh, ns] = rgbToHsl(nr, ng, nb);
    const delta = nh - dh;
    const mul = ds > 0.01 ? ns / ds : 1;
    pal = {
      accent: key,
      hotTop: shift(BASE.hotTop, delta, mul),
      hotBot: shift(BASE.hotBot, delta, mul),
      inkHot: shift(BASE.inkHot, delta, mul),
      glow: shift(BASE.glow, delta, mul),
    };
  }
  paletteCache.set(key, pal);
  return pal;
}

export function bindDraw(ctx) {
// --- palette --------------------------------------------------------------
// Dark smoked glass, with the accent doing all the colour work. The earlier
// light-blue glass was the mockup's, and over a comp it read as a dialog from
// an older Windows: pale, high-value, competing with the footage it sits on.
// A dark surface recedes instead, which is what an overlay should do — and it
// gives the accent somewhere to be bright.
//
// Values, not just hues: the idle hexagon sits near the bottom of the range so
// footage never has to fight it, the hot one lifts a step and gains an accent
// rim, and dead drops toward the ground with its ink at a quarter strength.
const C = {
  glassTop: "rgba(48,52,60,0.90)",
  glassBot: "rgba(28,30,36,0.88)",
  hotTop: "rgba(86,74,104,0.95)",
  hotBot: "rgba(52,44,66,0.94)",
  deadTop: "rgba(34,36,42,0.62)",
  deadBot: "rgba(24,26,30,0.58)",
  ink: "rgba(228,232,240,0.92)",
  inkHot: "#F2C8FA",
  inkDead: "rgba(210,216,228,0.26)",
  inkPending: "rgba(226,230,238,0.55)",
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
// `mode` is one of four, and the fourth is the point of this refactor:
//
//   idle     available, not under the cursor
//   hot      under the cursor, will fire on release
//   dead     CANNOT fire - wrong context, nothing bound
//   pending  can fire, but the gesture has not armed it yet
//
// `pending` used to be drawn as `dead`, which is a lie with consequences: a
// hexagon one movement away from working looked exactly like one that could
// never work, so the wheel appeared to be refusing an action it was perfectly
// willing to perform. It now reads as a dimmed version of itself.
function glassHex(x, y, r, mode, accent) {
  const round = CORNER * (r / R);
  const hot = mode === "hot";
  const dead = mode === "dead";
  const pending = mode === "pending";
  const P = hotPalette(accent || C.accent);

  ctx.save();
  if (pending) ctx.globalAlpha = 0.5;
  ctx.shadowColor = hot ? P.glow : "rgba(0,0,0,0.55)";
  ctx.shadowBlur = hot ? 30 : 16;
  ctx.shadowOffsetY = hot ? 5 : 3;
  hexPath(x, y, r, round);
  const g = ctx.createLinearGradient(x, y - r, x, y + r);
  g.addColorStop(0, dead ? C.deadTop : hot ? P.hotTop : C.glassTop);
  g.addColorStop(1, dead ? C.deadBot : hot ? P.hotBot : C.glassBot);
  ctx.fillStyle = g;
  ctx.fill();
  ctx.restore();

  // grain, clipped to the shape
  ctx.save();
  hexPath(x, y, r, round);
  ctx.clip();
  ctx.globalAlpha = dead ? 0.02 : pending ? 0.025 : 0.045;
  ctx.fillStyle = noise;
  ctx.fillRect(x - r, y - r, r * 2, r * 2);
  ctx.restore();

  // rim light: bright along the top edges, fading to nothing at the bottom
  ctx.save();
  hexPath(x, y, r, round);
  const rim = ctx.createLinearGradient(x, y - r, x, y + r);
  rim.addColorStop(
    0,
    dead ? "rgba(255,255,255,0.10)" : pending ? "rgba(255,255,255,0.20)" : "rgba(255,255,255,0.42)"
  );
  rim.addColorStop(0.45, "rgba(255,255,255,0.08)");
  rim.addColorStop(1, "rgba(255,255,255,0.03)");
  ctx.strokeStyle = rim;
  ctx.lineWidth = 1.4;
  ctx.stroke();
  ctx.restore();

  if (hot) {
    ctx.save();
    hexPath(x, y, r * 0.995, round);
    ctx.strokeStyle = P.accent;
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

// `radius` defaults to the live wheel's R. The settings window draws the same
// hexagon larger, so everything inside it — glyph, type, line height, corner —
// is scaled by the same factor rather than sized twice.
function drawHex(x, y, node, mode, radius, accent) {
  const rr = radius || R;
  const k = rr / R;
  // A slot may carry its own highlight colour; otherwise the wheel's.
  const acc = node.accent || accent || C.accent;
  glassHex(x, y, rr, mode, acc);
  const dead = mode === "dead";
  const col =
    dead ? C.inkDead : mode === "hot" ? hotPalette(acc).inkHot : mode === "pending" ? C.inkPending : C.ink;

  ctx.save();
  ctx.translate(x, y - 20 * k);
  ctx.scale(k, k);
  if (node.kind === "ring") glyphRing(0, 0, node.children || [], col);
  else if (node.widget === "search") glyphSearch(0, 0, col);
  else if (node.kind === "widget") glyphGrid(0, 0, col);
  else glyphVerb(0, 0, col);
  ctx.restore();

  ctx.font = `600 ${(13.5 * k).toFixed(1)}px system-ui, 'Segoe UI', sans-serif`;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillStyle = col;
  const lines = wrap(node.label || "", rr * 1.5);
  const lh = 15.5 * k;
  lines.forEach((ln, i) =>
    ctx.fillText(ln, x, y + 12 * k + (i - (lines.length - 1) / 2) * lh)
  );
}

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
  rim.addColorStop(0, "rgba(255,255,255,0.38)");
  rim.addColorStop(1, "rgba(255,255,255,0.04)");
  ctx.strokeStyle = rim;
  ctx.lineWidth = 1.4;
  ctx.stroke();
  ctx.restore();
}

  return {
    C,
    hotPalette,
    noise,
    hexPath,
    roundRect,
    wrap,
    glassHex,
    dot,
    glyphRing,
    glyphGrid,
    glyphSearch,
    glyphVerb,
    drawHex,
    glassPanel,
  };
}
