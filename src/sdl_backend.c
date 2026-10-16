/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-audio-altru - sdl_backend.c                               *
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

/* Pacing: the N64 samples are appended to a linear buffer; the SDL callback (or, in queue mode, the emulation thread) resamples them
 * to the device rate. The device runs all the time. The buffer level at the start of a callback is held at a target by nudging the
 * resample step by at most +-0.5 % ("dynamic rate control"); a real underrun outputs silence and refills before resuming. */

#include <SDL.h>
#include <SDL_audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "resamplers/resamplers.h"
#include "sdl_backend.h"

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_common.h"
#include "m64p_config.h"
#include "m64p_frontend.h"
#include "m64p_types.h"

#define FADE_FRAMES   64
#define RC_AVG_DIV    16
#define MAX_TRACE     400000
#define BUF_FRAMES    (1 << 16)
#define HIST_KEEP     2048   /* source frames kept behind the read position: resampler history and the rewind of the repeat */
#define MAX_REPEATS   4

struct trace_pt { unsigned t_ms; int level; int ring; unsigned n; };

struct sdl_backend
{
    SDL_AudioDeviceID device;
    m64p_handle config;
    int error;
    int queue_mode;

    /* sample buffer: frames [org, fill) are unread, [org - RS_HIST, org) is history for the resampler */
    int16_t* buf;
    size_t org, fill;

    unsigned in_hz, out_hz, period;
    unsigned swap_channels;

    const struct resampler_interface* ir;
    void* rs;
    char rs_id[40];

    unsigned speed_pct;        /* core speed factor */
    uint64_t step_nom;         /* source frames per output frame, Q32 */
    volatile int adj_ppm;
    int max_adj_ppm;
    double lvl_avg;

    /* buffering */
    int auto_buf;
    unsigned fixed_ms;
    size_t target, tmin, tmax, restart;
    unsigned long long last_under_us, last_shrink_us;
    int refill;
    int paused_q;

    int sync;
    int ff_mute;

    /* fast-forward */
    ptr_CoreDoCommand core_cmd;
    int ff_active;
    double speed_est;                 /* measured emulation speed relative to the game rate */
    unsigned long long win_start_us;
    unsigned long long win_frames;
    size_t dm_pos, dm_period, dm_chunk, dm_fade, dm_use, dm_carry_n;
    int dm_carry_ok;
    int16_t dm_carry[2 * FADE_FRAMES];
    int xfade;                        /* crossfade from the last output frame into the next data */
    int16_t last_frame[2];
    int conceal;
    int rep_run;
    int join_fade;                    /* the next appended frames continue the stream at another point */

    /* volume */
    int gain_cur;

    /* statistics */
    unsigned long long st_cb, st_under, st_silent, st_delay_n, st_delay_ms, st_push, st_over, st_grow, st_ffdrop, st_flush, st_in, st_out, st_repeat;
    int st_adj_min, st_adj_max;
    size_t st_tmin, st_tmax;
    unsigned long long t0_us;
    struct trace_pt* trace;
    unsigned trace_n;
};

static unsigned long long now_us(void)
{
    return SDL_GetPerformanceCounter() * 1000000ULL / SDL_GetPerformanceFrequency();
}

static unsigned select_output_frequency(unsigned input_frequency)
{
    if (input_frequency <= 11025) { return 11025; }
    else if (input_frequency <= 22050) { return 22050; }
    else { return 44100; }
}

static double step_double(const struct sdl_backend* b) { return (double)b->step_nom / 4294967296.0; }

/* unread frames expressed in output frames */
static size_t ring_level(const struct sdl_backend* b)
{
    return (size_t)((double)(b->fill - b->org) / step_double(b));
}

static size_t queued_level(const struct sdl_backend* b)
{
    return b->queue_mode ? SDL_GetQueuedAudioSize(b->device) / 4 : 0;
}

static void compact(struct sdl_backend* b)
{
    if (b->org > HIST_KEEP) {
        size_t drop = b->org - HIST_KEEP;
        memmove(b->buf, b->buf + 2 * drop, (b->fill - drop) * 4);
        b->org -= drop;
        b->fill -= drop;
    }
}

