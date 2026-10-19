#!/bin/bash
# Host tests (see docs/testing.md). Output goes to work/check/. Every local run is capped in memory.
#   scripts/check.sh [resample|a64|backend|plugin|quality|all]      (default: all except quality)
#   resample: polyphase resampler, scalar path alone (x86)       a64: NEON vs scalar under qemu-aarch64 + the mutation build, which must fail
#   backend: pacing / fast-forward on a synthetic tone, disk audio driver   plugin: lifecycle, seeding, suxty interface through a fake core
#   conceal: underrun concealment on tone / sweep / speech / noise with stalls of 20-200 ms (clicks, pitch, verbatim repeats), then the same with a deliberate error, which must fail
#   quality: passband / SNR / image numbers (needs numpy, scipy)
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
API=${M64P_API_INCLUDE_DIR:-$R/../mupen64plus-core-dento/src/api}
W=$R/work/check; mkdir -p "$W"
CAP=(systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 300)
SRC=("$R"/src/resamplers/resamplers.c "$R"/src/resamplers/poly.c "$R"/src/resamplers/trivial.c)
what=${1:-all}
want() { [ "$what" = all ] || [ "$what" = "$1" ]; }

if want resample; then
    gcc -O2 -Wall -I"$R/src" -o "$W/test_resample" "$R/tools/test_resample.c" "$R/src/resamplers/poly.c" -lm
    "${CAP[@]}" "$W/test_resample"
fi
if want a64; then
    aarch64-linux-gnu-gcc -O2 -Wall -I"$R/src" -o "$W/test_resample_a64" "$R/tools/test_resample.c" "$R/src/resamplers/poly.c" -lm
    aarch64-linux-gnu-gcc -O2 -DPOLY_MUTATE -I"$R/src" -o "$W/test_resample_a64_mut" "$R/tools/test_resample.c" "$R/src/resamplers/poly.c" -lm
    "${CAP[@]}" qemu-aarch64-static -L /usr/aarch64-linux-gnu "$W/test_resample_a64"
    if "${CAP[@]}" qemu-aarch64-static -L /usr/aarch64-linux-gnu "$W/test_resample_a64_mut" >/dev/null; then
        echo "MUTATION NOT CAUGHT" >&2; exit 1
    fi
    echo "mutation (a deliberate error in the NEON dot product) caught"
fi
if want backend; then
    gcc -O2 -Wall -I"$R/src" -I"$API" $(pkg-config --cflags sdl2) -o "$W/test_backend" "$R/tools/test_backend.c" "$R/src/sdl_backend.c" "${SRC[@]}" -lm $(pkg-config --libs sdl2) -lpthread
    for mode in 0 1; do
        echo "-- test_backend mode $mode ($([ $mode = 1 ] && echo 'mute in fast-forward' || echo 'normal pitch in fast-forward'))"
        rm -f "$W/out.raw"
        SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE="$W/out.raw" SDL_DISKAUDIODELAY=12 "${CAP[@]}" "$W/test_backend" $mode 2>/dev/null
        python3 "$R/tools/check_backend.py" "$W/out.raw" $([ $mode = 1 ] && echo mute)
    done
fi
if want conceal; then
    for v in "" "-DSTRETCH_MUTATE"; do
        gcc -O2 -Wall $v -I"$R/src" -I"$API" $(pkg-config --cflags sdl2) -o "$W/test_conceal$v" "$R/tools/test_conceal.c" "$R/src/sdl_backend.c" "${SRC[@]}" -lm $(pkg-config --libs sdl2) -lpthread
    done
    "$R/tools/run_conceal.sh" "$W/test_conceal" "$W/conceal" 20 80 120 200 || { echo "CONCEAL CHECK FAILED" >&2; exit 1; }
    echo "-- stress: 3 s burst into a full buffer, rate changes 32006 > 22050 > 32006 Hz, a stall (tone, click detector)"
    rm -f "$W/stress.raw"; SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE="$W/stress.raw" SDL_DISKAUDIODELAY=12 STRESS=1 ALTRU_STATS="$W/stress.csv" "${CAP[@]}" "$W/test_conceal" tone 0 10 2>/dev/null
    awk '/^out_freq/ {hz=$2} /^sec/ {ms=$2*1000/hz; printf "automatic SDL period: %d frames = %.1f ms\n", $2, ms; if (ms < 20) { print "PERIOD TOO SHORT (pipewire dropouts below ~20 ms)" > "/dev/stderr"; exit 1 }}' "$W/stress.csv.sum" || exit 1
    python3 "$R/tools/check_conceal.py" tone "$W/stress.raw" "$W/conceal/tone_0.raw" || { echo "STRESS CHECK FAILED" >&2; exit 1; }
    if "$R/tools/run_conceal.sh" "$W/test_conceal-DSTRETCH_MUTATE" "$W/conceal_mut" 80 120 200 >/dev/null; then
        echo "MUTATION NOT CAUGHT" >&2; exit 1
    fi
    echo "mutation (no window-sum normalisation in the stretch) caught"
fi
if want plugin; then
    make -C "$R/projects/unix" all APIDIR="$API" POSTFIX=-check OPTFLAGS="-O1 -g" >/dev/null
    gcc -O1 -g -Wall -I"$R/src" -I"$API" -rdynamic -o "$W/test_suxty_if" "$R/tools/test_suxty_if.c" -ldl -lpthread
    SDL_AUDIODRIVER=dummy "${CAP[@]}" "$W/test_suxty_if" "$R/projects/unix/mupen64plus-audio-altru-check.so"
    rm -rf "$R/projects/unix/_obj-check" "$R/projects/unix/mupen64plus-audio-altru-check.so"
fi
if [ "$what" = quality ]; then
    gcc -O2 -shared -fPIC -I"$R/src" -o "$W/librs.so" "$R/tools/quality/wrap.c" "$R/src/resamplers/poly.c" "$R/src/resamplers/trivial.c" -lm
    python3 "$R/tools/quality/quality.py" "$W/librs.so"
fi
