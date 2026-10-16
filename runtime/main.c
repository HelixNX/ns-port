/* mk8recomp: start-up. Does what the console's loader and rtld do, then enters the sdk:
 *   nn::init::Start(main thread handle, main's base, rtld's "initialised" callback, rtld's "run DT_INIT" callback)
 * on the main thread, whose stack size, priority and core come from main.npdm.
 *
 * Usage: mk8recomp [--game <dump dir>] [--image <image dir>] [--save <dir>] [--log <0-2>]
 *   --game   the dumped game: exefs/main.npdm, romfs/ (default: ../../dump_v21 from the executable)
 *   --image  module images from tools/lifter/a64image.py (default: ../image from the executable)
 *   --save   save data folders (default: ../save from the executable, i.e. build/save)
 *   --dump-after N   after N seconds, print every guest thread's state and backtrace, then exit */
#include "ipc.h"
#include "vk.h"
#include <stdlib.h>
#include <windows.h>
#include <process.h>

static char game_dir[1024], image_dir[1024], save_dir[1024];
static int dump_after;                                  /* seconds; then print every thread's state and exit */

static unsigned __stdcall dump_thread(void *p) {
    Sleep((DWORD)dump_after * 1000);
    hlog(0, "--dump-after %d: thread states", dump_after);
    kernel_dump_threads();
    fatal("stopped after %d seconds", dump_after);
}

void u_nnosGetDefaultUserExceptionStackForApplication(Ctx *c) {
    guest_trap(c, "call to the unresolved nnosGetDefaultUserExceptionStackForApplication", c->x[30]);
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

static void exe_relative(char *out, size_t n, const char *rel) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    snprintf(out, n, "%s\\%s", exe, rel);
}

typedef struct Npdm { u8 prio, core; u32 stack_size; } Npdm;

static Npdm read_npdm(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/exefs/main.npdm", game_dir);
    FILE *f = fopen(path, "rb");
    if (!f) fatal("cannot open %s", path);
    u8 meta[0x80];
    if (fread(meta, 1, sizeof meta, f) != sizeof meta || memcmp(meta, "META", 4)) fatal("%s: not an NPDM", path);
    fclose(f);
    Npdm n = {meta[0xe], meta[0xf], 0};
    memcpy(&n.stack_size, meta + 0x1c, 4);
    return n;
}

int main(int argc, char **argv) {
    exe_relative(game_dir, sizeof game_dir, "..\\..\\dump_v21");
    exe_relative(image_dir, sizeof image_dir, "..\\image");
    exe_relative(save_dir, sizeof save_dir, "..\\save");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) snprintf(game_dir, sizeof game_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) snprintf(image_dir, sizeof image_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) snprintf(save_dir, sizeof save_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-after") && i + 1 < argc) dump_after = atoi(argv[++i]);
        else { fprintf(stderr, "usage: mk8recomp [--game DIR] [--image DIR] [--save DIR] [--log 0-2] [--dump-after SECONDS]\n"); return 2; }
    }
    diag_init();
    mem_init();
    kernel_init();
    dispatch_init();
    services_init(game_dir, save_dir);
    loader_load(image_dir);
    window_init();
    vk_init();
    nvn_init();
    Npdm npdm = read_npdm();
    u64 stack_size = (npdm.stack_size + 0xfffull) & ~0xfffull;
    if (stack_size > MAINSTACK_MAX) fatal("main thread stack of %llx bytes is too large", stack_size);
    mem_map(MAINSTACK_BASE, stack_size, MS_STACK, MP_RW);
    u64 start = sym_lookup("_ZN2nn4init5StartEmmPFvvES2_");
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
    hlog(1, "main thread: stack %llx bytes, priority %u, core %u", stack_size, npdm.prio, npdm.core);
    thread_start(t);
    if (dump_after) _beginthreadex(NULL, 0, dump_thread, NULL, 0, NULL);
    window_loop(t->host);
    hlog(1, "the main thread exited");
    return 0;
}
