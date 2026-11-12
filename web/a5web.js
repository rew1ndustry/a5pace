/* a5web: the browser shell for a5pace. The map, drawn on a canvas, with the keys of a5hell and buttons for the same things.
   Row = iteration, the loop across; a take is a ribbon whose thickness is its rms (one byte a 0.2 s tick, asppeaks.h) and whose
   colour is green / yellow / red by the thresholds of a5hell.cfg. Hover lights a take and puts its details on the bottom row; a click
   selects (shift adds); the mixing language is typed as in a5hell and parsed by a5web (a5mixer.c), not here. */
"use strict";
const TPS = 5;                                      // ticks a second
const C = { bg: "#000", text: "#9a9a9a", dim: "#555", btn: "#111", green: "#3ddc6b", yellow: "#e0c040", red: "#ff4a3d" };
const FONT = "12px ui-monospace, Menlo, monospace";
const cv = document.getElementById("c"), g = cv.getContext("2d");
let W = 0, H = 0, dpr = 1;

const S = { state: null, takes: [], pins: [], grid: new Map(), meter: [-200, -200, -200, -200, 0, 0], cfg: { green: -24, red: -6 },
            top: null, follow: true, hover: null, mx: -1, my: -1, err: "", rows: 20,
            sel: new Set(), cur: null, entry: "", msg: "", menu: null, buttons: [], tip: null, chain: Promise.resolve() };
const lay = { head: 62, foot: 52, left: 56, right: 12, rowh: 26 };

