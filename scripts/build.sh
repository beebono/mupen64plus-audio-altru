#!/bin/bash
# Builds mupen64plus-audio-altru.so from a copy of the sources (work/<target>/), so a tree that is being edited does not matter.
#   scripts/build.sh native|aarch64|rg35xxsp [make variables...]    -> out/<target>/mupen64plus-audio-altru.so
# native: host gcc; aarch64: aarch64-linux-gnu-* with the host SDL2 headers (rgdsplus, ROCKNIX, glibc 2.44);
# rg35xxsp: $RG35_TOOLCHAIN (default ../../../resources/toolchains/rg35xxsp, relative to this repo): glibc 2.31 sysroot, SDL 2.30.9 link stub (the device has the mali SDL 2.28.5);
# its output needs glibc <= 2.17 and runs on both devices. USE_SPEEX=1 / USE_SRC=1 add the optional resamplers (libspeexdsp / libsamplerate).
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
API=${M64P_API_INCLUDE_DIR:-$R/../mupen64plus-core-dento/src/api}
RG35=${RG35_TOOLCHAIN:-$R/../../../resources/toolchains/rg35xxsp}
target=$1; shift || true
W=$R/work/$target; OUT=$R/out/$target
case $target in
native)   args=(PKG_CONFIG=pkg-config) ;;
aarch64)  args=(CROSS_COMPILE=aarch64-linux-gnu- HOST_CPU=aarch64 PKG_CONFIG=true
                SDL_CFLAGS="-I${SDL2_INCDIR:-/usr/aarch64-linux-gnu/include/SDL2} -D_REENTRANT"
                SDL_LDLIBS="-L${SDL2_LIBDIR:?set SDL2_LIBDIR to a directory with the device libSDL2-2.0.so.0} -l:libSDL2-2.0.so.0 -Wl,--allow-shlib-undefined") ;;
rg35xxsp) args=(CROSS_COMPILE=aarch64-linux-gnu- HOST_CPU=aarch64 PKG_CONFIG=true CC="$RG35/bin/rg35-gcc"
                SDL_CFLAGS="-I$RG35/sdl2-stub/include/SDL2 -D_REENTRANT"
                SDL_LDLIBS="-L$RG35/sdl2-stub/lib -l:libSDL2-2.0.so.0 -Wl,--allow-shlib-undefined") ;;
*) echo "usage: $0 native|aarch64|rg35xxsp [make variables]" >&2; exit 2 ;;
esac
mkdir -p "$W" "$OUT"
rsync -a --delete --exclude /out --exclude /work --exclude '_obj*' --exclude .git --exclude '*.so' "$R/" "$W/src/"
make -C "$W/src/projects/unix" all APIDIR="$API" "${args[@]}" "$@" -j"$(nproc)"
cp "$W/src/projects/unix/mupen64plus-audio-altru.so" "$OUT/"
f=$OUT/mupen64plus-audio-altru.so
case $target in native) NM=nm; OD=objdump ;; *) NM=aarch64-linux-gnu-nm; OD=aarch64-linux-gnu-objdump ;; esac
printf '%s\n  exports: %s\n' "$f" "$($NM -D --defined-only "$f" | awk '{print $3}' | sort | tr '\n' ' ')"
glibc=$($OD -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1); echo "  needs: $glibc"
if [ "$target" = rg35xxsp ] && [ "$(printf '%s\nGLIBC_2.17\n' "$glibc" | sort -V | tail -1)" != GLIBC_2.17 ]; then
    echo "  FAIL: glibc floor above 2.17" >&2; exit 1
fi
