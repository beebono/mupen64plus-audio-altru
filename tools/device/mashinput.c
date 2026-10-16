/* altru - mashinput.c: test-only input plugin for unattended runs. Presses A and Start periodically (after 6 s) so a game gets past its menus,
 * and drives the core from a thread: MASH_STOP=<s> stops the emulation after s seconds (clean RomClosed, so the plugins write their statistics),
 * MASH_FF="15:200,25:300,35:max,45:100" sets the speed factor / switches the speed limiter off at those times, MASH_DEBUG=1 logs it.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define M64P_PLUGIN_PROTOTYPES 1
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include "m64p_types.h"
#include "m64p_plugin.h"
#include "m64p_common.h"
#include "m64p_frontend.h"

static CONTROL *l_ctl;
static ptr_CoreDoCommand l_do;
static double l_stop;
static struct { double t; int v; } l_ff[16];
static int l_nff, l_iff;
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static double t0;

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle h, void *c, void (*d)(void *, int, const char *)) { (void)c; (void)d; l_do = (ptr_CoreDoCommand)dlsym(h, "CoreDoCommand"); if (getenv("MASH_STOP")) l_stop = atof(getenv("MASH_STOP"));
    if (getenv("MASH_FF")) { /* "15:200,25:300,35:max,45:100" */
        char *p = getenv("MASH_FF"), *e;
        while (*p && l_nff < 16) {
            l_ff[l_nff].t = strtod(p, &e);
            if (*e != ':') break;
            p = e + 1;
            l_ff[l_nff].v = strncmp(p, "max", 3) == 0 ? -1 : atoi(p);
            l_nff++;
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
        }
    } return M64ERR_SUCCESS; }
EXPORT m64p_error CALL PluginShutdown(void) { return M64ERR_SUCCESS; }
EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *t, int *v, int *a, const char **n, int *cap)
{
    if (t) *t = M64PLUGIN_INPUT;
    if (v) *v = 0x010000;
    if (a) *a = 0x020100;
    if (n) *n = "altru mash input (test)";
    if (cap) *cap = 0;
    return M64ERR_SUCCESS;
}
EXPORT void CALL InitiateControllers(CONTROL_INFO ci) { l_ctl = ci.Controls; l_ctl[0].Present = 1; l_ctl[0].RawData = 0; l_ctl[0].Plugin = PLUGIN_NONE; }
EXPORT void CALL GetKeys(int c, BUTTONS *k)
{
    double t;
    k->Value = 0;
    if (c != 0) return;
    if (getenv("MASH_DEBUG")) { static int once; if (!once++) fprintf(stderr, "mash: GetKeys called\n"); }
    if (!t0) t0 = now();
    t = now() - t0;
    if (t < 6) return;
    if (((int)((t - 6) * 10)) % 12 == 0) k->A_BUTTON = 1;
    if (((int)((t - 9) * 10)) % 50 == 0) k->START_BUTTON = 1;
}
EXPORT void CALL ControllerCommand(int c, unsigned char *cmd) { (void)c; (void)cmd; }
EXPORT void CALL ReadController(int c, unsigned char *cmd) { (void)c; (void)cmd; }
static void *sched(void *arg)
{
    double s0 = now();
    (void)arg;
    for (;;) {
        double t = now() - s0;
        usleep(20000);
        if (l_stop > 0 && t > l_stop && l_do) { l_stop = 0; l_do(M64CMD_STOP, 0, NULL); return NULL; }
        while (l_iff < l_nff && t >= l_ff[l_iff].t && l_do) {
            int v = l_ff[l_iff].v, on;
            if (v < 0) { int h = 100; l_do(M64CMD_CORE_STATE_SET, M64CORE_SPEED_FACTOR, &h); on = 0; l_do(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &on); }
            else { on = 1; l_do(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &on); l_do(M64CMD_CORE_STATE_SET, M64CORE_SPEED_FACTOR, &v); }
            if (getenv("MASH_DEBUG")) fprintf(stderr, "mash: FF %d at %.1f\n", v, t);
            l_iff++;
        }
        if (l_stop <= 0 && l_iff >= l_nff) return NULL;
    }
}
static pthread_t l_th;
EXPORT int CALL RomOpen(void) { l_iff = 0; pthread_create(&l_th, NULL, sched, NULL); return 1; }
EXPORT void CALL RomClosed(void) { t0 = 0; l_stop = 0; l_iff = l_nff; pthread_join(l_th, NULL); }
EXPORT void CALL SDL_KeyDown(int m, int k) { (void)m; (void)k; }
EXPORT void CALL SDL_KeyUp(int m, int k) { (void)m; (void)k; }
