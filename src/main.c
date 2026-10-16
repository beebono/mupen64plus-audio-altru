/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-audio-altru - main.c                                      *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2007-2009 Richard Goedeken                              *
 *   Copyright (C) 2007-2008 Ebenblues                                     *
 *   Copyright (C) 2003 JttL                                               *
 *   Copyright (C) 2002 Hacktarux                                          *
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

#include <SDL.h>
#include <SDL_audio.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <string.h>

#include "main.h"
#include "osal_dynamiclib.h"
#include "sdl_backend.h"
#include "resamplers/resamplers.h"

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_common.h"
#include "m64p_config.h"
#include "m64p_frontend.h"
#include "m64p_plugin.h"
#include "m64p_types.h"

/* version info */
#define AUDIO_PLUGIN_API_VERSION 0x020000
#define CONFIG_API_VERSION       0x020100
#define CONFIG_PARAM_VERSION     1.00
#define CONFIG_SECTION           "Audio-Altru"
#define LEGACY_SECTION           "Audio-SDL"

#define VERSION_PRINTF_SPLIT(x) (((x) >> 16) & 0xffff), (((x) >> 8) & 0xff), ((x) & 0xff)

/* This sets default frequency what is used if rom doesn't want to change it.
   Probably only game that needs this is Zelda: Ocarina Of Time Master Quest
   *NOTICE* We should try to find out why Demos' frequencies are always wrong
   They tend to rely on a default frequency, apparently, never the same one ;)*/
#define DEFAULT_FREQUENCY 33600

/* local variables */
static void (*l_DebugCallback)(void *, int, const char *) = NULL;
static void *l_DebugCallContext = NULL;
static int l_PluginInit = 0;
static m64p_handle l_ConfigAudio;

static struct sdl_backend* l_sdl_backend = NULL;
static ptr_CoreDoCommand l_CoreDoCommand = NULL;
static pthread_t l_EmuThread;

/* Read header for type definition */
static AUDIO_INFO AudioInfo;
// volume to scale the audio by, range of 0..100
// if muted, this holds the volume when not muted
static int VolPercent = 80;
// how much percent to increment/decrement volume by
static int VolDelta = 5;
// the actual volume passed into SDL, range of 0..SDL_MIX_MAXVOLUME
static int VolSDL = SDL_MIX_MAXVOLUME;
// Muted or not
static int VolIsMuted = 0;
/* definitions of pointers to Core config functions */
ptr_ConfigListSections     ConfigListSections = NULL;
ptr_ConfigListParameters   ConfigListParameters = NULL;
ptr_ConfigGetParameterType ConfigGetParameterType = NULL;
ptr_ConfigSaveSection      ConfigSaveSection = NULL;
ptr_ConfigOpenSection      ConfigOpenSection = NULL;
ptr_ConfigDeleteSection    ConfigDeleteSection = NULL;
ptr_ConfigSetParameter     ConfigSetParameter = NULL;
ptr_ConfigGetParameter     ConfigGetParameter = NULL;
ptr_ConfigGetParameterHelp ConfigGetParameterHelp = NULL;
ptr_ConfigSetDefaultInt    ConfigSetDefaultInt = NULL;
ptr_ConfigSetDefaultFloat  ConfigSetDefaultFloat = NULL;
ptr_ConfigSetDefaultBool   ConfigSetDefaultBool = NULL;
ptr_ConfigSetDefaultString ConfigSetDefaultString = NULL;
ptr_ConfigGetParamInt      ConfigGetParamInt = NULL;
ptr_ConfigGetParamFloat    ConfigGetParamFloat = NULL;
ptr_ConfigGetParamBool     ConfigGetParamBool = NULL;
ptr_ConfigGetParamString   ConfigGetParamString = NULL;

/* Global functions */
void DebugMessage(int level, const char *message, ...)
{
  char msgbuf[1024];
  va_list args;

  if (l_DebugCallback == NULL)
      return;

  va_start(args, message);
  vsprintf(msgbuf, message, args);

  (*l_DebugCallback)(l_DebugCallContext, level, msgbuf);

  va_end(args);
}


