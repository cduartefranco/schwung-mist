#!/usr/bin/env python3
"""MIST — offline granular-feedback PROCESSOR prototype (P0 reference).

Simplos-inspired granular processor: input audio is written into a ring buffer;
grains re-play its recent past (windowed, pitch-shifted); a tight feedback loop
(delay tracks grain size) writes wet back into the ring. A pre/post feedback
blend morphs grain-cloud -> spatial delay. This is the reference the C core
(mist_core.c) must match (same role as grain-proto.py for GRAIN).

P0 params (real Simplos names): size, amount(window curve), spike(attack),
blur(jitter), pitch(st), fb_pre, fb_post, width, mix, density(trigger).

Usage: mist_render.py [source.wav] [--out DIR]
Needs numpy + soundfile.
"""
import sys, os, argparse
import numpy as np

SR = 44100
STEP = 256                      # block size; must be <= feedback margin (causality)


def interp(buf, idx):
    """4-point Hermite (cubic) interpolation — clean grain reads, less dull on pitch."""
    i1 = np.floor(idx).astype(int)
    i1 = np.clip(i1, 1, len(buf) - 3)
    t = idx - i1
    x0 = buf[i1 - 1]; x1 = buf[i1]; x2 = buf[i1 + 1]; x3 = buf[i1 + 2]
    c0 = x1
    c1 = 0.5 * (x2 - x0)
    c2 = x0 - 2.5 * x1 + 2.0 * x2 - 0.5 * x3
    c3 = 0.5 * (x3 - x0) + 1.5 * (x1 - x2)
    return ((c3 * t + c2) * t + c1) * t + c0


def dc_block(prev_x, prev_y, x, R=0.995):
    """1-pole DC blocker — stops offset/noise ramp building in the feedback path."""
    y = np.empty_like(x); px, py = prev_x, prev_y
    for i in range(len(x)):
        yi = x[i] - px + R * py
        px = x[i]; py = yi; y[i] = yi
    return y, px, py


def soft_limit(x, T=0.85):
    """Transparent below T, gentle knee above — no coloring of normal-level audio."""
    a = np.abs(x); s = np.sign(x)
    over = a > T
    y = x.copy()
    y[over] = s[over] * (T + (1 - T) * np.tanh((a[over] - T) / (1 - T)))
    return y


def out_lp(x, fc=14000.0):
    """Gentle 2-pole lowpass to tame digital harshness/aliasing on the highs."""
    from scipy.signal import butter, lfilter
    b, a = butter(2, min(fc, SR * 0.49) / (SR * 0.5), btype='low')
    return lfilter(b, a, x)


