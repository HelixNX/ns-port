/* nifm: network interface manager, with no network. Every request the game submits is rejected at once (state Free,
 * a nifm error), so the SDK's SubmitAndWait returns, IsNetworkAvailable is false and online features stay off.
 * Command ids are from the SDK 4.4 proxies (IStaticService, IGeneralService, IRequest). */
#include "../ipc.h"
#include <stdlib.h>

#define R_NIFM_NO_NETWORK (110 | (1111 << 9))        /* nifm: network communication disabled */

enum { REQ_FREE = 1, REQ_ON_HOLD = 2, REQ_ACCEPTED = 3 };

typedef struct Request {
    KEvent *state_event, *other_event;              /* GetSystemEventReadableHandles: state changes, and a second */
    u32 state, result;
} Request;

static void req_destroy(SvcObj *o) {
    Request *q = o->data;
    kobj_unref(&q->state_event->obj);
    kobj_unref(&q->other_event->obj);
    free(q);
}
static void req_state(SvcObj *o, IpcReq *r) { out_u32(r, ((Request *)o->data)->state); }
static void req_result(SvcObj *o, IpcReq *r) { r->result = ((Request *)o->data)->result; }
static void req_events(SvcObj *o, IpcReq *r) {
    Request *q = o->data;
    out_event(r, q->state_event);
    out_event(r, q->other_event);
}
static void req_submit(SvcObj *o, IpcReq *r) {
    Request *q = o->data;
    q->state = REQ_FREE;
    q->result = R_NIFM_NO_NETWORK;
    event_signal(q->state_event);
}
static void req_cancel(SvcObj *o, IpcReq *r) {
    Request *q = o->data;
    q->state = REQ_FREE;
    event_signal(q->state_event);
}

static const Iface iface_request = {"IRequest", (const IpcCmd[]){
    {0, req_state, "GetRequestState"},
    {1, req_result, "GetResult"},
    {2, req_events, "GetSystemEventReadableHandles"},
    {3, req_cancel, "Cancel"},
    {4, req_submit, "Submit"},
    {5, ipc_noop, "SetRequirement"},
    {6, ipc_noop, "SetRequirementPreset"},
    {8, ipc_noop, "SetPriority"},
    {9, ipc_noop, "SetNetworkProfileId"},
    {10, ipc_noop, "SetRejectable"},
    {11, ipc_noop, "SetConnectionConfirmationOption"},
    {12, ipc_noop, "SetPersistent"},
    {13, ipc_noop, "SetInstant"},
    {14, ipc_noop, "SetSustainable"},
    {15, ipc_noop, "SetRawPriority"},
    {16, ipc_noop, "SetGreedy"},
    {17, ipc_noop, "SetSharable"},
    {18, ipc_noop, "SetRequirementByRevision"},
    {23, ipc_noop, "SetKeptInSleep"},
    {24, ipc_noop, "RegisterSocketDescriptor"},
    {25, ipc_noop, "UnregisterSocketDescriptor"},
    IPC_END}, req_destroy};

/* ---------------------------------------------------------------------------------------------- IGeneralService */
static void gs_client_id(SvcObj *o, IpcReq *r) {
    const IpcBuf *b = buf_out(r, 0);
    u32 id = 1;
    if (b && b->size >= 4) memcpy((void *)(uintptr_t)b->addr, &id, 4);
}
static void gs_create_request(SvcObj *o, IpcReq *r) {
    Request *q = calloc(1, sizeof *q);
    q->state_event = event_create();
    q->other_event = event_create();
    q->state = REQ_FREE;
    out_object(r, svcobj_new(&iface_request, q, NULL));
}
static void gs_no_network(SvcObj *o, IpcReq *r) { r->result = R_NIFM_NO_NETWORK; }
static void gs_false(SvcObj *o, IpcReq *r) { out_u8(r, 0); }

static const Iface iface_general = {"IGeneralService", (const IpcCmd[]){
    {1, gs_client_id, "GetClientId"},
    {4, gs_create_request, "CreateRequest"},
    {5, gs_no_network, "GetCurrentNetworkProfile"},
    {12, gs_no_network, "GetCurrentIpAddress"},
    {13, gs_no_network, "GetCurrentAccessPointOld"},
    {15, gs_no_network, "GetCurrentIpConfigInfo"},
    {17, gs_false, "IsWirelessCommunicationEnabled"},
    {18, gs_no_network, "GetInternetConnectionStatus"},
    {20, gs_false, "IsEthernetCommunicationEnabled"},
    {21, gs_false, "IsAnyInternetRequestAccepted"},
    {22, gs_false, "IsAnyForegroundRequestAccepted"},
    {36, gs_no_network, "GetCurrentAccessPoint"},
    IPC_END}, NULL};

IPC_OPEN(nifm_create_general, &iface_general)

static const Iface iface_nifm = {"nifm", (const IpcCmd[]){
    {4, nifm_create_general, "CreateGeneralServiceOld"},
    {5, nifm_create_general, "CreateGeneralService"},
    IPC_END}, NULL};

static SvcObj *open_nifm(const char *name) { return svcobj_new(&iface_nifm, NULL, name); }

void nifm_register(void) {
    service_register("nifm:u", open_nifm);
    service_register("nifm:s", open_nifm);
    service_register("nifm:a", open_nifm);
}