// ---- the control port, by http ----
async function get(path) {
  const r = await fetch(path, { cache: "no-store" });
  if (!r.ok) throw new Error((await r.text()).trim() || r.status);
  return (await r.text()).split("\n").filter(Boolean);
}
async function post(path) {                          // a write: a5web only takes these from this page (Origin)
  try { const r = await fetch(path, { method: "POST" }); return { ok: r.ok, text: (await r.text()).trim() }; }
  catch (e) { return { ok: false, text: "no answer from a5web" }; }
}
function say(t) { S.msg = t; draw(); }
const hexv = c => parseInt(c, 16);
function parseMap(lines) {                           // iter off frames in|out|open tick0 hex ; a long take comes in chunks
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
const key = t => t.iter + "." + t.off;

function loopTicks() { return S.state ? Math.max(1, Math.round(S.state.loop_frames / 9600)) : 40; }
function curIter() { return S.state ? S.state.iter : 1; }

async function pollCfg() {
  try { for (const l of await get("/api/cfg")) { const [k, v] = l.split(" "); if (k === "green_below_db") S.cfg.green = +v; if (k === "red_above_db") S.cfg.red = +v; } } catch (e) {}
}
let slowTimer = 0, slowBusy = false;
async function pollSlow() {                          // state, map, grid: once a second, and at once after an event or a key
  if (slowBusy) return;
  slowBusy = true; clearTimeout(slowTimer);
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
  slowBusy = false;
  draw();
  slowTimer = setTimeout(pollSlow, 1000);
}
function refreshSoon() { clearTimeout(slowTimer); slowTimer = setTimeout(pollSlow, 120); }
async function pollMeter() {
  try { S.meter = (await get("/api/meter"))[0].split(" ").map(Number); } catch (e) {}
  drawMeters();
  setTimeout(pollMeter, 200);
}
function watch() {                                    // the space's log as it is written
  const es = new EventSource("/api/watch");
  es.onmessage = e => {
    const m = /^@(\d+) (\S+) ?(.*)$/.exec(e.data); if (!m || m[2] === "top" || m[2] === "wrap") return;       // a tick and a pass are not news
    S.msg = m[2].replace(/_/g, " ") + (m[3] ? "  " + m[3].slice(0, 110) : "");
    refreshSoon(); draw();
  };
}

// ---- geometry: which rows are shown, where a take's tick lands ----
function rowsShown() {                               // main rows, then (if any) a gap and the pinned rows
  const it = curIter(), n = S.rows, top = S.follow || S.top === null ? Math.max(1, it - n + 1) : S.top;
  const main = []; for (let i = 0; i < n; i++) main.push(top + i);
  const pin = [...new Set(S.pins.map(t => t.iter))].sort((a, b) => a - b);
  return { main, pin, top };
}
function layout() {
  const npin = new Set(S.pins.map(t => t.iter)).size;
  const avail = H - lay.head - lay.foot - (npin ? 14 + npin * 20 : 0);
  S.rows = Math.max(4, Math.min(60, Math.floor(avail / 26)));
  lay.rowh = Math.max(14, Math.floor(avail / S.rows));
}
function takePos(t, k) { const T = loopTicks(), p = TPS * t.off + k; return { row: t.iter + Math.floor(p / T), col: p % T }; }
function color(d) { return d >= S.cfg.red ? C.red : d < S.cfg.green ? C.green : C.yellow; }
function thick(d, rowh) { const x = Math.min(1, Math.max(0, (d + 60) / 60)); return 1.5 + x * (rowh * 0.86 - 1.5); }

function build() {                                   // every ribbon segment of the shown takes, with its cell, so hover can find a take
  const T = loopTicks(), { main, pin } = rowsShown(), slot = new Map();
  main.forEach((r, i) => slot.set(r, lay.head + i * lay.rowh));
  pin.forEach((r, i) => slot.set(r, lay.head + main.length * lay.rowh + 14 + i * 20));
  const rh = r => pin.includes(r) && r > 89999 ? 20 : lay.rowh;
  const x0 = lay.left, tw = (W - lay.left - lay.right) / T, items = [];
  for (const t of [...S.takes, ...S.pins])
    for (let k = 0; k < t.pk.length; k++) {
      const { row, col } = takePos(t, k), y = slot.get(row);
      if (y === undefined) continue;
      items.push({ t, k, row, col, y, h: rh(row), x: x0 + (col + 0.5) * tw, d: db(t.pk[k]), tw });
    }
  return { items, slot, rh, x0, tw, T };
}

// ---- drawing ----
function draw() {
  layout();
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.fillStyle = C.bg; g.fillRect(0, 0, W, H);
  g.font = FONT; g.textBaseline = "middle"; g.textAlign = "left";
  const B = S.built = build(), st = S.state;
  S.hover = S.menu || S.mx < 0 ? null : find(S.mx, S.my);
  S.buttons = [];
  g.fillStyle = st && st.live ? C.red : C.green; g.fillText(st ? (st.live ? "LIVE" : "SAFE") : "…", 12, 17);
  g.fillStyle = C.text; g.fillText(st ? `${st.name}   ${st.automix}   iter ${st.iter}   takes ${st.loop_takes}   gate ${st.gate_db}dB   ${st.phase}` : "", 70, 17);
  if (S.err) { g.fillStyle = C.red; g.fillText("no space: " + S.err, 70 + 420, 17); }
  drawButtons();
  for (const [r, y] of B.slot) {
    const h = B.rh(r);
    g.fillStyle = st && r === st.iter ? C.text : C.dim; g.fillText(String(r), 8, y + h / 2);
    g.fillStyle = "#151515"; g.fillRect(B.x0, y + h / 2, W - lay.left - lay.right, 1);
  }
  g.fillStyle = "#222"; g.fillRect(B.x0 + B.T * B.tw - 1, lay.head, 1, H - lay.head - lay.foot);
  if (st && st.phase === "play") { const c = (((st.tick - st.origin) % B.T) + B.T) % B.T; g.fillStyle = "rgba(255,255,255,.18)"; g.fillRect(B.x0 + c * B.tw, lay.head, B.tw, H - lay.head - lay.foot); }
  // ribbons: out of the loop faint, then in, then the selected, then the one under the pointer
  const hv = S.hover && key(S.hover.t);
  for (const pass of [0, 1, 2, 3]) {
    g.globalAlpha = pass === 0 ? 0.28 : 1;
    const seg = new Map();
    for (const it of B.items) {
      const k = key(it.t), at = k === hv ? 3 : S.sel.has(k) ? 2 : it.t.st === 0 ? 0 : 1;
      if (at !== pass) continue;
      const sk = k + ":" + it.row; (seg.get(sk) || seg.set(sk, []).get(sk)).push(it);
    }
    for (const s of seg.values()) ribbon(s, pass >= 2, pass === 3 ? 14 : 6);
  }
  g.globalAlpha = 1;
  drawMeters(); drawFoot();
  if (S.menu) drawMenu();
}
function ribbon(seg, bright, blur) {                  // consecutive ticks of one take in one row: trapezoids between tick centres
  seg.sort((a, b) => a.col - b.col);
  if (bright) { g.shadowColor = "#fff"; g.shadowBlur = blur; }
  for (let i = 0; i < seg.length; i++) {
    const a = seg[i], b = seg[i + 1] && seg[i + 1].col === a.col + 1 ? seg[i + 1] : null, cy = a.y + a.h / 2;
    const ha = a.d > -150 ? thick(a.d, a.h) / 2 : 0, hb = b ? (b.d > -150 ? thick(b.d, b.h) / 2 : 0) : ha;
    const xb = b ? b.x : a.x + a.tw * 0.5, xa = i === 0 || seg[i - 1].col !== a.col - 1 ? a.x - a.tw * 0.5 : a.x;
    if (ha === 0 && hb === 0) continue;
    const c = color(Math.max(a.d, b ? b.d : a.d));
    g.fillStyle = bright ? lighten(c) : c;
    g.beginPath(); g.moveTo(xa, cy - ha); g.lineTo(xb, cy - hb); g.lineTo(xb, cy + hb); g.lineTo(xa, cy + ha); g.closePath(); g.fill();
  }
  g.shadowBlur = 0;
}
function lighten(c) { return c === C.red ? "#ff9a90" : c === C.green ? "#9affb5" : "#fff0a0"; }

function drawMeters() {                               // in l/r, out l/r: four thin bars top right
  const m = S.meter, x = W - 150, y = 8;
  g.fillStyle = C.bg; g.fillRect(x - 12, 0, 170, 32);
  g.font = "10px ui-monospace, Menlo, monospace"; g.textAlign = "left";
  [["in", 0], ["out", 2]].forEach(([n, i], r) => {
    g.fillStyle = C.dim; g.fillText(n, x - 8, y + 4 + r * 12);
    for (let ch = 0; ch < 2; ch++) {
      const d = m[i + ch], w = Math.max(0, Math.min(1, (d + 60) / 60)) * 56;
      g.fillStyle = "#111"; g.fillRect(x + 22 + ch * 62, y + r * 12, 56, 8);
      g.fillStyle = color(d); g.fillRect(x + 22 + ch * 62, y + r * 12, w, 8);
    }
  });
  g.font = FONT;
}

// ---- buttons: the same things as the keys ----
function button(x, y, label, hint, run) {
  const w = Math.ceil(g.measureText(label).width) + 16, h = 22;
  const over = S.mx >= x && S.mx < x + w && S.my >= y && S.my < y + h && !S.menu;
  g.fillStyle = over ? "#2a2a2a" : C.btn; g.fillRect(x, y, w, h);
  g.fillStyle = over ? "#ddd" : C.text; g.fillText(label, x + 8, y + h / 2 + 1);
  S.buttons.push({ x, y, w, h, run });
  if (over) S.tip = hint;
  return x + w + 6;
}
function drawButtons() {
  const st = S.state, y = 34; let x = 12;
  S.tip = null;
  x = button(x, y, st && st.live ? "go safe  s" : "go live  l", st && st.live ? "stop recording takes; the loop keeps playing" : "record takes again", () => act(st && st.live ? "safe" : "live"));
  x = button(x, y, "undo take  u", "take the last take out of the loop", () => act("undotake"));
  x = button(x, y, "undo iter  U", "take the last iteration out", () => act("undoiter"));
  x = button(x, y, "sync  r", "hunt the round trip again", () => act("sync"));
  x = button(x, y, "gate  g", "the gate: learn the floor, more or less sensitive", () => openGate());
  x = button(x, y, "mode  a", "loop, keep, swap, overlap", () => openMode());
  x = button(x, y, "history  h", "go back to how the loop was", () => openHistory());
  x = button(x, y, "wipe  w", "take the recorded takes out of the loop", () => openWipe());
  x = button(x, y, "takes  T", "every take as text", () => openTakes());
  x = button(x, y, "?", "the keys", () => openHelp());
}

function find(mx, my) {                               // the take under the pointer: its cell, the loudest in it, live over muted
  const B = S.built; let best = null; if (!B) return null;
  for (const it of B.items) {
    if (my < it.y || my >= it.y + it.h) continue;
    if (Math.floor((mx - B.x0) / B.tw) !== it.col || it.d < -150) continue;
    if (!best || it.t.st > best.t.st || (it.t.st === best.t.st && it.d > best.d)) best = it;
  }
  return best;
}
function rowAt(my) { const B = S.built; if (B) for (const [r, y] of B.slot) if (my >= y && my < y + B.rh(r)) return r; return null; }

function drawFoot() {
  const y = H - lay.foot;
  g.fillStyle = C.bg; g.fillRect(0, y, W, lay.foot);
  g.fillStyle = "#222"; g.fillRect(0, y, W, 1);
  g.textAlign = "left";
  const ln1 = y + 14, ln2 = y + 36;
  if (S.entry) { g.fillStyle = C.yellow; g.fillText("typing  " + S.entry + "▌   (a terminator . ! m p M P does it; esc cancels)", 12, ln1); }
  else { g.fillStyle = S.tip ? C.text : C.dim; g.fillText(S.tip || S.msg || (S.sel.size ? `${S.sel.size} selected  ·  type a gain +N -N, pan /N \\N, then . to queue or ! for now` : "click a take to select it; ? for the keys"), 12, ln1); }
  const h = S.hover;
  if (!h) { g.fillStyle = C.dim; g.fillText("hover over a take for its details", 12, ln2); return; }
  const t = h.t, gr = S.grid.get(key(t)), secs = (t.frames / 48000).toFixed(1);
  const gain = !gr || !gr.mixed ? "no mix" : `gain ${gr.gsh > 0 ? "+" : ""}${gr.gsh}  pan ${gr.pan > 0 ? "/" + gr.pan : gr.pan < 0 ? "\\" + -gr.pan : "0"}`;
  const state = t.st === 2 ? "recording" : t.st === 1 ? "in the loop" : "out of the loop";
  const parts = [key(t), `${secs} s`, state, gain, gr && gr.mono ? "mono" : "stereo", `tick ${h.k}: ${h.d.toFixed(1)} dB`];
  if (gr && gr.pending) parts.push("move pending");
  g.fillStyle = color(h.d); g.fillRect(12, ln2 - 5, 10, 10);
  g.fillStyle = C.text; g.fillText(parts.join("   ·   "), 30, ln2);
}

// ---- menus and text panels ----
function drawMenu() {
  const m = S.menu, n = m.items.length, w = Math.min(W - 20, m.w || 560), lh = 22, h = 56 + n * lh + 30;
  const x = Math.max(10, (W - w) / 2), y = Math.max(10, (H - h) / 2);
  g.fillStyle = "rgba(0,0,0,.72)"; g.fillRect(0, 0, W, H);
  g.fillStyle = "#0b0b0b"; g.fillRect(x, y, w, h); g.strokeStyle = "#333"; g.strokeRect(x + .5, y + .5, w, h);
  g.fillStyle = C.text; g.textAlign = "left"; g.fillText(m.title, x + 16, y + 22);
  m.rects = [];
  m.items.forEach((it, i) => {
    const iy = y + 46 + i * lh, on = it.run && i === m.at;
    if (it.run) { g.fillStyle = on ? "#222" : "#0b0b0b"; g.fillRect(x + 8, iy, w - 16, lh); m.rects.push({ x: x + 8, y: iy, w: w - 16, h: lh, i }); }
    g.fillStyle = it.run ? (on ? "#eee" : C.text) : C.dim; g.fillText((it.key ? it.key + "   " : "") + it.text, x + 16, iy + lh / 2);
  });
  g.fillStyle = C.dim; g.fillText(m.hint || "esc closes", x + 16, y + h - 16);
  m.box = { x, y, w, h };
}
function openMenu(title, items, hint, w) { S.menu = { title, items, hint, w, at: Math.max(0, items.findIndex(i => i.run)) }; draw(); }
function closeMenu() { S.menu = null; draw(); }
function runItem(it, ch, now) { closeMenu(); it.run(ch, now); }

function openGate() { openMenu(" gate", [
  { key: "l", text: "learn the floor again (when nothing is playing)", run: () => act("relearn") },
  { key: "+", text: "more sensitive (3 dB)", run: () => act("gate_more") },
  { key: "-", text: "less sensitive (3 dB)", run: () => act("gate_less") }]); }
function openMode() { openMenu(" mode", [
  { key: "l", text: "loop: every take goes into the loop", run: () => act("mode_loop") },
  { key: "k", text: "keep: takes are stored but not put in the loop", run: () => act("mode_keep") },
  { key: "p", text: "swap: successive takes swap channels", run: () => act("mode_swap") },
  { key: "e", text: "swap each: also between takes of one iteration", run: () => act("mode_swapeach") },
  { keys: "123456789", key: "1-9", text: "overlap: a take goes out when that many newer takes overlap it", run: ch => act("mode_overlap", "&n=" + (ch || 2)) }]); }
function openWipe() { openMenu(" wipe: take takes out of the loop (they stay in the store)", [
  { key: "y", text: "the recorded takes", run: () => act("wipe") },
  { key: "c", text: "all of them, the generated ones too (clicks, an imported file)", run: () => act("wipeall") }]); }
function openTakes() {
  get("/api/takes").then(ls => openMenu(" takes", ls.map(l => ({ text: l.length > 150 ? l.slice(0, 150) + "…" : l })), "esc closes", Math.min(W - 20, 1100)))
    .catch(e => say("takes: " + e.message));
}
function openHistory() {
  get("/api/history").then(ls => {
    const items = ls.map(l => { const t = l.split("\t"), seq = +t[1];
      return { text: `${t[0]}   ${t[2] || ""}`, run: (ch, now) => act("restore", "&seq=" + seq + (now ? "&now=1" : "")) }; });
    if (!items.length) items.push({ text: "(nothing yet)" });
    openMenu(" history, newest first: the loop as it was after each", items, "click or . puts the loop back from each take's next start;  shift-click or !  at once;  esc leaves", Math.min(W - 20, 900));
  }).catch(e => say("history: " + e.message));
}
function openHelp() { openMenu(" the keys", [
  "click a take to select it, shift-click adds; click an iteration number for the whole iteration",
  "arrows: left / right take to take, up / down row to row; shift adds to the selection; esc clears",
  "7  7.3  +2  -1  /1  \\1 : an iteration, a take (7.3 = iteration 7, offset 3), gain in bits, pan in bits; then a terminator:",
  "  .  queue for the take's next start     !  at once     m  mute     p  play     M  P  the same, at once",
  "  with nothing selected, the take under the pointer is the one",
  "l live   s safe   r sync   u undo take   U undo iteration   g gate   a mode   w wipe   h history   T takes as text",
  "mouse wheel, PgUp, PgDn, Home, End: scroll;  End follows the newest iteration",
  "not here yet: x export, f profile (use a5hell or a5export)"].map(text => ({ text })), "esc closes", 880); }

// ---- doing things ----
async function act(c, extra) {
  const r = await post("/api/do?c=" + c + (extra || ""));
  say(c.replace(/_/g, " ") + ": " + (r.ok ? "done" : r.text.split("\n").pop()));
  refreshSoon();
}
function targets() {                                    // the selection, else the take under the pointer
  if (S.sel.size) return [...S.sel];
  return S.hover ? [key(S.hover.t)] : [];
}
function typed(ch) {                                    // a character of the mixing language; a5web parses it
  S.chain = S.chain.then(async () => {
    S.entry += ch; draw();
    const r = await post("/api/mix?s=" + encodeURIComponent(S.entry) + "&t=" + encodeURIComponent(targets().join(",")));
    const t = r.text;
    if (t === "WAIT") return;
    const e = S.entry; S.entry = "";
    if (t === "BAD") say("not understood: " + e);
    else if (t === "NONE") say("nothing selected: click a take, or point at one");
    else if (r.ok && t.startsWith("OK")) { say(`mix ${e}: ${t.slice(3)} take(s) ` + (/[!MP]$/.test(e) ? "changed at once" : "queued for their next start")); refreshSoon(); }
    else say("mix: " + t);
  });
}

// ---- selection ----
function sortedTakes() { return [...S.takes].sort((a, b) => a.iter - b.iter || a.off - b.off); }
function selectTake(k, add) { if (!add) S.sel.clear(); if (add && S.sel.has(k)) S.sel.delete(k); else S.sel.add(k); S.cur = k; draw(); }
function selectIter(it, add) {
  if (!add) S.sel.clear();
  const ts = S.takes.filter(t => t.iter === it); ts.forEach(t => S.sel.add(key(t)));
  S.cur = ts.length ? key(ts[0]) : null; draw();
}
function move(dx, dy, add) {
  const ts = sortedTakes(); if (!ts.length) return;
  let i = ts.findIndex(t => key(t) === S.cur);
  if (i < 0) i = dx > 0 || dy > 0 ? -1 : ts.length;
  let j = i;
  if (dx) j = Math.max(0, Math.min(ts.length - 1, i + dx));
  else if (dy) {                                          // the nearest row with a take, its first take
    const it = i >= 0 && i < ts.length ? ts[i].iter : curIter();
    const cand = ts.filter(t => dy > 0 ? t.iter > it : t.iter < it);
    if (!cand.length) return;
    const near = dy > 0 ? cand[0].iter : cand[cand.length - 1].iter;
    j = ts.findIndex(t => t.iter === near);
  }
  const k = key(ts[j]); if (!add) S.sel.clear(); S.sel.add(k); S.cur = k; draw();
}
function scrollTo(top) {
  S.follow = false; S.top = Math.max(1, top);
  if (S.top >= curIter() - S.rows + 1) { S.follow = true; S.top = null; }
  refreshSoon(); draw();
}

// ---- events ----
cv.addEventListener("mousemove", e => { S.mx = e.offsetX; S.my = e.offsetY; draw(); });
cv.addEventListener("mouseleave", () => { S.mx = -1; draw(); });
cv.addEventListener("wheel", e => { e.preventDefault(); if (S.menu) return; scrollTo(rowsShown().top + Math.sign(e.deltaY) * 3); }, { passive: false });
cv.addEventListener("click", e => {
  const x = e.offsetX, y = e.offsetY;
  if (S.menu) {
    const m = S.menu, b = m.box, hit = (m.rects || []).find(r => x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h);
    if (hit) runItem(m.items[hit.i], null, e.shiftKey);
    else if (!b || x < b.x || x > b.x + b.w || y < b.y || y > b.y + b.h) closeMenu();
    return;
  }
  const bt = S.buttons.find(b => x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h);
  if (bt) { bt.run(); return; }
  if (x < lay.left) { const r = rowAt(y); if (r !== null) { selectIter(r, e.shiftKey); return; } }
  const h = find(x, y);
  if (h) selectTake(key(h.t), e.shiftKey); else if (!e.shiftKey) { S.sel.clear(); S.cur = null; draw(); }
});
addEventListener("keydown", e => {
  if (e.ctrlKey || e.metaKey || e.altKey) return;
  const k = e.key;
  if (S.menu) {                                           // a menu or panel has the keys
    const m = S.menu, runs = m.items.map((it, i) => i).filter(i => m.items[i].run);
    if (k === "Escape") { closeMenu(); e.preventDefault(); return; }
    if (k === "ArrowDown" || k === "ArrowUp") { const p = runs.indexOf(m.at), n = runs[Math.max(0, Math.min(runs.length - 1, p + (k === "ArrowDown" ? 1 : -1)))]; if (n !== undefined) m.at = n; draw(); e.preventDefault(); return; }
    if ((k === "Enter" || k === "." || k === "!") && m.items[m.at] && m.items[m.at].run) { runItem(m.items[m.at], null, k === "!"); e.preventDefault(); return; }
    const it = m.items.find(i => i.run && (i.keys ? i.keys.includes(k) : i.key === k));
    if (it) { runItem(it, k, false); e.preventDefault(); }
    return;
  }
  if (S.entry && k === "Escape") { S.entry = ""; draw(); return; }
  if (S.entry && k === "Backspace") { S.entry = S.entry.slice(0, -1); draw(); e.preventDefault(); return; }
  if (k.length === 1 && /[0-9+\-\/\\.!mMpP]/.test(k)) { typed(k); e.preventDefault(); return; }
  const add = e.shiftKey;
  switch (k) {
    case "ArrowLeft": move(-1, 0, add); break;       case "ArrowRight": move(1, 0, add); break;
    case "ArrowUp": move(0, -1, add); break;         case "ArrowDown": move(0, 1, add); break;
    case "Escape": S.sel.clear(); S.cur = null; draw(); break;
    case "PageUp": scrollTo(rowsShown().top - S.rows); break;   case "PageDown": scrollTo(rowsShown().top + S.rows); break;
    case "Home": scrollTo(1); break;                 case "End": S.follow = true; S.top = null; refreshSoon(); draw(); break;
    case "l": act("live"); break;                    case "s": act("safe"); break;
    case "r": act("sync"); break;                    case "g": openGate(); break;
    case "a": openMode(); break;                     case "w": openWipe(); break;
    case "h": openHistory(); break;                  case "T": openTakes(); break;
    case "?": openHelp(); break;
    case "u": case "U":
      if (S.state && S.state.automix === "keep") say("keep mode: undo has no effect (p / m put a take in or out of the loop)");
      else act(k === "u" ? "undotake" : "undoiter");
      break;
    case "x": case "f": say("x export and f profile are not in the web shell yet: use a5hell or a5export"); break;
    default: return;
  }
  e.preventDefault();
});
function resize() { dpr = window.devicePixelRatio || 1; W = innerWidth; H = innerHeight; cv.width = W * dpr; cv.height = H * dpr; draw(); }
addEventListener("resize", resize);
resize(); pollCfg(); pollSlow(); pollMeter(); watch();
