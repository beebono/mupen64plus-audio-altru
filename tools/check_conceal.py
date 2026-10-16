#!/usr/bin/env python3
"""Metrics of a test_conceal run (raw S16 stereo 44100 Hz), compared with a run of the same signal without stalls.
usage: check_conceal.py SIGNAL out.raw clean.raw      prints: clicks, pitch stability (tone), verbatim repeats (sweep, speech), silent seconds
  clicks      sample steps larger than 1.5x the largest step of the clean run (from 1.5 s on)
  pitch       tone only: share of 20 ms windows (not silent) whose zero-crossing frequency is more than 3 % off 1000 Hz, and the worst deviation
  repeats     sweep / speech / noise: 10 ms hops whose 20 ms segment matches an earlier segment 15-120 ms back with a normalised correlation > 0.9999
              (error energy under 0.02 %: a verbatim replay gives ~1.0; a stretch of a slowly varying signal stays around 0.99); the 0.99 count is printed too
  silent      seconds in runs of >= 2 ms with |x| < 30 (beyond the clean run's)."""
import sys
import numpy as np
sr = 44100

def load(p):
    d = np.fromfile(p, dtype='<i2').reshape(-1, 2).astype(float)[:, 0]
    return d[int(1.5 * sr):]

def silent_s(x):
    q = np.abs(x) < 30
    tot = 0; run = 0
    for v in q:
        if v: run += 1
        else:
            if run >= 88: tot += run
            run = 0
    if run >= 88: tot += run
    return tot / sr

def repeats(x):
    seg = int(0.02 * sr); hop = int(0.01 * sr); lo = int(0.015 * sr); hi = int(0.12 * sr)
    cnt = 0; strict = 0; n = 0
    for s in range(hi, len(x) - seg, hop):
        cur = x[s:s + seg]
        e = cur @ cur
        if e < 1e6: continue
        past = x[s - hi:s - lo + seg]
        c = np.correlate(past, cur, 'valid')
        cs = np.concatenate(([0], np.cumsum(past ** 2)))
        en = cs[seg:seg + len(c)] - cs[:len(c)]
        ncc = c / np.sqrt(en * e + 1e-9)
        n += 1
        m = ncc.max()
        if m > 0.99: cnt += 1
        if m > 0.9999: strict += 1
    return cnt, strict, n

def zc_freq(w):
    s = np.sign(w); s[s == 0] = 1
    z = np.sum(s[1:] != s[:-1])
    return z / 2 / (len(w) / sr)

sig, out, clean = sys.argv[1:4]
x = load(out); c = load(clean)
n = min(len(x), len(c))
cmax = np.abs(np.diff(c)).max()
dx = np.abs(np.diff(x))
clicks = int(np.sum(dx > 1.5 * cmax))
res = 'clicks %3d  maxstep %5.0f/%5.0f  silent %.3fs' % (clicks, dx.max(), cmax, max(0, silent_s(x) - silent_s(c)))
badp = 0
if sig == 'tone':
    crms = np.sqrt(np.mean(c ** 2)); w = int(0.02 * sr); bad = 0; tot = 0; worst = 0
    for s in range(0, len(x) - w, w):
        seg = x[s:s + w]
        if np.sqrt(np.mean(seg ** 2)) < 0.9 * crms: continue
        f = zc_freq(seg); tot += 1
        dev = abs(f - 1000) / 1000; worst = max(worst, dev)
        if dev > 0.03: bad += 1
    badp = bad
    res += '  pitch off>3%%: %d/%d worst %.1f%%' % (bad, tot, worst * 100)
else:
    rx, sx, nx = repeats(x); rc, sc, nc = repeats(c)
    res += '  repeats>0.9999: %d (clean %d)  >0.99: %d (clean %d) of %d hops' % (sx, sc, rx, rc, nx)
print(res)
# gate (exit code): no clicks; tone: a steady pitch; noise (the only signal that cannot repeat by itself): at most 3 more repeats than the clean run
bad = clicks > 0
if sig == 'tone': bad = bad or badp > 1
elif sig == 'noise': bad = bad or sx - sc > 0 or rx - rc > 8
sys.exit(1 if bad else 0)
