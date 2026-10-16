# Design

## What the stock plugin did

mupen64plus-audio-sdl keeps a circular buffer of N64 samples and resamples them in an SDL callback. After every push it estimated the buffer
level at the next callback (with 1 ms `SDL_GetTicks`) and then either `SDL_Delay`ed the core, called `SDL_PauseAudio(1)` when it predicted an underflow, or unpaused.
Fast-forward was folded into the resample ratio (pitch up) while the same delay kept the core on the audio clock. Measured on the handhelds, the stock plugin
starved the callback 25-130 times in 40 s in the `@@` games ([testing.md](testing.md#results)). Two bugs also showed up: the SDL device open result was
checked through an unsigned value (a failed open was treated as success), and with the speed limiter off (suxty's MAX) the audio delay throttled the core back to 100 %.

## Data path

N64 samples (`AiLenChanged`) are swapped to host order and appended to a linear buffer in the emulation thread. The SDL audio callback (default) resamples
them to the device rate (44100, 22050 or 11025 Hz, picked from the game rate as before) and applies the volume. The buffer keeps 2048 source frames behind the read
position: the resampler's history and the rewind of the concealment. Compaction moves the data down when the read position passes half the buffer.
The device is opened once per output rate and never paused; a game rate change within the same output rate only switches the ratio and the resampler.

## Rate control

At the start of each callback the level (unread source frames expressed in output frames) is low-passed (1/16 per callback) and compared with the target; the
error, scaled so that half a target of difference is full scale, sets the resample step offset: up to `RATE_ADJUST_PPM` (5000 = 0.5 %), more when the level is too high (consume faster).
The level is measured at the start of the callback, i.e. at its trough, so the target means "the lowest the buffer gets". The time base is `SDL_GetPerformanceCounter` (µs).
Only a proportional term is used: with a core that is slower than real time for a while (a game at 90 % in a heavy scene), the 0.5 % cannot bridge the difference and the
buffer drains; that is what the concealment is for. `RATE_ADJUST_PPM` can be raised (80000 was tried: it lowered the silence a little but changes pitch audibly).

**Backstop.** After a push, if `sync` is on, the core is not fast-forwarding and the level is more than `target + push size + 2 periods` the push sleeps for the excess (at most 40 ms).
With sync off, or while fast-forwarding, a level above the maximum target is cut back to the target instead (a short crossfade), so the latency stays bounded.

## Underruns

When the callback needs more than the buffer holds:

1. **Concealment** (`CONCEAL`, default on): a time-stretch of the buffered tail, in place, before the buffer runs dry. At the start of a callback, when the unread audio is below
   `min(period + 15 ms, 3/4 of the target)` (and the core is not fast-forwarding), the unread tail is stretched by WSOLA (waveform-similarity overlap-add): Hann grains of 16 ms (less for a short tail), 50 % overlap,
   written at a slower pace than they are read, each grain's start searched within +-6 ms for the best match with the natural continuation of the previous grain (coarse on a 4:1 box-decimated mono signal, then +-3 frames
   at full rate). The pitch does not change and nothing is replayed verbatim. The output starts with the tail's first frame and ends with its last (the tail of the last grain is appended), so both joins are seamless and the
   next push simply continues the stream. Between two pushes it can run four times, by 1.5, 2, 2 and 2 (a gap of 120 ms past the buffer is covered; the last pass fades out over its second half and the next push is
   crossfaded in by `join_fade`). It costs 80-300 µs for a 20-60 ms tail on an A55/A53 (`tools/bench_stretch.c`, scalar float; NEON would not pay for an event that happens a few times in 40 s). The replay of the last
   20 ms that this replaced (four times in a row) sounded like a stuck program.
2. **Silence**: the rest of the callback is silent (the last 64 frames before it fade out), the device keeps running, and the output stays silent until the level is back to half the
   target (at least one period). The next audio fades in from the last frame. This counts as an underrun and grows the automatic buffer.

## Buffer size

`BUFFER_MODE = 0` (automatic): the target starts at the minimum plus 40 ms (one period plus 20 ms, plus 40 ms), grows by 25 ms on each underrun (up to 300 ms), and shrinks by 5 ms every 5 s once
10 s have passed without an underrun. `BUFFER_MODE = 1`: the target is `BUFFER_MS` and stays. The SDL period (`SECONDARY_BUFFER_SIZE`, a power of two) is picked at device open:
0 means the power of two nearest below `output rate / 40` (1024 frames at 44100 Hz, 512 at 22050 Hz, 256 at 11025 Hz, about 23 ms; 11.6 ms periods made the pipewire path of the rgdsplus drop one 2.7 ms quantum of the stream every few hundred ms, which is audible as popping, see [testing.md](testing.md#popping-on-the-rgdsplus)), otherwise the configured value.

## Fast-forward

`SetSpeedFactor` gives the factor; the core's speed limiter is read with `CoreDoCommand(M64CMD_CORE_STATE_QUERY, M64CORE_SPEED_LIMITER)` on every push. suxty's MAX mode switches the limiter off and leaves the factor alone,
so it is only visible that way. Fast-forward is "factor above 100, or limiter off". While it lasts:

- The audio sync is not applied and the resample ratio stays at the game rate (normal pitch).
- The emulation speed is measured from the arriving samples (100 ms windows, smoothed), so a core that cannot reach the requested factor still gets the right amount of audio dropped (the factor only tells that fast-forward is on).
  Of every `speed` x 20 ms of source, 20 ms are kept, with a 64-frame crossfade from the frames the previous chunk would have continued with.
- `FF_MODE = 1` drops the samples instead and the output is silent (with a fade at its start).
- When fast-forward ends, the buffer is cut back to the target, the first new samples are crossfaded onto the last buffered frame, and the output fades from the last frame.

A factor below 100 (slow motion) lowers the ratio as before (pitch down).

## Output: callback or queue

`OUTPUT_MODE = 1` uses `SDL_QueueAudio` (SDL 2.0.4+): the resampling then happens in the emulation thread right after each push and the device has no callback. The rate control runs per push
on the queued size. It works on the rg35xxsp's SDL 2.28.5 but has no concealment or silence accounting, and the measurements gave it no advantage ([testing.md](testing.md#queue-or-callback)), so the callback is the default.

## Threads

The callback thread reads and moves the read position; the emulation thread appends, flushes and changes options; every such change holds `SDL_LockAudioDevice`. Counters are plain integers read after the device is closed.
Options changed through the suxty interface are applied on the emulation thread: flags are plain stores, the resampler is swapped under the device lock.

## Statistics

`ALTRU_STATS=<file>` writes, when the ROM closes, `<file>.sum` (underruns, silence, delays, repeats, flushes, adjustment range, target range, speed) and `<file>` (a CSV: time, buffer level after each push, source frames of the push).
The stock plugin with the same output is `tools/device/baseline-stats.patch`.
