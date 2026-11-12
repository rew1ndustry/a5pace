/* a5web: the browser shell for a5pace. step 2: the map, drawn on a canvas, read only.
   Row = iteration, the loop across; a take is a ribbon whose thickness is its rms (one byte a 0.2 s tick, asppeaks.h) and whose
   colour is green / yellow / red by the thresholds of a5hell.cfg. Hover lights a take and puts its details on the bottom row. */
"use strict";
const TICK_S = 0.2, TPS = 5;                       // ticks a second
const C = { bg: "#000", text: "#9a9a9a", dim: "#555", green: "#3ddc6b", yellow: "#e0c040", red: "#ff4a3d" };
const cv = document.getElementById("c"), g = cv.getContext("2d");
let W = 0, H = 0, dpr = 1;

const S = { state: null, takes: [], pins: [], grid: new Map(), meter: [-200, -200, -200, -200, 0, 0], cfg: { green: -24, red: -6 },
            top: null, follow: true, hover: null, mx: -1, my: -1, err: "", rows: 20 };
const lay = { head: 34, foot: 34, left: 56, right: 12, rowh: 26 };

// ---- the control port, by http ----
async function get(path) {
  const r = await fetch(path, { cache: "no-store" });
  if (!r.ok) throw new Error((await r.text()).trim() || r.status);
  return (await r.text()).split("\n").filter(Boolean);
}
const hexv = c => parseInt(c, 16);
function parseMap(lines) {                         // iter off frames in|out|open tick0 hex ; a long take comes in chunks
  const out = [];
  for (const l of lines) {
    const w = l.split(" ");
    if (w.length < 6) continue;
    const iter = +w[0], off = +w[1], frames = +w[2], st = w[3], t0 = +w[4], hex = w[5];
    let t = out[out.length - 1];
    if (!t || t.iter !== iter || t.off !== off)
      out.push(t = { iter, off, frames, st: st === "open" ? 2 : st === "in" ? 1 : 0, pk: [] });
    if (hex[0] === "-") { const n = Math.ceil(frames / 9600); t.pk = new Array(n).fill(1); continue; }
    for (let i = 0; i + 1 < hex.length; i += 2) t.pk[t0 + i / 2] = hexv(hex.substr(i, 2));
  }
  for (const t of out) for (let i = 0; i < t.pk.length; i++) if (t.pk[i] === undefined) t.pk[i] = 0;
  return out;
}
const db = b => b > 0 ? (b - 1) / 2 - 120 : -200;

function loopTicks() { return S.state ? Math.max(1, Math.round(S.state.loop_frames / 9600)) : 40; }
function curIter() { return S.state ? S.state.iter : 1; }

async function pollCfg() {
  try { for (const l of await get("/api/cfg")) { const [k, v] = l.split(" "); if (k === "green_below_db") S.cfg.green = +v; if (k === "red_above_db") S.cfg.red = +v; } } catch (e) {}
}
async function pollSlow() {                         // state, map, grid: about once a second
  try {
    S.state = JSON.parse((await get("/api/state"))[0]);
    const it = curIter(), lo = Math.max(1, (S.top ?? it - S.rows + 1));
    const [main, pins, grid] = await Promise.all([
      get(`/api/map?from=${lo}&to=${lo + S.rows + 2}`), get("/api/maphigh?n=8&above=89999"), get("/api/grid")]);
    S.takes = parseMap(main); S.pins = parseMap(pins);
    S.grid = new Map(grid.map(l => { const w = l.split(" ").map(Number);
      return [w[0] + "." + w[1], { id: w[2], inloop: w[3], mixed: w[4], gsh: w[5], pan: w[6], pending: w[7], mono: w[8], len: w[9] }]; }));
    S.err = "";
  } catch (e) { S.err = String(e.message || e); }
  draw();
  setTimeout(pollSlow, 1000);
}
async function pollMeter() {
  try { S.meter = (await get("/api/meter"))[0].split(" ").map(Number); } catch (e) {}
  drawMeters();
  setTimeout(pollMeter, 200);
}

// ---- geometry: which rows are shown, where a take's tick lands ----
function rowsShown() {                              // main rows, then (if any) a gap and the pinned rows
  const it = curIter(), T = S.rows, top = S.follow || S.top === null ? Math.max(1, it - T + 1) : S.top;
  const main = []; for (let i = 0; i < T; i++) main.push(top + i);
  const pin = [...new Set(S.pins.map(t => t.iter))].sort((a, b) => a - b);
  return { main, pin, top };
}
function layout() {
  const npin = new Set(S.pins.map(t => t.iter)).size;
  const avail = H - lay.head - lay.foot - (npin ? 14 + npin * 20 : 0);
  S.rows = Math.max(4, Math.min(60, Math.floor(avail / 26)));         // how many rows fit at 26 px; they then share the space
  lay.rowh = Math.max(14, Math.floor(avail / S.rows));
}
function rowY(slot) { return lay.head + slot * lay.rowh; }           // main slots 0..; pinned slots continue after a gap

