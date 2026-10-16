#include <stdlib.h>
#include <string.h>
#include "resamplers/resamplers.h"
extern const struct resampler_interface g_poly_iresampler, g_trivial_iresampler;
/* kind 0 poly, 1 trivial; step_ppm: step scaled by (1+ppm/1e6); in is int16 stereo with RS_HIST zero frames prepended by the caller */
size_t rs_run(int kind, const int16_t* in, size_t n, unsigned in_hz, unsigned out_hz, int ppm, int16_t* out, size_t cap)
{
    const struct resampler_interface* ir = kind ? &g_trivial_iresampler : &g_poly_iresampler;
    void* rs = ir->create("x", in_hz, out_hz);
    uint64_t step = (uint64_t)((double)in_hz / out_hz * (1.0 + ppm / 1e6) * 4294967296.0);
    size_t pos = 0, tot = 0;
    while (tot < cap) {
        size_t got = 0, want = cap - tot > 512 ? 512 : cap - tot;
        size_t c = ir->process(rs, in + 2 * pos, n - pos, out + 2 * tot, want, step, &got);
        pos += c; tot += got;
        if (got < want) break;
    }
    ir->release(rs);
    return tot;
}
