# Testing

## Host tests: `scripts/check.sh`

Every local run is memory-capped (`systemd-run --user --scope -p MemoryMax=2G`). `scripts/check.sh [resample|a64|backend|plugin|quality]`:

| test | what it checks |
|---|---|
| `resample` (`tools/test_resample.c`) | the polyphase resampler on x86 (scalar) against itself with chunked, per-call ratio changes, and the DC gain |
| `a64` | the same program for aarch64 under qemu-aarch64: NEON against scalar must give 0 differences in all 49 cases (random, full-scale and quiet input, 7 rate pairs, fixed and changing ratios). The same build with `-DPOLY_MUTATE` (a deliberate error in the NEON dot product) must fail: it does (36 of 49 cases differ) |
| `backend` (`tools/test_backend.c`, `check_backend.py`) | the sample buffer, rate control, concealment and fast-forward on a synthetic 1 kHz tone through SDL's disk audio driver, with 4 s at normal speed, 6 s at normal speed with a 150 ms stall every 2 s, 4 s at 200 %, 4 s at 300 %, 4 s unthrottled at 6x, 4 s normal. Passes when the tone stays at 1 kHz (+-20 Hz) in every phase, no sample step exceeds the tone's own, and the silence is under 3 % (15 % with stalls). In the mute mode the fast-forward phases must be silent. It found and fixed: ringing across a buffer cut, a decimator crossfade longer than its carried samples, a hard cut into silence |
| `plugin` (`tools/test_suxty_if.c`) | the built plugin loaded the way the core does, from a fake core that provides the config API: the `Audio-SDL` seeding (once, `Audio-SDL` unchanged), the suxty interface (table sanity, `E_STATE`, range and step errors, set before and during a game, next-launch values, `save_default`, `E_THREAD` from another thread), pushes at several rates and speeds including the limiter off, and 20 ROM open / close cycles. It also passes under ASAN + UBSAN |
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
