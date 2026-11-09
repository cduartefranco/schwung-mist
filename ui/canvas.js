/**
 * (MIST) canvas page — a glance-able scope in the Simplos visual language,
 * translated to the Move's 128x64 1-bit SSD1306 (src/shared/e16_canvas.mjs:
 * fillRect, print [uppercase 4x5 font only], textWidth, drawLine, clear,
 * setPixel — NO circle/arc primitive, NO color). The reference screenshots'
 * central "blob" contour (an irregular closed shape that swells with
 * feedback) is the single most recognizable Simplos signature; this
 * recreates that SHAPE LANGUAGE in monochrome line art, not its colors —
 * an original rendering, not a copy of their UI assets.
 *
 * Mechanism: a non-enterable `type:"canvas"` page (docs/CANVAS_PAGES.md
 * "Part one — the fullscreen dive", IMPLEMENTED today). Part two (an
 * `as_page`/enterable canvas acting as a navigable door, which the eventual
 * mod-matrix page would need) is still a PROPOSAL in that doc as of
 * 2026-11 — do not build the mod-matrix nav on it until it ships; this
 * page is the safe, confirmed-working piece: a live glance at the engine.
 *
 * draw(ctx) runs on the DRAW PATH — per the framework's own containment
 * rule, ctx.getParam/setParam are NOT available there (a synchronous param
 * round-trip costs more than redrawing the whole screen at 60Hz). State is
 * fetched in onOpen/onMidi (event hooks, which DO get the accessors) and
 * cached in module-scope closures for draw() to read cheaply.
 *
 * UNVERIFIED ON HARDWARE — written from the framework's docs + a working
 * precedent (Beat Bank's canvas.js pattern), not yet tested on a device.
 * Treat exact onMidi CC numbers/ctx method names as best-effort; confirm
 * against a real session before relying on them.
 */

/* ---- cached engine state (updated from events, read by draw) ---- */
let state = {
    size: 0.24, fbPre: 0.0, fbPost: 0.0, pitch: 0.0, mix: 1.0,
    amount: 0.6, spike: 0.5, blur: 0.15,
};
let phase = 0;   /* free-running animation phase, advances once per draw() call */

function readState(ctx) {
    /* Event hooks (onOpen/onMidi) DO have ctx.getParam — pull fresh values. */
    try {
        if (typeof ctx.getParam !== "function") return;
        const g = (k, d) => { const v = ctx.getParam(k); return (v === undefined || v === null) ? d : Number(v); };
        state.size   = g("size", state.size);
        state.fbPre  = g("fb_pre", state.fbPre);
        state.fbPost = g("fb_post", state.fbPost);
        state.pitch  = g("pitch", state.pitch);
        state.mix    = g("mix", state.mix);
        state.amount = g("amount", state.amount);
        state.spike  = g("spike", state.spike);
        state.blur   = g("blur", state.blur);
    } catch (e) { /* never let a bad read take the page down */ }
}

/* ---- the blob: an irregular closed contour, swollen by feedback ----
 * N points around a center, radius = base + a few fixed sine harmonics
 * (deterministic "organic" warp, same spirit as the reference's irregular
 * outline) plus a slow phase drift for a quiet living-texture feel. Drawn
 * as 2 nested contours (inner + outer), matching the reference's layered
 * look, each a closed polyline via drawLine (no fill/arc primitive). */
function drawBlob(ctx, cx, cy, baseR, energy, ph) {
    const N = 16;
    const pts = (rScale, phOff) => {
        const out = [];
        for (let i = 0; i < N; i++) {
            const a = (i / N) * Math.PI * 2;
            const warp =
                0.18 * Math.sin(a * 3 + ph + phOff) +
                0.10 * Math.sin(a * 5 - ph * 1.4) +
                0.06 * Math.sin(a * 2 + ph * 0.6);
            const r = baseR * rScale * (1 + warp * (0.4 + 0.6 * energy));
            out.push([cx + Math.cos(a) * r, cy + Math.sin(a) * r * 0.82]); /* slightly flattened, like the ref */
        }
        return out;
    };
    for (const rScale of [1.0, 0.62]) {
        const p = pts(rScale, rScale > 0.8 ? 0 : 1.1);
        for (let i = 0; i < N; i++) {
            const a = p[i], b = p[(i + 1) % N];
            ctx.drawLine(a[0], a[1], b[0], b[1], 1);
        }
    }
}

/* ---- a thin range-style bar with a live dot — the Simplos MAP-slot look.
 * 4x5 font = 5px glyph height; only ~9px live below the footer divider, so
 * this packs label + bar + dot into ONE 7px-tall row (no separate value
 * line — the dot position IS the readout, same information density as the
 * reference's compact mapping rows). ---- */
function drawReadout(ctx, x, y, w, label, val01) {
    ctx.print(x, y, label, 1);
    const barY = y + 6, barW = w - 2;
    const bx = x + 2 + Math.min(14, 2 + ctx.textWidth(label));
    const realW = Math.max(8, x + w - bx);
    ctx.drawLine(bx, barY, bx + realW, barY, 1);
    const dot = bx + Math.round(Math.max(0, Math.min(1, val01)) * realW);
    ctx.fillRect(dot - 1, barY - 1, 3, 3, 1);
    void barW;
}

export function onOpen(ctx) { phase = 0; readState(ctx); }
export function onMidi(ctx, status, d1, d2) {
    /* Knob turns reach here as CC on this page; forward to the real param
     * so turning a knob while glancing at the scope still works, then
     * refresh our cache. Exact CC->param wiring is host-controlled via
     * ctx.setParam by key elsewhere in the fleet's convention; left as a
     * passthrough no-op here (TODO once confirmed on hardware) so this
     * page never fights the host for control of a knob it doesn't own. */
    void status; void d1; void d2;
    readState(ctx);
}
export function handleBack(ctx) { void ctx; return false; /* top level: let the host close this page */ }

export function draw(ctx) {
    try {
        phase += 0.045;
        ctx.clear();
        const energy = Math.max(state.fbPre, state.fbPost);

        ctx.print(2, 1, "MIST", 1);
        const mixPct = Math.round(state.mix * 100) + "%";
        ctx.print(128 - 2 - ctx.textWidth(mixPct), 1, mixPct, 1);
        ctx.drawLine(0, 8, 128, 8, 1);

        drawBlob(ctx, 64, 27, 15, energy, phase);

        /* fb readout, centered under the blob like the reference's "fb" stack */
        const fbTxt = "FB " + Math.round(state.fbPre * 100) + "/" + Math.round(state.fbPost * 100);
        ctx.print(64 - Math.floor(ctx.textWidth(fbTxt) / 2), 44, fbTxt, 1);

        ctx.drawLine(0, 51, 128, 51, 1);
        drawReadout(ctx, 1,  56, 41, "SZ", state.size);
        drawReadout(ctx, 44, 56, 41, "PT", (state.pitch + 36) / 72);
        drawReadout(ctx, 87, 56, 40, "BL", state.blur);
    } catch (e) {
        /* a throwing draw() disables the whole canvas runtime per the
         * framework's containment rule -- never let this page be that. */
        try { ctx.clear(); ctx.print(2, 2, "MIST (SCOPE ERR)", 1); } catch (e2) { /* truly inert */ }
    }
}