static void section_cb(void *ctx, const char *name)
{
    struct { const char *want; int found; } *q = ctx;
    if (strcmp(q->want, name) == 0)
        q->found = 1;
}

static int ConfigSectionExists(const char *name)
{
    struct { const char *want; int found; } q = { name, 0 };
    ConfigListSections(&q, section_cb);
    return q.found;
}

/* Copies the keys both plugins have in common into Audio-Altru, unless they are already set there. */
static void SeedFromLegacy(void)
{
    static const struct { const char *key; m64p_type type; } keys[] = {
        { "DEFAULT_FREQUENCY", M64TYPE_INT }, { "SWAP_CHANNELS", M64TYPE_BOOL },
        { "SECONDARY_BUFFER_SIZE", M64TYPE_INT }, { "RESAMPLE", M64TYPE_STRING },
        { "VOLUME_ADJUST", M64TYPE_INT }, { "VOLUME_DEFAULT", M64TYPE_INT }, { "AUDIO_SYNC", M64TYPE_BOOL },
    };
    m64p_handle old;
    size_t i;

    if (ConfigOpenSection(LEGACY_SECTION, &old) != M64ERR_SUCCESS)
        return;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
    {
        m64p_type t;
        char str[256];
        int v;
        if (ConfigGetParameterType(old, keys[i].key, &t) != M64ERR_SUCCESS || t != keys[i].type)
            continue;
        if (ConfigGetParameterType(l_ConfigAudio, keys[i].key, &t) == M64ERR_SUCCESS)
            continue;
        if (keys[i].type == M64TYPE_STRING)
        {
            if (ConfigGetParameter(old, keys[i].key, M64TYPE_STRING, str, sizeof(str)) == M64ERR_SUCCESS)
                ConfigSetParameter(l_ConfigAudio, keys[i].key, M64TYPE_STRING, str);
        }
        else if (ConfigGetParameter(old, keys[i].key, keys[i].type, &v, sizeof(v)) == M64ERR_SUCCESS)
            ConfigSetParameter(l_ConfigAudio, keys[i].key, keys[i].type, &v);
    }
    DebugMessage(M64MSG_INFO, "Seeded '" CONFIG_SECTION "' from '" LEGACY_SECTION "'");
}

static void VolumeCommit(void);

