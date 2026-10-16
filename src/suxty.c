/* altru - suxty.c: the suxty options interface (altru_suxty.h) and the option lookup the rest of the plugin uses.
 * Copyright (C) 2026 altru contributors; SPDX-License-Identifier: GPL-2.0-or-later */

#include <pthread.h>
#include <string.h>

#include "altru_suxty.h"
#include "main.h"
#include "sdl_backend.h"

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_common.h"
#include "m64p_config.h"
#include "m64p_types.h"

enum { OPT_VOLUME, OPT_FF_MODE, OPT_SYNC, OPT_RESAMPLER, OPT_BUFFER_MODE, OPT_BUFFER_MS, OPT_CONCEAL, OPT_COUNT };

static const char *const ff_labels[] = { "Normal Pitch", "Muted" };
static const char *const buf_labels[] = { "Automatic", "Fixed" };
static const char *const rs_ids[] = {
    "poly", "trivial",
#ifdef USE_SPEEX
    "speex-fixed-4",
#endif
#ifdef USE_SRC
    "src-sinc-fastest",
#endif
};
static const char *const rs_labels[] = {
    "Polyphase", "Nearest",
#ifdef USE_SPEEX
    "Speex",
#endif
#ifdef USE_SRC
    "Sinc (libsamplerate)",
#endif
};
#define RS_COUNT ((int)(sizeof(rs_ids) / sizeof(rs_ids[0])))

static const struct altru_suxty_option l_options[OPT_COUNT] = {
    [OPT_VOLUME]      = { "VOLUME_DEFAULT", "Volume", "Output volume in percent.", ALTRU_OPT_INT, ALTRU_OPTF_LIVE, 0, 100, 5, NULL, 80 },
    [OPT_FF_MODE]     = { "FF_MODE", "Fast-Forward Audio", "Play at normal pitch while fast-forwarding, or mute.", ALTRU_OPT_ENUM, ALTRU_OPTF_LIVE, 0, 1, 1, ff_labels, 0 },
    [OPT_SYNC]        = { "AUDIO_SYNC", "Audio Sync", "Delay the core when it runs far ahead of the audio clock.", ALTRU_OPT_BOOL, ALTRU_OPTF_LIVE, 0, 1, 1, NULL, 1 },
    [OPT_RESAMPLER]   = { "RESAMPLE", "Resampler", "Sample rate conversion method.", ALTRU_OPT_ENUM, ALTRU_OPTF_LIVE, 0, RS_COUNT - 1, 1, rs_labels, 0 },
    [OPT_BUFFER_MODE] = { "BUFFER_MODE", "Buffer Size", "Automatic grows on underruns and shrinks slowly; fixed uses Buffer Length.", ALTRU_OPT_ENUM, 0, 0, 1, 1, buf_labels, 0 },
    [OPT_BUFFER_MS]   = { "BUFFER_MS", "Buffer Length", "Fixed buffer level in milliseconds.", ALTRU_OPT_INT, 0, 20, 300, 10, NULL, 60 },
    [OPT_CONCEAL]     = { "CONCEAL", "Underrun Concealment", "Replay the last 20 ms instead of going silent when the core falls behind.", ALTRU_OPT_BOOL, ALTRU_OPTF_LIVE, 0, 1, 1, NULL, 1 },
};

static int l_ov_set[OPT_COUNT], l_ov_val[OPT_COUNT];
static int l_run_val[OPT_COUNT], l_run_known[OPT_COUNT];

static int key_index(const char *key)
{
    int i;
    for (i = 0; i < OPT_COUNT; i++)
        if (strcmp(l_options[i].key, key) == 0)
            return i;
    return -1;
}

static int cfg_value(int i)
{
    m64p_handle h = altru_config();
    if (i == OPT_RESAMPLER) {
        const char *id = ConfigGetParamString(h, "RESAMPLE");
        int k;
        for (k = 0; id && k < RS_COUNT; k++)
            if (strcmp(id, rs_ids[k]) == 0)
                return k;
        return 0;
    }
    if (l_options[i].type == ALTRU_OPT_BOOL)
        return ConfigGetParamBool(h, l_options[i].key);
    return ConfigGetParamInt(h, l_options[i].key);
}

/* The value a launch starts with: a set() value if there is one, else the config. */
int altru_opt_int(const char *key)
{
    int i = key_index(key);
    if (i >= 0 && l_ov_set[i])
        return l_ov_val[i];
    return i >= 0 ? cfg_value(i) : ConfigGetParamInt(altru_config(), key);
}