/* Skips unread frames. The resampler history is replaced by the new first frame, so the filter does not ring across the cut. */
static void skip_frames(struct sdl_backend* b, size_t n)
{
    size_t k;
    b->org += n;
    for (k = 1; k <= RS_HIST && k <= b->org; k++) {
        b->buf[2 * (b->org - k)] = b->buf[2 * b->org];
        b->buf[2 * (b->org - k) + 1] = b->buf[2 * b->org + 1];
    }
}

static void recompute_step(struct sdl_backend* b)
{
    unsigned pct = b->speed_pct < 100 ? b->speed_pct : 100;
    b->step_nom = (uint64_t)(((double)b->in_hz * pct / 100.0) / b->out_hz * 4294967296.0);
}

static void set_target(struct sdl_backend* b, size_t t)
{
    if (t < b->tmin) t = b->tmin;
    if (t > b->tmax) t = b->tmax;
    b->target = t;
    b->restart = t / 2 > b->period ? t / 2 : b->period;
    if (b->target < b->st_tmin) b->st_tmin = b->target;
    if (b->target > b->st_tmax) b->st_tmax = b->target;
}

static void on_underrun(struct sdl_backend* b)
{
    unsigned long long now = now_us();
    b->st_under++;
    b->last_under_us = now;
    if (b->auto_buf) {
        size_t step = b->out_hz / 40; /* 25 ms */
        set_target(b, b->target + step);
        b->st_grow++;
    }
}

static void auto_shrink(struct sdl_backend* b, unsigned long long now)
{
    if (!b->auto_buf || now - b->last_under_us < 10000000ULL || now - b->last_shrink_us < 5000000ULL)
        return;
    b->last_shrink_us = now;
    set_target(b, b->target - b->out_hz / 200); /* 5 ms every 5 s of quiet after 10 s */
}

static void rc_update(struct sdl_backend* b, size_t lvl)
{
    double e;
    int adj;

    b->lvl_avg += ((double)lvl - b->lvl_avg) / RC_AVG_DIV;
    e = (b->lvl_avg - (double)b->target) / ((double)b->target * 0.5);
    e = e > 1.0 ? 1.0 : e < -1.0 ? -1.0 : e;
    adj = (int)(e * b->max_adj_ppm);
    b->adj_ppm = adj;
    if (adj < b->st_adj_min) b->st_adj_min = adj;
    if (adj > b->st_adj_max) b->st_adj_max = adj;
}

static void apply_volume(struct sdl_backend* b, int16_t* out, size_t nf)
{
    int g1 = altru_gain();
    int g0 = b->gain_cur;
    size_t i;

    b->gain_cur = g1;
    if (g0 == g1 && g1 == 128)
        return;
    for (i = 0; i < nf; i++) {
        int g = nf > 1 ? g0 + (g1 - g0) * (int)i / (int)(nf - 1) : g1;
        out[2 * i] = (int16_t)((out[2 * i] * g) >> 7);
        out[2 * i + 1] = (int16_t)((out[2 * i + 1] * g) >> 7);
    }
}

/* Silence that follows audible output starts with a short fade from the last frame. */
static void silence(struct sdl_backend* b, int16_t* out, size_t nf)
{
    size_t i, n = nf < FADE_FRAMES ? nf : FADE_FRAMES;
    memset(out, 0, nf * 4);
    if (b->last_frame[0] || b->last_frame[1])
        for (i = 0; i < n; i++) {
            int w = (int)(FADE_FRAMES - i);
            out[2 * i] = (int16_t)(b->last_frame[0] * w / (FADE_FRAMES + 1));
            out[2 * i + 1] = (int16_t)(b->last_frame[1] * w / (FADE_FRAMES + 1));
        }
    b->last_frame[0] = b->last_frame[1] = 0;
}

