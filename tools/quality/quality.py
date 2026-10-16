#!/usr/bin/env python3
"""Resampler quality: passband gain, SNR and image/alias rejection of the polyphase resampler (and the nearest one for comparison).
usage: quality.py [librs.so]  (scripts/check.sh quality builds the library)"""
import ctypes, numpy as np, sys
from scipy import signal
import os, sys
lib = ctypes.CDLL(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'librs.so'))
lib.rs_run.restype = ctypes.c_size_t
def run(kind, x, fin, fout, ppm=0):
    n = len(x)
    buf = np.zeros((n + 16, 2), np.int16); buf[16:, 0] = x; buf[16:, 1] = x
    cap = int((n) * fout / fin * (1 - ppm / 1e6)) - 64
    out = np.zeros((cap, 2), np.int16)
    got = lib.rs_run(kind, buf[16:].ctypes.data_as(ctypes.c_void_p), n, fin, fout, ppm, out.ctypes.data_as(ctypes.c_void_p), cap)
    return out[:got, 0].astype(np.float64)
def fit(y, f, fs):
    t = np.arange(len(y)) / fs; w = np.blackman(len(y))
    A = np.stack([np.cos(2*np.pi*f*t), np.sin(2*np.pi*f*t)], 1)
    c, *_ = np.linalg.lstsq(A * w[:, None], y * w, rcond=None)
    res = y - A @ c
    return np.hypot(*c), np.sqrt(np.mean((res*w)**2) / np.mean(w**2))
def ref(x, fin, fout):
    from math import gcd
    g = gcd(fin, fout); return signal.resample_poly(x, fout // g, fin // g, window=('kaiser', 12))
def analyse(fin, fout, name):
    n = 40000; amp = 0.5 * 32767
    lim = 0.5 * min(fin, fout)
    fr = np.linspace(100, 0.92 * lim, 24)
    print(f'== {name}: {fin} -> {fout}')
    for kind, label in ((0, 'poly'), (1, 'trivial')):
        rip = []
        for f in fr:
            t = np.arange(n) / fin
            x = np.round(amp * np.sin(2*np.pi*f*t))
            y = run(kind, x, fin, fout)[300:-300]
            a, _ = fit(y, f, fout); rip.append(20*np.log10(a / amp))
        rip = np.array(rip)
        ok = fr <= 0.8 * lim
        print(f'{label:8s} passband gain dB: worst up to 0.8*Nyq {rip[ok].min():+.2f}..{rip[ok].max():+.2f};  at 0.92*Nyq {rip[-1]:+.2f}')
        # SNR of 1 kHz at -6 dBFS, including a +0.3 % rate-control offset
        for ppm in (0, 3000, -3000):
            f = 1000.0; t = np.arange(n) / fin
            x = np.round(amp * np.sin(2*np.pi*f*t))
            y = run(kind, x, fin, fout, ppm)[300:-300]
            fo = f * (1 + ppm/1e6)
            a, r = fit(y, fo, fout)
            print(f'{label:8s} SNR 1 kHz (rate offset {ppm/1e4:+.1f} %): {20*np.log10(a/np.sqrt(2)/ (r+1e-9)):.1f} dB (residue rms {r:.2f} LSB)')
        # image / alias rejection: tones across the band, worst spurious component relative to the tone, measured with a fit of the tone and a spectrum of the residue
        worst = -999
        freqs = np.linspace(0.05, 0.45, 9) * min(fin, fout) if fin <= fout else np.linspace(0.55, 0.95, 9) * 0.5 * fin
        for f in freqs:
            t = np.arange(n) / fin
            x = np.round(amp * np.sin(2*np.pi*f*t))
            y = run(kind, x, fin, fout)[300:-300]
            aliasf = None
            # expected tone frequency (folded into 0..fout/2)
            fo = f % fout
            if fo > fout/2: fo = fout - fo
            if fo < 1: continue
            a, r = fit(y, fo, fout)
            if f <= 0.5 * fout:
                worst_f = 20*np.log10((r+1e-9)/a)
            else:
                worst_f = 20*np.log10((a+1e-9)/amp)
            if f <= 0.5*fout: worst = max(worst, worst_f)
            if f > 0.5 * fout and kind == 0:
                print(f'          alias: input {f:.0f} Hz (above output Nyquist) leaves {worst_f:.1f} dB')
        if fin <= fout:
            print(f'{label:8s} worst residue (images, noise) over tones 0.05..0.45 of Nyquist rate: {worst:.1f} dB re tone')
analyse(32006, 44100, 'up 32k->44.1k')
analyse(22047, 22050, 'near 1:1')
analyse(48000, 44100, 'down 48k->44.1k')
analyse(11025, 22050, 'up x2')