function takePos(t, k) {                            // the loop tick of the take's tick k: row offset and column
  const T = loopTicks(), p = TPS * t.off + k;
  return { row: t.iter + Math.floor(p / T), col: p % T };
}
function color(d) { return d >= S.cfg.red ? C.red : d < S.cfg.green ? C.green : C.yellow; }
function thick(d, rowh) { const x = Math.min(1, Math.max(0, (d + 60) / 60)); return 1.5 + x * (rowh * 0.86 - 1.5); }

// ---- drawing ----
function build() {                                  // every ribbon segment of the shown takes, with its cell, so hover can find a take
  const T = loopTicks(), { main, pin } = rowsShown(), slot = new Map();
  main.forEach((r, i) => slot.set(r, rowY(i)));
  pin.forEach((r, i) => slot.set(r, lay.head + main.length * lay.rowh + 14 + i * 20));
  const rh = r => (pin.includes(r) && r > 89999) ? 20 : lay.rowh;
  const x0 = lay.left, tw = (W - lay.left - lay.right) / T;
  const items = [];
  for (const t of [...S.takes, ...S.pins]) {
    for (let k = 0; k < t.pk.length; k++) {
      const { row, col } = takePos(t, k), y = slot.get(row);
      if (y === undefined) continue;
      items.push({ t, k, row, col, y, h: rh(row), x: x0 + (col + 0.5) * tw, d: db(t.pk[k]), tw });
    }
  }
  return { items, slot, rh, x0, tw, T };
}

function draw() {
  layout();
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.fillStyle = C.bg; g.fillRect(0, 0, W, H);
  g.font = "12px ui-monospace, Menlo, monospace"; g.textBaseline = "middle";
  const B = S.built = build(), st = S.state;
  S.hover = S.mx >= 0 ? find(S.mx, S.my) : null;                  // the take under the pointer, from this round's data
  // header
  g.fillStyle = st && st.live ? C.red : C.green; g.fillText(st ? (st.live ? "LIVE" : "SAFE") : "…", 12, 17);
  g.fillStyle = C.text; g.fillText(st ? `${st.name}   ${st.automix}   iter ${st.iter}   takes ${st.loop_takes}   gate ${st.gate_db}dB` : "", 70, 17);
  g.textAlign = "right"; g.fillText(S.err ? "no space: " + S.err : (st ? st.phase : ""), W - 12 - 160, 17); g.textAlign = "left";
  // row labels and faint lines
  const { main, pin } = rowsShown();
  for (const [r, y] of B.slot) {
    const h = B.rh(r);
    g.fillStyle = st && r === st.iter ? C.text : C.dim; g.fillText(String(r), 8, y + h / 2);
    g.fillStyle = "#151515"; g.fillRect(B.x0, y + h / 2, W - lay.left - lay.right, 1);
  }
  // loop-end marker and playhead
  g.fillStyle = "#222"; g.fillRect(B.x0 + B.T * B.tw - 1, lay.head, 1, H - lay.head - lay.foot);
  if (st && st.phase === "play") { const c = (((st.tick - st.origin) % B.T) + B.T) % B.T; g.fillStyle = "rgba(255,255,255,.18)"; g.fillRect(B.x0 + c * B.tw, lay.head, B.tw, H - lay.head - lay.foot); }
  // ribbons: out of the loop first and faint, then in, then the lit one
  const lit = S.hover;
  for (const pass of [0, 1, 2]) {
    g.globalAlpha = pass === 0 ? 0.28 : 1;
    const bytake = new Map();
    for (const it of B.items) {
      const mine = lit && it.t.iter === lit.t.iter && it.t.off === lit.t.off, want = pass === 2 ? mine : (!mine && (it.t.st === 0 ? 0 : 1) === pass);
      if (!want) continue;
      const key = it.t.iter + "." + it.t.off + ":" + it.row; (bytake.get(key) || bytake.set(key, []).get(key)).push(it);
    }
    for (const seg of bytake.values()) ribbon(seg, pass === 2);
  }
  g.globalAlpha = 1;
  drawMeters(); drawFoot();
}
function ribbon(seg, bright) {                      // consecutive ticks of one take in one row: trapezoids between tick centres
  seg.sort((a, b) => a.col - b.col);
  if (bright) { g.shadowColor = "#fff"; g.shadowBlur = 10; }
  for (let i = 0; i < seg.length; i++) {
    const a = seg[i], b = seg[i + 1] && seg[i + 1].col === a.col + 1 ? seg[i + 1] : null, cy = a.y + a.h / 2;
    const ha = a.d > -150 ? thick(a.d, a.h) / 2 : 0;
    const hb = b ? (b.d > -150 ? thick(b.d, b.h) / 2 : 0) : ha;
    const xb = b ? b.x : a.x + a.tw * 0.5, xa = i === 0 || seg[i - 1].col !== a.col - 1 ? a.x - a.tw * 0.5 : a.x;
    if (ha === 0 && hb === 0) continue;
    g.fillStyle = bright ? lighten(color(Math.max(a.d, b ? b.d : a.d))) : color(Math.max(a.d, b ? b.d : a.d));
    g.beginPath(); g.moveTo(xa, cy - ha); g.lineTo(xb, cy - hb); g.lineTo(xb, cy + hb); g.lineTo(xa, cy + ha); g.closePath(); g.fill();
  }
  g.shadowBlur = 0;
}
function lighten(c) { return c === C.red ? "#ff9a90" : c === C.green ? "#9affb5" : "#fff0a0"; }