/* Mupen64Plus plugin functions */
EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle CoreLibHandle, void *Context,
                                   void (*DebugCallback)(void *, int, const char *))
{
    ptr_CoreGetAPIVersions CoreAPIVersionFunc;

    int ConfigAPIVersion, DebugAPIVersion, VidextAPIVersion;
    float fConfigParamsVersion = 0.0f;

    if (l_PluginInit)
        return M64ERR_ALREADY_INIT;

    /* first thing is to set the callback function for debug info */
    l_DebugCallback = DebugCallback;
    l_DebugCallContext = Context;

    /* attach and call the CoreGetAPIVersions function, check Config API version for compatibility */
    CoreAPIVersionFunc = (ptr_CoreGetAPIVersions) osal_dynlib_getproc(CoreLibHandle, "CoreGetAPIVersions");
    if (CoreAPIVersionFunc == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Core emulator broken; no CoreAPIVersionFunc() function found.");
        return M64ERR_INCOMPATIBLE;
    }

    (*CoreAPIVersionFunc)(&ConfigAPIVersion, &DebugAPIVersion, &VidextAPIVersion, NULL);
    if ((ConfigAPIVersion & 0xffff0000) != (CONFIG_API_VERSION & 0xffff0000))
    {
        DebugMessage(M64MSG_ERROR, "Emulator core Config API (v%i.%i.%i) incompatible with plugin (v%i.%i.%i)",
                VERSION_PRINTF_SPLIT(ConfigAPIVersion), VERSION_PRINTF_SPLIT(CONFIG_API_VERSION));
        return M64ERR_INCOMPATIBLE;
    }

    /* Get the core config function pointers from the library handle */
    ConfigOpenSection = (ptr_ConfigOpenSection) osal_dynlib_getproc(CoreLibHandle, "ConfigOpenSection");
    ConfigDeleteSection = (ptr_ConfigDeleteSection) osal_dynlib_getproc(CoreLibHandle, "ConfigDeleteSection");
    ConfigSetParameter = (ptr_ConfigSetParameter) osal_dynlib_getproc(CoreLibHandle, "ConfigSetParameter");
    ConfigGetParameter = (ptr_ConfigGetParameter) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParameter");
    ConfigSetDefaultInt = (ptr_ConfigSetDefaultInt) osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultInt");
    ConfigSetDefaultFloat = (ptr_ConfigSetDefaultFloat) osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultFloat");
    ConfigSetDefaultBool = (ptr_ConfigSetDefaultBool) osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultBool");
    ConfigSetDefaultString = (ptr_ConfigSetDefaultString) osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultString");
    ConfigGetParamInt = (ptr_ConfigGetParamInt) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamInt");
    ConfigGetParamFloat = (ptr_ConfigGetParamFloat) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamFloat");
    ConfigGetParamBool = (ptr_ConfigGetParamBool) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamBool");
    ConfigGetParamString = (ptr_ConfigGetParamString) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamString");

    if (!ConfigOpenSection || !ConfigDeleteSection || !ConfigSetParameter || !ConfigGetParameter ||
        !ConfigSetDefaultInt || !ConfigSetDefaultFloat || !ConfigSetDefaultBool || !ConfigSetDefaultString ||
        !ConfigGetParamInt   || !ConfigGetParamFloat   || !ConfigGetParamBool   || !ConfigGetParamString)
        return M64ERR_INCOMPATIBLE;

    l_CoreDoCommand = (ptr_CoreDoCommand) osal_dynlib_getproc(CoreLibHandle, "CoreDoCommand");
    ConfigListSections = (ptr_ConfigListSections) osal_dynlib_getproc(CoreLibHandle, "ConfigListSections");
    ConfigListParameters = (ptr_ConfigListParameters) osal_dynlib_getproc(CoreLibHandle, "ConfigListParameters");
    ConfigGetParameterType = (ptr_ConfigGetParameterType) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParameterType");
    ConfigSaveSection = (ptr_ConfigSaveSection) osal_dynlib_getproc(CoreLibHandle, "ConfigSaveSection");
    if (!ConfigListSections || !ConfigGetParameterType || !ConfigSaveSection)
        return M64ERR_INCOMPATIBLE;

    if (ConfigOpenSection(CONFIG_SECTION, &l_ConfigAudio) != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_ERROR, "Couldn't open config section '" CONFIG_SECTION "'");
        return M64ERR_INPUT_NOT_FOUND;
    }

    if (ConfigGetParameter(l_ConfigAudio, "Version", M64TYPE_FLOAT, &fConfigParamsVersion, sizeof(float)) != M64ERR_SUCCESS)
    {
        /* a fresh section; keys set before startup (--set) stay */
        if (ConfigSectionExists(LEGACY_SECTION))
            SeedFromLegacy();
    }
    else if (((int) fConfigParamsVersion) != ((int) CONFIG_PARAM_VERSION))
    {
        DebugMessage(M64MSG_WARNING, "Incompatible version %.2f in '" CONFIG_SECTION "' config section: current is %.2f. Setting defaults.", fConfigParamsVersion, (float) CONFIG_PARAM_VERSION);
        ConfigDeleteSection(CONFIG_SECTION);
        ConfigOpenSection(CONFIG_SECTION, &l_ConfigAudio);
    }

    /* set the default values for this plugin */
    ConfigSetDefaultFloat(l_ConfigAudio, "Version",             CONFIG_PARAM_VERSION,  "altru audio plugin config parameter version number");
    ConfigSetDefaultInt(l_ConfigAudio, "DEFAULT_FREQUENCY",     DEFAULT_FREQUENCY,     "Frequency which is used if rom doesn't want to change it");
    ConfigSetDefaultBool(l_ConfigAudio, "SWAP_CHANNELS",        0,                     "Swaps left and right channels");
    ConfigSetDefaultInt(l_ConfigAudio, "SECONDARY_BUFFER_SIZE", 0,                     "SDL device period in output samples (a power of two); 0 picks about 12 ms from the output rate.");
    ConfigSetDefaultString(l_ConfigAudio, "RESAMPLE",           DEFAULT_RESAMPLER,     "Resampler: poly (polyphase sinc, default), trivial (nearest), and in builds with them speex-fixed-{0-10}, src-*");
    ConfigSetDefaultInt(l_ConfigAudio, "BUFFER_MODE",           0,                     "0 = automatic buffer size (grows on underruns, shrinks slowly), 1 = fixed size (BUFFER_MS)");
    ConfigSetDefaultInt(l_ConfigAudio, "BUFFER_MS",             60,                    "Buffer level in milliseconds in fixed mode");
    ConfigSetDefaultInt(l_ConfigAudio, "FF_MODE",               0,                     "Fast-forward audio: 0 = normal pitch, 1 = muted");
    ConfigSetDefaultInt(l_ConfigAudio, "RATE_ADJUST_PPM",       5000,                  "Largest resample ratio change of the dynamic rate control, in parts per million (5000 = 0.5 %)");
    ConfigSetDefaultBool(l_ConfigAudio, "CONCEAL",             1,                     "When the core falls behind, replay the last 20 ms of audio (up to four times in a row) before going silent");
    ConfigSetDefaultInt(l_ConfigAudio, "OUTPUT_MODE",           0,                     "0 = SDL callback, 1 = SDL_QueueAudio (experimental)");
    ConfigSetDefaultInt(l_ConfigAudio, "VOLUME_ADJUST",         5,                     "Percentage change each time the volume is increased or decreased");
    ConfigSetDefaultInt(l_ConfigAudio, "VOLUME_DEFAULT",        80,                    "Default volume when a game is started");
    ConfigSetDefaultBool(l_ConfigAudio, "AUDIO_SYNC",           1,                     "Delay the core when it runs far ahead of the audio clock");

    l_PluginInit = 1;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    if (!l_PluginInit)
        return M64ERR_NOT_INIT;

    altru_opt_shutdown();

    /* reset some local variables */
    l_DebugCallback = NULL;
    l_DebugCallContext = NULL;

    l_PluginInit = 0;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *PluginType, int *PluginVersion, int *APIVersion, const char **PluginNamePtr, int *Capabilities)
{
    /* set version info */
    if (PluginType != NULL)
        *PluginType = M64PLUGIN_AUDIO;

    if (PluginVersion != NULL)
        *PluginVersion = ALTRU_PLUGIN_VERSION;

    if (APIVersion != NULL)
        *APIVersion = AUDIO_PLUGIN_API_VERSION;

    if (PluginNamePtr != NULL)
        *PluginNamePtr = "altru (mupen64plus-audio-altru)";

    if (Capabilities != NULL)
    {
        *Capabilities = 0;
    }

    return M64ERR_SUCCESS;
}