/* Produces nf output frames. Callback thread (device lock held), or the emulation thread in queue mode. */
static void render(struct sdl_backend* b, int16_t* out, size_t nf)
{
    size_t got = 0, cons, i;
    uint64_t step;
    int adj = b->adj_ppm;

    if (b->t0_us) {
        b->st_cb++;
        b->st_out += nf;
    }
    if (b->ff_active && b->ff_mute) {
        silence(b, out, nf);
        apply_volume(b, out, nf);
        return;
    }
    if (b->refill) {
        if (ring_level(b) < b->restart) {
            silence(b, out, nf);
            apply_volume(b, out, nf);
            if (b->t0_us)
                b->st_silent += nf;
            return;
        }
        b->refill = 0;
        b->xfade = 1;
    }

    step = b->step_nom / 1000000 * (uint64_t)(1000000 + adj) + (b->step_nom % 1000000) * (uint64_t)(1000000 + adj) / 1000000;
    {
        size_t produced = 0, join_at = (size_t)-1;
        int16_t join_from[2] = { b->last_frame[0], b->last_frame[1] };

        for (;;) {
            cons = b->ir->process(b->rs, b->buf + 2 * b->org, b->fill - b->org, out + 2 * produced, nf - produced, step, &got);
            b->org += cons;
            produced += got;
            if (produced >= nf)
                break;
            /* starved: replay the last 20 ms of source instead of going silent, a few times in a row */
            if (b->conceal && b->rep_run < MAX_REPEATS && b->org >= b->in_hz / 50 + RS_HIST && b->in_hz / 50 < HIST_KEEP - 64) {
                b->org -= b->in_hz / 50;
                b->rep_run++;
                b->st_repeat++;
                if (join_at == (size_t)-1) {
                    join_at = produced;
                    if (produced > 0) {
                        join_from[0] = out[2 * (produced - 1)];
                        join_from[1] = out[2 * (produced - 1) + 1];
                    }
                }
                continue;
            }
            break;
        }
        got = produced;
        if (join_at != (size_t)-1) {
            size_t n = nf - join_at < 32 ? nf - join_at : 32;
            for (i = 0; i < n && join_at + i < got; i++) {
                int w = (int)((i + 1) * 256 / (n + 1));
                out[2 * (join_at + i)] = (int16_t)((join_from[0] * (256 - w) + out[2 * (join_at + i)] * w) >> 8);
                out[2 * (join_at + i) + 1] = (int16_t)((join_from[1] * (256 - w) + out[2 * (join_at + i) + 1] * w) >> 8);
            }
        }
    }

    if (got < nf) {
        size_t fo = got < FADE_FRAMES ? got : FADE_FRAMES;
        for (i = 0; i < fo; i++) {
            int w = (int)(fo - i);
            out[2 * (got - fo + i)] = (int16_t)(out[2 * (got - fo + i)] * w / (int)(fo + 1));
            out[2 * (got - fo + i) + 1] = (int16_t)(out[2 * (got - fo + i) + 1] * w / (int)(fo + 1));
        }
        if (got == 0)
            silence(b, out, nf);
        else
            memset(out + 2 * got, 0, (nf - got) * 4);
        b->st_silent += nf - got;
        b->refill = 1;
        b->rep_run = 0;
        on_underrun(b);
    }
    else if (b->xfade && nf > 0) {
        size_t n = nf < FADE_FRAMES ? nf : FADE_FRAMES;
        for (i = 0; i < n; i++) {
            int w = (int)((i + 1) * 256 / (n + 1));
            out[2 * i] = (int16_t)((b->last_frame[0] * (256 - w) + out[2 * i] * w) >> 8);
            out[2 * i + 1] = (int16_t)((b->last_frame[1] * (256 - w) + out[2 * i + 1] * w) >> 8);
        }
        b->xfade = 0;
    }
    if (got > 0) {
        b->last_frame[0] = out[2 * (got - 1)];
        b->last_frame[1] = out[2 * (got - 1) + 1];
    }

    apply_volume(b, out, nf);
    if (b->org > BUF_FRAMES / 2)
        compact(b);
}

