/* altru - poly.c
 * Polyphase windowed-sinc resampler for stereo int16: 32 taps, 128 phases of a Kaiser-windowed sinc in Q15 (computed at creation, the cutoff
 * follows the nominal ratio), linear interpolation between adjacent phases for the fractional position, a Q32 step that can change per call.
 * The dot products are exact integer sums, so the NEON and the scalar versions give identical output.
 * Copyright (C) 2026 altru contributors; SPDX-License-Identifier: GPL-2.0-or-later */

#include "resamplers/poly.h"
#include "resamplers/resamplers.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef POLY_HAVE_NEON
#include <arm_neon.h>
#endif

#define KAISER_BETA 7.5
#define CUTOFF_SCALE 0.97

struct poly
{
    uint32_t frac;
    int16_t* tab; /* (POLY_PHASES + 1) rows of POLY_TAPS */
};

static int l_scalar;

void poly_force_scalar(int on) { l_scalar = on; }

int poly_has_neon(void)
{
#ifdef POLY_HAVE_NEON
    return 1;
#else
    return 0;
#endif
}

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    int k;
    for (k = 1; k < 64; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-14 * sum)
            break;
    }
    return sum;
}

static void make_table(int16_t* tab, double fc)
{
    const double half = POLY_TAPS / 2;
    const double norm = 1.0 / bessel_i0(KAISER_BETA);
    const double pi = 3.14159265358979323846;
    int p, k;

    for (p = 0; p <= POLY_PHASES; p++) {
        double h[POLY_TAPS], sum = 0;
        for (k = 0; k < POLY_TAPS; k++) {
            double x = (k - (half - 1)) - (double)p / POLY_PHASES;
            double r = x / half;
            double w = fabs(r) < 1.0 ? bessel_i0(KAISER_BETA * sqrt(1.0 - r * r)) * norm : 0.0;
            double s = fabs(x) < 1e-12 ? 1.0 : sin(2.0 * pi * fc * x) / (2.0 * pi * fc * x);
            h[k] = 2.0 * fc * s * w;
            sum += h[k];
        }
        for (k = 0; k < POLY_TAPS; k++) {
            long v = lround(h[k] / sum * 32768.0);
            tab[p * POLY_TAPS + k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
}

void poly_dot_scalar(const int16_t* w, const int16_t* c0, const int16_t* c1, int32_t acc[4])
{
    int32_t l0 = 0, r0 = 0, l1 = 0, r1 = 0;
    int k;
    for (k = 0; k < POLY_TAPS; k++) {
        l0 += (int32_t)w[2 * k] * c0[k];
        r0 += (int32_t)w[2 * k + 1] * c0[k];
        l1 += (int32_t)w[2 * k] * c1[k];
        r1 += (int32_t)w[2 * k + 1] * c1[k];
    }
    acc[0] = l0; acc[1] = r0; acc[2] = l1; acc[3] = r1;
}

#ifdef POLY_HAVE_NEON
void poly_dot_neon(const int16_t* w, const int16_t* c0, const int16_t* c1, int32_t acc[4])
{
    int32x4_t l0 = vdupq_n_s32(0), r0 = l0, l1 = l0, r1 = l0;
    int b;
    for (b = 0; b < POLY_TAPS / 8; b++) {
        int16x8x2_t s = vld2q_s16(w + 16 * b);
        int16x8_t a = vld1q_s16(c0 + 8 * b), c = vld1q_s16(c1 + 8 * b);
        l0 = vmlal_s16(l0, vget_low_s16(s.val[0]), vget_low_s16(a));
        l0 = vmlal_high_s16(l0, s.val[0], a);
        r0 = vmlal_s16(r0, vget_low_s16(s.val[1]), vget_low_s16(a));
        r0 = vmlal_high_s16(r0, s.val[1], a);
        l1 = vmlal_s16(l1, vget_low_s16(s.val[0]), vget_low_s16(c));
#ifdef POLY_MUTATE /* test-only deliberate error: tools/test_resample.c must fail */
        l1 = vmlal_high_s16(l1, s.val[1], c);
#else
        l1 = vmlal_high_s16(l1, s.val[0], c);
#endif
        r1 = vmlal_s16(r1, vget_low_s16(s.val[1]), vget_low_s16(c));
        r1 = vmlal_high_s16(r1, s.val[1], c);
    }
    acc[0] = vaddvq_s32(l0); acc[1] = vaddvq_s32(r0); acc[2] = vaddvq_s32(l1); acc[3] = vaddvq_s32(r1);
}
#endif

static void* poly_create(const char* id, unsigned in_hz, unsigned out_hz)
{
    struct poly* s = calloc(1, sizeof(*s));
    double ratio = (double)out_hz / (in_hz ? in_hz : 1);
    (void)id;
    if (s == NULL)
        return NULL;
    s->tab = aligned_alloc(16, (POLY_PHASES + 1) * POLY_TAPS * sizeof(int16_t));
    if (s->tab == NULL) {
        free(s);
        return NULL;
    }
    make_table(s->tab, 0.5 * CUTOFF_SCALE * (ratio < 1.0 ? ratio : 1.0));
    return s;
}

static void poly_release(void* p)
{
    struct poly* s = p;
    if (s) {
        free(s->tab);
        free(s);
    }
}

static void poly_reset(void* p) { ((struct poly*)p)->frac = 0; }

static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

static size_t poly_process(void* p, const int16_t* src, size_t avail, int16_t* dst, size_t want, uint64_t step, size_t* produced)
{
    struct poly* s = p;
    uint64_t pos = s->frac;
    size_t n_out = 0, cons;
#ifdef POLY_HAVE_NEON
    const int neon = !l_scalar;
#endif

    while (n_out < want) {
        size_t n = (size_t)(pos >> 32);
        uint32_t fr = (uint32_t)pos;
        unsigned ph = fr >> 25;
        int64_t f = (fr >> 9) & 0xFFFF;
        int32_t a[4];
        int32_t y0, y1;

        if (n + RS_LOOK >= avail)
            break;
#ifdef POLY_HAVE_NEON
        if (neon)
            poly_dot_neon(src + 2 * ((ptrdiff_t)n - (POLY_TAPS / 2 - 1)), s->tab + ph * POLY_TAPS, s->tab + (ph + 1) * POLY_TAPS, a);
        else
#endif
            poly_dot_scalar(src + 2 * ((ptrdiff_t)n - (POLY_TAPS / 2 - 1)), s->tab + ph * POLY_TAPS, s->tab + (ph + 1) * POLY_TAPS, a);

        y0 = (a[0] + 16384) >> 15;
        y1 = (a[2] + 16384) >> 15;
        dst[2 * n_out] = sat16(y0 + (int32_t)(((y1 - y0) * f) >> 16));
        y0 = (a[1] + 16384) >> 15;
        y1 = (a[3] + 16384) >> 15;
        dst[2 * n_out + 1] = sat16(y0 + (int32_t)(((y1 - y0) * f) >> 16));

        pos += step;
        n_out++;
    }

    cons = (size_t)(pos >> 32);
    if (cons > avail)
        cons = avail;
    s->frac = (uint32_t)pos;
    *produced = n_out;
    return cons;
}

const struct resampler_interface g_poly_iresampler = { "poly", poly_create, poly_release, poly_reset, poly_process };
