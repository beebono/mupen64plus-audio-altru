/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - main.h                                                  *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2008-2012 Tillin9, Richard42                            *
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

/* version info */
#include "m64p_config.h"

struct sdl_backend;

#include <stddef.h>

#if defined(__GNUC__)
#define ATTR_FMT(fmtpos, attrpos) __attribute__ ((format (printf, fmtpos, attrpos)))
#else
#define ATTR_FMT(fmtpos, attrpos)
#endif

struct resampler_interface;

/* volume mixer types */
enum {
    VOLUME_TYPE_SDL = 1,
    VOLUME_TYPE_OSS = 2,
};

#define ALTRU_PLUGIN_VERSION 0x000100

/* current output gain, 0..128 */
int altru_gain(void);

/* plugin state for suxty.c */
m64p_handle altru_config(void);
int altru_started(void);          /* PluginStartup done */
int altru_running(void);          /* ROM open, backend exists */
int altru_on_emu_thread(void);
struct sdl_backend* altru_backend(void);
int altru_volume_get(void);
void altru_volume_set(int percent);

/* options, with the values suxty set() for the coming launch taking precedence over the config */
int altru_opt_int(const char* key);
const char* altru_opt_resampler(void);
void altru_opt_started(void);
void altru_opt_stopped(void);
void altru_opt_shutdown(void);

/* declarations of pointers to Core config functions */
extern ptr_ConfigListSections     ConfigListSections;
extern ptr_ConfigOpenSection      ConfigOpenSection;
extern ptr_ConfigGetParameterType ConfigGetParameterType;
extern ptr_ConfigSaveSection      ConfigSaveSection;
extern ptr_ConfigDeleteSection    ConfigDeleteSection;
extern ptr_ConfigListParameters   ConfigListParameters;
extern ptr_ConfigSetParameter     ConfigSetParameter;
extern ptr_ConfigGetParameter     ConfigGetParameter;
extern ptr_ConfigGetParameterHelp ConfigGetParameterHelp;
extern ptr_ConfigSetDefaultInt    ConfigSetDefaultInt;
extern ptr_ConfigSetDefaultFloat  ConfigSetDefaultFloat;
extern ptr_ConfigSetDefaultBool   ConfigSetDefaultBool;
extern ptr_ConfigSetDefaultString ConfigSetDefaultString;
extern ptr_ConfigGetParamInt      ConfigGetParamInt;
extern ptr_ConfigGetParamFloat    ConfigGetParamFloat;
extern ptr_ConfigGetParamBool     ConfigGetParamBool;
extern ptr_ConfigGetParamString   ConfigGetParamString;

void DebugMessage(int level, const char *message, ...) ATTR_FMT(2,3);

