/* vi: displays and layers. Rendering is intercepted at NVN, so a layer is only an id and a parcel naming a native
 * window that nothing reads; the binder relay is never used for frames. The display's vsync event is signalled
 * at 60 Hz by a host thread. */
#include "../ipc.h"
#include <windows.h>
#include <process.h>

static KEvent *vsync_event, *binder_event;
static u64 next_layer = 1;

static unsigned __stdcall vsync_thread(void *p) {
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -166667;
    SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (u64 frame = 1;; frame++) {
        WaitForSingleObject(timer, INFINITE);
        event_signal(vsync_event);
        /* next tick on the 60 Hz grid, without drifting */
        QueryPerformanceCounter(&now);
        s64 next = (s64)((frame + 1) * (u64)freq.QuadPart / 60) - (now.QuadPart - start.QuadPart);
        due.QuadPart = next > 0 ? -(next * 10000000 / freq.QuadPart) : -1;
        SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
    }
    return 0;
}

/* the parcel OpenLayer returns: one flattened binder object (magic 2, process 1, u64 binder id, "dispdrv"), and
 * one entry in the object list */
static u64 write_window_parcel(IpcReq *r, u64 layer) {
    if (!r->nb || r->b[0].size < 0x3c) return 0;
    u8 *p = (u8 *)(uintptr_t)r->b[0].addr;
    u32 hdr[4] = {0x28, 0x10, 4, 0x38};
    u8 win[0x28] = {0};
    u32 magic = 2, pid = 1;
    memcpy(win, &magic, 4);
    memcpy(win + 4, &pid, 4);
    memcpy(win + 8, &layer, 8);
    memcpy(win + 0x18, "dispdrv", 8);
    memset(p, 0, r->b[0].size);
    memcpy(p, hdr, 16);
    memcpy(p + 0x10, win, 0x28);
    return 0x3c;
}

/* ---------------------------------------------------------------------------------------------- relay */
/* The layer's IGraphicBufferProducer, as far as nn::vi::CreateLayer uses it (connect, query, settings): frames
 * never come this way. Request parcels start with the interface token (u32 policy, String16); replies are
 * the results followed by an s32 status. */
enum { GBP_QUERY = 9, GBP_CONNECT = 10 };

static void binder_transact(SvcObj *o, IpcReq *r) {
    u32 code = in_u32(r, 4);
    /* TransactParcelAuto sends A+X and B+C pairs, one of each empty */
    const IpcBuf *in = r->na && r->a[0].size ? &r->a[0] : r->nx ? &r->x[0] : NULL;
    const IpcBuf *outb = r->nb && r->b[0].size ? &r->b[0] : r->nc ? &r->c[0] : NULL;
    hlog(2, "vi: binder %u, in %llx+%llx out %llx+%llx", in_u32(r, 4), in ? in->addr : 0, in ? in->size : 0,
         outb ? outb->addr : 0, outb ? outb->size : 0);
    const u8 *ip = in ? (const u8 *)(uintptr_t)in->addr : NULL;
    s32 args[4] = {0};
    if (ip && in->size >= 0x10) {
        u32 doff, dsize;
        memcpy(&dsize, ip, 4);
        memcpy(&doff, ip + 4, 4);
        const u8 *d = ip + doff;
        u32 len;
        memcpy(&len, d + 4, 4);                              /* token length in UTF-16 units */
        u32 pos = 8 + ((len + 1) * 2 + 3) / 4 * 4;
        for (int i = 0; i < 4 && pos + 4 * i + 4 <= dsize; i++) memcpy(&args[i], d + pos + 4 * i, 4);
    }
    u32 reply[16] = {0};
    int n = 0;
    switch (code) {
    case GBP_CONNECT:                                        /* QueueBufferOutput: width, height, transform, pending */
        reply[n++] = 1920; reply[n++] = 1080; reply[n++] = 0; reply[n++] = 0;
        break;
    case GBP_QUERY: {
        s32 what = args[0];
        u32 v = what == 0 ? 1920 : what == 1 ? 1080 : what == 2 ? 1 : what == 3 ? 2 : 0;
        reply[n++] = v;
        break;
    }
    default:
        hlog(1, "vi: binder transaction %u (args %d %d)", code, args[0], args[1]);
        break;
    }
    reply[n++] = 0;                                          /* status: OK */
    if (outb && outb->size >= 0x10 + 4u * n) {
        u8 *op = (u8 *)(uintptr_t)outb->addr;
        u32 hdr[4] = {4u * n, 0x10, 0, 0x10 + 4u * n};
        memcpy(op, hdr, 16);
        memcpy(op + 16, reply, 4u * n);
    }
}
static void binder_native_handle(SvcObj *o, IpcReq *r) { out_event(r, binder_event); }

