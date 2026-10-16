/* altru - test_suxty_if.c: loads the plugin the way the core does, from a fake core that exports the config API, and exercises the plugin
 * lifecycle (20 ROM open/close cycles with audio pushes), the Audio-SDL seeding and the suxty interface. Exit code 0 = all checks passed.
 *   usage: SDL_AUDIODRIVER=dummy test_suxty_if <mupen64plus-audio-altru.so>      (link with -ldl -lpthread -rdynamic)
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_types.h"
#include "m64p_common.h"
#include "m64p_config.h"
#include "m64p_frontend.h"
#include "m64p_plugin.h"
#include "altru_suxty.h"

/* ---- a minimal config store in the role of the core ---- */
struct entry { char name[40]; m64p_type type; int i; float f; char s[128]; };
struct section { char name[40]; struct entry e[48]; int n; int saves; };
static struct section l_sec[8];
static int l_nsec;
static int l_limiter = 1;

#define X __attribute__((visibility("default")))
X m64p_error CoreGetAPIVersions(int* c, int* d, int* v, int* e) { *c = 0x020100; *d = 0x020100; *v = 0x030300; if (e) *e = 0; return M64ERR_SUCCESS; }
X m64p_error CoreDoCommand(m64p_command c, int p, void* v) { if (c == M64CMD_CORE_STATE_QUERY && p == M64CORE_SPEED_LIMITER) { *(int*)v = l_limiter; return M64ERR_SUCCESS; } return M64ERR_INPUT_INVALID; }
static struct section* find(const char* n) { int i; for (i = 0; i < l_nsec; i++) if (!strcmp(l_sec[i].name, n)) return &l_sec[i]; return NULL; }
static struct entry* fe(struct section* s, const char* n) { int i; for (i = 0; i < s->n; i++) if (!strcmp(s->e[i].name, n)) return &s->e[i]; return NULL; }
X m64p_error ConfigListSections(void* ctx, void (*cb)(void*, const char*)) { int i; for (i = 0; i < l_nsec; i++) cb(ctx, l_sec[i].name); return M64ERR_SUCCESS; }
X m64p_error ConfigOpenSection(const char* n, m64p_handle* h) { struct section* s = find(n); if (!s) { s = &l_sec[l_nsec++]; snprintf(s->name, sizeof s->name, "%s", n); } *h = s; return M64ERR_SUCCESS; }
X m64p_error ConfigDeleteSection(const char* n) { struct section* s = find(n); if (s) s->n = 0; return M64ERR_SUCCESS; }
X m64p_error ConfigSaveSection(const char* n) { struct section* s = find(n); if (!s) return M64ERR_INPUT_NOT_FOUND; s->saves++; return M64ERR_SUCCESS; }
X m64p_error ConfigGetParameterType(m64p_handle h, const char* n, m64p_type* t) { struct entry* e = fe(h, n); if (!e) return M64ERR_INPUT_NOT_FOUND; *t = e->type; return M64ERR_SUCCESS; }
static struct entry* mk(struct section* s, const char* n, m64p_type t) { struct entry* e = fe(s, n); if (!e) { e = &s->e[s->n++]; memset(e, 0, sizeof *e); snprintf(e->name, sizeof e->name, "%s", n); e->type = t; } return e; }
X m64p_error ConfigSetParameter(m64p_handle h, const char* n, m64p_type t, const void* v)
{
    struct entry* e = mk(h, n, t);
    e->type = t;
    if (t == M64TYPE_INT || t == M64TYPE_BOOL) e->i = *(const int*)v; else if (t == M64TYPE_FLOAT) e->f = *(const float*)v; else snprintf(e->s, sizeof e->s, "%s", (const char*)v);
    return M64ERR_SUCCESS;
}
X m64p_error ConfigGetParameter(m64p_handle h, const char* n, m64p_type t, void* v, int sz)
{
    struct entry* e = fe(h, n);
    (void)sz;
    if (!e) return M64ERR_INPUT_NOT_FOUND;
    if (t == M64TYPE_INT || t == M64TYPE_BOOL) *(int*)v = e->i; else if (t == M64TYPE_FLOAT) *(float*)v = e->f; else snprintf(v, (size_t)sz, "%s", e->s);
    return M64ERR_SUCCESS;
}
X m64p_error ConfigSetDefaultInt(m64p_handle h, const char* n, int v, const char* help) { (void)help; if (!fe(h, n)) mk(h, n, M64TYPE_INT)->i = v; return M64ERR_SUCCESS; }
X m64p_error ConfigSetDefaultFloat(m64p_handle h, const char* n, float v, const char* help) { (void)help; if (!fe(h, n)) mk(h, n, M64TYPE_FLOAT)->f = v; return M64ERR_SUCCESS; }
X m64p_error ConfigSetDefaultBool(m64p_handle h, const char* n, int v, const char* help) { (void)help; if (!fe(h, n)) mk(h, n, M64TYPE_BOOL)->i = v; return M64ERR_SUCCESS; }
X m64p_error ConfigSetDefaultString(m64p_handle h, const char* n, const char* v, const char* help) { (void)help; if (!fe(h, n)) snprintf(mk(h, n, M64TYPE_STRING)->s, 128, "%s", v); return M64ERR_SUCCESS; }
X int ConfigGetParamInt(m64p_handle h, const char* n) { struct entry* e = fe(h, n); return e ? e->i : 0; }
X float ConfigGetParamFloat(m64p_handle h, const char* n) { struct entry* e = fe(h, n); return e ? e->f : 0; }
X int ConfigGetParamBool(m64p_handle h, const char* n) { struct entry* e = fe(h, n); return e ? e->i : 0; }
X const char* ConfigGetParamString(m64p_handle h, const char* n) { struct entry* e = fe(h, n); return e ? e->s : ""; }