static void my_audio_callback(void* userdata, unsigned char* stream, int len)
{
    struct sdl_backend* b = userdata;
    if (!b->refill)
        rc_update(b, ring_level(b));
    auto_shrink(b, now_us());
    render(b, (int16_t*)stream, (size_t)len / 4);
}

static void queue_pump(struct sdl_backend* b)
{
    int16_t tmp[512 * 2];
    for (;;) {
        size_t avail = b->fill - b->org, can, nf;
        if (avail <= RS_LOOK + 2)
            break;
        can = (size_t)((double)(avail - RS_LOOK - 2) / (step_double(b) * 1.006));
        if (can == 0)
            break;
        nf = can < 512 ? can : 512;
        render(b, tmp, nf);
        SDL_QueueAudio(b->device, tmp, (Uint32)(nf * 4));
    }
}

static int open_device(struct sdl_backend* b)
{
    SDL_AudioSpec desired, obtained;
    unsigned sec = (unsigned)ConfigGetParamInt(b->config, "SECONDARY_BUFFER_SIZE");

    memset(&desired, 0, sizeof(desired));
    desired.freq = (int)b->out_hz;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    if (sec == 0) {
        sec = 256;
        while (sec * 2 <= b->out_hz / 80)
            sec *= 2;
    }
    desired.samples = (Uint16)sec;
    desired.callback = b->queue_mode ? NULL : my_audio_callback;
    desired.userdata = b;

    b->device = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    if (b->device == 0) {
        DebugMessage(M64MSG_ERROR, "Couldn't open audio: %s", SDL_GetError());
        return -1;
    }
    b->period = obtained.samples;
    DebugMessage(M64MSG_VERBOSE, "Audio device: %i Hz, %i samples per period, %s mode", obtained.freq, obtained.samples, b->queue_mode ? "queue" : "callback");
    return 0;
}

static void setup_levels(struct sdl_backend* b)
{
    size_t fixed = (size_t)b->fixed_ms * b->out_hz / 1000;

    b->tmin = b->period + b->out_hz / 50;           /* one period plus 20 ms */
    b->tmax = (size_t)b->out_hz * 3 / 10;            /* 300 ms */
    if (b->tmax < b->tmin * 2) b->tmax = b->tmin * 2;
    b->st_tmin = (size_t)-1;
    b->st_tmax = 0;
    if (b->auto_buf)
        set_target(b, b->tmin + b->out_hz / 25);     /* start small: min + 40 ms */
    else {
        b->tmin = fixed < b->period + 64 ? b->period + 64 : fixed;
        b->tmax = b->tmin;
        set_target(b, b->tmin);
    }
    b->lvl_avg = (double)b->target;
}

static void reset_stream(struct sdl_backend* b)
{
    b->org = HIST_KEEP;
    b->fill = HIST_KEEP;
    memset(b->buf, 0, HIST_KEEP * 4);
    b->rep_run = 0;
    if (b->rs) b->ir->reset(b->rs);
    b->refill = !b->queue_mode;
    b->dm_pos = 0;
    b->dm_carry_ok = 0;
    b->dm_carry_n = 0;
    b->adj_ppm = 0;
    b->lvl_avg = (double)b->target;
}

static int make_resampler(struct sdl_backend* b, const char* id)
{
    void* rs = NULL;
    const struct resampler_interface* ir = get_iresampler(id, &rs, b->in_hz, b->out_hz);
    if (ir == NULL)
        return -1;
    if (b->rs) b->ir->release(b->rs);
    b->ir = ir;
    b->rs = rs;
    if (id != b->rs_id)
        snprintf(b->rs_id, sizeof(b->rs_id), "%s", id);
    return 0;
}

