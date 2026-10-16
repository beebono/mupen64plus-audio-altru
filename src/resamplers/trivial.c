/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-audio-altru - trivial.c                                   *
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

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* nearest source frame */
static void* trivial_create(const char* id, unsigned in_hz, unsigned out_hz)
{
    (void)id; (void)in_hz; (void)out_hz;
    return calloc(1, sizeof(uint32_t));
}

static void trivial_release(void* rs) { free(rs); }

static void trivial_reset(void* rs) { *(uint32_t*)rs = 0; }

static size_t trivial_process(void* rs, const int16_t* src, size_t avail, int16_t* dst, size_t want, uint64_t step, size_t* produced)
{
    uint64_t pos = *(uint32_t*)rs;
    size_t n_out = 0, cons;

    while (n_out < want) {
        size_t n = (size_t)(pos >> 32);
        if (n >= avail)
            break;
        ((uint32_t*)dst)[n_out++] = ((const uint32_t*)src)[n];
        pos += step;
    }
    cons = (size_t)(pos >> 32);
    if (cons > avail)
        cons = avail;
    *(uint32_t*)rs = (uint32_t)pos;
    *produced = n_out;
    return cons;
}

const struct resampler_interface g_trivial_iresampler = { "trivial", trivial_create, trivial_release, trivial_reset, trivial_process };
