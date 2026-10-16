/* altru - bench_stretch.c: cost of one tail stretch (the concealment's only work), on the build host or the device.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/sdl_backend.c"
#include <time.h>
ptr_ConfigGetParamInt ConfigGetParamInt;
ptr_ConfigGetParamBool ConfigGetParamBool;
ptr_ConfigGetParamString ConfigGetParamString;
void DebugMessage(int level, const char* message, ...) { (void)level; (void)message; }
int altru_gain(void) { return 128; }
int altru_opt_int(const char* key) { (void)key; return 0; }
const char* altru_opt_resampler(void) { return "poly"; }
int main(void)
{
    struct sdl_backend* b = calloc(1, sizeof(*b));
    unsigned L[] = { 600, 1000, 1400, 2000 }, k, n;
    int pass;
    b->buf = calloc(BUF_FRAMES + HIST_KEEP, 4);
    b->sw = malloc(sizeof(*b->sw));
    b->in_hz = 32006;
    for (k = 0; k < sizeof(L) / sizeof(L[0]); k++) {
        double tot = 0, worst = 0;
        for (n = 0; n < 2000; n++) {
            struct timespec t0, t1;
            unsigned i;
            double us;
            b->org = HIST_KEEP; b->fill = HIST_KEEP + L[k];
            for (i = 0; i < L[k]; i++) b->buf[2 * (HIST_KEEP + i)] = b->buf[2 * (HIST_KEEP + i) + 1] = (int16_t)(8000 * sin(i * 0.2 + n) + (rand() & 1023));
            pass = n % 3;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            stretch_tail(b, pass);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
            tot += us; if (us > worst) worst = us;
        }
        printf("tail %4u frames (%.0f ms): mean %.0f us  worst %.0f us\n", L[k], L[k] * 1000.0 / 32006, tot / 2000, worst);
    }
    return 0;
}
