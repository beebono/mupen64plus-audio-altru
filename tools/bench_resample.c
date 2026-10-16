/* altru - bench_resample.c: CPU time of the resamplers per second of output audio. SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "resamplers/resamplers.h"
#include "resamplers/poly.h"

extern const struct resampler_interface g_poly_iresampler, g_trivial_iresampler;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static double run(const struct resampler_interface* ir, int scalar, unsigned in_hz, unsigned out_hz, unsigned seconds)
{
    size_t n = (size_t)in_hz * 2 + 64, produced_total = 0, pos = 0;
    int16_t* in = malloc((n + RS_HIST) * 4);
    int16_t* out = malloc(1024 * 4);
    uint64_t step = (uint64_t)in_hz * RS_ONE / out_hz;
    void* rs = ir->create("x", in_hz, out_hz);
    size_t want_total = (size_t)out_hz * seconds, i;
    double t0, t1;

    for (i = 0; i < (n + RS_HIST) * 2; i++)
        in[i] = (int16_t)(rand() * 7);
    poly_force_scalar(scalar);
    t0 = now();
    while (produced_total < want_total) {
        size_t got = 0, c = ir->process(rs, in + 2 * (RS_HIST + pos), n - pos, out, 1024, step, &got);
        pos += c;
        produced_total += got;
        if (n - pos < 64)
            pos = 0;
    }
    t1 = now();
    ir->release(rs);
    free(in);
    free(out);
    return (t1 - t0) / seconds; /* seconds of CPU per second of audio */
}

int main(void)
{
    static const unsigned cases[][2] = { { 32006, 44100 }, { 22047, 22050 }, { 44100, 44100 }, { 11025, 22050 } };
    unsigned c;
    printf("%-14s %-9s %12s %14s\n", "rates", "impl", "ns/out frame", "% of one core");
    for (c = 0; c < 4; c++) {
        double t;
        t = run(&g_trivial_iresampler, 0, cases[c][0], cases[c][1], 10);
        printf("%5u->%-7u %-9s %12.1f %13.3f%%\n", cases[c][0], cases[c][1], "trivial", t / cases[c][1] * 1e9, t * 100);
        t = run(&g_poly_iresampler, 1, cases[c][0], cases[c][1], 10);
        printf("%5u->%-7u %-9s %12.1f %13.3f%%\n", cases[c][0], cases[c][1], "poly-sc", t / cases[c][1] * 1e9, t * 100);
        if (poly_has_neon()) {
            t = run(&g_poly_iresampler, 0, cases[c][0], cases[c][1], 10);
            printf("%5u->%-7u %-9s %12.1f %13.3f%%\n", cases[c][0], cases[c][1], "poly-neon", t / cases[c][1] * 1e9, t * 100);
        }
    }
    return 0;
}