function drawMeters() {                             // in l/r, out l/r: four thin bars top right
  const m = S.meter, x = W - 150, y = 8;
  g.fillStyle = C.bg; g.fillRect(x - 4, 0, 160, lay.head);
  g.font = "10px ui-monospace, Menlo, monospace";
  [["in", 0], ["out", 2]].forEach(([n, i], r) => {
    g.fillStyle = C.dim; g.fillText(n, x - 2 - 4, y + 6 + r * 12);
    for (let ch = 0; ch < 2; ch++) {
      const d = m[i + ch], w = Math.max(0, Math.min(1, (d + 60) / 60)) * 56;
      g.fillStyle = "#111"; g.fillRect(x + 22 + ch * 62, y + r * 12, 56, 8);
      g.fillStyle = color(d); g.fillRect(x + 22 + ch * 62, y + r * 12, w, 8);
    }
  });
  g.font = "12px ui-monospace, Menlo, monospace";
}

function find(mx, my) {                             // the take under the pointer: its cell, the loudest in it, live over muted
  const B = S.built; if (!B) return null;
  let best = null;
  for (const it of B.items) {
    if (my < it.y || my >= it.y + it.h) continue;
    const c = Math.floor((mx - B.x0) / B.tw);
    if (c !== it.col || it.d < -150) continue;
    if (!best || it.t.st > best.t.st || (it.t.st === best.t.st && it.d > best.d)) best = it;
  }
  return best;
}
function drawFoot() {
  const y = H - lay.foot;
  g.fillStyle = C.bg; g.fillRect(0, y, W, lay.foot);
  g.fillStyle = "#222"; g.fillRect(0, y, W, 1);
  const h = S.hover; g.fillStyle = C.text;
  if (!h) { g.fillStyle = C.dim; g.fillText("hover over a take for its details", 12, y + lay.foot / 2); return; }
  const t = h.t, gr = S.grid.get(t.iter + "." + t.off), secs = (t.frames / 48000).toFixed(1);
  const gain = !gr || !gr.mixed ? "no mix" : `gain ${gr.gsh > 0 ? "+" : ""}${gr.gsh}  pan ${gr.pan > 0 ? "/" + gr.pan : gr.pan < 0 ? "\\" + -gr.pan : "0"}`;
  const state = t.st === 2 ? "recording" : t.st === 1 ? "in the loop" : "out of the loop";
  const parts = [`${t.iter}.${t.off}`, `${secs} s`, state, gain, gr && gr.mono ? "mono" : "stereo", `tick ${h.k}: ${h.d.toFixed(1)} dB`];
  if (gr && gr.pending) parts.push("move pending");
  g.fillStyle = color(h.d); g.fillRect(12, y + lay.foot / 2 - 5, 10, 10);
  g.fillStyle = C.text; g.fillText(parts.join("   ·   "), 30, y + lay.foot / 2);
}

// ---- events ----
cv.addEventListener("mousemove", e => { S.mx = e.offsetX; S.my = e.offsetY; draw(); });
cv.addEventListener("mouseleave", () => { S.mx = -1; S.hover = null; draw(); });
cv.addEventListener("wheel", e => { e.preventDefault();
  const { top } = rowsShown(); S.follow = false; S.top = Math.max(1, top + Math.sign(e.deltaY) * 3);
  if (S.top >= curIter() - S.rows + 1) { S.follow = true; S.top = null; } draw(); }, { passive: false });
function resize() { dpr = window.devicePixelRatio || 1; W = innerWidth; H = innerHeight; cv.width = W * dpr; cv.height = H * dpr; draw(); }
addEventListener("resize", resize);
resize(); pollCfg(); pollSlow(); pollMeter();
