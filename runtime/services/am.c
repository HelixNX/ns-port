/* am: the applet manager, as an application sees it (appletOE -> IApplicationProxy and its controllers).
 * The game runs docked (operation mode console, 1920x1080), always in focus. */
#include "../ipc.h"
#include <stdlib.h>
#include <windows.h>

#define R_AM_NO_MESSAGE 0x680u                  /* am 3 */
#define R_AM_NO_DATA    0x480u                  /* am 2: nothing in the channel */

enum { MSG_FOCUS_STATE_CHANGED = 15, MSG_OPERATION_MODE_CHANGED = 30, MSG_PERFORMANCE_MODE_CHANGED = 31 };

static SRWLOCK am_lock = SRWLOCK_INIT;
static u32 msgq[32];
static int nmsg;
static KEvent *msg_event, *launchable_event, *suspended_tick_event, *gpu_error_event, *resolution_event,
              *sleep_lock_event, *hdcp_event;

static void push_message(u32 m) {
    AcquireSRWLockExclusive(&am_lock);
    if (nmsg < 32) msgq[nmsg++] = m;
    ReleaseSRWLockExclusive(&am_lock);
    event_signal(msg_event);
}

/* ---------------------------------------------------------------------------------------------- ICommonStateGetter */
static void csg_get_event_handle(SvcObj *o, IpcReq *r) { out_event(r, msg_event); }

static void csg_receive_message(SvcObj *o, IpcReq *r) {
    AcquireSRWLockExclusive(&am_lock);
    if (!nmsg) {
        ReleaseSRWLockExclusive(&am_lock);
        r->result = R_AM_NO_MESSAGE;
        return;
    }
    u32 m = msgq[0];
    memmove(msgq, msgq + 1, --nmsg * sizeof *msgq);
    bool empty = nmsg == 0;
    ReleaseSRWLockExclusive(&am_lock);
    if (empty) event_clear(msg_event);
    hlog(1, "am: message %u", m);
    out_u32(r, m);
}

static void csg_operation_mode(SvcObj *o, IpcReq *r) { out_u8(r, 1); }          /* console (docked) */
static void csg_performance_mode(SvcObj *o, IpcReq *r) { out_u32(r, 1); }       /* boost (docked) */
static void csg_boot_mode(SvcObj *o, IpcReq *r) { out_u8(r, 0); }
static void csg_focus_state(SvcObj *o, IpcReq *r) { out_u8(r, 1); }             /* in focus */
static void csg_sleep_lock_event(SvcObj *o, IpcReq *r) { out_event(r, sleep_lock_event); }
static void csg_false(SvcObj *o, IpcReq *r) { out_u8(r, 0); }
static void csg_default_resolution(SvcObj *o, IpcReq *r) { out_u32(r, 1920); out_u32(r, 1080); }
static void csg_resolution_event(SvcObj *o, IpcReq *r) { out_event(r, resolution_event); }
static void csg_hdcp_state(SvcObj *o, IpcReq *r) { out_u32(r, 1); }
static void csg_hdcp_event(SvcObj *o, IpcReq *r) { out_event(r, hdcp_event); }

static const Iface iface_csg = {"ICommonStateGetter", (const IpcCmd[]){
    {0, csg_get_event_handle, "GetEventHandle"},
    {1, csg_receive_message, "ReceiveMessage"},
    {5, csg_operation_mode, "GetOperationMode"},
    {6, csg_performance_mode, "GetPerformanceMode"},
    {8, csg_boot_mode, "GetBootMode"},
    {9, csg_focus_state, "GetCurrentFocusState"},
    {10, ipc_noop, "RequestToAcquireSleepLock"},
    {11, ipc_noop, "ReleaseSleepLock"},
    {13, csg_sleep_lock_event, "GetAcquiredSleepLockEvent"},
    {50, csg_false, "IsVrModeEnabled"},
    {51, ipc_noop, "SetVrModeEnabled"},
    {52, ipc_noop, "SetLcdBacklighOffEnabled"},
    {55, csg_false, "IsInControllerFirmwareUpdateSection"},
    {60, csg_default_resolution, "GetDefaultDisplayResolution"},
    {61, csg_resolution_event, "GetDefaultDisplayResolutionChangeEvent"},
    {62, csg_hdcp_state, "GetHdcpAuthenticationState"},
    {63, csg_hdcp_event, "GetHdcpAuthenticationStateChangeEvent"},
    {66, ipc_noop, "SetCpuBoostMode"},
    IPC_END}, NULL};

