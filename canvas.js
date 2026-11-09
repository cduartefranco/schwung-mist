/*
 * (MIST) canvas.js — Simplos-style graphics for the Move's 128x64 1-bit screen.
 *
 * ONE FILE, TWO SURFACES (both loaders look for <module>/canvas.js):
 *   1. the "Scope" page (a type:"canvas" param): the feedback BLOB — nested,
 *      irregular contours that swell with feedback and wobble with blur — with
 *      the live grains orbiting it, plus Simplos-like readouts.
 *   2. knob-cell widgets: custom:mist_window (grain window over Amount/Spike),
 *      custom:mist_range (Simplos range slider: min/max handles + live dot),
 *      custom:mist_euclid (step strip + playhead).
 *
 * HOST CONTRACT (schwung 1.9.2, read from the device's shadow_ui.js — v0.2's
 * canvas crash-looped because it broke both rules below):
 *   - the script sets globalThis.canvas_overlay; ES `export`s are NOT read.
 *   - draw/tick get a ctx WITHOUT getParam/setParam: never read params while
 *     drawing. The page's data arrives in onValues({values}) from the declared
 *     extra_keys ("viz", one compact snapshot); widgets get values as an argument.
 *   - the page ctx has drawLine + no circles; the widget ctx has line + circles.
 *     Everything below goes through the adapters L()/P()/circle(), so it runs
 *     on either.
 *   - one throw disables the overlay/widget for the session, so be defensive:
 *     every number goes through num() and every array access is bounds-checked.
 */

function num(v, d) {
    var n = Number(v);
    return isFinite(n) ? n : d;
}
function clamp(v, a, b) { return v < a ? a : (v > b ? b : v); }

/* ---- drawing adapters (page ctx vs widget frame ctx) ---- */
function L(ctx, x0, y0, x1, y1) {
    if (typeof ctx.line === "function") ctx.line(x0, y0, x1, y1, 1);
    else if (typeof ctx.drawLine === "function") ctx.drawLine(x0, y0, x1, y1, 1);
}
function P(ctx, x, y, c) {
    if (x < 0 || y < 0 || x >= ctx.width || y >= ctx.height) return;
    if (typeof ctx.setPixel === "function") ctx.setPixel(Math.round(x), Math.round(y), c === 0 ? 0 : 1);
    else ctx.fillRect(Math.round(x), Math.round(y), 1, 1, c === 0 ? 0 : 1);
}
function circle(ctx, cx, cy, r) {               /* hollow, any ctx */
    if (r <= 0.6) { P(ctx, cx, cy, 1); return; }
    if (typeof ctx.drawCircle === "function") { ctx.drawCircle(Math.round(cx), Math.round(cy), Math.round(r), 1); return; }
    var n = Math.max(8, Math.round(r * 6));
    for (var i = 0; i < n; i++) {
        var a = i / n * 6.283185;
        P(ctx, cx + Math.cos(a) * r, cy + Math.sin(a) * r, 1);
    }
}
function disc(ctx, cx, cy, r) {                 /* filled, any ctx */
    if (typeof ctx.fillCircle === "function") { ctx.fillCircle(Math.round(cx), Math.round(cy), Math.round(r), 1); return; }
    var R = Math.round(r);
    for (var dy = -R; dy <= R; dy++) {
        var dx = Math.floor(Math.sqrt(Math.max(0, R * R - dy * dy)));
        ctx.fillRect(Math.round(cx) - dx, Math.round(cy) + dy, dx * 2 + 1, 1, 1);
    }
}
function text(ctx, x, y, s) { if (typeof ctx.print === "function") ctx.print(Math.round(x), Math.round(y), String(s), 1); }
function tw(ctx, s) {
    s = String(s);
    if (typeof ctx.measureText === "function") return ctx.measureText(s);
    if (typeof ctx.textWidth === "function") return ctx.textWidth(s);
    return s.length * 6;
}

/* the reference grain window (dsp build_window / mist_render.py grain_window) */
function windowAt(x, spike, amount) {
    var atk = 0.02 + 0.33 * (1 - spike), ka = 0.3 + 3 * spike;
    if (x < atk) return Math.pow(x / atk, 1 / ka);
    var t = (x - atk) / (1 - atk);
    return (1 - amount) * (1 - t) + amount * (0.5 + 0.5 * Math.cos(Math.PI * t));
}

