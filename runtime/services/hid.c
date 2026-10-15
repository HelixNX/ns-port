/* hid: input. The applet resource's shared memory (0x40000 bytes, the SDK's layout) is where input appears: a host
 * thread writes npad 0 as a connected Pro Controller every 16 ms, with the state hid_set_input gives (M6 feeds it
 * from SDL3). Commands that only configure or activate succeed. */
#include "../ipc.h"
#include <windows.h>
#include <process.h>

static KShmem *hid_shmem;
static KEvent *style_event, *xpad_event, *palma_event;

static void ar_shmem(SvcObj *o, IpcReq *r) {
    u32 h;
    if (handle_new(&hid_shmem->obj, &h)) fatal("out of handles");
    out_copy_handle(r, h);
}
static const Iface iface_applet_resource = {"IAppletResource", (const IpcCmd[]){
    {0, ar_shmem, "GetSharedMemoryHandle"},
    IPC_END}, NULL};

static const Iface iface_vibration_list = {"IActiveVibrationDeviceList", (const IpcCmd[]){
    {0, ipc_noop, "ActivateVibrationDevice"},
    IPC_END}, NULL};

IPC_OPEN(hid_create_applet_resource, &iface_applet_resource)
IPC_OPEN(hid_create_vibration_list, &iface_vibration_list)
static void hid_style_event(SvcObj *o, IpcReq *r) { out_event(r, style_event); }
static void hid_xpad_event(SvcObj *o, IpcReq *r) { out_event(r, xpad_event); }
static void hid_palma_event(SvcObj *o, IpcReq *r) { out_event(r, palma_event); }
static void hid_true(SvcObj *o, IpcReq *r) { out_u8(r, 1); }
static void hid_false(SvcObj *o, IpcReq *r) { out_u8(r, 0); }
static void hid_u64_zero(SvcObj *o, IpcReq *r) { out_u64(r, 0); }
static void hid_style_set(SvcObj *o, IpcReq *r) { out_u32(r, 0x1f); }        /* full key, handheld, dual, left, right */
/* VibrationDeviceInfo: type (1 linear resonant actuator), position (1 left, 2 right by the handle's index) */
static void hid_vibration_info(SvcObj *o, IpcReq *r) {
    u32 handle = in_u32(r, 0);
    out_u32(r, 1);
    out_u32(r, (handle >> 16 & 0xff) == 0 ? 1 : 2);
}
static void hid_actual_vibration(SvcObj *o, IpcReq *r) {
    float v[4] = {0, 160.0f, 0, 320.0f};                                        /* amp low, freq low, amp high, freq high */
    out_data(r, v, 16);
}

static const Iface iface_hid = {"hid", (const IpcCmd[]){
    {0, hid_create_applet_resource, "CreateAppletResource"},
    {40, hid_xpad_event, "AcquireXpadIdEventHandle"},
    {68, hid_false, "IsSixAxisSensorFusionEnabled"},
    {82, hid_true, "IsSixAxisSensorAtRest"},
    {101, hid_style_set, "GetSupportedNpadStyleSet"},
    {106, hid_style_event, "AcquireNpadStyleSetUpdateEventHandle"},
    {121, hid_u64_zero, "GetNpadJoyHoldType"},
    {129, hid_u64_zero, "GetNpadHandheldActivationMode"},
    {131, hid_false, "IsUnintendedHomeButtonInputProtectionEnabled"},
    {200, hid_vibration_info, "GetVibrationDeviceInfo"},
    {202, hid_actual_vibration, "GetActualVibrationValue"},
    {203, hid_create_vibration_list, "CreateActiveVibrationDeviceList"},
    {205, hid_true, "IsVibrationPermitted"},
    {211, hid_true, "IsVibrationDeviceMounted"},
    {400, hid_false, "IsUsbFullKeyControllerEnabled"},
    {502, hid_palma_event, "AcquirePalmaOperationCompleteEvent"},
    IPC_END}, NULL};

/* ---------------------------------------------------------------------------------------------- npads */
#define NPAD_BASE      0x9a00u
#define NPAD_SIZE      0x5000u
#define LIFO_FULLKEY   0x28u            /* then handheld, dual, left, right, palma, system ext: 0x350 each */
#define LIFO_SIZE      0x350u
#define LIFO_ENTRIES   17
#define NPAD_DEVTYPE   0x4188u
#define NPAD_SYSPROPS  0x4190u
enum { STYLE_FULLKEY = 1 };
enum { ATTR_CONNECTED = 1, ATTR_WIRED = 2 };

static struct { u64 buttons; s32 lx, ly, rx, ry; } input[8];
static u64 sampling;