/* ---------------------------------------------------------------------------------------------- ISelfController */
static void sc_launchable_event(SvcObj *o, IpcReq *r) { out_event(r, launchable_event); }
static void sc_create_layer(SvcObj *o, IpcReq *r) { out_u64(r, 1); }
static void sc_u32_zero(SvcObj *o, IpcReq *r) { out_u32(r, 0); }
static void sc_u64_zero(SvcObj *o, IpcReq *r) { out_u64(r, 0); }
static void sc_false(SvcObj *o, IpcReq *r) { out_u8(r, 0); }
static void sc_suspended_tick_event(SvcObj *o, IpcReq *r) { out_event(r, suspended_tick_event); }

static const Iface iface_sc = {"ISelfController", (const IpcCmd[]){
    {0, ipc_noop, "Exit"},
    {1, ipc_noop, "LockExit"},
    {2, ipc_noop, "UnlockExit"},
    {3, ipc_noop, "EnterFatalSection"},
    {4, ipc_noop, "LeaveFatalSection"},
    {9, sc_launchable_event, "GetLibraryAppletLaunchableEvent"},
    {10, ipc_noop, "SetScreenShotPermission"},
    {11, ipc_noop, "SetOperationModeChangedNotification"},
    {12, ipc_noop, "SetPerformanceModeChangedNotification"},
    {13, ipc_noop, "SetFocusHandlingMode"},
    {14, ipc_noop, "SetRestartMessageEnabled"},
    {15, ipc_noop, "SetScreenShotAppletIdentityInfo"},
    {16, ipc_noop, "SetOutOfFocusSuspendingEnabled"},
    {19, ipc_noop, "SetAlbumImageOrientation"},
    {40, sc_create_layer, "CreateManagedDisplayLayer"},
    {50, ipc_noop, "SetHandlesRequestToDisplay"},
    {51, ipc_noop, "ApproveToDisplay"},
    {60, ipc_noop, "OverrideAutoSleepTimeAndDimmingTime"},
    {61, ipc_noop, "SetMediaPlaybackState"},
    {62, ipc_noop, "SetIdleTimeDetectionExtension"},
    {63, sc_u32_zero, "GetIdleTimeDetectionExtension"},
    {65, ipc_noop, "ReportUserIsActive"},
    {67, sc_false, "IsIlluminanceAvailable"},
    {68, ipc_noop, "SetAutoSleepDisabled"},
    {69, sc_false, "IsAutoSleepDisabled"},
    {90, sc_u64_zero, "GetAccumulatedSuspendedTickValue"},
    {91, sc_suspended_tick_event, "GetAccumulatedSuspendedTickChangedEvent"},
    {100, ipc_noop, "SetAlbumImageTakenNotificationEnabled"},
    IPC_END}, NULL};

/* ---------------------------------------------------------------------------------------------- the others */
#define ARUID 0x51ull                           /* the applet resource user id, as hid and others will see it */
static void wc_aruid(SvcObj *o, IpcReq *r) { out_u64(r, ARUID); }
static const Iface iface_wc = {"IWindowController", (const IpcCmd[]){
    {1, wc_aruid, "GetAppletResourceUserId"},
    {10, ipc_noop, "AcquireForegroundRights"},
    {11, ipc_noop, "ReleaseForegroundRights"},
    {12, ipc_noop, "RejectToChangeIntoBackground"},
    IPC_END}, NULL};

static void ac_volume(SvcObj *o, IpcReq *r) { float v = 1.0f; out_data(r, &v, 4); }
static const Iface iface_ac = {"IAudioController", (const IpcCmd[]){
    {0, ipc_noop, "SetExpectedMasterVolume"},
    {1, ac_volume, "GetMainAppletExpectedMasterVolume"},
    {2, ac_volume, "GetLibraryAppletExpectedMasterVolume"},
    {3, ipc_noop, "ChangeMainAppletMasterVolume"},
    {4, ipc_noop, "SetTransparentVolumeRate"},
    IPC_END}, NULL};

