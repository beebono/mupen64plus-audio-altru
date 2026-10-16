/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-audio-sdl - resamplers.c                                  *
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

#include "m64p_types.h"

#include <string.h>

extern const struct resampler_interface g_poly_iresampler;
extern const struct resampler_interface g_trivial_iresampler;
#ifdef USE_SPEEX
extern const struct resampler_interface g_speex_iresampler;
#endif
#ifdef USE_SRC
extern const struct resampler_interface g_src_iresampler;
#endif

const struct resampler_interface* get_iresampler(const char* id, void** rs, unsigned in_hz, unsigned out_hz)
{
    const struct resampler_interface* ir = &g_poly_iresampler;

    if (strcmp(id, "trivial") == 0)
        ir = &g_trivial_iresampler;
#ifdef USE_SPEEX
    else if (strncmp(id, "speex-", 6) == 0)
        ir = &g_speex_iresampler;
#endif
#ifdef USE_SRC
    else if (strncmp(id, "src-", 4) == 0)
        ir = &g_src_iresampler;
#endif
    else if (strcmp(id, "poly") != 0)
        DebugMessage(M64MSG_WARNING, "Resampler '%s' is not available in this build; using %s", id, ir->name);

    *rs = ir->create(id, in_hz, out_hz);
    if (*rs == NULL)
        return NULL;
    return ir;
}
