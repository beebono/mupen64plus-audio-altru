#!/usr/bin/env python3
"""Checks the raw S16 stereo 44100 Hz output of test_backend (phases: see the header of test_backend.c).
usage: check_backend.py out.raw [mute]     exit code 0 = all checks passed"""
import sys
import numpy as np

mute = len(sys.argv) > 2 and sys.argv[2] == 'mute'
d = np.fromfile(sys.argv[1], dtype='<i2').reshape(-1, 2).astype(float)[:, 0]
sr = 44100
bad = 0
# the output runs about 1 s ahead of the push times of the phases (the device is open before the first push), so the windows stay clear of the edges
phases = (('normal', 1, 4, 'tone'), ('normal+stalls', 4.5, 10, 'stalls'), ('200%', 11, 14, 'ff'), ('300%', 15, 18, 'ff'),
          ('unthrottled', 18.5, 20.5, 'ff'), ('back to normal', 22.5, 25, 'tone'))
for name, a, b, kind in phases:
    x = d[int(a * sr):int(b * sr)]
    if len(x) < sr // 2:
        print('%-15s too short' % name); bad += 1; continue
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    f = np.argmax(spec) * sr / len(x)
    silent = 100 * np.mean(np.abs(x) < 30)
    dx = np.abs(np.diff(x))
    clicks = int(np.sum(dx > 2600))      # a clean 12000-amplitude 1 kHz sine moves by at most 1710 per sample
    ok = clicks == 0
    if kind == 'ff' and mute:
        ok = ok and silent > 90
    else:
        ok = ok and abs(f - 1000) < 20 and silent < (15 if kind == 'stalls' else 3)
    print('%-15s peak %7.1f Hz  rms %6.0f  silent %5.1f %%  steps>2600: %d  max step %5.0f  %s' % (name, f, np.sqrt(np.mean(x ** 2)), silent, clicks, dx.max(), 'ok' if ok else 'FAIL'))
    bad += not ok
sys.exit(1 if bad else 0)
