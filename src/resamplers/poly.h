/* altru - poly.h: test hooks of the polyphase resampler. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ALTRU_POLY_H
#define ALTRU_POLY_H

#include <stdint.h>

#if defined(__aarch64__) && defined(__ARM_NEON)
#define POLY_HAVE_NEON 1
#endif

#define POLY_TAPS   32
#define POLY_PHASES 128

/* 0: NEON when built in (default), 1: the scalar reference; both give identical output. */
void poly_force_scalar(int on);
int poly_has_neon(void);

/* Exposed for the bit-exactness test: dot products of 32 stereo frames w[0..31] with two coefficient rows. */
void poly_dot_scalar(const int16_t* w, const int16_t* c0, const int16_t* c1, int32_t acc[4]);
#ifdef POLY_HAVE_NEON
void poly_dot_neon(const int16_t* w, const int16_t* c0, const int16_t* c1, int32_t acc[4]);
#endif

#endif
