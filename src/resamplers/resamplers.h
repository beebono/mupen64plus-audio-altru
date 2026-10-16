/* altru - resamplers.h
 * Stereo int16 resamplers with a ratio that can change on every call (dynamic rate control).
 * Copyright (C) 2026 altru contributors; derived from mupen64plus-audio-sdl, SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef ALTRU_RESAMPLERS_H
#define ALTRU_RESAMPLERS_H

#include <stddef.h>
#include <stdint.h>

/* The caller keeps RS_HIST frames of history before the first source frame it passes and
 * a resampler needs up to RS_LOOK frames past the position it reads. */
#define RS_HIST 16
#define RS_LOOK 16

#define RS_ONE ((uint64_t)1 << 32)

struct resampler_interface
{
    const char* name;

    void* (*create)(const char* id, unsigned in_hz, unsigned out_hz);
    void (*release)(void* rs);
    void (*reset)(void* rs);

    /* src: source frames [0, avail) with RS_HIST valid frames before src[0]. step: source frames per output frame, Q32.
     * Writes up to want frames to dst, returns the whole source frames consumed (the fraction stays in the resampler). */
    size_t (*process)(void* rs, const int16_t* src, size_t avail, int16_t* dst, size_t want, uint64_t step, size_t* produced);
};

/* "poly" (default), "trivial", and with USE_SPEEX / USE_SRC "speex-fixed-{0..10}", "src-*". Unknown ids give the default. */
const struct resampler_interface* get_iresampler(const char* id, void** rs, unsigned in_hz, unsigned out_hz);

#define DEFAULT_RESAMPLER "poly"

#endif