/* ----------- Audio Functions ------------- */
static unsigned int vi_clock_from_system_type(int system_type)
{
    switch (system_type)
    {
    default:
        DebugMessage(M64MSG_WARNING, "Invalid system_type %d. Assuming NTSC", system_type);
        /* fallback */
    case SYSTEM_NTSC: return 48681812;
    case SYSTEM_PAL:  return 49656530;
    case SYSTEM_MPAL: return 48628316;
    }
}

static unsigned int dacrate2freq(unsigned int vi_clock, uint32_t dacrate)
{
    return vi_clock / (dacrate + 1);
}

EXPORT void CALL AiDacrateChanged(int SystemType)
{
    if (!l_PluginInit || l_sdl_backend == NULL)
        return;

    unsigned int frequency = dacrate2freq(vi_clock_from_system_type(SystemType), *AudioInfo.AI_DACRATE_REG);

    sdl_set_frequency(l_sdl_backend, frequency);
}

EXPORT void CALL AiLenChanged(void)
{
    if (!l_PluginInit || l_sdl_backend == NULL)
        return;

    sdl_push_samples(l_sdl_backend, AudioInfo.RDRAM + (*AudioInfo.AI_DRAM_ADDR_REG & 0xffffff), *AudioInfo.AI_LEN_REG);
}