void hid_set_input(int npad, u64 buttons, s32 lx, s32 ly, s32 rx, s32 ry) {
    if (npad < 0 || npad >= 8) return;
    input[npad].buttons = buttons;
    input[npad].lx = lx; input[npad].ly = ly; input[npad].rx = rx; input[npad].ry = ry;
}

/* a lifo: {s64 timestamp, s64 total (17), s64 tail, s64 count}, then 17 x {s64 sampling, state} */
static void lifo_push(u8 *lifo, const void *state, u32 state_size, u64 sampling_number) {
    u64 tail, count;
    memcpy(&tail, lifo + 0x10, 8);
    memcpy(&count, lifo + 0x18, 8);
    u64 next = (tail + 1) % LIFO_ENTRIES;
    u8 *e = lifo + 0x20 + next * (8 + state_size);
    memcpy(e + 8, state, state_size);
    memcpy(e, &sampling_number, 8);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    u64 total = LIFO_ENTRIES;
    if (count < LIFO_ENTRIES) count++;
    memcpy(lifo, &sampling_number, 8);
    memcpy(lifo + 8, &total, 8);
    memcpy(lifo + 0x18, &count, 8);
    __atomic_store_n((u64 *)(lifo + 0x10), next, __ATOMIC_RELEASE);
}

static void npad_update(void) {
    u8 *mem = shmem_ptr(hid_shmem);
    u8 *np = mem + NPAD_BASE;                                    /* npad 0: player 1 */
    u32 style = STYLE_FULLKEY, color_ok = 0, body = 0xff2d2d2d, buttons_color = 0xffe6e6e6, devtype = 1;
    memcpy(np, &style, 4);
    memcpy(np + 8, &color_ok, 4);                                /* full-key colours: attribute ok, body, buttons */
    memcpy(np + 0xc, &body, 4);
    memcpy(np + 0x10, &buttons_color, 4);
    memcpy(np + NPAD_DEVTYPE, &devtype, 4);
    u64 props = 1ull << 13 | 1ull << 14 | 1ull << 15;            /* plus, minus, directional buttons */
    memcpy(np + NPAD_SYSPROPS, &props, 8);
    /* NPadGenericState: s64 sampling, u64 buttons, s32 lx, ly, rx, ry, u32 attributes, u32 reserved */
    u8 st[0x28] = {0};
    u64 n = ++sampling;
    u32 attr = ATTR_CONNECTED | ATTR_WIRED;
    memcpy(st, &n, 8);
    memcpy(st + 8, &input[0].buttons, 8);
    memcpy(st + 0x10, &input[0].lx, 16);
    memcpy(st + 0x20, &attr, 4);
    lifo_push(np + LIFO_FULLKEY, st, sizeof st, n);
    lifo_push(np + LIFO_FULLKEY + 6 * LIFO_SIZE, st, sizeof st, n);   /* system ext */
}

static unsigned __stdcall input_thread(void *p) {
    for (;;) {
        npad_update();
        Sleep(16);
    }
    return 0;
}

/* the commands that only activate or configure: success without data, and without the stub's log */
static void quiet(SvcObj *o, IpcReq *r) {}
static IpcCmd hid_cmds[128];
static Iface hid_full;

static SvcObj *open_hid(const char *name) { return svcobj_new(&hid_full, NULL, name); }

void hid_register(void) {
    static const u32 quiet_ids[] = {1, 11, 21, 31, 51, 56, 60, 61, 63, 64, 66, 67, 69, 70, 72, 79, 81, 91, 92, 100, 102,
                                    103, 104, 107, 109, 120, 122, 123, 124, 125, 126, 127, 128, 130, 132, 134, 201,
                                    204, 206, 209, 210, 300, 301, 302, 1000, 1002, 1004, 3150};
    int n = 0;
    for (const IpcCmd *c = iface_hid.cmds; c->fn; c++) hid_cmds[n++] = *c;
    for (size_t i = 0; i < sizeof quiet_ids / sizeof *quiet_ids; i++)
        hid_cmds[n++] = (IpcCmd){quiet_ids[i], quiet, "(activate / configure)"};
    hid_cmds[n] = (IpcCmd)IPC_END;
    hid_full = iface_hid;
    hid_full.cmds = hid_cmds;
    hid_shmem = shmem_create(0x40000, "hid");
    style_event = event_create();
    event_signal(style_event);                                   /* npad 0 is connected from the start */
    xpad_event = event_create();
    palma_event = event_create();
    service_register("hid", open_hid);
    npad_update();
    _beginthreadex(NULL, 0, input_thread, NULL, 0, NULL);
}