const char *altru_opt_resampler(void)
{
    int i = OPT_RESAMPLER;
    if (l_ov_set[i] && l_ov_val[i] >= 0 && l_ov_val[i] < RS_COUNT)
        return rs_ids[l_ov_val[i]];
    return ConfigGetParamString(altru_config(), "RESAMPLE");
}

void altru_opt_started(void)
{
    int i;
    for (i = 0; i < OPT_COUNT; i++) {
        l_run_val[i] = i == OPT_VOLUME ? 0 : altru_opt_int(l_options[i].key);
        l_run_known[i] = 1;
    }
}

void altru_opt_stopped(void)
{
    memset(l_run_known, 0, sizeof(l_run_known));
}

void altru_opt_shutdown(void)
{
    memset(l_ov_set, 0, sizeof(l_ov_set));
    memset(l_run_known, 0, sizeof(l_run_known));
}

static int check(unsigned index)
{
    if (!altru_started())
        return ALTRU_SUXTY_E_STATE;
    if (index >= OPT_COUNT)
        return ALTRU_SUXTY_E_INDEX;
    if (!altru_on_emu_thread())
        return ALTRU_SUXTY_E_THREAD;
    return 0;
}

static int in_range(unsigned index, int v)
{
    const struct altru_suxty_option *o = &l_options[index];
    if (v < o->min || v > o->max)
        return 0;
    return o->type != ALTRU_OPT_INT || o->step <= 1 || (v - o->min) % o->step == 0;
}

static int l_get(unsigned index, int *value)
{
    int r = check(index);
    if (r < 0)
        return r;
    if (altru_running()) {
        if (index == OPT_VOLUME)
            *value = altru_volume_get();
        else
            *value = l_run_val[index];
    }
    else
        *value = index == OPT_VOLUME && !l_ov_set[index] ? ConfigGetParamInt(altru_config(), "VOLUME_DEFAULT") : altru_opt_int(l_options[index].key);
    return 0;
}

static int l_set(unsigned index, int value)
{
    int r = check(index);
    struct sdl_backend *b;
    if (r < 0)
        return r;
    if (!in_range(index, value))
        return ALTRU_SUXTY_E_RANGE;
    l_ov_set[index] = 1;
    l_ov_val[index] = value;
    if (!altru_running())
        return ALTRU_SUXTY_APPLIED;
    if (!(l_options[index].flags & ALTRU_OPTF_LIVE))
        return ALTRU_SUXTY_NEXT_LAUNCH;

    b = altru_backend();
    switch (index) {
    case OPT_VOLUME: altru_volume_set(value); break;
    case OPT_FF_MODE: sdl_set_ff_mute(b, value); break;
    case OPT_SYNC: sdl_set_sync(b, value); break;
    case OPT_RESAMPLER: sdl_set_resampler(b, rs_ids[value]); break;
    case OPT_CONCEAL: sdl_set_conceal(b, value); break;
    }
    l_run_val[index] = value;
    return ALTRU_SUXTY_APPLIED;
}

static int l_save_default(unsigned index, int value)
{
    int r = check(index);
    m64p_handle h = altru_config();
    const struct altru_suxty_option *o;
    if (r < 0)
        return r;
    if (!in_range(index, value))
        return ALTRU_SUXTY_E_RANGE;
    o = &l_options[index];
    if (index == OPT_RESAMPLER)
        r = ConfigSetParameter(h, o->key, M64TYPE_STRING, rs_ids[value]);
    else if (o->type == ALTRU_OPT_BOOL)
        r = ConfigSetParameter(h, o->key, M64TYPE_BOOL, &value);
    else
        r = ConfigSetParameter(h, o->key, M64TYPE_INT, &value);
    if (r != M64ERR_SUCCESS || ConfigSaveSection("Audio-Altru") != M64ERR_SUCCESS)
        return ALTRU_SUXTY_E_CONFIG;
    return 0;
}

static const struct altru_suxty_interface l_if = {
    ALTRU_SUXTY_ABI, sizeof(struct altru_suxty_interface), ALTRU_PLUGIN_VERSION, OPT_COUNT, l_options, l_get, l_set, l_save_default,
};

__attribute__((visibility("default"))) const struct altru_suxty_interface *AltruSuxtyGetInterface(void)
{
    return &l_if;
}