static const Iface iface_binder = {"IHOSBinderDriver", (const IpcCmd[]){
    {0, binder_transact, "TransactParcel"},
    {1, ipc_noop, "AdjustRefcount"},
    {2, binder_native_handle, "GetNativeHandle"},
    {3, binder_transact, "TransactParcelAuto"},
    IPC_END}, NULL};

static const Iface iface_system = {"ISystemDisplayService", (const IpcCmd[]){
    {2205, ipc_noop, "SetLayerZ"},
    {2207, ipc_noop, "SetLayerVisibility"},
    IPC_END}, NULL};

static void mgr_create_layer(SvcObj *o, IpcReq *r) { out_u64(r, next_layer++); }
static const Iface iface_manager = {"IManagerDisplayService", (const IpcCmd[]){
    {2010, mgr_create_layer, "CreateManagedLayer"},
    {2011, ipc_noop, "DestroyManagedLayer"},
    {6000, ipc_noop, "AddToLayerStack"},
    {6002, ipc_noop, "SetLayerVisibility"},
    IPC_END}, NULL};

/* ---------------------------------------------------------------------------------------------- IApplicationDisplayService */
IPC_OPEN(ads_relay, &iface_binder)
IPC_OPEN(ads_system, &iface_system)
IPC_OPEN(ads_manager, &iface_manager)

static void ads_list_displays(SvcObj *o, IpcReq *r) {
    if (r->nb && r->b[0].size >= 0x60) {
        u8 *e = (u8 *)(uintptr_t)r->b[0].addr;
        memset(e, 0, 0x60);
        strcpy((char *)e, "Default");
        u64 layers = 1, w = 1920, h = 1080;
        e[0x40] = 1;                                     /* has layer limit */
        memcpy(e + 0x48, &layers, 8);
        memcpy(e + 0x50, &w, 8);
        memcpy(e + 0x58, &h, 8);
        out_u64(r, 1);
    } else {
        out_u64(r, 0);
    }
}
static void ads_open_display(SvcObj *o, IpcReq *r) { out_u64(r, 0); }
static void ads_resolution(SvcObj *o, IpcReq *r) { out_u64(r, 1920); out_u64(r, 1080); }

static void ads_open_layer(SvcObj *o, IpcReq *r) {
    u64 layer = in_u64(r, 0x40);
    hlog(1, "vi: OpenLayer(%llu)", layer);
    out_u64(r, write_window_parcel(r, layer));
}

static void ads_create_stray_layer(SvcObj *o, IpcReq *r) {
    u64 layer = next_layer++;
    out_u64(r, layer);
    out_u64(r, write_window_parcel(r, layer));
}

static void ads_convert_scaling(SvcObj *o, IpcReq *r) { out_u64(r, in_u32(r, 0)); }
static void ads_vsync_event(SvcObj *o, IpcReq *r) { out_event(r, vsync_event); }

static const Iface iface_ads = {"IApplicationDisplayService", (const IpcCmd[]){
    {100, ads_relay, "GetRelayService"},
    {101, ads_system, "GetSystemDisplayService"},
    {102, ads_manager, "GetManagerDisplayService"},
    {103, ads_relay, "GetIndirectDisplayTransactionService"},
    {1000, ads_list_displays, "ListDisplays"},
    {1010, ads_open_display, "OpenDisplay"},
    {1011, ads_open_display, "OpenDefaultDisplay"},
    {1020, ipc_noop, "CloseDisplay"},
    {1101, ipc_noop, "SetDisplayEnabled"},
    {1102, ads_resolution, "GetDisplayResolution"},
    {2020, ads_open_layer, "OpenLayer"},
    {2021, ipc_noop, "CloseLayer"},
    {2030, ads_create_stray_layer, "CreateStrayLayer"},
    {2031, ipc_noop, "DestroyStrayLayer"},
    {2101, ipc_noop, "SetLayerScalingMode"},
    {2102, ads_convert_scaling, "ConvertScalingMode"},
    {5202, ads_vsync_event, "GetDisplayVsyncEvent"},
    {5203, ads_vsync_event, "GetDisplayVsyncEventForDebug"},
    IPC_END}, NULL};

IPC_OPEN(vi_get_display_service, &iface_ads)

static const Iface iface_vi = {"vi", (const IpcCmd[]){
    {0, vi_get_display_service, "GetDisplayService"},
    {1, vi_get_display_service, "GetDisplayService"},
    {2, vi_get_display_service, "GetDisplayService"},
    {3, vi_get_display_service, "GetDisplayServiceWithProxyNameExchange"},
    IPC_END}, NULL};

static SvcObj *open_vi(const char *name) { return svcobj_new(&iface_vi, NULL, name); }

void vi_register(void) {
    vsync_event = event_create();
    binder_event = event_create();
    service_register("vi:u", open_vi);
    service_register("vi:s", open_vi);
    service_register("vi:m", open_vi);
    _beginthreadex(NULL, 0, vsync_thread, NULL, 0, NULL);
}
