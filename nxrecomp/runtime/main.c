/* Start-up. Does what the console's loader and rtld do, then enters the sdk:
 *   nn::init::Start(main thread handle, main's base, rtld's "initialised" callback, rtld's "run DT_INIT" callback
 *                   [, rtld's "run DT_FINI" callback: newer SDKs])
 * on the main thread, whose stack size, priority and core come from main.npdm.
 *
 * Usage: <game> [--game <dump dir>] [--image <image dir>] [--save <dir>] [--log <0-2>]
 *   --game   the dumped game: exefs/main.npdm, romfs/ (default: game.json's dump, NX_GAME_DIR in game_config.h)
 *   --image  module images from tools/lifter/a64image.py (default: ../image from the executable)
 *   --save   save data folders (default: ../save from the executable, i.e. build/save)
 *   --dump-after N   after N seconds, print every guest thread's state and backtrace, then exit */
#include "ipc.h"
#include "vk.h"
#include "audio_dsp.h"
#include "game_config.h"
#include "nca.h"
#ifdef NX_NETPLAY
#include "net/net.h"
#endif
#include <stdlib.h>
#include <windows.h>
#include <process.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

static char game_dir[1024], image_dir[1024], save_dir[1024];
void audout_dump(void);
static int dump_after;                                  /* seconds; then print every thread's state and exit */

/* NX_DUMP_MEM=addr:len,...: hex dumps of guest memory (16 bytes a line); *addr dumps where the pointer at addr
 * points. Printed by --dump-after and on a crash */
static bool mem_readable(u64 a, u64 n) {
    MemInfo mi;
    mem_query(a, &mi);
    return mi.state != MS_FREE && mi.state != MS_INACCESSIBLE && mi.perm & MP_R && a + n <= mi.addr + mi.size;
}

void dump_mem_env(void) {
    for (const char *e = getenv("NX_DUMP_MEM"); e && *e;) {
        char *end;
        bool deref = *e == '*';
        u64 a = strtoull(e + deref, &end, 16), n = *end == ':' ? strtoull(end + 1, &end, 16) : 0x40;
        if (deref) a = mem_readable(a, 8) ? *(const u64 *)(uintptr_t)a : 0;
        hlog(0, "mem dump at %llx:", a);
        for (u64 o = 0; o < n && mem_readable(a + o, 16); o += 16) {
            const u32 *w = (const u32 *)(uintptr_t)(a + o);
            hlog(0, "mem %llx: %08x %08x %08x %08x", a + o, w[0], w[1], w[2], w[3]);
        }
        e = *end == ',' ? end + 1 : end + strlen(end);
    }
}

static unsigned __stdcall dump_thread(void *p) {
    Sleep((DWORD)dump_after * 1000);
    hlog(0, "--dump-after %d: thread states", dump_after);
    kernel_dump_threads();
    audout_dump();
    dump_mem_env();
    fatal("stopped after %d seconds", dump_after);
}

static void h_rtld_initialized(Ctx *c) { (void)c; }

/* rtld walks its module list from the front, where each module it found was inserted: sdk, subsdk0, main */
static void h_rtld_run_dt_init(Ctx *c) {
    for (int i = num_loaded_mods - 1; i >= 0; i--) {
        LoadedMod *m = &loaded_mods[i];
        if (!m->dt_init) continue;
        hlog(1, "DT_INIT of %s", m->name);
        guest_call(c, m->dt_init);
    }
}

/* finalisers in the opposite order: main, subsdk0, sdk */
static void h_rtld_run_dt_fini(Ctx *c) {
    for (int i = 0; i < num_loaded_mods; i++) {
        LoadedMod *m = &loaded_mods[i];
        if (!m->dt_fini) continue;
        hlog(1, "DT_FINI of %s", m->name);
        guest_call(c, m->dt_fini);
    }
}

static void exe_relative(char *out, size_t n, const char *rel) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    snprintf(out, n, "%s\\%s", exe, rel);
}

/* Where things live. A development build keeps everything in the build folder (the executable's parent): module
 * images, shaders, pipeline caches, saves. A packaged build (NX_PACKAGE) is one folder for players: the game's
 * files in game\ next to the executable, the shader translator in translator\, and everything the game writes
 * under %LOCALAPPDATA%\<title>\ (save\, cache\, log.txt) */
static char cache_dir[1024], data_dir[1024];
const char *nx_cache_dir(void) { return cache_dir; }
const char *nx_data_dir(void) { return data_dir; }
static bool from_nsp;                                   /* the game is read from the user's NSPs (nca.c) */
bool nx_from_nsp(void) { return from_nsp; }

static void make_dirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 3; *s; s++)
        if (*s == '\\' || *s == '/') { char c = *s; *s = 0; CreateDirectoryA(p, NULL); *s = c; }
    CreateDirectoryA(p, NULL);
}

