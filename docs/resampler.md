# Resampler

`RESAMPLE = poly` (default): polyphase windowed-sinc for stereo int16 (`src/resamplers/poly.c`).

- 32 taps, 128 phases (+1) in Q15, computed when the resampler is created: a sinc at cutoff `0.485` (of the lower rate: `0.5 x min(1, out/in) x 0.97` cycles per source frame), Kaiser window beta 7.5, each phase
  normalised to unity gain. The table follows the nominal ratio; the +-0.5 % rate control does not change it.
- The position is a Q32 source-frame counter. Each output frame takes the two neighbouring phases (the integer dot products of 32 stereo taps, exact in 32 bits) and interpolates linearly between the two results with a 16-bit fraction. The step (source frames per output frame) is
  passed on every call, so the ratio can change continuously; the fraction is kept inside the resampler between calls.
- `poly_dot_neon`: `vld2q_s16` deinterleaves 8 frames, `vmlal_s16` / `vmlal_high_s16` accumulate both phases and both channels (16 multiply-accumulates per 8 taps). `poly_dot_scalar` is the reference. Everything else is shared code, and the dot product
  is exact integer arithmetic, so the two are bit-identical (`tools/test_resample.c`: 49 cases of random, full-scale and quiet input, 7 rate pairs, fixed and per-call-changing ratios, chunked; run under qemu-aarch64; with `-DPOLY_MUTATE`, a deliberate NEON error,
  36 of the 49 cases differ).
- On x86 (and anywhere NEON is not built) the scalar path is used: it is cheap enough (7 ns per output frame on the desktop) and keeps the output the same on every host. `trivial` (nearest frame, but with the same variable ratio) stays selectable.
- `USE_SPEEX=1` (`speex-fixed-{0-10}`) and `USE_SRC=1` (`src-*`) are build-time options, off by default and never auto-detected. They go through the same interface (the ratio is re-set when it changes). Not built or run in this tree: speexdsp and libsamplerate are not installed on the host.

## Cost

Per second of output audio, one core, `tools/bench_resample.c`, clocks pinned (A53 1512 MHz, A55 `performance`, about 1.58 GHz real):

| | trivial | poly, scalar (`-fno-tree-vectorize`) | poly, NEON |
|---|---|---|---|
| rg35xxsp A53, 32006 -> 44100 | 3.4 ns / frame (0.015 %) | 208 ns (0.92 %) | 86 ns (0.38 %) |
| rgdsplus A55, 32006 -> 44100 | 3.2 ns (0.014 %) | 196 ns (0.86 %) | 83-85 ns (0.37 %) |

The ratio is about 2.4x. The default build lets gcc auto-vectorize the scalar path at -O3, which gives the same time as the intrinsics. The other rate pairs cost the same per output frame (22050 -> 22050: 0.19 %).

## Quality

`tools/quality/quality.py`: sine tones through the resampler, fits at the known frequency.

| | poly | trivial |
|---|---|---|
| gain, 100 Hz up to 0.8 of the lower Nyquist | 0.00 dB (within 0.005) | -2.2 to 0 dB |
| gain at 0.92 of the lower Nyquist | -1.7 dB (-1.95 down-sampling) | -3.3 dB |
| SNR, 1 kHz at -6 dBFS | 79-82 dB (residue 1 LSB, mostly the Q15 coefficient and 16-bit rounding), also with +-0.3 % ratio offsets | 15-28 dB |
| worst image residue, tones 0.05 to 0.45 of the lower rate | -51.5 dB re tone (the 0.45 tone sits in the transition band; below 0.4 it is far lower) | -2.8 dB |
| aliasing, 48 -> 44.1 kHz, tone at 22.8 kHz (above the output Nyquist) | -17 dB | (not measured) |

speex-fixed-N is not available on the host, so there is no side-by-side number. The N64 music/effects content sits far below 0.4 of the rate; the transition band is where the 32 taps give out.
