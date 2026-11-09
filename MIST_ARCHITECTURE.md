# (MIST) — granular feedback processor · Architecture & Pre-Plan

Written for: the ABLENTON dev (future-me + the user) as the build plan for a schwung
`audio_fx` granular processor inspired by **Simplos** (Kentaro Tools,
https://kentaro.tools/simplos). Personal-use re-creation of the *behavior* — an original
DSP engine, not a port of Simplos's Max/gen~ patch or any of its code.

Working name: **`(MIST)`** (granular + feedback + airy/spatial; pairs with our existing
`GRAIN` generator). Rename freely — it only touches `module.json` + catalog-cache
([[custom-module-naming]]).

Primary target: **Ableton Move** (schwung `audio_fx`, in-place int16@48k). Secondary via the
portable core: offline Python renderer (A/B by ear), MPC Live 3 (future VST2), KICK (future).

---

## 1. What it is (and isn't)

**Is:** a granular *processor* — it sits on a track, captures incoming audio into a ring
buffer, and re-plays it as a cloud of windowed, pitch-shifted grains with a **tight feedback
loop**. A pre-/post-feedback **blend** morphs it from a dry grain texture into a spatial delay
instrument. Every parameter is modulatable by up to **10 built-in modulators**.

**The Simplos character to hit (by ear):**
- *Minimal feedback latency* — the ring buffer length tracks the grain size, so the shortest
  feedback path ≈ one grain. This is the signature "immediate, responsive" feel.
- *Crisp feedback textures* — feedback is clean and controllable, not a mushy wash.
- *Clean, articulate pitch shifting* — grain-based, artifact-light.
- *Window curve* shapes attack/decay — from percussive (sharp attack) to rounded (smooth).
- *Pre/post blend* — the knob that turns a grain cloud into an expansive, airy spatial delay.

**Isn't:** a 1:1 clone of Simplos (its exact gen~ topology is proprietary; we match feel). Not
a sound generator (that's our separate [[grain-module]] `GRAIN`). No lifting of Kentaro's code.

---

## 2. Portability model (STANDING RULE — [[build-platforms-and-portability]])

```
            ┌─────────────────────────────┐
            │  mist_core  (pure DSP)       │  float I/O, no host deps, no malloc in process
            │  mist_core.h / mist_core.c   │  grain scheduler + ring buffer + feedback + mods
            └──────────────┬──────────────┘
        ┌──────────────────┼───────────────────┬──────────────────┐
        ▼                  ▼                    ▼                  ▼
  mist_movefx.c      mist_render.py        mist_vst2.cpp       mist_kick.*
  (schwung           (offline A/B,         (MPC3, future)      (future)
   audio_fx,          mirrors grain-proto.py)
   int16@48k)
```

- **Core** is the only place DSP lives. ~80–90% reused everywhere.
- **Huge head start:** [[grain-module]] already has a working grain engine (`grain-proto.py`
  reference + running `dsp.so`). `mist_core` = that grain scheduler + an **adaptive feedback
  ring buffer** + the **pre/post blend** + the **mod engine**. We are extending, not starting.
- Wrapper = thin glue: int16↔float, param mapping, UI, host transport/tempo.

---

## 3. Module layout (proposed)

```
modules/mist/
  dsp/
    mist_core.h          # public core API (below)
    mist_core.c          # grain scheduler + ring buffer + feedback + blend + mod engine
    mist_grains.h        # grain voice pool (reuse/share with GRAIN)
    mist_mods.h          # 10 modulator generators + routing matrix
  wrappers/
    move/mist_movefx.c   # audio_fx_api glue (move_audio_fx_init_v2, int16@48k in-place)
    move/canvas.js       # mod-matrix routing UI (Beat Bank-style canvas page)
  tools/
    mist_render.py       # importlib-loadable offline renderer (A/B, by ear)
  scripts/               # fork-CI Dockerfile (aarch64 cross-gcc) -> dsp.so
  module.json  help.json
  MIST_ARCHITECTURE.md
```

---

## 4. Core API (host-agnostic, real-time safe)

```c
typedef struct mist mist_t;               /* opaque; fixed-size, caller-allocated/pooled */

mist_t *mist_create(float sample_rate, int max_grains);  /* allocates ring + grain pool ONCE */
void    mist_destroy(mist_t*);
void    mist_reset(mist_t*);              /* clear ring + kill grains (transport stop/preset) */

/* control-rate, call any time (NOT per-sample) */
void    mist_set_param(mist_t*, int param_id, float value01);   /* base params, normalized 0..1 */
void    mist_set_tempo(mist_t*, float bpm);                     /* sync LFOs / seq mod          */
/* modulation routing (the matrix) */
void    mist_mod_config(mist_t*, int mod_id, int shape, float rate01, float depth01, int sync);
/* range-based routing (Simplos model): a mod slot sweeps dest between min01..max01.
   dest_param may be another modulator's param -> inter-modulator routing. */
void    mist_mod_route (mist_t*, int mod_id, int slot /*0..3*/, int dest_param,
                        float min01, float max01);
void    mist_mod_clear (mist_t*, int mod_id, int slot);
void    mist_snapshot  (mist_t*, int abcd /*0..3*/, int store /*1=save,0=recall*/);  /* A/B/C/D morph */

/* audio-rate, real-time: NO malloc/lock/IO. Deinterleaved stereo float, in-place OK. */
void    mist_process(mist_t*, float* L, float* R, int nframes);
```

**RT rules:** all allocation in `mist_create` (ring buffer sized to the *max* grain size, grain
pool to `max_grains`). `mist_process` is lock-free, denormal-guarded, bounded grain count.

---

## 5. Parameter model (base params, before modulation)

Normalized 0..1 everywhere; the core de-normalizes. **These are the *real* Simplos destination
names** read off the mapping racks (so a preset/muscle-memory translates). All are modulatable.

| id | Param (Simplos name) | Meaning | Group |
|----|----------------------|---------|-------|
| 0 | TRIGGER (`main trigger`) | grain trigger / gate rate (how often grains fire) | Grain |
| 1 | SIZE (`main size`) | grain length; **also sets the min feedback delay** | Grain |
| 2 | AMOUNT (`main amount`) | window-**curve** amount (⌁ icon — attack/decay shaping) | Grain |
| 3 | SPIKE (`main spike`) | window attack spike / transient sharpness (→\| icon) | Grain |
| 4 | BLUR (`main blur`) | grain position/time smear + decay rounding (⦀ icon) | Grain |
| 5 | PITCH (`main pitch`) | grain pitch in semitones (range runs well past ±12, saw 36 st) | Grain |
| 6 | FB-PRE (`fb pre-wet`) | pre-feedback wet level (dry grain cloud into the loop) | Feedback |
| 7 | FB-POST (`fb post-wet`) | post-feedback wet level (the spatial-delay tail) | Feedback |
| 8 | WIDTH (`util width`) | stereo width (`wdth` knob) | Space/Util |
| 9 | VOL (`util vol`) | output level (`vol` knob) | Util |
| 10 | MIX (`util mix`) | dry ↔ wet (`mix` knob) | Util |

**Notes from the UI:** `fb` displays as two stacked values = **FB-PRE / FB-POST** (the pre/post
blend is two separate routable params, not one crossfade). Top-of-engine extras seen: **FREEZE**
(∞, holds the buffer) and **REVERSE** (↶) — add as toggles. The engine readouts group as
`fb` · `size blur spk` · `amt pitch`, with `wdth vol mix` as the three macro knobs.

**Move mapping (8 knobs, Performance page):** SIZE, AMOUNT, SPIKE, BLUR, PITCH, FB-PRE, FB-POST,
MIX. WIDTH/VOL/TRIGGER + FREEZE/REVERSE toggles live on a second level (TRIGGER is a prime
modulation destination).

---

## 6. DSP signal flow

```
in ─►[write]─► RING BUFFER (length = f(SIZE), adaptive) ───────────────┐
                    ▲                                                   │
                    │                              ┌── grain scheduler ─┤ spawns grains:
          [FEEDBACK gain]◄─[DAMP tilt]◄─ wet ◄──┐   │   pos=POSITION±SPRAY
                    ▲                           │   │   rate=PITCH±SPRAY
                    │                           │   ▼   win=WINDOW, pan=±SPREAD
                    └───────── post-feedback ───┤  [sum windowed grains] ─► wet
                                                │           │
  dry ──────────────────────────────[MIX]──►(+)│           │
                                    pre ───────►[BLEND pre↔post]──► wet' ─► out
```

- **Adaptive ring buffer** = the Simplos trick. Buffer/read latency scales with `SIZE`, so the
  feedback loop is always ≈ one grain long → "minimal feedback latency," immediate response.
- **Grain scheduler**: reuse GRAIN's pool/scheduler. Each grain = windowed read at a rate
  (PITCH), from POSITION, with per-grain SPRAY jitter and a SPREAD pan. DENSITY sets
  spawn rate/overlap.
- **WINDOW curve**: single param morphs the grain envelope attack/decay (sharp→rounded). Owns
  the "percussive flow vs airy" character.
- **Feedback path**: wet → DAMP tilt → FEEDBACK gain → back into the ring write. Clean because
  it's band-tilted + per-grain windowed, not a raw delay smear.
- **BLEND**: crossfade between the *pre*-feedback grain sum and the *post*-feedback (delayed)
  signal → the dry-cloud ↔ spatial-delay morph.

Reuse from GRAIN: grain voice struct, `read_cubic`, window tables, scheduler. New here: adaptive
ring sizing, feedback write-back, DAMP tilt, pre/post blend, the mod engine (§7).

---

## 7. The modulation system (the hard part)

10 modulators. **Confirmed from the UI, each modulator instance has:** a *shape/type*, a *rate*
(free **Hz** or tempo-**synced**, e.g. `1/8`), a rate **×mult** (saw `5×`), **rand %**, and a
**slew** (`slw %`) smoother. Each gets up to **4 MAP slots**; a slot = a **destination + a
min↔max range** it sweeps (two handles on a slider, live dot = current value) — this is
**range-based routing, not signed depth**. Destinations include the §5 params *and other
modulators' params* (confirmed: `lfo-cluster-1 main rate`) → **inter-modulator routing**. The
`A/B/C/D` tabs on each rack are **morph snapshots** of the whole mapping set.

**`mod-*` (4) — signal/event-derived:**
- `mod-atrg` — attack/transient trigger → fast envelope (follows input onsets)
- `mod-d3` — 3-stage decay envelope (triggered)
- `mod-floe` — very slow drift ("flow") source
- `mod-capta` — capture / sample-&-hold of another source on trigger

**`lfo-*` (6) — generators (visuals seen in the UI):**
- `lfo-morf` — morphing waveshape (sine↔tri↔saw↔square sweep)
- `lfo-pnoise` — smooth (perlin-ish) noise
- `lfo-cluster` — orbital/clustered source (the concentric-rings visualizer)
- `lfo-rrnd` — stepped random (S&H)
- `lfo-sketch` — user-drawn shape (the 8-point polar visualizer)
- `lfo-seq` — step sequencer, tempo-synced (the vertical-bars + `1/8` visualizer)

**Engine:** once per audio block, compute the active mods (shape→value, then `×mult`, `rand`,
`slew`) → apply each route by mapping the mod's 0..1 value into its slot's `[min01,max01]` and
writing the dest (sum/replace, clamped). Evaluate modulators in dependency order so
inter-modulator routes resolve. Cheap (control-rate, not per-sample). Synced mods use
`mist_set_tempo`.

**Why it's hard on Move:** 11 base params + a free range-based matrix + inter-mod routing +
snapshots vs **8 physical knobs**. Solution = a **canvas.js mod-matrix page** (Beat Bank
precedent, [[beatbank-custom-patterns]]): jog picks a modulator → one of its 4 MAP slots →
destination + min/max; knobs set shape/rate/rand/slew and the two range handles. The 8 hardware
knobs stay on the Performance page. Not one-knob-per-param. (Skip the fancy Simplos visualizers
— node webs, 4-D cubes — they're eye-candy; the Move UI just needs the functional routing.)

---

## 8. UI approach (Move) — lean on the schwung framework, don't rebuild it

The schwung UI framework (repo `charlesvestal/schwung`, manual at schwung.dev) already provides
most of the "hard part." **Display is 128×64, 1-bit** (`src/shared/e16_canvas.mjs`:
`WIDTH=128 HEIGHT=64`, `createCanvas()`). What we reuse:

- **Native modulated-knob overlay.** The host already draws *"an LFO on a knob shows the range it
  sweeps"* (`showKnobOverlay`). So MIST's **range-based routing renders for free on the 8
  Performance knobs** — this is exactly Simplos's "handle range + live dot" look, no custom code.
- **Canvas "door" pages** (`docs/CANVAS_PAGES.md`): a `type:"canvas"` param with `enterable:true`
  becomes a navigable screen — jog walks, click enters a level, Back goes up via the module's
  `handleBack(ctx)` contract. **That is exactly the mod-matrix navigation** (mod → slot →
  destination) — **but as of 2026-11 that "Part two" (`as_page`/enterable canvas as a door) is a
  PROPOSAL in the framework's own doc, not shipped.** "Part one" (a non-enterable, glance-only
  fullscreen canvas) IS implemented today — that's what P1's `ui/canvas.js` scope uses. Build the
  mod-matrix nav on the door mechanism only once it's confirmed live in a real session.
- **Built-in widgets** (`src/shared/param_pages/widget_registry.mjs`, manual): `LFO`, `envelope`,
  `filter`, `waveform`, `fader`, `sample`, plus knob/number/choice/switch/action. The **`LFO`
  widget draws a waveform at its rate/depth** — reuse it for modulator shape displays instead of
  hand-drawing them.
- **Custom widgets** via `registerWidget(kind, impl)` / `registerOverlayWidgets` — this is where we
  bring the **Simplos design line**: a custom "grain-cloud blob" scope cell + a slim range-slider
  cell, drawn minimally for 128×64 1-bit. Eye-candy (node webs, 4-D cubes) stays out; the blob +
  range-dots language survives.
- **Toggles:** FREEZE/REVERSE as two-option enums — the framework **flips them on click** (no menu).
- **Card overlay** (`knob_card.mjs`) + gestures (jog scroll, jog-click enter, Shift+jog-click =
  section picker, Back exit) — all free.

**Pages:**
- **Page 1 — Performance:** the 8 base knobs (§5) + FREEZE/REVERSE; mod ranges show on the knobs
  natively.
- **Scope (BUILT, 2026-11-09):** `modules/mist/ui/canvas.js` — a non-enterable glance canvas
  (the confirmed-implemented mechanism) rendering the Simplos blob language in 128×64 1-bit:
  nested irregular contours that swell with `max(fb_pre, fb_post)`, an animated drift phase, an
  FB-level readout, and SZ/PT/BL range-dot readouts. Wired into `module.json` as a `"scope"` param
  (`type:"canvas"`, not in the `"knobs"` list — reachable like any extra param). Verified offline
  against the real `e16_canvas.mjs` (Node harness, zero throws across param extremes incl.
  undefined values, 60-frame animation loop) — **not yet tested on actual hardware.**
- **Page 2 — Mods (enterable canvas door, BLOCKED on the framework shipping "Part two" — §7):**
  walk the 10 modulators → each one's 4 MAP slots → destination + min/max; knobs set
  rate/×mult/rand/slew + the two range handles; `LFO` widget shows the shape. `handleBack` goes up
  one level (mod ← slot ← destination). Do not start until the door mechanism is confirmed live.
- **Snapshots:** A/B/C/D morph states (Simplos rack tabs) via `mist_snapshot`.
- **Presets:** schwung preset save/recall stores base params + routing table + snapshots.

---

## 9. Build & deploy

- **Core:** plain C99, no deps → compiles anywhere. Validate first with `mist_render.py`
  (offline, by ear) — mirrors [[grain-module]]'s `grain-proto.py` flow.
- **Move:** fork-CI cloud-Docker (aarch64 cross-gcc) → `dsp.so`, deploy to
  `schwung/modules/audio_fx/mist/`, **catalog-register** (else it vanishes on reboot —
  [[beatbank-custom-patterns]]). Same pipeline that built [[eco-module]]/[[grain-module]].
- **audio_fx ABI:** in-place int16@48k, `move_audio_fx_init_v2` (as ECO). Wrapper keeps an
  internal float ring; converts in/out.

---

## 10. Phasing

- **P0 — Core cloud (no mods).** Extend GRAIN's engine into `mist_core`: adaptive ring +
  feedback write-back + WINDOW + PITCH + BLEND. Fixed params. Validate in `mist_render.py`.
  *Deliverable: it granularizes + self-feeds + blends to spatial delay, offline.*
- **P1 — Move audio_fx, 8 knobs.** `mist_movefx.c`, Performance page only, no modulators.
  Build via fork-CI, deploy, tune by ear on hardware.
- **P2 — Mod engine + a few sources.** Add `lfo-morf`, `lfo-rrnd`, `mod-atrg`, `mod-d3` + a
  minimal fixed routing (e.g. 4 slots). Prove the matrix concept.
- **P3 — Full matrix + canvas UI + presets.** All 10 modulators, free routing page, preset
  save/recall. (The biggest UI lift.)
- **P4 — Polish/ports.** MPC3 VST2 wrapper; KICK if/when it ships.

**Do P0 now** — pure code + offline ear test, no hardware needed, and it reuses GRAIN.

---

## 11. Risks / unknowns

- **CPU on ARM.** Granular + feedback + 10 mods. Cap grain voices (start ~16–24); mods are
  control-rate (cheap). Profile on-device at P1.
- **Mod-matrix UX on 8 knobs** — the real design work; canvas page is the plan but needs
  iteration. Biggest scope item.
- **Feedback stability** — self-oscillation near FEEDBACK=1 with the short adaptive ring; needs
  a soft limiter/DAMP in the loop so it stays "crisp," not runaway.
- **Exact Simplos fingerprint** — unmatchable 1:1; target "great immediate granular-feedback
  processor," tuned by ear against the reference.
- **int16 headroom** — grain sums + feedback can clip; keep internal float, limit before int16.
```