static const Iface iface_dc = {"IDisplayController", (const IpcCmd[]){IPC_END}, NULL};
static const Iface iface_pwc = {"IProcessWindingController", (const IpcCmd[]){IPC_END}, NULL};
static const Iface iface_lac = {"ILibraryAppletCreator", (const IpcCmd[]){IPC_END}, NULL};
static const Iface iface_df = {"IDebugFunctions", (const IpcCmd[]){IPC_END}, NULL};

/* ---------------------------------------------------------------------------------------------- IStorage */
typedef struct Storage { u8 *data; u64 size; } Storage;

static void storage_destroy(SvcObj *o) { Storage *s = o->data; free(s->data); free(s); }
static void sa_size(SvcObj *o, IpcReq *r) { out_u64(r, ((Storage *)o->data)->size); }
static void sa_read(SvcObj *o, IpcReq *r) {
    Storage *s = o->data;
    u64 off = in_u64(r, 0);
    const IpcBuf *b = buf_out(r, 0);
    if (!b || off > s->size) { r->result = 0x1f680; return; }     /* am: out of bounds */
    u64 n = b->size < s->size - off ? b->size : s->size - off;
    memcpy((void *)(uintptr_t)b->addr, s->data + off, n);
}
static void sa_write(SvcObj *o, IpcReq *r) {
    Storage *s = o->data;
    u64 off = in_u64(r, 0);
    const IpcBuf *b = buf_in(r, 0);
    if (!b || off > s->size) { r->result = 0x1f680; return; }
    u64 n = b->size < s->size - off ? b->size : s->size - off;
    memcpy(s->data + off, (const void *)(uintptr_t)b->addr, n);
}
static const Iface iface_storage_accessor = {"IStorageAccessor", (const IpcCmd[]){
    {0, sa_size, "GetSize"},
    {10, sa_write, "Write"},
    {11, sa_read, "Read"},
    IPC_END}, storage_destroy};

static void st_open(SvcObj *o, IpcReq *r) {
    Storage *s = o->data;                               /* the accessor works on its own copy */
    Storage *view = calloc(1, sizeof *view);
    view->size = s->size;
    view->data = malloc(s->size);
    memcpy(view->data, s->data, s->size);
    out_object(r, svcobj_new(&iface_storage_accessor, view, NULL));
}
static const Iface iface_storage = {"IStorage", (const IpcCmd[]){
    {0, st_open, "Open"},
    IPC_END}, storage_destroy};

static SvcObj *storage_new(const void *data, u64 size) {
    Storage *s = calloc(1, sizeof *s);
    s->data = malloc(size);
    memcpy(s->data, data, size);
    s->size = size;
    return svcobj_new(&iface_storage, s, NULL);
}

/* ---------------------------------------------------------------------------------------------- IApplicationFunctions */
extern const u64 acc_user_id[2];
static bool preselected_popped;

/* kind 1: user channel (nothing); kind 2: the preselected user {magic, selected, pad, uid, pad} */
static void af_pop_launch_parameter(SvcObj *o, IpcReq *r) {
    u32 kind = in_u32(r, 0);
    if (kind == 2 && !preselected_popped) {
        u8 p[0x88] = {0};
        u32 magic = 0xc79497ca;
        memcpy(p, &magic, 4);
        p[4] = 1;
        memcpy(p + 8, acc_user_id, 16);
        preselected_popped = true;
        hlog(1, "am: PopLaunchParameter(PreselectedUser)");
        out_object(r, storage_new(p, sizeof p));
        return;
    }
    hlog(1, "am: PopLaunchParameter(%u): none", kind);
    r->result = R_AM_NO_DATA;
}
static void af_ensure_save_data(SvcObj *o, IpcReq *r) { out_u64(r, 0); }
static void af_desired_language(SvcObj *o, IpcReq *r) { out_u64(r, 0x00000053552d6e65ull); }   /* "en-US" */
static void af_display_version(SvcObj *o, IpcReq *r) { char v[16] = "3.0.5"; out_data(r, v, 16); }
static void af_notify_running(SvcObj *o, IpcReq *r) { out_u8(r, 0); }
static void af_pseudo_device_id(SvcObj *o, IpcReq *r) { out_u64(r, 0x0123456789abcdefull); out_u64(r, 0xfedcba9876543210ull); }
static void af_gpu_error_event(SvcObj *o, IpcReq *r) { out_event(r, gpu_error_event); }
static void af_save_data_size(SvcObj *o, IpcReq *r) { out_u64(r, 0x4000000); out_u64(r, 0x4000000); }

