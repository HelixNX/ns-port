/* Services whose offline answers must be specific (the generic stub's zeros would mean success): sockets fail as
 * on a console without a network, and the profanity filter passes text through. Other offline services (caps,
 * prepo, nfc/nfp, notif, nim, nsd, ...) use the generic stub. Command ids are from the SDK proxies. */
#include "../ipc.h"

/* ---------------------------------------------------------------------------------------------- bsd */
#define BSD_ENETDOWN 50                         /* the FreeBSD errno numbers the socket library uses */

static void bsd_register_client(SvcObj *o, IpcReq *r) { (void)o; out_u32(r, 0); }
/* every socket call: ret -1, errno ENETDOWN */
static void bsd_fail(SvcObj *o, IpcReq *r) { (void)o; out_u32(r, (u32)-1); out_u32(r, BSD_ENETDOWN); }
static void bsd_ok(SvcObj *o, IpcReq *r) { (void)o; out_u32(r, 0); out_u32(r, 0); }

static const Iface iface_bsd = {"bsd", (const IpcCmd[]){
    {0, bsd_register_client, "RegisterClient"},
    {1, ipc_noop, "StartMonitoring"},
    {2, bsd_fail, "Socket"}, {3, bsd_fail, "SocketExempt"}, {4, bsd_fail, "Open"}, {5, bsd_fail, "Select"},
    {6, bsd_fail, "Poll"}, {7, bsd_fail, "Sysctl"}, {8, bsd_fail, "Recv"}, {9, bsd_fail, "RecvFrom"},
    {10, bsd_fail, "Send"}, {11, bsd_fail, "SendTo"}, {12, bsd_fail, "Accept"}, {13, bsd_fail, "Bind"},
    {14, bsd_fail, "Connect"}, {15, bsd_fail, "GetPeerName"}, {16, bsd_fail, "GetSockName"},
    {17, bsd_fail, "GetSockOpt"}, {18, bsd_fail, "Listen"}, {19, bsd_fail, "Ioctl"}, {20, bsd_fail, "Fcntl"},
    {21, bsd_fail, "SetSockOpt"}, {22, bsd_fail, "Shutdown"}, {23, bsd_ok, "ShutdownAllSockets"},
    {24, bsd_fail, "Write"}, {25, bsd_fail, "Read"}, {26, bsd_fail, "Close"}, {27, bsd_fail, "DuplicateSocket"},
    {29, bsd_fail, "RecvMMsg"}, {30, bsd_fail, "SendMMsg"}, {31, bsd_fail, "EventFd"},
    {32, ipc_noop, "RegisterResourceStatisticsName"}, {33, bsd_register_client, "Initialize2"},
    IPC_END}, NULL};
static SvcObj *open_bsd(const char *name) { return svcobj_new(&iface_bsd, NULL, name); }

/* ---------------------------------------------------------------------------------------------- ngct */
static void ngct_match(SvcObj *o, IpcReq *r) { (void)o; out_u8(r, 0); }          /* no NG word found */
static void ngct_filter(SvcObj *o, IpcReq *r) {                                   /* the text, unchanged */
    (void)o;
    const IpcBuf *in = buf_in(r, 0), *out = buf_out(r, 0);
    if (in && out && in->size && out->size) {
        u64 n = in->size < out->size ? in->size : out->size;
        memmove((void *)(uintptr_t)out->addr, (const void *)(uintptr_t)in->addr, (size_t)n);
    }
}
static const Iface iface_ngct = {"ngct", (const IpcCmd[]){
    {0, ngct_match, "Match"},
    {1, ngct_filter, "Filter"},
    IPC_END}, NULL};
static SvcObj *open_ngct(const char *name) { return svcobj_new(&iface_ngct, NULL, name); }

void offline_register(void) {
    service_register("bsd:u", open_bsd);
    service_register("bsd:s", open_bsd);
    service_register("ngct:u", open_ngct);
}
