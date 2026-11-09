// Usage: tests/fetch_host_ui.sh && node tests/canvas_harness.mjs canvas.js <viz captures...>
// Device-faithful harness for modules/mist/canvas.js (schwung 1.9.2 contract).
import fs from "node:fs";
import vm from "node:vm";
// HOST_UI = folder fetched by tests/fetch_host_ui.sh (the DEVICE's own UI code)
const HOST_UI = process.env.HOST_UI || new URL("./host_ui", import.meta.url).pathname;
const { frameCtx } = await import(HOST_UI + "/shared/param_pages/frame_ctx.mjs");

const W = 128, H = 64;
const fb = new Uint8Array(W * H);
const texts = [];
const set = (x, y, c) => { if (x >= 0 && y >= 0 && x < W && y < H) fb[y * W + x] = c ? 1 : 0; };
// host C bindings (shadow context): bare snake_case globals, as on the device
const host = {
  clear_screen() { fb.fill(0); texts.length = 0; },
  set_pixel(x, y, c) { set(x, y, c); },
  fill_rect(x, y, w, h, c) { for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) set(x + i, y + j, c); },
  draw_rect(x, y, w, h, c) { for (let i = 0; i < w; i++) { set(x + i, y, c); set(x + i, y + h - 1, c); } for (let j = 0; j < h; j++) { set(x, y + j, c); set(x + w - 1, y + j, c); } },
  draw_line(x0, y0, x1, y1, c) { let dx = Math.abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -Math.abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (let g = 0; g < 4096; g++) { set(x0, y0, c); if (x0 === x1 && y0 === y1) break; const e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } } },
  print(x, y, s, c) { texts.push({ x, y, s: String(s) }); },
  text_width(s) { return String(s).length * 6; },
};
let reads = 0;
// EXACT copy of the device createCanvasRuntimeContext surface (shadow_ui.js ~23205)
function pageCtx(state) {
  return {
    width: W, height: H, state,
    clear() { host.clear_screen(); },
    setPixel(x, y, v) { host.set_pixel(Math.round(x), Math.round(y), v ? 1 : 0); },
    drawRect(x, y, w, h, v) { host.draw_rect(Math.round(x), Math.round(y), Math.round(w), Math.round(h), v ? 1 : 0); },
    fillRect(x, y, w, h, v) { host.fill_rect(Math.round(x), Math.round(y), Math.round(w), Math.round(h), v ? 1 : 0); },
    drawLine(a, b, c, d, v) { host.draw_line(Math.round(a), Math.round(b), Math.round(c), Math.round(d), v ? 1 : 0); },
    print(x, y, t, c = 1) { host.print(Math.round(x), Math.round(y), String(t), c ? 1 : 0); },
    measureText(t) { return host.text_width(String(t == null ? "" : t)); },
    now() { return Date.now(); }, random() { return Math.random(); }, shiftHeld() { return false; },
    close() { return true; }, getValue() { reads++; return ""; }, setValue() { return false; },
    getParam() { reads++; return null; }, setParam() { return false; }, sourcePath() { return ""; },
  };
}
// device canvasHookCtx: draw/tick get the ctx WITHOUT the accessors
const stripped = (c) => { const { getParam, setParam, getValue, setValue, close, ...rest } = c; return rest; };

// load canvas.js the way loadCanvasOverlayScript does: a script that sets globalThis.canvas_overlay
const src = fs.readFileSync(process.argv[2], "utf8");
const sandbox = { Math, Number, String, isFinite, Date, JSON, Array, Object };
sandbox.globalThis = sandbox;
vm.createContext(sandbox);
vm.runInContext(src, sandbox, { filename: "canvas.js" });
const ov = sandbox.canvas_overlay;
if (!ov) { console.log("FAIL: no globalThis.canvas_overlay"); process.exit(1); }

let fails = 0;
const call = (name, ctx, payload) => { try { return ov[name] && ov[name](ctx, payload || {}); } catch (e) { fails++; console.log(`FAIL: ${name} threw: ${e}`); } };

const frames = [];
for (const capfile of process.argv.slice(3)) {
  const lines = fs.readFileSync(capfile, "utf8").split("\n");
  const state = {};
  const full = pageCtx(state), draw = stripped(full);
  call("onOpen", full);
  let i = 0;
  for (const l of lines) {
    if (!l.startsWith("V\t")) continue;
    call("onValues", full, { values: { viz: l.slice(2) }, nowMs: 0 });
    for (let k = 0; k < 3; k++) call("tick", draw);
    host.clear_screen(); call("draw", draw);
    if (i % 30 === 15) frames.push({ name: `${capfile.split("/").pop()}#${i}`, fb: Array.from(fb), texts: texts.slice() });
    i++;
  }
  // robustness: garbage / empty / truncated values must not throw
  for (const bad of [undefined, "", "x", "1,2,3", "1," + "nan,".repeat(20), "1,0,0,0,0,0,0,0,0,0,0,0,0,1,99,0,0,99;;;1,2", "2,1,1"]) {
    call("onValues", full, { values: { viz: bad } }); host.clear_screen(); call("draw", draw);
  }
}
// widgets through the device's REAL frameCtx (clipping + clip counter)
const parent = { width: W, height: H, fillRect: host.fill_rect, print: host.print, textWidth: host.text_width };
const widgetCases = [
  ["custom:mist_window", { spike: 0.95, amount: 0.15 }, {}],
  ["custom:mist_window", { spike: 0.5, amount: 0.6 }, {}],
  ["custom:mist_range", { trg1_min: "0.00", trg1_max: "0.35", trg1_dest: "7", trg_env: "0.6" }, { roles: { min: "trg1_min", max: "trg1_max", dest: "trg1_dest" } }],
  ["custom:mist_range", { trg1_min: "0.54", trg1_max: "0.57", trg1_dest: "11", trg_env: "0.9" }, { roles: { min: "trg1_min", max: "trg1_max", dest: "trg1_dest" } }],
  ["custom:mist_range", { trg1_min: "-1", trg1_max: "1", trg1_dest: "0" }, { roles: { min: "trg1_min", max: "trg1_max", dest: "trg1_dest" } }],
  ["custom:mist_euclid", { eu_pos: "5,16,5,0,1" }, {}],
  ["custom:mist_euclid", { eu_pos: "2,12,7,10,0" }, {}],
  ["custom:mist_euclid", { eu_pos: "" }, {}],
];
const sizes = [[64, 17], [64, 12], [64, 26], [32, 10], [96, 17], [64, 6]];
let clip = 0;
for (const [kind, values, group] of widgetCases) for (const [w, h] of sizes) {
  host.clear_screen();
  const fctx = frameCtx(parent, { x: 0, y: 0, w, h });
  try { ov.widgetKinds[kind](fctx, { group: { kind, ...group }, values, nowMs: 0 }); }
  catch (e) { fails++; console.log(`FAIL: ${kind} ${w}x${h} threw: ${e}`); }
  if (fctx.clipped()) console.log(`  clip ${fctx.clipped()} ${kind} ${w}x${h} ${JSON.stringify(values).slice(0,50)}`); clip += fctx.clipped();
  if (w === 64 && h === 17) frames.push({ name: `${kind} ${JSON.stringify(values).slice(0, 40)}`, fb: Array.from(fb), texts: texts.slice(), crop: [w, h] });
}
fs.writeFileSync(process.env.OUT || "frames.json", JSON.stringify(frames));
console.log(`hooks+widgets run; throws=${fails}  draw-path reads=${reads}  widget clipped draws=${clip}  frames=${frames.length}`);
process.exit(fails ? 1 : 0);