static const Iface iface_af = {"IApplicationFunctions", (const IpcCmd[]){
    {1, af_pop_launch_parameter, "PopLaunchParameter"},
    {20, af_ensure_save_data, "EnsureSaveData"},
    {21, af_desired_language, "GetDesiredLanguage"},
    {22, ipc_noop, "SetTerminateResult"},
    {23, af_display_version, "GetDisplayVersion"},
    {25, af_ensure_save_data, "ExtendSaveData"},
    {26, af_save_data_size, "GetSaveDataSize"},
    {30, ipc_noop, "BeginBlockingHomeButtonShortAndLongPressed"},
    {31, ipc_noop, "EndBlockingHomeButtonShortAndLongPressed"},
    {32, ipc_noop, "BeginBlockingHomeButton"},
    {33, ipc_noop, "EndBlockingHomeButton"},
    {40, af_notify_running, "NotifyRunning"},
    {50, af_pseudo_device_id, "GetPseudoDeviceId"},
    {60, ipc_noop, "SetMediaPlaybackStateForApplication"},
    {66, ipc_noop, "InitializeGamePlayRecording"},
    {67, ipc_noop, "SetGamePlayRecordingState"},
    {90, ipc_noop, "EnableApplicationCrashReport"},
    {100, ipc_noop, "InitializeApplicationCopyrightFrameBuffer"},
    {101, ipc_noop, "SetApplicationCopyrightImage"},
    {102, ipc_noop, "SetApplicationCopyrightVisibility"},
    {130, af_gpu_error_event, "GetGpuErrorDetectedSystemEvent"},
    IPC_END}, NULL};

/* ---------------------------------------------------------------------------------------------- the proxy */
IPC_OPEN(ap_csg, &iface_csg)
IPC_OPEN(ap_sc, &iface_sc)
IPC_OPEN(ap_wc, &iface_wc)
IPC_OPEN(ap_ac, &iface_ac)
IPC_OPEN(ap_dc, &iface_dc)
IPC_OPEN(ap_pwc, &iface_pwc)
IPC_OPEN(ap_lac, &iface_lac)
IPC_OPEN(ap_af, &iface_af)
IPC_OPEN(ap_df, &iface_df)

static const Iface iface_proxy = {"IApplicationProxy", (const IpcCmd[]){
    {0, ap_csg, "GetCommonStateGetter"},
    {1, ap_sc, "GetSelfController"},
    {2, ap_wc, "GetWindowController"},
    {3, ap_ac, "GetAudioController"},
    {4, ap_dc, "GetDisplayController"},
    {10, ap_pwc, "GetProcessWindingController"},
    {11, ap_lac, "GetLibraryAppletCreator"},
    {20, ap_af, "GetApplicationFunctions"},
    {1000, ap_df, "GetDebugFunctions"},
    IPC_END}, NULL};

static void oe_open_proxy(SvcObj *o, IpcReq *r) {
    out_object(r, svcobj_new(&iface_proxy, NULL, NULL));
}

static const Iface iface_oe = {"appletOE", (const IpcCmd[]){
    {0, oe_open_proxy, "OpenApplicationProxy"},
    IPC_END}, NULL};

static SvcObj *open_oe(const char *name) { return svcobj_new(&iface_oe, NULL, name); }

void am_register(void) {
    msg_event = event_create();
    launchable_event = event_create();
    suspended_tick_event = event_create();
    gpu_error_event = event_create();
    resolution_event = event_create();
    sleep_lock_event = event_create();
    hdcp_event = event_create();
    event_signal(launchable_event);
    push_message(MSG_FOCUS_STATE_CHANGED);
    service_register("appletOE", open_oe);
}
