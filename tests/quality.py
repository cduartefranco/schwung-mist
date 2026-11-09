#!/usr/bin/env python3
"""Objective sound-quality metrics for MIST, run against the REAL dsp/mist.c.
   python3 tests/quality.py [path/to/mist.c]   (default: ../dsp/mist.c)"""
import os, sys, subprocess, tempfile, time
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
SR = 44100

def build(src):
    tmp = tempfile.mkdtemp(); so = os.path.join(tmp, 'm.so'); exe = os.path.join(tmp, 'w')
    subprocess.check_call(['clang', '-O2', '-shared', '-fPIC', '-o', so, src, '-I', os.path.dirname(src)])
    subprocess.check_call(['clang', '-O2', '-o', exe, os.path.join(HERE, '..', 'tools', 'mist_wav.c')])
    return so, exe

def run(so, exe, x, params, tail=0):
    xi = (np.clip(x, -1, 1) * 32767).astype('<i2')
    args = [exe, so, '120', str(tail)] + sum([[k, str(v)] for k, v in params.items()], [])
    t0 = time.perf_counter()
    out = subprocess.run(args, input=xi.tobytes(), capture_output=True, check=True).stdout
    dt = time.perf_counter() - t0
    return np.frombuffer(out, dtype='<i2').astype(float).reshape(-1, 2) / 32768, dt

BASE = dict(mix=1, fb_pre=0, fb_post=0, trg1_dest='Off', trg2_dest='Off', blur=0, spike=0.5, amount=0.6,
            size=120, density=22)

def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'dsp', 'mist.c')
    so, exe = build(os.path.abspath(src))
    t = np.arange(4 * SR) / SR
    def mono(s): return np.stack([s, s], 1)
    sl = slice(SR, 4 * SR)

    # 1) aliasing: 6 kHz tone up +24 st -> 24 kHz (above Nyquist) -> ideally silent
    y, _ = run(so, exe, mono(0.4 * np.sin(2 * np.pi * 6000 * t)), dict(BASE, pitch=24))
    y0, _ = run(so, exe, mono(0.4 * np.sin(2 * np.pi * 1000 * t)), dict(BASE, pitch=24))
    alias = 20 * np.log10(np.sqrt((y[sl] ** 2).mean()) / np.sqrt((y0[sl] ** 2).mean()) + 1e-9)
    print(f"aliasing   6k@+24st level vs 1k@+24st : {alias:6.1f} dB   (lower = better; ideal < -40)")

    # 2) stereo separation: L=500 Hz, R=1500 Hz
    x = np.stack([0.4 * np.sin(2 * np.pi * 500 * t), 0.4 * np.sin(2 * np.pi * 1500 * t)], 1)
    y, _ = run(so, exe, x, dict(BASE, pitch=0, spread=0, width=0.5))
    F = np.fft.rfftfreq(len(y[sl]), 1 / SR); L = np.abs(np.fft.rfft(y[sl, 0]))
    sep = 20 * np.log10(L[np.abs(F - 500).argmin()] / (L[np.abs(F - 1500).argmin()] + 1e-9))
    print(f"stereo     left: 500 Hz over 1500 Hz   : {sep:6.1f} dB   (higher = better; 0 = mono)")

    # 3) grain buzz / roughness: envelope ripple of a steady tone (pitch 0 and -12)
    for st in (0, -12):
        y, _ = run(so, exe, mono(0.4 * np.sin(2 * np.pi * 440 * t)), dict(BASE, pitch=st))
        m = y[sl].mean(1); env = np.array([np.sqrt((m[i:i + 441] ** 2).mean()) for i in range(0, len(m) - 441, 441)])
        print(f"roughness  pitch {st:+3d}: envelope ripple : {100 * env.std() / env.mean():6.1f} %  (lower = smoother)")

    # 4) CPU: dense worst-ish case, real-time factor on this Mac
    y, dt = run(so, exe, mono(0.3 * np.random.default_rng(1).standard_normal(len(t))),
                dict(BASE, density=100, size=300, pitch=12, fb_post=0.5, blur=0.3))
    print(f"cpu        dense case (Mac)            : {100 * dt / 4:6.2f} % of real time")

if __name__ == '__main__':
    main()
