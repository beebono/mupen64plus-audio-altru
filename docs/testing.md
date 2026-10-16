# Testing

## Host tests: `scripts/check.sh`

Every local run is memory-capped (`systemd-run --user --scope -p MemoryMax=2G`). `scripts/check.sh [resample|a64|backend|plugin|quality]`:

| test | what it checks |
|---|---|
| `resample` (`tools/test_resample.c`) | the polyphase resampler on x86 (scalar) against itself with chunked, per-call ratio changes, and the DC gain |
| `a64` | the same program for aarch64 under qemu-aarch64: NEON against scalar must give 0 differences in all 49 cases (random, full-scale and quiet input, 7 rate pairs, fixed and changing ratios). The same build with `-DPOLY_MUTATE` (a deliberate error in the NEON dot product) must fail: it does (36 of 49 cases differ) |
| `backend` (`tools/test_backend.c`, `check_backend.py`) | the sample buffer, rate control, concealment and fast-forward on a synthetic 1 kHz tone through SDL's disk audio driver, with 4 s at normal speed, 6 s at normal speed with a 150 ms stall every 2 s, 4 s at 200 %, 4 s at 300 %, 4 s unthrottled at 6x, 4 s normal. Passes when the tone stays at 1 kHz (+-20 Hz) in every phase, no sample step exceeds the tone's own, and the silence is under 3 % (15 % with stalls). In the mute mode the fast-forward phases must be silent. It found and fixed: ringing across a buffer cut, a decimator crossfade longer than its carried samples, a hard cut into silence |
| `plugin` (`tools/test_suxty_if.c`) | the built plugin loaded the way the core does, from a fake core that provides the config API: the `Audio-SDL` seeding (once, `Audio-SDL` unchanged), the suxty interface (table sanity, `E_STATE`, range and step errors, set before and during a game, next-launch values, `save_default`, `E_THREAD` from another thread), pushes at several rates and speeds including the limiter off, and 20 ROM open / close cycles. It also passes under ASAN + UBSAN |
| `conceal` (`tools/test_conceal.c`, `run_conceal.sh`, `check_conceal.py`) | synthetic underruns (stalls of 20-200 ms every 1.5 s) on a 1 kHz tone, a sweep, a speech-like signal and band-limited noise, through the disk driver; measures sample-step clicks (more than 1.5x the clean run's largest step), the pitch of 20 ms windows (tone: within 3 %), verbatim-like repeats (20 ms segments that match an earlier segment 15-120 ms back with a normalised correlation above 0.9999, and above 0.99, noise only gated) and the extra silence. A stress run adds a 3 s burst into a full buffer, core rate changes 32006 > 22050 > 32006 Hz and a stall, with the same click detector, and checks that the automatic SDL period is at least 20 ms. A build with a deliberate error in the stretch (`-DSTRETCH_MUTATE`, no window-sum normalisation) must fail: it does (19-33 clicks on the tone) |
| `quality` | the resampler's gain, SNR and image numbers ([resampler.md](resampler.md#quality)); needs numpy and scipy |

An x86 end-to-end smoke test: mupen64plus-ui-console with the core, `--gfx dummy` (or foir built with `NEON=0`), rsp-hle, `SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy`, `--audio mupen64plus-audio-altru.so`, Super Mario 64. It plays and stops cleanly
(SIGTERM). SDL's dummy audio driver sleeps a whole number of milliseconds per period, so it runs about 5 % fast and the buffer reports underruns; use `SDL_AUDIODRIVER=disk SDL_DISKAUDIODELAY=12` for a driver that is a little slow instead.

## On the devices: `tools/device/session.sh`

`tools/device/session.sh rg35|rgds SECS COND:GAME ...` runs foir + arMLEt + the input plugin `mashinput.c` (it presses A and Start now and then so that the games leave their menus, and stops the core after SECS seconds so that the plugins write their statistics; `MASH_FF`
schedules speed changes) with the audio plugin `plug/mupen64plus-audio-<COND>.so` and the real SDL audio output. Clocks are pinned (rg35xxsp `cpu_perf.sh on`, 1512 MHz; rgdsplus `performance`) and restored; each run has a private HOME, XDG and configdir. The stock plugin is measured with
`tools/device/baseline-stats.patch` applied (the same counters, nothing else changed; its config is `RESAMPLE = trivial`, as on the play setup).

**The rg35xxsp's codec PCM is held by the UI** (PyUI keeps `hw:0` open as long as it runs, and a failed open was silently accepted by the stock plugin), and stopping the UI is not something a test should do. The sessions therefore play to the HDMI codec's PCM (`AUDIODEV=hw:2,0`): a real ALSA hardware device with
its own clock and the usual 44.1 / 22.05 kHz, two-period buffer; it is silent. The final listening check on the real speaker path is still to be done by hand (the N64 Dev launcher with the plugin in `play/plug`).

## Results

Games: Jet Force Gemini (jfg), Pokemon Puzzle League (ppl), Star Wars Episode I Racer (swr), Earthworm Jim 3D (ewj), Super Mario 64 from a save state (sm64). 40 s each from the first push, no human input beyond `mashinput.c`.
*Underruns*: callbacks that had to play silence. *Silence*: milliseconds of silence after the first push (for altru including the fill after every underrun, and the first fill). The stock plugin on top of that paused its device (*pauses*).
rg35xxsp: two stock sessions, one altru session. rgdsplus: one session each.

| game | device | stock underruns | stock silence ms | stock pauses (events / ms) | stock delays | altru underruns | altru silence ms | altru replays (20 ms) | altru target at the end |
|---|---|---|---|---|---|---|---|---|---|
| jfg | rg35xxsp | 27, 29 | 1254, 1347 | 2 / 37, 0 / 0 | 374, 333 | 4 | 576 | 28 | 3233 (147 ms) |
| ppl | rg35xxsp | 75, 77 | 3483, 3576 | 8 / 35, 11 / 19 | 246, 252 | 4 | 1030 | 27 | 3343 (152 ms) |
| swr | rg35xxsp | 126, 89 | 5851, 4133 | 2 / 56, 2 / 257 | 334, 364 | 2 | 578 | 114 | 2241 (102 ms) |
| ewj | rg35xxsp | 61, 133 | 1416, 3088 | 11 / 416, 35 / 551 | 398, 221 | 2 | 422 | 140 | 3822 (87 ms) |
| sm64 | rg35xxsp | 66, 68 | 1533, 1579 | 0, 4 / 3 | 376, 278 | 3 | 996 | 20 | 5364 (122 ms) |
| jfg | rgdsplus | 25 | 1161 | 1 / 20 | 857 | 4 | 681 | 19 | 3233 (147 ms) |
| ppl | rgdsplus | 73 | 3390 | 5 / 52 | 974 | 4 | 1032 | 44 | 3453 (157 ms) |
| swr | rgdsplus | 119 | 5526 | 4 / 419 | 369 | 3 | 603 | 134 | 2792 (127 ms) |
| ewj | rgdsplus | 96 | 2229 | 14 / 177 | 804 | 2 | 405 | 152 | 3822 (87 ms) |
| sm64 | rgdsplus | 54 | 1254 | 2 / 13 | 1075 | 3 | 1040 | 15 | 5364 (122 ms) |

Reading it:

- The stock plugin starved its callback 25-133 times in 40 s. altru has 2-4, the rest being concealed by replays, with no pause and no `SDL_Delay` on the rg35xxsp (the rgdsplus core is sometimes ahead of the audio clock, where the backstop delays are used and are what keep it in step).
- **The core is not at 100 % in these scenes.** The audio the games hand over is only 90-97 % of real time (measured from the pushed samples; this includes the start-up seconds), and in the parts of SW Racer and EWJ 80-90 %. No pacing can hide
  a missing 10 %: it is either replayed (altru: 100-150 replays in 40 s in the worst games, one every 0.3 s) or silent. That is why the remaining silence is 0.4-1 s in 40 s, nearly all of it one or two stalls of the core (up to 0.5 s without a push) and the fill after them.
- The cost is latency: the automatic buffer grew from 72 ms (22.05 kHz) / 46 ms to 87-157 ms in these games, against the stock fixed 93 / 46 ms plus its own pauses. It shrinks again after 10 s without an underrun.
- Without the concealment (`CONCEAL = False`) altru has 5-19 underruns and 1.2-3.3 s of silence in the same sessions (rg35xxsp): rate control and the automatic size account for the reduction of the underruns to a quarter, the concealment for most of the rest.
- A wider rate control (`RATE_ADJUST_PPM = 80000`, 8 %) moved SW Racer from 5.4 to 4.3 s of silence before the concealment existed. Not kept.

### Queue or callback

`OUTPUT_MODE = 1` (`SDL_QueueAudio`, rg35xxsp, same four games, concealment off in both): the queue had 8 / 19 / 19 / 6 underruns (jfg / swr / ewj / sm64) against 7 / 17 / 19 / 5 for the callback. The queue works on the mali SDL 2.28.5 (open with no callback, queue size readable) and its latency is the same, but a
starved queue cannot conceal or be accounted per sample, and the resampling moves into the emulation thread. No gain, so the callback stays the default and the queue mode stays as an experiment.

### Fast-forward

`MASH_FF=12:200,24:300,36:max,48:100`, SM64 from the state, foir `FOIR_FRAMESKIP=3` and `VSync = False` (foir's vsync would cap the core at 60 Hz). Speed = source samples pushed per second over the game rate.

| | 100 % | 200 % | 300 % | MAX (limiter off) |
|---|---|---|---|---|
| x86 (foir scalar, desktop), stock | 101 | 200 | 300 | 100 (audio delay throttles the core) |
| x86, altru | 100 | 200 | 300 | unthrottled (limited by the desktop only) |
| rgdsplus A55, stock | 99 | 177-200 | 168-243 (the core's limit) | 98 |
| rgdsplus A55, altru | 99 | 161-200 | 176-237 | 126-158 |
| rg35xxsp A53 | 99 | 99 | 99 | 99 (both plugins; the core is at its limit in this scene) |

With the stock plugin, FF at 200 / 300 % plays pitched up 2x / 3x; altru plays at normal pitch (`test_backend`: 1002 Hz at 200 %, 1003 Hz at 300 %, 1000 Hz unthrottled, for a 1000 Hz tone). The rg35xxsp cannot run this scene faster than real time at all, so its FF could not be measured.

### Concealment: replay against time-stretch

`tools/run_conceal.sh` with the replay build (the commit before the time-stretch) and the time-stretch build, fixed 40 ms buffer (so that stalls of 5-80 ms also run dry), 8 s runs, stalls every 1.5 s (5 per run). "Noise repeats" are the 20 ms segments of the noise signal that match an earlier one 15-120 ms back at above 0.99 (the clean run has 0), "extra silence" is the silence beyond the clean run.

| stall | replay: clicks / extra silence / noise repeats | time-stretch: clicks / extra silence / noise repeats |
|---|---|---|
| 5-40 ms | 0 / 0 / 0 | 0 / 0 / 0 |
| 80 ms | 0 / 0 / 2 | 0 / 0 / 2 |
| 120 ms | 0 / 0 / 14 | 0 / 0 / 1 |
| 200 ms | 0 / 0.24-0.26 s / 18 | 0 / 0.26 s / 0 |

The pitch of the tone stays within 2.5 % in both (the replay of a periodic tone is inaudible); on the sweep and the speech-like signal both stay click-free. The two cover about the same span (the replay 4 x 20 ms, the stretch 120 ms past the buffer), the difference is in what is heard: the replay plays the same 20 ms again, which is a buzz on noise-like and speech content; the stretch does not.
Cost of one stretch (`tools/bench_stretch.c`, 31 ms tail): 9 µs x86 desktop, 133 µs rgdsplus A55 (1.58 GHz), 143 µs rg35xxsp A53 (1512 MHz); the first version (full-rate search) took 345 µs on the A55.
On the devices, with the five games (30-40 s, real output on the rgdsplus, HDMI PCM on the rg35xxsp) the silence and the underruns are as before (0.4-1.2 s per 40 s, 1-4 underruns), the replays (15-150 per 40 s) became stretches (19-196).

### Popping on the rgdsplus

After the first listening session the user heard slight popping, most in SM64. The tap `ALTRU_TAP=<file>` (the stream as the callback hands it over, plus an event log of underruns, stretches, refills, flushes, drops, delays, rate changes and late callbacks) showed nothing: no sample step or second-difference outlier in 38 s of SM64 anywhere near an event, no overflow drop, one rate change (at start).
What the callbacks showed: 70-110 of 3400 arrived late (18-24 ms gaps for an 11.6 ms period). ROCKNIX plays through pipewire (graph quantum 128 frames at 48 kHz, the speaker sink is on `alsa_output...sound-speakers`). Recording the sink's monitor with `pw-record` (`-P '{ stream.capture.sink=true }'`) gave the objective answer: with the 512-frame (11.6 ms) period the pipewire output has 57-65 dropouts of 1.5-5 ms of digital zeros inside loud audio per 20 s (SM64, EWJ, SWR; 18-22 in JFG, PPL at 256 frames), 59 sample-step outliers in SM64; with a 1024-frame period (23 ms) it has none.
Cause: the period was too short for the pipewire path (and for the load: four busy threads), so the stream sometimes underflowed by one quantum; nothing in the plugin's own output. The stock plugin's period was 1024 frames. Fix: the automatic period is now the power of two nearest below `output rate / 40` (1024 at 44100 Hz, 512 at 22050 Hz, 256 at 11025 Hz). It adds about 12 ms of device latency.

| game (rgdsplus, pipewire monitor, 20 s) | dropouts with the 11.6 ms period | dropouts with the 23 ms period |
|---|---|---|
| sm64 | 60 (146 ms) | 0 |
| ewj | 63 (136 ms) | 0 |
| swr | 57 (107 ms) | 0 |
| jfg | 22 (35 ms) | 0 |
| ppl | 18 (34 ms) | 0 |

The rg35xxsp plays the HDMI PCM through plain ALSA in the sessions; its popping on the speaker path is still to be checked by ear.
