/* ldn:u: local wireless, unavailable (as in airplane mode). Initialize fails, so nn::ldn::Initialize returns that
 * error and the game reports it (main+925278) instead of opening a lobby. Command ids: IUserLocalCommunicationService
 * as in SDK 4.4 (nn::ldn::Initialize calls vtable slot 25 = command 400). */
#include "../ipc.h"

#define R_LDN_UNAVAILABLE (203 | (22 << 9))      /* ldn: local communication disabled (airplane mode) */

static KEvent *state_event;

static void lc_state(SvcObj *o, IpcReq *r) { out_u32(r, 0); }                /* State None */
static void lc_state_event(SvcObj *o, IpcReq *r) { out_event(r, state_event); }
static void lc_unavailable(SvcObj *o, IpcReq *r) { r->result = R_LDN_UNAVAILABLE; }

static const Iface iface_local = {"IUserLocalCommunicationService", (const IpcCmd[]){
    {0, lc_state, "GetState"},
    {100, lc_state_event, "AttachStateChangeEvent"},
    {102, lc_unavailable, "Scan"},
    {200, lc_unavailable, "OpenAccessPoint"},
    {300, lc_unavailable, "OpenStation"},
    {400, lc_unavailable, "Initialize"},
    {401, ipc_noop, "Finalize"},
    {402, lc_unavailable, "InitializeSystem2"},
    IPC_END}, NULL};

IPC_OPEN(ldn_create_user, &iface_local)

static const Iface iface_ldn = {"ldn:u", (const IpcCmd[]){
    {0, ldn_create_user, "CreateUserLocalCommunicationService"},
    IPC_END}, NULL};

static SvcObj *open_ldn(const char *name) { return svcobj_new(&iface_ldn, NULL, name); }

void ldn_register(void) {
    state_event = event_create();
    service_register("ldn:u", open_ldn);
}
