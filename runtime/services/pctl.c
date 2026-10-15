/* pctl: parental controls, never restricting. aoc:u: add-on content, none installed. */
#include "../ipc.h"

static KEvent *sync_event, *play_timer_event, *aoc_event;

static void pc_false(SvcObj *o, IpcReq *r) { out_u8(r, 0); }
static void pc_true(SvcObj *o, IpcReq *r) { out_u8(r, 1); }
static void pc_sync_event(SvcObj *o, IpcReq *r) { out_event(r, sync_event); }
static void pc_play_timer_event(SvcObj *o, IpcReq *r) { out_event(r, play_timer_event); }

static const Iface iface_pctl_service = {"IParentalControlService", (const IpcCmd[]){
    {1, ipc_noop, "Initialize"},
    {1001, ipc_noop, "CheckFreeCommunicationPermission"},
    {1002, ipc_noop, "ConfirmLaunchApplicationPermission"},
    {1003, ipc_noop, "ConfirmResumeApplicationPermission"},
    {1004, ipc_noop, "ConfirmSnsPostPermission"},
    {1006, pc_false, "IsRestrictionTemporaryUnlocked"},
    {1013, ipc_noop, "ConfirmStereoVisionPermission"},
    {1031, pc_false, "IsRestrictionEnabled"},
    {1061, ipc_noop, "ConfirmStereoVisionRestrictionConfigurable"},
    {1064, ipc_noop, "ResetConfirmedStereoVisionPermission"},
    {1065, pc_true, "IsStereoVisionPermitted"},
    {1403, pc_false, "IsPairingActive"},
    {1432, pc_sync_event, "GetSynchronizationEvent"},
    {1451, ipc_noop, "StartPlayTimer"},
    {1452, ipc_noop, "StopPlayTimer"},
    {1453, pc_false, "IsPlayTimerEnabled"},
    {1455, pc_false, "IsRestrictedByPlayTimer"},
    {1457, pc_play_timer_event, "GetPlayTimerEventToRequestSuspension"},
    {1458, pc_false, "IsPlayTimerAlarmDisabled"},
    IPC_END}, NULL};

IPC_OPEN(pctl_create_service, &iface_pctl_service)

static const Iface iface_pctl = {"pctl", (const IpcCmd[]){
    {0, pctl_create_service, "CreateService"},
    {1, pctl_create_service, "CreateServiceWithoutInitialize"},
    IPC_END}, NULL};

static SvcObj *open_pctl(const char *name) { return svcobj_new(&iface_pctl, NULL, name); }

/* ---------------------------------------------------------------------------------------------- aoc:u */
static void aoc_count(SvcObj *o, IpcReq *r) { out_u32(r, 0); }
static void aoc_base_id(SvcObj *o, IpcReq *r) { out_u64(r, 0x0100152000023000ull); }   /* program id + 0x1000 */
static void aoc_event_cmd(SvcObj *o, IpcReq *r) { out_event(r, aoc_event); }

static const Iface iface_aoc = {"aoc:u", (const IpcCmd[]){
    {0, aoc_count, "CountAddOnContentByApplicationId"},
    {1, aoc_count, "ListAddOnContentByApplicationId"},
    {2, aoc_count, "CountAddOnContent"},
    {3, aoc_count, "ListAddOnContent"},
    {4, aoc_base_id, "GetAddOnContentBaseIdByApplicationId"},
    {5, aoc_base_id, "GetAddOnContentBaseId"},
    {6, ipc_noop, "PrepareAddOnContentByApplicationId"},
    {7, ipc_noop, "PrepareAddOnContent"},
    {8, aoc_event_cmd, "GetAddOnContentListChangedEvent"},
    IPC_END}, NULL};

static SvcObj *open_aoc(const char *name) { return svcobj_new(&iface_aoc, NULL, name); }

void pctl_register(void) {
    sync_event = event_create();
    play_timer_event = event_create();
    aoc_event = event_create();
    service_register("pctl", open_pctl);
    service_register("pctl:s", open_pctl);
    service_register("pctl:r", open_pctl);
    service_register("pctl:a", open_pctl);
    service_register("aoc:u", open_aoc);
}