static int init_audio(struct sdl_backend* b)
{
    b->error = 0;
    if (SDL_WasInit(SDL_INIT_AUDIO | SDL_INIT_TIMER) != (SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
            DebugMessage(M64MSG_ERROR, "Failed to initialize SDL audio subsystem.");
            b->error = 1;
            return -1;
        }
    }
    b->out_hz = select_output_frequency(b->in_hz);
    if (open_device(b) < 0) {
        b->error = 1;
        return -1;
    }
    setup_levels(b);
    recompute_step(b);
    if (make_resampler(b, b->rs_id) < 0) {
        b->error = 1;
        return -1;
    }
    reset_stream(b);
    if (b->queue_mode)
        SDL_PauseAudioDevice(b->device, 1), b->paused_q = 1;
    else
        SDL_PauseAudioDevice(b->device, 0);
    return 0;
}

static void close_device(struct sdl_backend* b)
{
    if (b->device) {
        SDL_PauseAudioDevice(b->device, 1);
        SDL_CloseAudioDevice(b->device);
        b->device = 0;
    }
}

struct sdl_backend* init_sdl_backend_from_config(m64p_handle config, ptr_CoreDoCommand core_cmd)
{
    struct sdl_backend* b = calloc(1, sizeof(*b));
    const char* rs_id = altru_opt_resampler();

    if (b == NULL)
        return NULL;
    b->buf = calloc(BUF_FRAMES + HIST_KEEP, 4);
    if (b->buf == NULL) {
        free(b);
        return NULL;
    }
    b->config = config;
    b->core_cmd = core_cmd;
    b->in_hz = (unsigned)ConfigGetParamInt(config, "DEFAULT_FREQUENCY");
    b->swap_channels = (unsigned)ConfigGetParamBool(config, "SWAP_CHANNELS");
    b->sync = altru_opt_int("AUDIO_SYNC") != 0;
    b->ff_mute = altru_opt_int("FF_MODE") == 1;
    b->auto_buf = altru_opt_int("BUFFER_MODE") == 0;
    b->fixed_ms = (unsigned)altru_opt_int("BUFFER_MS");
    b->queue_mode = ConfigGetParamInt(config, "OUTPUT_MODE") == 1;
    b->conceal = altru_opt_int("CONCEAL") != 0;
    b->max_adj_ppm = ConfigGetParamInt(config, "RATE_ADJUST_PPM");
    if (b->max_adj_ppm < 0) b->max_adj_ppm = 0;
    if (b->max_adj_ppm > 200000) b->max_adj_ppm = 200000;
    b->speed_pct = 100;
    b->speed_est = 1.0;
    b->gain_cur = altru_gain();
    snprintf(b->rs_id, sizeof(b->rs_id), "%s", rs_id ? rs_id : DEFAULT_RESAMPLER);
    if (getenv("ALTRU_STATS"))
        b->trace = calloc(MAX_TRACE, sizeof(*b->trace));

    b->adj_ppm = 0;
    b->st_adj_min = 0;
    b->st_adj_max = 0;
    DebugMessage(M64MSG_INFO, "Initializing SDL audio subsystem...");
    init_audio(b);
    return b;
}

static void write_stats(struct sdl_backend* b)
{
    const char* path = getenv("ALTRU_STATS");
    char sum[600];
    FILE* f;
    double dur;

    if (path == NULL || b->t0_us == 0)
        return;
    dur = (now_us() - b->t0_us) / 1e6;
    snprintf(sum, sizeof(sum), "%s.sum", path);
    f = fopen(sum, "w");
    if (f) {
        fprintf(f, "dur_s %.1f\ncallbacks %llu\nunderrun_cb %llu\nsilent_ms %.0f\npause_events 0\npaused_ms 0\ndelay_events %llu\ndelay_ms %llu\npushes %llu\n"
                   "out_freq %u\nin_freq %u\nsec %u\ntarget %zu\nadj_ppm %d..%d\ntarget_range %zu..%zu\ngrow %llu\noverflow_drops %llu\nff_dropped_frames %llu\nflushes %llu\nrepeats %llu\nin_frames %llu\nout_frames %llu\ngame_speed_pct %.2f\nmode %s\nresampler %s\n",
                dur, b->st_cb, b->st_under, b->st_silent * 1000.0 / (b->out_hz ? b->out_hz : 1), b->st_delay_n, b->st_delay_ms, b->st_push,
                b->out_hz, b->in_hz, b->period, b->target, b->st_adj_min, b->st_adj_max, b->st_tmin == (size_t)-1 ? 0 : b->st_tmin, b->st_tmax,
                b->st_grow, b->st_over, b->st_ffdrop, b->st_flush, b->st_repeat, b->st_in, b->st_out, 100.0 * b->st_in / ((now_us() - b->t0_us) / 1e6) / b->in_hz, b->queue_mode ? "queue" : "callback", b->rs_id);
        fclose(f);
    }
    f = fopen(path, "w");
    if (f) {
        unsigned i;
        fprintf(f, "t_ms,level,avail,n\n");
        for (i = 0; i < b->trace_n; i++)
            fprintf(f, "%u,%d,%d,%u\n", b->trace[i].t_ms, b->trace[i].level, b->trace[i].ring, b->trace[i].n);
        fclose(f);
    }
}

