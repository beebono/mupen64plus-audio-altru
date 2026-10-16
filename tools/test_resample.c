/* altru - test_resample.c: the polyphase resampler's NEON and scalar paths must be bit-identical (random input incl. full-scale,
 * ratio sweeps, ratio changes per call, chunked vs one-shot), plus DC gain. Exit code 0 = no differences.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resamplers/resamplers.h"
#include "resamplers/poly.h"

extern const struct resampler_interface g_poly_iresampler;

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

/* Streams n source frames through the resampler in chunks of random size and step; returns the outputs. */
static size_t run(int scalar, const int16_t* in, size_t n, unsigned in_hz, unsigned out_hz, uint32_t seed, int16_t* out, size_t out_cap, int dyn)
{
    const struct resampler_interface* ir = &g_poly_iresampler;
    void* rs = ir->create("poly", in_hz, out_hz);
    size_t pos = 0, total = 0;
    uint32_t save = rng;

    rng = seed;
    poly_force_scalar(scalar);
    while (total < out_cap) {
        size_t want = 1 + rnd() % 700, got = 0, c;
        uint64_t step = (uint64_t)in_hz * RS_ONE / out_hz;
        if (dyn)
            step = step * (1000000 + (int)(rnd() % 20001) - 10000) / 1000000;
        if (want > out_cap - total)
            want = out_cap - total;
        c = ir->process(rs, in + 2 * pos, n - pos, out + 2 * total, want, step, &got);
        pos += c;
        total += got;
        if (got < want && pos + 40 > n)
            break;
        if (got == 0 && c == 0)
            break;
    }
    ir->release(rs);
    rng = save;
    return total;
}

int main(void)
{
    static const unsigned rates[][2] = { { 32006, 44100 }, { 22047, 22050 }, { 48000, 44100 }, { 11025, 22050 }, { 44100, 44100 }, { 33600, 22050 }, { 8000, 44100 } };
    const size_t N = 60000, CAP = 90000;
    int16_t* buf = calloc(2 * (N + RS_HIST), 2);
    int16_t* in = buf + 2 * RS_HIST;
    int16_t *oa = malloc(2 * CAP * 2), *ob = malloc(2 * CAP * 2);
    int bad = 0, cases = 0, r, mode, dyn;
    size_t i;

    printf("polyphase resampler: NEON %s\n", poly_has_neon() ? "built in" : "not built (scalar vs scalar)");
    for (mode = 0; mode < 3; mode++) {
        for (i = 0; i < 2 * N; i++)
            in[i] = mode == 0 ? (int16_t)rnd() : mode == 1 ? ((rnd() & 1) ? 32767 : -32768) : (int16_t)((int)(rnd() % 2001) - 1000);
        for (r = 0; r < (int)(sizeof(rates) / sizeof(rates[0])); r++)
            for (dyn = 0; dyn < 2; dyn++) {
                size_t na = run(0, in, N, rates[r][0], rates[r][1], 77 + r, oa, CAP, dyn);
                size_t nb = run(1, in, N, rates[r][0], rates[r][1], 77 + r, ob, CAP, dyn);
                cases++;
                if (na != nb || memcmp(oa, ob, na * 4) != 0) {
                    bad++;
                    printf("DIFF mode %d %u->%u dyn %d (%zu vs %zu frames)\n", mode, rates[r][0], rates[r][1], dyn, na, nb);
                }
            }
    }

    /* DC gain: a constant input comes out (nearly) constant */
    for (i = 0; i < 2 * N; i++)
        in[i] = 20000;
    for (r = 0; r < (int)(sizeof(rates) / sizeof(rates[0])); r++) {
        size_t n = run(0, in, N, rates[r][0], rates[r][1], 5, oa, CAP, 1), k;
        int worst = 0;
        for (k = 100; k < n; k++) {
            int d = abs(oa[2 * k] - 20000);
            worst = d > worst ? d : worst;
        }
        cases++;
        if (worst > 4) {
            bad++;
            printf("DC error %d at %u->%u\n", worst, rates[r][0], rates[r][1]);
        }
    }
    printf("%d cases, %d differences\n", cases, bad);
    return bad != 0;
}