def bjorklund(k, n):
    """Euclidean rhythm: k pulses over n steps -> list of 0/1."""
    if k <= 0 or n <= 0: return [0] * max(0, n)
    if k >= n: return [1] * n
    pattern = []; counts = []; remainders = [k]; divisor = n - k; level = 0
    while True:
        counts.append(divisor // remainders[level])
        remainders.append(divisor % remainders[level])
        divisor = remainders[level]; level += 1
        if remainders[level] <= 1: break
    counts.append(divisor)
    def build(lvl):
        if lvl == -1: pattern.append(0)
        elif lvl == -2: pattern.append(1)
        else:
            for _ in range(counts[lvl]): build(lvl - 1)
            if remainders[lvl] != 0: build(lvl - 2)
    build(level)
    i = pattern.index(1)
    pattern = pattern[i:] + pattern[:i]
    return pattern


def grain_window(n, spike, amount):
    """Asymmetric window: SPIKE sharpens the attack, AMOUNT rounds the decay.
    spike 1 -> near-instant attack (percussive); amount 1 -> long rounded tail."""
    atk = max(1, int((0.02 + 0.33 * (1 - spike)) * n))
    dec = max(1, n - atk)
    w = np.empty(n)
    # attack: power curve (spike high -> steep)
    ka = 0.3 + 3.0 * spike
    w[:atk] = (np.arange(atk) / atk) ** (1.0 / ka)
    # decay: blend linear -> raised-cosine by AMOUNT (rounder tail)
    t = np.arange(dec) / dec
    lin = 1 - t
    cos = 0.5 + 0.5 * np.cos(np.pi * t)
    w[atk:] = (1 - amount) * lin + amount * cos
    return w


def damp_lp(prev, x, fc):
    """1-pole LP over a block, continuing from prev state. Returns (y, state)."""
    a = np.exp(-2 * np.pi * fc / SR)
    y = np.empty_like(x)
    s = prev
    for i in range(len(x)):
        s = a * s + (1 - a) * x[i]
        y[i] = s
    return y, s


def render(buf, p, dur=5.0, seed=1):
    """buf: mono input (processed). p: param dict. -> stereo float32."""
    rng = np.random.default_rng(seed)
    N = int(dur * SR)
    nin = len(buf)
    inp = np.zeros(N)
    # play the source over the FIRST part, then silence -> the tail reveals the
    # feedback character (dry stops dead; heavy fb blooms; delay echoes).
    L = min(nin, int(0.45 * N))
    inp[:L] = buf[:L]
    # de-click the input edges (so grains/feedback never read a hard step)
    fi = min(256, L // 2)
    if fi > 0:
        inp[:fi] *= np.linspace(0, 1, fi)
        inp[L - fi:L] *= np.linspace(1, 0, fi)

    size_n = max(STEP * 2, int(p['size'] / 1000 * SR))
    ratio_max = 2 ** (abs(p['pitch']) / 12.0) * (1 + p['blur'])
    grain_delay = int(size_n * max(1.0, ratio_max)) + STEP   # grains read recent audio (tight)
    # feedback TIME is decoupled from grain size: None = tight bloom (= grain_delay);
    # a ms value = an audible echo (spatial-delay character).
    ft = p.get('fb_time')
    Dpost = grain_delay if ft is None else max(STEP + 1, int(ft / 1000 * SR))
    Dpre = grain_delay                                      # pre = tight, dense texture
    win = grain_window(size_n, p['spike'], p['amount'])

    ring = np.zeros(N + grain_delay + 8)                   # input + feedback live here
    wetm = np.zeros(N + 8)                                 # wet mono (for feedback tap)
    dwet = np.zeros(N + 8)                                 # damped wet
    out = np.zeros((N, 2))
    dstate = 0.0
    damp_fc = p.get('damp_hz', 6500.0)

    # --- grain onset schedule: uniform(+humanize) OR euclidean -------------
    onsets = []
    eu = p.get('euclid')                                   # (pulses, steps, rate_hz, jitter)
    t0 = grain_delay + size_n                              # wait for enough history (no neg reads)
    if eu:
        pulses, steps, rate_hz, ej = eu
        pat = bjorklund(pulses, steps)
        step_n = SR / max(0.1, rate_hz)                    # samples per step
        s = float(t0); i = 0
        while s < N:
            if pat[i % steps]:
                onsets.append(int(s + rng.uniform(-1, 1) * ej * step_n))
            s += step_n; i += 1
    else:
        spacing = SR / max(1.0, p['density'])
        t = float(t0)
        while t < N:
            onsets.append(int(t)); t += spacing * rng.uniform(0.85, 1.15)

    # --- precompute grains (onset, read_base, ratio, pan) ------------------
    grains = []
    for onset in onsets:
        st = p['pitch'] + (rng.uniform(-1, 1) * 12.0 * p['blur'])   # pitch + blur scatter (st)
        ratio = 2 ** (st / 12.0)
        posjit = int(rng.uniform(-1, 1) * p['blur'] * size_n)       # position smear
        read_base = max(0, onset - grain_delay + posjit)            # never negative -> no mid-grain jump
        pan = rng.uniform(-1, 1) * p['spread']
        grains.append((onset, read_base, ratio, pan))
    grains.sort()

    # overlap (grains stacked at once) -> normalize so denser != louder/pumping
    overlap = max(1.0, (len(onsets) / (N / SR)) * (size_n / SR))
    gnorm = 1.0 / np.sqrt(overlap)

    fb_pre, fb_post = p['fb_pre'], p['fb_post']
    dcx = dcy = 0.0
    gi = 0
    active = []
    for bs in range(0, N, STEP):
        be = min(bs + STEP, N)
        # 1) fill ring[bs:be] = input + feedback (taps already-computed; delay >= STEP)
        seg = inp[bs:be].copy()
        # feedback make-up (MK) compensates the ~0.5 re-granulation loss so the loop
        # can reach unity (true freeze); the soft-limited tap keeps it bounded.
        MK = 1.7
        if fb_post > 0 and bs - Dpost >= 0:                # post: delayed wet -> spatial delay
            seg += fb_post * MK * dwet[bs - Dpost: be - Dpost]
        if fb_pre > 0 and bs - Dpre >= 0:                  # pre: tight wet -> dense texture
            seg += fb_pre * MK * dwet[bs - Dpre: be - Dpre]
        ring[bs:be] = seg                                  # NO saturation here (kept clean)
        # 2) activate grains starting in this block
        while gi < len(grains) and grains[gi][0] < be:
            active.append(grains[gi]); gi += 1
        # 3) sum active grains into out[bs:be]
        still = []
        for (onset, read_base, ratio, pan) in active:
            gend = onset + size_n
            if gend <= bs:
                continue
            still.append((onset, read_base, ratio, pan))
            a = max(onset, bs); b = min(gend, be)
            if b <= a:
                continue
            k = np.arange(a, b) - onset
            idx = read_base + k * ratio
            g = interp(ring, idx) * win[k]
            gl = g * np.sqrt(0.5 * (1 - pan)); gr = g * np.sqrt(0.5 * (1 + pan))
            out[a:b, 0] += gl; out[a:b, 1] += gr
        active = still
        # 4) wet mono -> DC-block -> damp -> soft-limit = clean, stable feedback tap
        wm = 0.5 * (out[bs:be, 0] + out[bs:be, 1]) * gnorm
        wetm[bs:be] = wm
        wb, dcx, dcy = dc_block(dcx, dcy, wm)
        dy, dstate = damp_lp(dstate, wb, damp_fc)
        dwet[bs:be] = soft_limit(dy, 0.9)

    # --- mix: dry/wet, width, output level ---------------------------------
    out *= gnorm                                           # overlap-normalized wet
    dry = np.stack([inp, inp], axis=1)
    mid = 0.5 * (out[:, 0] + out[:, 1]); side = 0.5 * (out[:, 0] - out[:, 1]) * (0.3 + 1.7 * p['width'])
    wet = np.stack([mid + side, mid - side], axis=1)
    mix = p['mix']
    y = dry * (1 - mix) + wet * mix
    # tame the highs (stops the "cracking" / out-of-range HF)
    y[:, 0] = out_lp(y[:, 0], p.get('out_hz', 14000.0))
    y[:, 1] = out_lp(y[:, 1], p.get('out_hz', 14000.0))
    y *= p.get('vol', 0.9)
    y = soft_limit(y, 0.7)                                 # transparent below 0.7, gentle knee above
    # LEAVE HEADROOM: scale down to ~-4.5 dBFS target, never slam to 0 dBFS
    TARGET = 0.6
    pk = np.abs(y).max() + 1e-9
    if pk > TARGET:
        y *= TARGET / pk
    # de-click: fade in (~12ms) and out (~40ms) so there is no onset/offset pop
    fin = int(0.012 * SR); fout = int(0.040 * SR)
    y[:fin] *= np.linspace(0, 1, fin)[:, None]
    y[-fout:] *= np.linspace(1, 0, fout)[:, None]
    return y.astype(np.float32)


def load_mono(path):
    import soundfile as sf
    y, sr = sf.read(path)
    if y.ndim > 1:
        y = y.mean(axis=1)
    if sr != SR:
        x = np.linspace(0, 1, len(y)); xi = np.linspace(0, 1, int(len(y) * SR / sr))
        y = np.interp(xi, x, y)
    return (y / (np.abs(y).max() + 1e-9)).astype(np.float64)


# ---- preset bank: each shows one facet of the engine -----------------------
def P(**kw):
    base = dict(size=120, amount=0.6, spike=0.5, blur=0.15, pitch=0.0,
                fb_pre=0.0, fb_post=0.0, width=0.6, mix=1.0, density=28,
                spread=0.7, vol=0.9, damp_hz=7000.0, euclid=None, fb_time=None)
    base.update(kw); return base


PRESETS = {
    # A: bright, tight, FAST grains, NO feedback -> a clean shimmer cloud
    "01 dry cloud":       P(fb_pre=0, fb_post=0, size=85, density=50, amount=0.45,
                            spike=0.6, blur=0.1, width=0.5),
    # B: HEAVY tight-bloom feedback -> thick sustaining wash, slow + big grains
    "02 feedback build":  P(fb_post=0.94, size=200, density=22, amount=0.85,
                            spike=0.25, blur=0.15, width=0.7, damp_hz=6000),
    # C: real DELAY -> long feedback time (380ms) = audible repeating echoes, wide
    "03 spatial delay":   P(fb_pre=0.25, fb_post=0.8, fb_time=380, blur=0.22, size=110,
                            density=28, width=0.95, spread=0.9, damp_hz=6500),
    # pitched feedback compounds -> keep fb low + damp the loop so it doesn't spiral
    "04 pitch up +7":     P(pitch=7, fb_post=0.18, size=120, density=34, damp_hz=4200),
    # the winner — kept; cleaner now without the tanh squash
    "05 pitch down -12":  P(pitch=-12, fb_post=0.45, size=220, density=22),
    # EUCLIDEAN perc: 5-in-8 rhythmic grain triggers instead of a flat stream
    "06 spike perc":      P(spike=0.95, amount=0.15, size=60, fb_post=0.3, blur=0.1,
                            euclid=(5, 8, 14.0, 0.05)),
    # freeze vibe + more texture (blur/scatter up, denser) — very high fb = holds
    "07 near-freeze":     P(fb_post=0.97, size=150, density=34, blur=0.3, spread=0.85),
    # pulled back from chaos — focused, still wide
    "08 wide blur":       P(blur=0.3, width=0.85, fb_pre=0.1, fb_post=0.32, size=150, density=30),
}


def main():
    import soundfile as sf
    ap = argparse.ArgumentParser()
    ap.add_argument('src', nargs='?',
                    default="/Users/camipro/Desktop/ABLENTON/WIP/dn-audio/s4-textures/S4 Drift Pad.wav")
    ap.add_argument('--out', default="/Users/camipro/Desktop/ABLENTON/WIP/dn-audio/mist")
    ap.add_argument('--dur', type=float, default=5.0)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    buf = load_mono(a.src)
    base = os.path.splitext(os.path.basename(a.src))[0]
    print(f"source: {base}  ({len(buf)/SR:.1f}s)  -> {a.out}")
    for name, p in PRESETS.items():
        y = render(buf, p, dur=a.dur, seed=1)
        fn = os.path.join(a.out, f"MIST {name}.wav")
        sf.write(fn, y, SR)
        print(f"  {name:20s} size{p['size']:>4}ms  fb {p['fb_pre']:.2f}/{p['fb_post']:.2f}  "
              f"pitch {p['pitch']:+.0f}st  blur {p['blur']:.2f}")
    print("done.")


if __name__ == "__main__":
    main()
