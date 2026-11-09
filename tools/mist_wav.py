#!/usr/bin/env python3
"""Render WAVs through the REAL Move DSP (dsp/mist.c) — the listening tool.

Builds dsp/mist.c + tools/mist_wav.c for the Mac (clang), then for each preset
pipes the source through it exactly like the Move host (44.1k, 128-frame blocks).
Unlike tools/mist_render.py (the P0 Python reference), this can't drift from
what ships — it IS what ships.

  python3 tools/mist_wav.py SRC.wav [--out DIR] [--bpm 137] [--secs 8] [--tail 4]
                            [--preset NAME ...] [--set key=val ...]
"""
import argparse, os, subprocess, sys, tempfile
import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
DSP = os.path.join(HERE, '..', 'dsp')
SR = 44100

# Preset bank (param keys = the module's own keys). Defaults apply for anything unset.
PRESETS = {
    "00 defaults":       {},
    "01 dense cloud":    dict(density=45, size=140, blur=0.2, spread=0.9, fb_post=0.3, mix=1),
    "02 pitch -12 (ref 05)": dict(pitch=-12, fb_post=0.45, size=220, density=22, mix=1),
    "03 spatial delay":  dict(fb_time=380, fb_pre=0.25, fb_post=0.8, blur=0.22, size=110,
                              density=28, width=0.95, spread=0.9, pitch=0, mix=1),
    "04 euclid 5-8 perc": dict(gmode=1, eu_pattern=0, eu_rate=2, spike=0.95, amount=0.15,
                               size=60, fb_post=0.3, blur=0.1, pitch=0, mix=1),
    "05 euclid bossa burst": dict(gmode=1, eu_pattern=10, eu_rate=2, eu_burst=3, density=60,
                                  size=90, spike=0.8, pitch=-12, fb_post=0.35, mix=1),
    "06 euclid 7-16 +7": dict(gmode=1, eu_pattern=12, eu_rate=2, eu_burst=2, density=40,
                              size=120, pitch=7, fb_post=0.2, blur=0.1, spread=1, mix=1),
}


def build():
    tmp = tempfile.mkdtemp(prefix='mistwav-')
    so, exe = os.path.join(tmp, 'mist.so'), os.path.join(tmp, 'mist_wav')
    subprocess.check_call(['clang', '-O2', '-shared', '-fPIC', '-o', so,
                           os.path.join(DSP, 'mist.c'), '-I', DSP])
    subprocess.check_call(['clang', '-O2', '-o', exe, os.path.join(HERE, 'mist_wav.c')])
    return so, exe


def load(src, secs):
    x, sr = sf.read(src, dtype='float64', always_2d=True)
    if x.shape[1] == 1: x = np.repeat(x, 2, axis=1)
    x = x[:, :2]
    if sr != SR:
        g = np.gcd(SR, sr); x = resample_poly(x, SR // g, sr // g, axis=0)
    x = x[: int(secs * SR)]
    pk = np.abs(x).max() + 1e-9
    if pk > 0.5: x *= 0.5 / pk                      # ~-6 dBFS in, like a mix bus
    return (np.clip(x, -1, 1) * 32767).astype('<i2')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('--out', default=os.path.expanduser('~/Desktop/MIST renders'))
    ap.add_argument('--bpm', type=float, default=120)
    ap.add_argument('--secs', type=float, default=8)
    ap.add_argument('--tail', type=float, default=4)
    ap.add_argument('--preset', action='append')
    ap.add_argument('--set', action='append', default=[], help='key=val override for all presets')
    a = ap.parse_args()

    so, exe = build()
    raw = load(a.src, a.secs).tobytes()
    os.makedirs(a.out, exist_ok=True)
    base = os.path.splitext(os.path.basename(a.src))[0][:40]
    names = a.preset or list(PRESETS)
    for name in names:
        params = dict(PRESETS[name])
        params.update(dict(kv.split('=', 1) for kv in a.set))
        args = [exe, so, str(a.bpm), str(a.tail)]
        for k, v in params.items(): args += [k, str(v)]
        out = subprocess.run(args, input=raw, capture_output=True, check=True).stdout
        y = np.frombuffer(out, dtype='<i2').reshape(-1, 2)
        path = os.path.join(a.out, f"{base} - {name}.wav")
        sf.write(path, y, SR, subtype='PCM_16')
        f = y.astype(float) / 32768
        print(f"  {name:26s} peak {20*np.log10(np.abs(f).max()+1e-9):6.1f} dBFS  "
              f"rms {10*np.log10((f**2).mean()+1e-12):6.1f} dBFS  -> {os.path.basename(path)}")


if __name__ == '__main__':
    sys.exit(main())