/* euclid (same Bresenham rule as the DSP) */
function euHit(i, k, n) { return n > 0 && ((i * k) % n) < k; }

/* ---------------------------------------------------------------- */
/* Scope page                                                         */
/* ---------------------------------------------------------------- */

function parseViz(s) {
    var out = { ok: false, grains: [] };
    if (typeof s !== "string" || s.length < 3) return out;
    var parts = s.split(";");
    var h = parts[0].split(",");
    if (h.length < 18 || h[0] !== "1") return out;
    out.size = num(h[1], 220); out.blur = num(h[2], 0); out.spike = num(h[3], 0.5);
    out.amount = num(h[4], 0.6); out.pitch = num(h[5], 0); out.fbPre = num(h[6], 0);
    out.fbPost = num(h[7], 0); out.mix = num(h[8], 0.5); out.density = num(h[9], 22);
    out.trg = num(h[10], 0); out.inPk = num(h[11], 0); out.outPk = num(h[12], 0);
    out.mode = num(h[13], 0); out.step = num(h[14], 0); out.n = num(h[15], 8);
    out.k = num(h[16], 5); out.rot = num(h[17], 0);
    for (var i = 1; i < parts.length && i <= 24; i++) {
        var g = parts[i].split(",");
        if (g.length < 5) continue;
        out.grains.push({ id: num(g[0], 0), ph: num(g[1], 0) / 99, st: num(g[2], 0),
                          amp: num(g[3], 0) / 9, pan: num(g[4], 0) / 9 });
    }
    out.ok = true;
    return out;
}

/* smooth pseudo-random contour: a few slow sines per contour, phases drifting */
function blobR(theta, base, wob, t, layer) {
    var r = 1
        + wob * 0.16 * Math.sin(2 * theta + t * 0.7 + layer * 1.3)
        + wob * 0.11 * Math.sin(3 * theta - t * 1.1 + layer * 0.4)
        + wob * 0.07 * Math.sin(5 * theta + t * 1.7 + layer * 2.1)
        + 0.04 * Math.sin(7 * theta - t * 0.5);
    return base * r;
}

/* blob size + wobble; shared by the grain orbit so grains always sit outside it */
function blobBase(v) {
    var fb = clamp(v.fbPre + v.fbPost, 0, 1.6) / 1.6;
    var lvl = clamp(v.outPk * 1.4, 0, 1);
    return 8 + 8 * fb + 2 * lvl + 3 * clamp(v.trg, 0, 1);   /* swells with feedback, pulses on hits */
}
function blobWob(v) { return 0.3 + 0.7 * clamp(v.blur, 0, 1) + 0.3 * clamp(v.trg, 0, 1); }

function drawBlob(ctx, cx, cy, v, t) {
    var base = blobBase(v), wob = blobWob(v);
    /* NESTED, never crossing (the reference look): every ring is the SAME contour
       scaled, only gently counter-rotated — a star-shaped curve scaled about its
       centre cannot intersect itself. */
    var N = 48, layers = [1.0, 0.76, 0.54, 0.32];
    for (var li = 0; li < layers.length; li++) {
        var px = 0, py = 0, fx = 0, fy = 0;
        for (var i = 0; i <= N; i++) {
            var th = i / N * 6.283185;
            var r = blobR(th + li * 0.12, base * layers[li], wob, t, 0);
            var x = cx + Math.cos(th) * r, y = cy + Math.sin(th) * r * 0.92;
            if (i === 0) { fx = x; fy = y; } else L(ctx, Math.round(px), Math.round(py), Math.round(x), Math.round(y));
            px = x; py = y;
        }
        L(ctx, Math.round(px), Math.round(py), Math.round(fx), Math.round(fy));
    }
    /* the soft outer "glow" of the reference, as a sparse dither ring */
    for (var j = 0; j < 64; j++) {
        var th2 = j / 64 * 6.283185 + t * 0.15;
        var rr = blobR(th2, base * 1.18, wob, t, 0) + ((j * 7) % 3);
        if ((j & 1) === 0) P(ctx, cx + Math.cos(th2) * rr, cy + Math.sin(th2) * rr * 0.92, 1);
    }
}

