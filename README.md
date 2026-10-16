# altru (mupen64plus-audio-altru)

altru is a Mupen64Plus **audio plugin** for aarch64 retro handhelds, a fork of
[mupen64plus-audio-sdl](https://github.com/mupen64plus/mupen64plus-audio-sdl). It targets smooth audio on slow CPUs: no pauses of the output
device, a buffer that sizes itself, fast-forward at normal pitch, and an options interface for the
[suxty](../mupen64plus-input-suxty) in-game menu.

Status: the plugin is renamed and builds for both devices; the pacing rework is in progress (see the commit log).

## Using it

```sh
mupen64plus --gfx mupen64plus-rdp-foir.so --rsp mupen64plus-rsp-armlet.so --audio mupen64plus-audio-altru.so game.z64
```

Settings live in the config section `Audio-Altru`. The first time it is missing and `Audio-SDL` exists, the keys the two plugins share are copied
over; `Audio-SDL` is never modified. Options: [docs/options.md](docs/options.md).

## Building

```sh
scripts/build.sh native                   # host gcc -> out/native/
SDL2_LIBDIR=<dir with the device's libSDL2-2.0.so.0> scripts/build.sh aarch64      # rgdsplus (ROCKNIX)
scripts/build.sh rg35xxsp                 # glibc 2.31 sysroot (../toolchains/rg35xxsp), runs on glibc >= 2.17; or toolchains/rg35xxsp/build.sh altru
```

The build runs on a copy of the sources (`work/`). It links `libSDL2-2.0.so.0` and nothing else (SDL 2.0.4+ API only, so the
rg35xxsp's SDL 2.28.5 is enough). `projects/unix/Makefile` is the plain make build; `-O3`, generic armv8-a.

## License

GPL-2.0-or-later, as the original plugin; see the file headers and `LICENSES`.
