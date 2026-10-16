# altru (mupen64plus-audio-altru)

altru is a Mupen64Plus **audio plugin** for aarch64 retro handhelds, a fork of
[mupen64plus-audio-sdl](https://github.com/mupen64plus/mupen64plus-audio-sdl). The SDL output runs all the time, whatever the core does, instead of
being paused and restarted around every near-underrun:

- **Dynamic rate control**: the buffer level is held near a target by nudging the resample ratio (at most +-0.5 %), the way RetroArch does. There are no pauses and no
  hard delays except as a backstop when the core is far ahead. A real underrun is concealed by replaying the last 20 ms, then silence, then a refill.
- **Automatic buffer size**: it starts small, grows on an underrun and shrinks slowly; a fixed size is an option ([design](docs/design.md#buffer-size)).
- **Fast-forward at normal pitch**: audio sync is off while the core runs fast (a speed factor above 100 %, or the speed limiter off), and chunks of the audio are kept
  with short crossfades; or mute instead.
- **Own polyphase resampler** (32-tap Kaiser-windowed sinc, NEON): no libsamplerate or speex needed ([docs/resampler.md](docs/resampler.md)); both stay available as build options.
- **Options interface for [suxty](../mupen64plus-input-suxty)** (`AltruSuxtyGetInterface`), the same kind as foir's ([docs/options.md](docs/options.md)).

Measurements, stock plugin against altru on both devices: [docs/testing.md](docs/testing.md).

## Using it

```sh
mupen64plus --gfx mupen64plus-rdp-foir.so --rsp mupen64plus-rsp-armlet.so --audio mupen64plus-audio-altru.so game.z64
```

Settings live in the config section `Audio-Altru`. The first time it is missing and `Audio-SDL` exists, the keys the two plugins share are copied
over; `Audio-SDL` is never modified. All keys: [docs/options.md](docs/options.md).

## Building

```sh
scripts/build.sh native                   # host gcc -> out/native/
SDL2_LIBDIR=<dir with the device's libSDL2-2.0.so.0> scripts/build.sh aarch64      # rgdsplus (ROCKNIX)
scripts/build.sh rg35xxsp                 # glibc 2.31 sysroot (../toolchains/rg35xxsp), runs on both devices; or toolchains/rg35xxsp/build.sh altru
scripts/check.sh                          # host tests
```

The build runs on a copy of the sources (`work/`). It links `libSDL2-2.0.so.0` and libm and nothing else (SDL 2.0.4+ API only, so the rg35xxsp's
SDL 2.28.5 is enough). `USE_SPEEX=1` / `USE_SRC=1` add the speex / libsamplerate resamplers (opt-in). `projects/unix/Makefile` is the plain make build; `-O3`, generic armv8-a.

## Layout

| path | content |
|---|---|
| `src/main.c` | plugin API, config (`Audio-Altru`, seeding from `Audio-SDL`), volume |
| `src/sdl_backend.c` | the sample buffer, rate control, buffer sizing, concealment, fast-forward, SDL device (callback or queue), statistics |
| `src/suxty.c`, `src/altru_suxty.h` | the suxty interface and the option lookup |
| `src/resamplers/` | `poly.c` (polyphase, NEON + scalar), `trivial.c`, opt-in `speex.c`, `src.c` |
| `tools/` | tests and measurement tools ([docs/testing.md](docs/testing.md)) |
| `scripts/` | `build.sh`, `check.sh` |

## License

GPL-2.0-or-later, as the original plugin; see the file headers and `LICENSES`.