void release_sdl_backend(struct sdl_backend* b)
{
    if (b == NULL)
        return;
    if (b->device) {
        SDL_PauseAudioDevice(b->device, 1);
        SDL_CloseAudioDevice(b->device);
    }
    write_stats(b);
    if (SDL_WasInit(SDL_INIT_AUDIO) != 0)
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (SDL_WasInit(SDL_INIT_TIMER) != 0)
        SDL_QuitSubSystem(SDL_INIT_TIMER);
    if (b->rs) b->ir->release(b->rs);
    free(b->trace);
    free(b->buf);
    free(b);
}

void sdl_set_frequency(struct sdl_backend* b, unsigned frequency)
{
    unsigned out = select_output_frequency(frequency);

    if (b->error != 0 || frequency == 0)
        return;
    if (b->device && out == b->out_hz) {
        SDL_LockAudioDevice(b->device);
        b->in_hz = frequency;
        recompute_step(b);
        make_resampler(b, b->rs_id);
        reset_stream(b);
        SDL_UnlockAudioDevice(b->device);
        return;
    }
    close_device(b);
    b->in_hz = frequency;
    init_audio(b);
}

/* ---- fast-forward ---- */

static int core_limiter_on(struct sdl_backend* b)
{
    int v = 1;
    if (b->core_cmd && (*b->core_cmd)(M64CMD_CORE_STATE_QUERY, M64CORE_SPEED_LIMITER, &v) != M64ERR_SUCCESS)
        v = 1;
    return v;
}

static void measure_speed(struct sdl_backend* b, size_t frames, unsigned long long now)
{
    b->win_frames += frames;
    if (b->win_start_us == 0) {
        b->win_start_us = now;
        return;
    }
    if (now - b->win_start_us >= 100000ULL) {
        double s = (double)b->win_frames * 1e6 / (double)(now - b->win_start_us) / (double)b->in_hz;
        b->speed_est = b->speed_est * 0.7 + s * 0.3;
        b->win_start_us = now;
        b->win_frames = 0;
    }
}

/* Keeps a chunk of dm_chunk frames out of every dm_period source frames, crossfading the end of the previous chunk (the frames the
 * source goes on with) into the start of the next one. Returns the frames written to dst. */