function drawGrains(ctx, cx, cy, v, t) {
    /* orbit just outside the blob's widest point (blobR peaks at ~1+0.38*wob) */
    var ring = Math.min(26, blobBase(v) * (1 + 0.38 * blobWob(v)) + 4);
    for (var i = 0; i < v.grains.length; i++) {
        var g = v.grains[i];
        /* each grain owns a stable angle (golden angle by id), travels a little
           along its life, sits further out the higher it is pitched */
        var ang = (g.id * 2.39996) % 6.283185 + g.ph * 0.9 + g.pan * 0.4;
        var rad = ring + clamp(g.st, -24, 24) * 0.12;      /* higher pitch -> further out */
        var x = cx + Math.cos(ang) * rad, y = cy + Math.sin(ang) * rad * 0.92;
        var r = 0.8 + 2.4 * g.amp;
        if (x < 1 || x > 62 || y < 1 || y > 62) continue;
        circle(ctx, x, y, r);
        if (g.amp > 0.75) P(ctx, x, y, 1);
    }
}

function readout(ctx, x, y, label, value) {
    text(ctx, x, y, label);
    text(ctx, x + 22, y, value);
}

function pct(x) { return Math.round(clamp(x, 0, 9.99) * 100) + "%"; }
function p2(x) { return String(Math.round(clamp(x, 0, 9.99) * 100)); }

function drawScope(ctx, st) {
    var v = st.viz;
    var t = st.t || 0;
    if (!v || !v.ok) {
        text(ctx, 70, 28, "waiting");
        drawBlob(ctx, 32, 32, { fbPre: 0, fbPost: 0.3, outPk: 0, trg: 0, blur: 0.2 }, t);
        return;
    }
    drawBlob(ctx, 32, 32, v, t);
    drawGrains(ctx, 32, 32, v, t);

    /* right column: Simplos-style readouts */
    var x = 66;
    readout(ctx, x, 1,  "fb",   p2(v.fbPre) + "/" + p2(v.fbPost));     /* pre/post, %: fits 6 chars */
    readout(ctx, x, 10, "size", Math.round(v.size) + "ms");
    readout(ctx, x, 19, "blur", pct(v.blur));
    readout(ctx, x, 28, "spk",  pct(v.spike));
    readout(ctx, x, 37, "pch",  (v.pitch > 0 ? "+" : "") + v.pitch.toFixed(1));

    /* bottom: euclid strip (euclid mode) or density, + input/output meters */
    var by = 49;
    if (v.mode === 1 && v.n > 0) {
        var n = Math.min(16, v.n), cw = Math.floor(60 / n);
        for (var i = 0; i < n; i++) {
            var hit = euHit((i + v.rot) % n, v.k, n);
            var sx = x + i * cw;
            if (hit) ctx.fillRect(sx, by, Math.max(1, cw - 1), 4, 1);
            else P(ctx, sx + Math.floor((cw - 1) / 2), by + 3, 1);
            if (i === (v.step % n)) ctx.fillRect(sx, by + 6, Math.max(1, cw - 1), 1, 1);
        }
    } else {
        text(ctx, x, by - 1, Math.round(v.density) + "/s");
    }
    /* meters: in (top) / out (bottom), 60 px */
    var mi = Math.round(60 * clamp(v.inPk, 0, 1)), mo = Math.round(60 * clamp(v.outPk, 0, 1));
    if (mi > 0) ctx.fillRect(x, 59, mi, 1, 1);
    if (mo > 0) ctx.fillRect(x, 62, mo, 1, 1);
    /* trigger flash: a short tick mark at the blob's top */
    if (v.trg > 0.15) ctx.fillRect(30, 0, 5, Math.max(1, Math.round(3 * v.trg)), 1);
}

/* ---------------------------------------------------------------- */
/* knob-cell widgets                                                  */
/* ---------------------------------------------------------------- */

function W_window(ctx, payload) {
    var vals = (payload && payload.values) || {};
    var w = ctx.width, h = ctx.height;
    if (w < 8 || h < 5) return;
    var spike = clamp(num(vals.spike, 0.5), 0, 1), amount = clamp(num(vals.amount, 0.6), 0, 1);
    var py = h - 1;
    ctx.fillRect(0, h - 1, w, 1, 1);                           /* baseline */
    for (var x = 0; x < w; x++) {
        var y = (h - 1) - Math.round(windowAt(x / (w - 1), spike, amount) * (h - 2));
        if (x > 0) L(ctx, x - 1, py, x, y);
        if ((x & 1) === 0 && y < h - 1) ctx.fillRect(x, y, 1, (h - 1) - y, 1);   /* dithered fill (no 0-height rects: the frame ctx counts them as overflow) */
        py = y;
    }
}