static void paths_init(void) {
#ifdef NX_PACKAGE
    const char *la = getenv("LOCALAPPDATA");
    snprintf(data_dir, sizeof data_dir, "%s\\%s", la ? la : ".", NX_TITLE);
    snprintf(cache_dir, sizeof cache_dir, "%s\\cache", data_dir);
    snprintf(save_dir, sizeof save_dir, "%s\\save", data_dir);
    snprintf(image_dir, sizeof image_dir, "%s\\image", cache_dir);
    exe_relative(game_dir, sizeof game_dir, "game");
    make_dirs(cache_dir);
    char log[1100];                                     /* no console: the log goes to a file */
    snprintf(log, sizeof log, "%s\\log.txt", data_dir);
    freopen(log, "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
#else
    exe_relative(game_dir, sizeof game_dir, NX_GAME_DIR);
    exe_relative(image_dir, sizeof image_dir, "..\\image");
    exe_relative(save_dir, sizeof save_dir, "..\\save");
    exe_relative(cache_dir, sizeof cache_dir, "..");
    exe_relative(data_dir, sizeof data_dir, "..");
#endif
}

/* the game from NSPs: open them, and build the module images once (into the cache); a problem is shown to the
 * player and ends the program */
static void nsp_open(void) {
    if (!nx_game_present(game_dir)) {
#ifdef NX_PACKAGE
        char msg[1400];
        snprintf(msg, sizeof msg, "Put your own %s files in\n%s\n\nSee README.txt.", NX_TITLE, game_dir);
        MessageBoxA(NULL, msg, NX_TITLE, MB_ICONINFORMATION);
        exit(1);
#endif
        return;                                         /* a development build: the extracted dump */
    }
    char err[1024];
    from_nsp = true;
    if (!nx_game_open(game_dir, err, sizeof err) || (make_dirs(image_dir), !image_build(image_dir, err, sizeof err))) {
        hlog(0, "%s", err);
        MessageBoxA(NULL, err, NX_TITLE, MB_ICONERROR);
        exit(1);
    }
}

typedef struct Npdm { u8 prio, core; u32 stack_size, system_resource_size; } Npdm;

static Npdm read_npdm(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/exefs/main.npdm", game_dir);
    u8 meta[0x80];
    if (from_nsp) {
        u64 n;
        u8 *p = nx_game_exefs_file("main.npdm", &n);
        if (!p || n < sizeof meta) fatal("the game's ExeFS has no main.npdm");
        memcpy(meta, p, sizeof meta);
        free(p);
        snprintf(path, sizeof path, "main.npdm");
    } else {
        FILE *f = fopen(path, "rb");
        if (!f) fatal("cannot open %s", path);
        if (fread(meta, 1, sizeof meta, f) != sizeof meta) memset(meta, 0, sizeof meta);
        fclose(f);
    }
    if (memcmp(meta, "META", 4)) fatal("%s: not an NPDM", path);
    Npdm n = {meta[0xe], meta[0xf], 0};
    memcpy(&n.stack_size, meta + 0x1c, 4);
    memcpy(&n.system_resource_size, meta + 0x14, 4);
    return n;
}

int main(int argc, char **argv) {
    paths_init();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) snprintf(game_dir, sizeof game_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) snprintf(image_dir, sizeof image_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) snprintf(save_dir, sizeof save_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-after") && i + 1 < argc) dump_after = atoi(argv[++i]);
        else { fprintf(stderr, "usage: " NX_NAME " [--game DIR] [--image DIR] [--save DIR] [--log 0-2] [--dump-after SECONDS]\n"); return 2; }
    }
    diag_init();
    /* 1 ms timer resolution for the whole run: guest waits sleep in ms (kernel.c ksleep) and would otherwise round up
     * to the 15.6 ms tick. Netplay builds got it from ENet's init; now every build has it */
    timeBeginPeriod(1);
    nsp_open();
#ifdef NX_NETPLAY
    netplay_startup(save_dir, game_dir);                /* the room browser, before anything else opens */
#endif
    mem_init();
    kernel_init();
    dispatch_init();
    services_init(game_dir, save_dir);
    loader_load(image_dir);
    window_init();
    audio_init();
    vk_init();
    nvn_init();
    Npdm npdm = read_npdm();
    system_resource_size = npdm.system_resource_size;
    u64 stack_size = (npdm.stack_size + 0xfffull) & ~0xfffull;
    if (stack_size > MAINSTACK_MAX) fatal("main thread stack of %llx bytes is too large", stack_size);
    mem_map(MAINSTACK_BASE, stack_size, MS_STACK, MP_RW);
    /* SDK 4 (MK8): Start(handle, base, initialised, DT_INIT); later SDKs (13: Violet) add a DT_FINI callback,
     * called after nnMain returns */
    u64 start = sym_lookup("_ZN2nn4init5StartEmmPFvvES2_");
    bool with_fini = false;
    if (!start && (start = sym_lookup("_ZN2nn4init5StartEmmPFvvES2_S2_")) != 0) with_fini = true;
    if (!start) fatal("no nn::init::Start in the sdk");
    KThread *t = thread_create(start, 0, MAINSTACK_BASE + stack_size, npdm.prio, npdm.core);
    snprintf(t->name, sizeof t->name, "main");
    u32 h;
    handle_new(&t->obj, &h);
    t->handle = h;
    Ctx *c = t->ctx;
    c->x[0] = h;
    c->x[1] = loaded_mods[0].base;
    c->x[2] = hostcall_register("rtld initialised", h_rtld_initialized);
    c->x[3] = hostcall_register("rtld DT_INIT", h_rtld_run_dt_init);
    if (with_fini) c->x[4] = hostcall_register("rtld DT_FINI", h_rtld_run_dt_fini);
    hlog(1, "main thread: stack %llx bytes, priority %u, core %u", stack_size, npdm.prio, npdm.core);
    thread_start(t);
    if (dump_after) _beginthreadex(NULL, 0, dump_thread, NULL, 0, NULL);
    window_loop(t->host);
    hlog(1, "the main thread exited");
    return 0;
}