static size_t decimate(struct sdl_backend* b, const int16_t* src, size_t n, int16_t* dst)
{
    size_t i, o = 0;
    for (i = 0; i < n; i++, src += 2) {
        size_t q = b->dm_pos;
        if (q == 0) {
            double s = b->speed_est < 1.0 ? 1.0 : b->speed_est;
            b->dm_chunk = b->in_hz / 50;
            b->dm_period = (size_t)(b->dm_chunk * s);
            b->dm_fade = b->dm_period - b->dm_chunk < FADE_FRAMES ? b->dm_period - b->dm_chunk : FADE_FRAMES;
            b->dm_use = b->dm_carry_n < b->dm_fade ? b->dm_carry_n : b->dm_fade;
        }
        if (q < b->dm_use && b->dm_carry_ok) {
            int w = (int)((q + 1) * 256 / (b->dm_use + 1));
            dst[2 * o] = (int16_t)((b->dm_carry[2 * q] * (256 - w) + src[0] * w) >> 8);
            dst[2 * o + 1] = (int16_t)((b->dm_carry[2 * q + 1] * (256 - w) + src[1] * w) >> 8);
            o++;
        }
        else if (q < b->dm_chunk) {
            dst[2 * o] = src[0];
            dst[2 * o + 1] = src[1];
            o++;
        }
        else if (q < b->dm_chunk + b->dm_fade) {
            b->dm_carry[2 * (q - b->dm_chunk)] = src[0];
            b->dm_carry[2 * (q - b->dm_chunk) + 1] = src[1];
            if (q + 1 == b->dm_chunk + b->dm_fade) {
                b->dm_carry_ok = 1;
                b->dm_carry_n = b->dm_fade;
            }
        }
        if (++b->dm_pos >= b->dm_period)
            b->dm_pos = 0;
    }
    return o;
}

static void flush_to_target(struct sdl_backend* b)
{
    size_t keep = (size_t)((double)b->target * step_double(b));
    size_t avail = b->fill - b->org;
    if (avail > keep) {
        skip_frames(b, avail - keep);
        b->xfade = 1;
        b->st_flush++;
    }
}

/* ---- push ---- */