var DEST = ["off", "size", "amount", "spike", "blur", "pitch", "fb pre", "fb post", "mix", "width", "vol", "density", "spread", "scatter"];

function W_range(ctx, payload) {
    var vals = (payload && payload.values) || {};
    var keys = (payload && payload.group && payload.group.roles) || {};
    var w = ctx.width, h = ctx.height;
    if (w < 16 || h < 7) return;
    var mn = clamp(num(vals[keys.min], 0), -1, 1), mx = clamp(num(vals[keys.max], 0), -1, 1);
    var di = Math.round(num(vals[keys.dest], 0));
    var env = clamp(num(vals.trg_env, 0), 0, 1);
    var pad = 4, span = w - 2 * pad - 1;
    var X = function (u) { return pad + Math.round((u + 1) / 2 * span); };
    var y = h - 5;
    /* label: destination name */
    if (h >= 13) text(ctx, 0, 0, di > 0 && di < DEST.length ? DEST[di] : "off");
    /* track: dashed outside the range, solid inside (the reference's look) */
    var a = Math.min(X(mn), X(mx)), b = Math.max(X(mn), X(mx));
    for (var x = pad; x <= pad + span; x++) {
        if (x >= a && x <= b) P(ctx, x, y, 1);
        else if ((x % 3) === 0) P(ctx, x, y, 1);
    }
    P(ctx, X(0), y - 2, 1); P(ctx, X(0), y + 2, 1);           /* centre (knob position) tick */
    circle(ctx, X(mn), y, 2);                                    /* min handle: small ring */
    circle(ctx, X(mx), y, 3);                                    /* max handle: bigger ring */
    if (di > 0) disc(ctx, X(mn + (mx - mn) * env), y, 1.6);     /* live dot */
}

function W_euclid(ctx, payload) {
    var vals = (payload && payload.values) || {};
    var w = ctx.width, h = ctx.height;
    if (w < 16 || h < 5) return;
    var p = String(vals.eu_pos || "").split(",");
    var step = num(p[0], 0), n = num(p[1], 8), k = num(p[2], 5), rot = num(p[3], 0), mode = num(p[4], 0);
    if (!(n > 0)) n = 8;
    n = Math.min(16, n);
    var cw = Math.max(2, Math.floor(w / n)), bh = Math.max(2, h - 3);
    for (var i = 0; i < n; i++) {
        var sx = i * cw;
        if (euHit((i + rot) % n, k, n)) {
            if (mode === 1) ctx.fillRect(sx, 0, cw - 1, bh, 1);
            else {                                   /* Free mode: hollow = pattern parked */
                ctx.fillRect(sx, 0, cw - 1, 1, 1); ctx.fillRect(sx, bh - 1, cw - 1, 1, 1);
                ctx.fillRect(sx, 0, 1, bh, 1);     ctx.fillRect(sx + cw - 2, 0, 1, bh, 1);
            }
        } else {
            P(ctx, sx + Math.floor((cw - 1) / 2), bh - 1, 1);
        }
        if (mode === 1 && i === (step % n)) ctx.fillRect(sx, h - 1, cw - 1, 1, 1);
    }
}

globalThis.canvas_overlay = {
    /* page lifecycle — events may read; draw/tick may not */
    onOpen: function (ctx) {
        if (!ctx.state) return;
        ctx.state.t = 0;
        ctx.state.viz = null;
    },
    onValues: function (ctx, payload) {
        if (!ctx.state) return;
        var vals = (payload && payload.values) || {};
        ctx.state.viz = parseViz(vals.viz);
    },
    tick: function (ctx) {
        if (ctx.state) ctx.state.t = (num(ctx.state.t, 0) + 0.09) % 10000;
    },
    draw: function (ctx) {
        drawScope(ctx, ctx.state || {});
        return true;
    },
    widgetKinds: {
        "custom:mist_window": W_window,
        "custom:mist_range": W_range,
        "custom:mist_euclid": W_euclid,
    },
};
