/* altru - test_conceal.c: synthetic underruns through SDL's disk audio driver, for tools/check_conceal.py.
 *   SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=out.raw SDL_DISKAUDIODELAY=12 test_conceal tone|sweep|speech|noise STALL_MS SECS [conceal 0|1 [fixed buffer ms, 0 = automatic]]
 * STRESS=1 (tone only, 10 s): a burst of 3 s of audio in one go (the buffer overflows), a core rate change to 22.05 kHz and back, and a stall, with the same click detector as the stall runs (tools/check_conceal.py).
 * Pushes 60 Hz frames of a synthetic signal in real time; every 1.5 s (from 2 s on) the core stalls for STALL_MS (0 = never).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL.h>
#include "main.h"
#include "sdl_backend.h"

ptr_ConfigGetParamInt ConfigGetParamInt;
ptr_ConfigGetParamBool ConfigGetParamBool;
ptr_ConfigGetParamString ConfigGetParamString;
void DebugMessage(int level, const char* message, ...) { (void)level; (void)message; }
int altru_gain(void) { return 128; }
static int l_conceal = 1, l_bufms;
int altru_opt_int(const char* key) { if (!strcmp(key, "BUFFER_MODE")) return l_bufms != 0; if (!strcmp(key, "BUFFER_MS")) return l_bufms; return !strcmp(key, "AUDIO_SYNC") || (!strcmp(key, "CONCEAL") && l_conceal); }
const char* altru_opt_resampler(void) { return "poly"; }
static int stub_int(m64p_handle h, const char* n)
{
    (void)h;
    if (!strcmp(n, "DEFAULT_FREQUENCY")) return 32006;
    if (!strcmp(n, "SECONDARY_BUFFER_SIZE")) return 0;   /* the automatic period */
    if (!strcmp(n, "BUFFER_MS")) return l_bufms;
    if (!strcmp(n, "BUFFER_MODE")) return l_bufms != 0;
    if (!strcmp(n, "RATE_ADJUST_PPM")) return 5000;
    return 0;
}
static int stub_bool(m64p_handle h, const char* n) { (void)h; return !strcmp(n, "AUDIO_SYNC") || (!strcmp(n, "CONCEAL") && l_conceal); }
static const char* stub_str(m64p_handle h, const char* n) { (void)h; (void)n; return "poly"; }
static m64p_error core_cmd(m64p_command c, int p, void* v)
{
    if (c == M64CMD_CORE_STATE_QUERY && p == M64CORE_SPEED_LIMITER) { *(int*)v = 1; return M64ERR_SUCCESS; }
    return M64ERR_INPUT_INVALID;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static unsigned rng = 12345;
static double frand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0 - 0.5; }

static double l_hz = 32006;
static int sig;
static double ph, ph_t, f0, am, noise_lp;
static double formant_ph[3];
/* one source frame (n = running index) */
static int16_t gen(long n)
{
    const double hz = l_hz, t = n / 32006.0;
    if (sig == 3) {    /* band-limited noise with a 3 Hz envelope: nothing in it may come back verbatim */
        static double lp1, lp2;
        double env = 0.5 + 0.5 * sin(2 * M_PI * 3.0 * t);
        lp1 += 0.35 * (frand() * 2 - lp1); lp2 += 0.35 * (lp1 - lp2);
        return (int16_t)(env * 20000 * lp2);
    }
    if (sig == 0) { ph += 2 * M_PI * 1000.0 / hz; return (int16_t)(12000 * sin(ph)); }
    if (sig == 1) {    /* sweep 200 Hz - 4 kHz over 3 s, back and forth (log) */
        double u = fmod(t / 3.0, 2.0); if (u > 1) u = 2 - u;
        double f = 200.0 * pow(20.0, u);
        ph += 2 * M_PI * f / hz;
        return (int16_t)(12000 * sin(ph));
    }
    /* speech-like: jittering voiced source (pitch 100-140 Hz with drift), three moving formants, 3.3 Hz syllable envelope, a little breath noise */
    {
        double p0 = 120 + 20 * sin(2 * M_PI * 0.7 * t) + 6 * sin(2 * M_PI * 3.1 * t + 1);
        double s = 0, env = fmax(0.0, sin(2 * M_PI * 3.3 * t)); env = env * env;
        int h;
        ph_t += 2 * M_PI * p0 / hz;
        double fm[3] = { 500 + 250 * sin(2 * M_PI * 0.9 * t), 1500 + 600 * sin(2 * M_PI * 0.5 * t + 2), 2600 + 200 * sin(2 * M_PI * 0.3 * t) };
        for (h = 1; h <= 30; h++) {
            double fh = p0 * h, a = 0; int k;
            if (fh > 4500) break;
            for (k = 0; k < 3; k++) a += 1.0 / (1.0 + pow((fh - fm[k]) / 150.0, 2)) * (k == 0 ? 1.0 : 0.5);
            s += a * sin(h * ph_t) / h;
        }
        noise_lp += 0.3 * (frand() - noise_lp);
        (void)formant_ph; (void)f0; (void)am;
        return (int16_t)(env * 14000 * s + 800 * noise_lp);
    }
}