void sdl_push_samples(struct sdl_backend* b, const void* src, size_t size)
{
    const unsigned char* in = src;
    size_t n, i, lvl;
    unsigned long long now;
    int ff;
    int16_t tmp[2 * 4096];

    if (b->error != 0 || b->device == 0)
        return;
    if (size & 3)
        DebugMessage(M64MSG_WARNING, "sdl_push_samples: pushing non full samples: %zu bytes !", size);
    n = size / 4;
    now = now_us();
    if (b->t0_us == 0)
        b->t0_us = now;
    b->st_push++;
    b->st_in += n;
    b->rep_run = 0;

    ff = b->speed_pct > 100 || !core_limiter_on(b);
    measure_speed(b, n, now);
    if (!ff) {
        b->speed_est = 1.0;
    }

    if (b->queue_mode) {
        size_t q = queued_level(b);
        lvl = q + ring_level(b);
        if (q == 0 && !b->paused_q && b->st_push > 2) {
            on_underrun(b);
            SDL_PauseAudioDevice(b->device, 1);
            b->paused_q = 1;
        }
        rc_update(b, lvl);
        auto_shrink(b, now);
    }

    SDL_LockAudioDevice(b->device);

    if (ff != b->ff_active) {
        b->ff_active = ff;
        b->dm_pos = 0;
        b->dm_carry_ok = 0;
        b->dm_carry_n = 0;
        if (!ff) {
            flush_to_target(b);
            b->join_fade = 1;
        }
        else if (b->ff_mute) {
            skip_frames(b, b->fill - b->org);
            b->refill = 1;
        }
        if (!ff && b->ff_mute)
            b->refill = !b->queue_mode;
    }

    if (ff && b->ff_mute) {
        b->st_ffdrop += n;
        b->xfade = 1;
    }
    else {
        size_t done = 0;
        while (done < n) {
            size_t chunk = n - done < 4096 ? n - done : 4096, m = chunk;
            const unsigned char* p = in + 4 * done;
            int16_t* q = tmp;

            /* The core stores the 32-bit words in native order, so on a little-endian host a plain copy swaps the channels. */
            if (b->swap_channels ^ (SDL_BYTEORDER == SDL_BIG_ENDIAN))
                memcpy(tmp, p, chunk * 4);
            else
                for (i = 0; i < chunk; i++) {
                    memcpy((unsigned char*)tmp + 4 * i, p + 4 * i + 2, 2);
                    memcpy((unsigned char*)tmp + 4 * i + 2, p + 4 * i, 2);
                }

            if (ff && b->speed_est > 1.05) {
                static int16_t out[2 * 4096];
                m = decimate(b, tmp, chunk, out);
                b->st_ffdrop += chunk - m;
                q = out;
            }
            if (b->fill + m > BUF_FRAMES + HIST_KEEP) {
                compact(b);
                if (b->fill + m > BUF_FRAMES + HIST_KEEP) {
                    size_t drop = b->fill + m - (BUF_FRAMES + HIST_KEEP);
                    skip_frames(b, drop > b->fill - b->org ? b->fill - b->org : drop);
                    compact(b);
                    b->st_over++;
                    b->xfade = 1;
                }
            }
            if (b->join_fade && m > 0 && b->fill > b->org) {
                size_t nf = m < FADE_FRAMES ? m : FADE_FRAMES, k;
                const int16_t* last = b->buf + 2 * (b->fill - 1);
                for (k = 0; k < nf; k++) {
                    int w = (int)((k + 1) * 256 / (nf + 1));
                    q[2 * k] = (int16_t)((last[0] * (256 - w) + q[2 * k] * w) >> 8);
                    q[2 * k + 1] = (int16_t)((last[1] * (256 - w) + q[2 * k + 1] * w) >> 8);
                }
            }
            if (m > 0)
                b->join_fade = 0;
            memcpy(b->buf + 2 * b->fill, q, m * 4);
            b->fill += m;
            done += chunk;
        }
    }

    /* bound the latency when nothing throttles the core (sync off, fast-forward) */
    if ((!b->sync || ff) && ring_level(b) + queued_level(b) > b->tmax + b->period) {
        flush_to_target(b);
        b->st_over++;
    }

    if (b->queue_mode) {
        queue_pump(b);
        if (b->paused_q && queued_level(b) >= b->restart) {
            SDL_PauseAudioDevice(b->device, 0);
            b->paused_q = 0;
        }
    }
    lvl = ring_level(b) + queued_level(b);
    SDL_UnlockAudioDevice(b->device);

    if (b->trace && b->trace_n < MAX_TRACE) {
        b->trace[b->trace_n].t_ms = (unsigned)((now - b->t0_us) / 1000);
        b->trace[b->trace_n].level = (int)lvl;
        b->trace[b->trace_n].ring = (int)ring_level(b);
        b->trace[b->trace_n].n = (unsigned)n;
        b->trace_n++;
    }

    /* backstop: the core is far ahead of the audio clock, so wait for the excess */
    if (b->sync && !ff) {
        size_t slack = n * b->out_hz / b->in_hz + 2 * (size_t)b->period;
        if (lvl > b->target + slack) {
            unsigned ms = (unsigned)((lvl - b->target - slack) * 1000 / b->out_hz);
            if (ms > 40) ms = 40;
            if (ms > 0) {
                b->st_delay_n++;
                b->st_delay_ms += ms;
                SDL_Delay(ms);
            }
        }
    }
}

void sdl_set_speed_factor(struct sdl_backend* b, unsigned pct)
{
    if (pct < 10 || pct > 1000 || b == NULL || b->error)
        return;
    if (b->device) SDL_LockAudioDevice(b->device);
    b->speed_pct = pct;
    recompute_step(b);
    if (b->device) SDL_UnlockAudioDevice(b->device);
}

/* ---- live options ---- */

void sdl_set_sync(struct sdl_backend* b, int on) { if (b) b->sync = on != 0; }

void sdl_set_ff_mute(struct sdl_backend* b, int on) { if (b) b->ff_mute = on != 0; }

void sdl_set_conceal(struct sdl_backend* b, int on) { if (b) b->conceal = on != 0; }

int sdl_set_resampler(struct sdl_backend* b, const char* id)
{
    int r;
    if (b == NULL || b->error || b->device == 0)
        return -1;
    SDL_LockAudioDevice(b->device);
    r = make_resampler(b, id);
    if (r == 0)
        b->ir->reset(b->rs);
    SDL_UnlockAudioDevice(b->device);
    return r;
}

const char* sdl_resampler_id(const struct sdl_backend* b) { return b ? b->rs_id : NULL; }
