/* altru - test_backend.c: drives the sample buffer / rate control / fast-forward code (src/sdl_backend.c) with a synthetic 1 kHz tone through
 * SDL's disk audio driver (run with SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=out.raw) and phases of normal speed, stalls, 200 %, 300 %, unthrottled.
 * tools/check_backend.py analyses the raw output. SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL.h>
#include "main.h"
#include "sdl_backend.h"

static int stub_int(m64p_handle h, const char* n);
/* stubs for what main.c provides */
ptr_ConfigGetParamInt ConfigGetParamInt;
ptr_ConfigGetParamBool ConfigGetParamBool;
ptr_ConfigGetParamString ConfigGetParamString;
void DebugMessage(int level, const char* message, ...) { (void)level; (void)message; }
int altru_gain(void) { return 128; }
int altru_opt_int(const char* key) { return stub_int(NULL, key) | (!strcmp(key, "AUDIO_SYNC") || !strcmp(key, "CONCEAL")); }
const char* altru_opt_resampler(void) { return "poly"; }

static int l_ffmute, l_buffer_mode, l_queue;
static int stub_int(m64p_handle h, const char* n)
{
    (void)h;
    if (!strcmp(n, "DEFAULT_FREQUENCY")) return 32006;
    if (!strcmp(n, "SECONDARY_BUFFER_SIZE")) return 512;
    if (!strcmp(n, "FF_MODE")) return l_ffmute;
    if (!strcmp(n, "BUFFER_MODE")) return l_buffer_mode;
    if (!strcmp(n, "BUFFER_MS")) return 80;
    if (!strcmp(n, "OUTPUT_MODE")) return l_queue;
    if (!strcmp(n, "RATE_ADJUST_PPM")) return 5000;
    return 0;
}
static int stub_bool(m64p_handle h, const char* n)
{
    (void)h;
    return !strcmp(n, "AUDIO_SYNC") || !strcmp(n, "CONCEAL");
}
static const char* stub_str(m64p_handle h, const char* n) { (void)h; (void)n; return "poly"; }

static int l_limiter = 1;
static m64p_error core_cmd(m64p_command c, int p, void* v)
{
    if (c == M64CMD_CORE_STATE_QUERY && p == M64CORE_SPEED_LIMITER) {
        *(int*)v = l_limiter;
        return M64ERR_SUCCESS;
    }
    return M64ERR_INPUT_INVALID;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static double l_phase;
static void push_ms(struct sdl_backend* b, double ms, double speed)
{
    static int16_t buf[2 * 8192];
    const double hz = 32006;
    int n = (int)(hz * ms / 1000.0 * speed), i;
    if (n > 8192) n = 8192;
    for (i = 0; i < n; i++) {
        int16_t v = (int16_t)(12000 * sin(l_phase));
        l_phase += 2 * M_PI * 1000.0 / hz;
        /* the core hands over words in native order; with the default swap setting the plugin swaps the halves back */
        buf[2 * i] = v;
        buf[2 * i + 1] = v;
    }
    sdl_push_samples(b, buf, (size_t)n * 4);
}

/* wall seconds, speed (x real time), core speed factor %, limiter, stall: ms stall every 2 s */
static void phase(struct sdl_backend* b, double secs, double speed, int pct, int limiter, int stall_ms)
{
    double t0 = now(), next = t0, last_stall = t0;
    l_limiter = limiter;
    sdl_set_speed_factor(b, (unsigned)pct);
    while (now() - t0 < secs) {
        push_ms(b, 1000.0 / 60.0, speed);
        if (stall_ms && now() - last_stall > 2.0) {
            struct timespec ts = { 0, stall_ms * 1000000L };
            nanosleep(&ts, NULL);
            last_stall = now();
            next = now();
        }
        next += 1.0 / 60.0;
        while (now() < next) {
            struct timespec ts = { 0, 500000 };
            nanosleep(&ts, NULL);
        }
    }
}

int main(int argc, char** argv)
{
    struct sdl_backend* b;
    int mode = argc > 1 ? atoi(argv[1]) : 0; /* bit 0: mute in FF, bit 1: fixed buffer, bit 2: queue output */

    ConfigGetParamInt = stub_int;
    ConfigGetParamBool = stub_bool;
    ConfigGetParamString = stub_str;
    l_ffmute = mode & 1;
    l_buffer_mode = (mode >> 1) & 1;
    l_queue = (mode >> 2) & 1;
    b = init_sdl_backend_from_config(NULL, core_cmd);
    if (b == NULL)
        return 1;
    phase(b, 4, 1.0, 100, 1, 0);      /* 0-4   s normal */
    phase(b, 6, 1.0, 100, 1, 150);    /* 4-10  s normal with a 150 ms stall every 2 s */
    phase(b, 4, 2.0, 200, 1, 0);      /* 10-14 s 200 % */
    phase(b, 4, 3.0, 300, 1, 0);      /* 14-18 s 300 % */
    phase(b, 4, 6.0, 100, 0, 0);      /* 18-22 s limiter off, 6x */
    phase(b, 4, 1.0, 100, 1, 0);      /* 22-26 s back to normal */
    release_sdl_backend(b);
    return 0;
}