static long l_n;

static void push_ms(struct sdl_backend* b, double ms)
{
    static int16_t buf[2 * 8192];
    int n = (int)(l_hz * ms / 1000.0), i;
    for (i = 0; i < n; i++) { int16_t v = gen(l_n++); buf[2 * i] = v; buf[2 * i + 1] = v; }
    sdl_push_samples(b, buf, (size_t)n * 4);
}

int main(int argc, char** argv)
{
    struct sdl_backend* b;
    double secs, stall, t0, next, last_stall;
    if (argc < 4) return 2;
    sig = !strcmp(argv[1], "sweep") ? 1 : !strcmp(argv[1], "speech") ? 2 : !strcmp(argv[1], "noise") ? 3 : 0;
    stall = atof(argv[2]); secs = atof(argv[3]);
    if (argc > 4) l_conceal = atoi(argv[4]);
    if (argc > 5) l_bufms = atoi(argv[5]);
    ConfigGetParamInt = stub_int; ConfigGetParamBool = stub_bool; ConfigGetParamString = stub_str;
    b = init_sdl_backend_from_config(NULL, core_cmd);
    if (b == NULL) return 1;
    t0 = now(); next = t0; last_stall = t0 + 0.5;
    if (getenv("STRESS")) {
        int step = 0;
        secs = 10; stall = 0;
        while (now() - t0 < secs) {
            double t = now() - t0;
            if (t > 2 && step == 0) { int k; for (k = 0; k < 45; k++) push_ms(b, 1000.0 / 60.0 * 4); step++; }   /* 3 s at once */
            if (t > 4 && step == 1) { l_hz = 22050; sdl_set_frequency(b, 22050); step++; }
            if (t > 6 && step == 2) { l_hz = 32006; sdl_set_frequency(b, 32006); step++; }
            if (t > 7 && step == 3) { step += 2; }
            if (t > 8.5 && step == 5) { struct timespec ts = { 0, 150000000L }; nanosleep(&ts, NULL); next = now(); step++; }
            push_ms(b, 1000.0 / 60.0);
            next += 1.0 / 60.0;
            while (now() < next) { struct timespec ts = { 0, 500000 }; nanosleep(&ts, NULL); }
        }
        release_sdl_backend(b);
        return 0;
    }
    while (now() - t0 < secs) {
        push_ms(b, 1000.0 / 60.0);
        if (stall > 0 && now() - last_stall > 1.5) {
            struct timespec ts = { (time_t)(stall / 1000), (long)(fmod(stall, 1000) * 1e6) };
            nanosleep(&ts, NULL);
            last_stall = now(); next = now();
        }
        next += 1.0 / 60.0;
        while (now() < next) { struct timespec ts = { 0, 500000 }; nanosleep(&ts, NULL); }
    }
    release_sdl_backend(b);
    return 0;
}