EXPORT int CALL InitiateAudio(AUDIO_INFO Audio_Info)
{
    if (!l_PluginInit)
        return 0;

    AudioInfo = Audio_Info;

    return 1;
}

EXPORT int CALL RomOpen(void)
{
    if (!l_PluginInit || l_sdl_backend != NULL)
        return 0;

    VolDelta = ConfigGetParamInt(l_ConfigAudio, "VOLUME_ADJUST");
    VolPercent = altru_opt_int("VOLUME_DEFAULT");
    if (VolPercent < 0) VolPercent = 0;
    if (VolPercent > 100) VolPercent = 100;
    l_EmuThread = pthread_self();
    VolIsMuted = 0;
    VolumeCommit();

    l_sdl_backend = init_sdl_backend_from_config(l_ConfigAudio, l_CoreDoCommand);
    altru_opt_started();

    return 1;
}

EXPORT void CALL RomClosed(void)
{
    if (!l_PluginInit)
        return;

    release_sdl_backend(l_sdl_backend);
    l_sdl_backend = NULL;
    altru_opt_stopped();
}

EXPORT void CALL ProcessAList(void)
{
}

EXPORT void CALL SetSpeedFactor(int percentage)
{
    if (!l_PluginInit || l_sdl_backend == NULL)
        return;

    sdl_set_speed_factor(l_sdl_backend, percentage);
}

int altru_gain(void)
{
    return VolSDL;
}

m64p_handle altru_config(void) { return l_ConfigAudio; }
int altru_started(void) { return l_PluginInit; }
int altru_running(void) { return l_sdl_backend != NULL; }
int altru_on_emu_thread(void) { return l_sdl_backend == NULL || pthread_equal(pthread_self(), l_EmuThread); }
struct sdl_backend* altru_backend(void) { return l_sdl_backend; }
int altru_volume_get(void) { return VolPercent; }
void altru_volume_set(int percent) { VolumeSetLevel(percent); }

// Returns the most recent ummuted volume level.
static int VolumeGetUnmutedLevel(void)
{
    return VolPercent;
}

// Sets the volume level based on the contents of VolPercent and VolIsMuted
static void VolumeCommit(void)
{
    int levelToCommit = VolIsMuted ? 0 : VolPercent;

    VolSDL = SDL_MIX_MAXVOLUME * levelToCommit / 100;
}

EXPORT void CALL VolumeMute(void)
{
    if (!l_PluginInit)
        return;

    // Store the volume level in order to restore it later
    if (!VolIsMuted)
        VolPercent = VolumeGetUnmutedLevel();

    // Toogle mute
    VolIsMuted = !VolIsMuted;
    VolumeCommit();
}

EXPORT void CALL VolumeUp(void)
{
    if (!l_PluginInit)
        return;

    VolumeSetLevel(VolumeGetUnmutedLevel() + VolDelta);
}

EXPORT void CALL VolumeDown(void)
{
    if (!l_PluginInit)
        return;

    VolumeSetLevel(VolumeGetUnmutedLevel() - VolDelta);
}

EXPORT int CALL VolumeGetLevel(void)
{
    return VolIsMuted ? 0 : VolumeGetUnmutedLevel();
}

EXPORT void CALL VolumeSetLevel(int level)
{
    if (!l_PluginInit)
        return;

    //if muted, unmute first
    VolIsMuted = 0;

    // adjust volume
    VolPercent = level;
    if (VolPercent < 0)
        VolPercent = 0;
    else if (VolPercent > 100)
        VolPercent = 100;

    VolumeCommit();
}

EXPORT const char * CALL VolumeGetString(void)
{
    static char VolumeString[32];

    if (VolIsMuted)
    {
        strcpy(VolumeString, "Mute");
    }
    else
    {
        sprintf(VolumeString, "%i%%", VolPercent);
    }

    return VolumeString;
}