static int l_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); l_fail++; } } while (0)

static void dbg(void* ctx, int level, const char* msg) { (void)ctx; if (level <= M64MSG_WARNING) printf("plugin: %s\n", msg); }

static const struct altru_suxty_interface* l_if;
static void* thread_get(void* arg) { int v; *(int*)arg = l_if->get(0, &v); return NULL; }

int main(int argc, char** argv)
{
    void* so = dlopen(argc > 1 ? argv[1] : "./mupen64plus-audio-altru.so", RTLD_NOW);
    void* core = dlopen(NULL, RTLD_NOW);
    m64p_error (*startup)(m64p_dynlib_handle, void*, void (*)(void*, int, const char*));
    m64p_error (*shutdown)(void);
    int (*init)(AUDIO_INFO);
    int (*romopen)(void);
    void (*romclosed)(void), (*dacrate)(int), (*ailen)(void), (*speed)(int);
    const struct altru_suxty_interface* (*getif)(void);
    int v, i, cyc, r;
    static unsigned char rdram[1 << 16];
    unsigned int dram_addr = 0, ai_len = 0, dacreg = 0;
    AUDIO_INFO ai;
    m64p_handle h;
    pthread_t th;

    if (!so) { printf("dlopen: %s\n", dlerror()); return 2; }
    startup = dlsym(so, "PluginStartup"); shutdown = dlsym(so, "PluginShutdown"); init = dlsym(so, "InitiateAudio");
    romopen = dlsym(so, "RomOpen"); romclosed = dlsym(so, "RomClosed"); dacrate = dlsym(so, "AiDacrateChanged");
    ailen = dlsym(so, "AiLenChanged"); speed = dlsym(so, "SetSpeedFactor"); getif = dlsym(so, "AltruSuxtyGetInterface");
    CHECK(startup && shutdown && init && romopen && romclosed && dacrate && ailen && speed && getif);
    l_if = getif();
    CHECK(l_if->abi == ALTRU_SUXTY_ABI && l_if->size == sizeof(struct altru_suxty_interface) && l_if->option_count > 0);

    /* option table sanity */
    for (i = 0; i < (int)l_if->option_count; i++) {
        const struct altru_suxty_option* o = &l_if->options[i];
        int j;
        CHECK(o->key && o->label && o->help && o->def >= o->min && o->def <= o->max);
        if (o->type == ALTRU_OPT_ENUM) { CHECK(o->enum_labels != NULL); for (j = 0; j <= o->max; j++) CHECK(o->enum_labels[j] != NULL); }
        for (j = 0; j < i; j++) CHECK(strcmp(o->key, l_if->options[j].key) != 0);
    }

    CHECK(l_if->get(0, &v) == ALTRU_SUXTY_E_STATE);

    /* seeding from an existing Audio-SDL section; Audio-SDL stays as it was */
    ConfigOpenSection("Audio-SDL", &h);
    ConfigSetDefaultInt(h, "DEFAULT_FREQUENCY", 22050, ""); ConfigSetDefaultBool(h, "AUDIO_SYNC", 0, ""); ConfigSetDefaultString(h, "RESAMPLE", "trivial", "");
    ConfigSetDefaultInt(h, "VOLUME_DEFAULT", 33, ""); ConfigSetDefaultInt(h, "UNRELATED", 7, "");
    CHECK(startup(core, NULL, dbg) == M64ERR_SUCCESS);
    CHECK(find("Audio-Altru") != NULL);
    CHECK(ConfigGetParamInt(find("Audio-Altru"), "DEFAULT_FREQUENCY") == 22050);
    CHECK(ConfigGetParamBool(find("Audio-Altru"), "AUDIO_SYNC") == 0);
    CHECK(strcmp(ConfigGetParamString(find("Audio-Altru"), "RESAMPLE"), "trivial") == 0);
    CHECK(ConfigGetParamInt(find("Audio-Altru"), "VOLUME_DEFAULT") == 33);
    CHECK(fe(find("Audio-Altru"), "UNRELATED") == NULL);
    CHECK(find("Audio-SDL")->n == 5 && ConfigGetParamInt(find("Audio-SDL"), "VOLUME_DEFAULT") == 33);
    CHECK(shutdown() == M64ERR_SUCCESS);
    ConfigSetParameter(find("Audio-SDL"), "VOLUME_DEFAULT", M64TYPE_INT, &(int){ 90 });
    CHECK(startup(core, NULL, dbg) == M64ERR_SUCCESS);
    CHECK(ConfigGetParamInt(find("Audio-Altru"), "VOLUME_DEFAULT") == 33); /* not seeded twice */

    /* interface before the ROM is open */
    CHECK(l_if->get(0, &v) == 0 && v == 33);
    CHECK(l_if->set(0, 40) == ALTRU_SUXTY_APPLIED && l_if->get(0, &v) == 0 && v == 40);
    CHECK(l_if->set(0, 41) == ALTRU_SUXTY_E_RANGE && l_if->set(0, 105) == ALTRU_SUXTY_E_RANGE && l_if->set(99, 0) == ALTRU_SUXTY_E_INDEX);
    CHECK(ConfigGetParamInt(find("Audio-Altru"), "VOLUME_DEFAULT") == 33); /* set() never writes the config */

    memset(&ai, 0, sizeof ai);
    ai.RDRAM = rdram; ai.AI_DRAM_ADDR_REG = &dram_addr; ai.AI_LEN_REG = &ai_len; ai.AI_DACRATE_REG = &dacreg;
    CHECK(init(ai) == 1);
    CHECK(romopen() == 1);
    CHECK(l_if->get(0, &v) == 0 && v == 40);           /* the launch started with the set() value */
    CHECK(l_if->set(1, 1) == ALTRU_SUXTY_APPLIED && l_if->get(1, &v) == 0 && v == 1);
    CHECK(l_if->set(2, 0) == ALTRU_SUXTY_APPLIED && l_if->set(3, 1) == ALTRU_SUXTY_APPLIED && l_if->get(3, &v) == 0 && v == 1);
    CHECK(l_if->set(3, 0) == ALTRU_SUXTY_APPLIED);
    CHECK(l_if->set(5, 100) == ALTRU_SUXTY_NEXT_LAUNCH && l_if->get(5, &v) == 0 && v == 60); /* the running game keeps its value */
    r = -1; pthread_create(&th, NULL, thread_get, &r); pthread_join(th, NULL);
    CHECK(r == ALTRU_SUXTY_E_THREAD);
    CHECK(l_if->save_default(0, 55) == 0 && ConfigGetParamInt(find("Audio-Altru"), "VOLUME_DEFAULT") == 55 && find("Audio-Altru")->saves == 1);
    CHECK(l_if->get(0, &v) == 0 && v == 40);           /* save_default leaves the running value alone */

    /* pushes at a few rates and speeds */
    dacreg = 48681812 / 32006 - 1;
    dacrate(0);
    for (i = 0; i < 400; i++) {
        unsigned k;
        for (k = 0; k < sizeof rdram; k++) rdram[k] = (unsigned char)(k * 7 + i);
        ai_len = 2000 + (i % 7) * 4; ailen();
        if (i == 100) speed(200);
        if (i == 200) { speed(100); dacreg = 48681812 / 22050 - 1; dacrate(0); }
        if (i == 300) { l_limiter = 0; }
        usleep(2000);
    }
    l_limiter = 1;
    romclosed();
    CHECK(l_if->get(5, &v) == 0 && v == 100);          /* the next launch reads the set() value */

    /* ROM close/open cycles */
    for (cyc = 0; cyc < 20; cyc++) {
        CHECK(romopen() == 1);
        dacreg = 48681812 / (cyc % 2 ? 44100 : 32006) - 1; dacrate(0);
        for (i = 0; i < 40; i++) { ai_len = 3000; ailen(); usleep(1000); }
        if (cyc % 5 == 0) speed(300);
        romclosed();
    }
    CHECK(shutdown() == M64ERR_SUCCESS);
    printf(l_fail ? "%d checks FAILED\n" : "all checks passed\n", l_fail);
    return l_fail != 0;
}
