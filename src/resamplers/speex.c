/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-audio-altru - speex.c                                     *
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

#include <speex/speex_resampler.h>

#include "m64p_types.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Opt-in (USE_SPEEX=1). The ratio is passed as Q16 fractions and only re-set when it changes. */
struct spx
{
    SpeexResamplerState* st;
    unsigned in_hz, out_hz;
    uint32_t ratio;
};

static void* speex_create(const char* id, unsigned in_hz, unsigned out_hz)
{
    int error, q = 4;
    struct spx* s = calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;
    if (strncmp(id, "speex-fixed-", 12) == 0)
        q = atoi(id + 12);
    if (q < 0 || q > 10)
        q = 4;
    s->st = speex_resampler_init(2, in_hz, out_hz, q, &error);
    if (s->st == NULL) {
        DebugMessage(M64MSG_ERROR, "Speex error: %s", speex_resampler_strerror(error));
        free(s);
        return NULL;
    }
    s->in_hz = in_hz;
    s->out_hz = out_hz;
    s->ratio = (uint32_t)(((uint64_t)in_hz << 16) / out_hz);
    return s;
}

static void speex_release(void* rs)
{
    struct spx* s = rs;
    if (s) {
        speex_resampler_destroy(s->st);
        free(s);
    }
}

static void speex_reset(void* rs) { speex_resampler_reset_mem(((struct spx*)rs)->st); }

static size_t speex_process(void* rs, const int16_t* src, size_t avail, int16_t* dst, size_t want, uint64_t step, size_t* produced)
{
    struct spx* s = rs;
    uint32_t ratio = (uint32_t)(step >> 16);
    spx_uint32_t in_len = (spx_uint32_t)avail, out_len = (spx_uint32_t)want;

    if (ratio != s->ratio) {
        speex_resampler_set_rate_frac(s->st, ratio, 65536, s->in_hz, s->out_hz);
        s->ratio = ratio;
    }
    if (speex_resampler_process_interleaved_int(s->st, src, &in_len, dst, &out_len) != RESAMPLER_ERR_SUCCESS) {
        *produced = 0;
        return avail;
    }
    *produced = out_len;
    return in_len;
}

const struct resampler_interface g_speex_iresampler = { "speex", speex_create, speex_release, speex_reset, speex_process };
