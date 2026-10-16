/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-audio-altru - src.c                                       *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2017 Bobby Smiles                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "resamplers/resamplers.h"
#include "main.h"

#include <samplerate.h>

#include "m64p_types.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Opt-in (USE_SRC=1): libsamplerate through float buffers. */
struct srcr
{
    SRC_STATE* state;
    float* in;
    float* out;
    size_t in_cap, out_cap;
};

static void* src_create(const char* id, unsigned in_hz, unsigned out_hz)
{
    static const struct { const char* name; int type; } types[] = {
        { "src-sinc-best-quality", SRC_SINC_BEST_QUALITY }, { "src-sinc-medium-quality", SRC_SINC_MEDIUM_QUALITY },
        { "src-sinc-fastest", SRC_SINC_FASTEST }, { "src-zero-order-hold", SRC_ZERO_ORDER_HOLD }, { "src-linear", SRC_LINEAR },
    };
    size_t i;
    int error = 0;
    struct srcr* s = calloc(1, sizeof(*s));
    (void)in_hz; (void)out_hz;
    if (s == NULL)
        return NULL;
    for (i = 0; i < sizeof(types) / sizeof(types[0]) - 1 && strcmp(types[i].name, id) != 0; i++)
        ;
    s->state = src_new(types[i].type, 2, &error);
    if (s->state == NULL) {
        DebugMessage(M64MSG_ERROR, "SRC error: %s", src_strerror(error));
        free(s);
        return NULL;
    }
    return s;
}

static void src_release(void* rs)
{
    struct srcr* s = rs;
    if (s) {
        src_delete(s->state);
        free(s->in);
        free(s->out);
        free(s);
    }
}

static void src_reset_(void* rs) { src_reset(((struct srcr*)rs)->state); }

static size_t src_process_(void* rs, const int16_t* src, size_t avail, int16_t* dst, size_t want, uint64_t step, size_t* produced)
{
    struct srcr* s = rs;
    SRC_DATA d;
    double ratio = (double)RS_ONE / (double)step;
    size_t in_n = avail;

    /* the sinc converters need more input than the ratio says; cap it to bound the conversion work */
    if (in_n > want * 5 / 2 + 16)
        in_n = want * 5 / 2 + 16;
    if (s->in_cap < in_n * 2) {
        s->in = realloc(s->in, in_n * 2 * sizeof(float));
        s->in_cap = in_n * 2;
    }
    if (s->out_cap < want * 2) {
        s->out = realloc(s->out, want * 2 * sizeof(float));
        s->out_cap = want * 2;
    }
    src_short_to_float_array(src, s->in, (int)(in_n * 2));
    memset(&d, 0, sizeof(d));
    d.data_in = s->in;
    d.input_frames = (long)in_n;
    d.data_out = s->out;
    d.output_frames = (long)want;
    d.src_ratio = ratio;
    if (src_process(s->state, &d) != 0) {
        *produced = 0;
        return in_n;
    }
    src_float_to_short_array(s->out, dst, (int)(d.output_frames_gen * 2));
    *produced = (size_t)d.output_frames_gen;
    return (size_t)d.input_frames_used;
}

const struct resampler_interface g_src_iresampler = { "src", src_create, src_release, src_reset_, src_process_ };
