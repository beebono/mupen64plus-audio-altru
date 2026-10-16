# Options

## Config section `Audio-Altru`

In `mupen64plus.cfg`. When the section does not exist yet and `Audio-SDL` does, these keys are copied from it on the first start: `DEFAULT_FREQUENCY`, `SWAP_CHANNELS`,
`SECONDARY_BUFFER_SIZE`, `RESAMPLE`, `VOLUME_ADJUST`, `VOLUME_DEFAULT`, `AUDIO_SYNC`, but only values that differ from audio-sdl's stock defaults (33600, False, 1024, `trivial` / `speex-fixed-4` / `src-sinc-medium-quality`, 5, 80, True): an untuned config gets altru's own defaults (the stock buffer keys `PRIMARY_BUFFER_SIZE` / `PRIMARY_BUFFER_TARGET` have no counterpart and are not copied).
Keys given with `--set Audio-Altru[...]` before the start are kept. `Audio-SDL` is only read. A `RESAMPLE` value this build does not have (for example `src-sinc-fastest` in a default build) falls back to `poly` with a warning.

| key | default | meaning |
|---|---|---|
| `DEFAULT_FREQUENCY` | 33600 | rate used until the game sets one |
| `SWAP_CHANNELS` | False | swap left and right |
| `SECONDARY_BUFFER_SIZE` | 0 | SDL device period in output samples (a power of two); 0 = automatic, about 23 ms (1024 at 44100 Hz; see [design.md](design.md#buffer-size)) |
| `RESAMPLE` | poly | `poly`, `trivial`; with `USE_SPEEX=1` `speex-fixed-{0-10}`; with `USE_SRC=1` `src-*` ([resampler.md](resampler.md)) |
| `BUFFER_MODE` | 0 | 0 = automatic buffer size, 1 = fixed ([design.md](design.md#buffer-size)) |
| `BUFFER_MS` | 60 | buffer level in milliseconds in fixed mode |
| `AUDIO_SYNC` | True | delay the core when it is far ahead of the audio clock (backstop only) |
| `CONCEAL` | True | time-stretch the audio left in the buffer (pitch kept) when the core falls behind, before fading to silence ([design.md](design.md#underruns)) |
| `FF_MODE` | 0 | fast-forward audio: 0 = normal pitch, 1 = muted |
| `RATE_ADJUST_PPM` | 5000 | largest resample ratio change of the rate control (5000 = 0.5 %) |
| `OUTPUT_MODE` | 0 | 0 = SDL callback, 1 = `SDL_QueueAudio` (experimental) |
| `VOLUME_ADJUST`, `VOLUME_DEFAULT` | 5, 80 | volume step, start volume (percent) |

Environment: `ALTRU_STATS=<file>` writes the counters and the buffer trace when the ROM closes ([design.md](design.md#statistics)); `ALTRU_TAP=<file>` (debugging) writes the output stream as raw S16 stereo (up to 60 s) and `<file>.ev`, an event log (underrun, stretch, refill end, flush, drop, delay, rate change, late or slow callback) with the output frame of each.

## suxty interface

`src/altru_suxty.h`, modelled on foir's `foir_suxty.h`. The shared object exports
`const struct altru_suxty_interface *AltruSuxtyGetInterface(void)`, a pointer to a static const table. A client checks `abi` (1) and `size` first. It holds a const option table
and three calls, `get`, `set` and `save_default`, all on plain `int` values.

**Discovery (suxty side).** As for foir (`src/foir_if.c` in suxty): walk the loaded objects with `dl_iterate_phdr`, take a handle with `dlopen(name, RTLD_NOW | RTLD_NOLOAD)` and `dlsym` for `AltruSuxtyGetInterface`;
accept it when `abi == ALTRU_SUXTY_ABI`, `size >= sizeof(struct altru_suxty_interface)` and the option count fits. No symbol means another audio plugin and no Audio page.

**Option descriptor.** `key` (the config parameter name), `label` (title case), `help` (one line), `type` (bool 0/1, int with `min`/`max`/`step`, enum 0..`max` with `enum_labels`), `flags` (`ALTRU_OPTF_LIVE`: takes effect at once, otherwise at the next launch), `def`.

| id | key | label | type | live | default |
|---|---|---|---|---|---|
| 0 | `VOLUME_DEFAULT` | Volume | int 0-100 step 5 | yes | 80 |
| 1 | `FF_MODE` | Fast-Forward Audio | enum: Normal Pitch, Muted | yes | Normal Pitch |
| 2 | `AUDIO_SYNC` | Audio Sync | bool | yes | on |
| 3 | `RESAMPLE` | Resampler | enum: Polyphase, Nearest (+ Speex, Sinc in builds with them) | yes | Polyphase |
| 4 | `BUFFER_MODE` | Buffer Size | enum: Automatic, Fixed | next launch | Automatic |
| 5 | `BUFFER_MS` | Buffer Length | int 20-300 ms step 10 | next launch | 60 |
| 6 | `CONCEAL` | Underrun Concealment | bool | yes | on |

**Calls.**

- `get(id, &v)`: the value in effect. Running: the running value (the volume as the plugin has it now). Before `RomOpen`: what the game will start with: a `set()` value if there is one, else the config.
- `set(id, v)`: never writes `mupen64plus.cfg`. Before `RomOpen` it overrides the config for the coming launch (this is what suxty does at game start for its per-game values; the `[audio] volume` of its per-ROM file is option 0).
  Running: a live option is applied at once (`ALTRU_SUXTY_APPLIED`), a next-launch option is remembered and the running game keeps its value (`ALTRU_SUXTY_NEXT_LAUNCH`).
  Overrides last until `PluginShutdown`, across ROM close and open.
- `save_default(id, v)`: writes the key in `[Audio-Altru]` and saves that section (the user's global default). It does not touch the running game or the `set()` values.
- Errors: `ALTRU_SUXTY_E_INDEX`, `E_RANGE` (also a value off the step), `E_THREAD`, `E_STATE` (plugin not started), `E_CONFIG` (`save_default`).

**Threading.** Like foir's: `get`/`set`/`save_default` on the emulation thread (the thread that calls `RomOpen`/`AiLenChanged`; suxty's menu runs there, inside the buffer swap), not from inside a plugin call. With the ROM open a call from another thread returns `ALTRU_SUXTY_E_THREAD`.
Before `RomOpen` any one thread may call.

**What suxty needs to do** (follow-up, not in this repository): discover the interface as above, add an Audio page from the descriptors (like the foir page), apply its per-game `[audio]` values with `set()` before the game starts (the volume as `set(0, volume)`),
and offer `save_default`. For fast-forward nothing changes: suxty's `M64CORE_SPEED_FACTOR` / `M64CORE_SPEED_LIMITER` calls are what altru detects (no extra call).
