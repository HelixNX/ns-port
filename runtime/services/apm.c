/* apm: performance modes. Always docked ("boost"), with the docked default configuration. */
#include "../ipc.h"

static u32 configuration = 0x00020003;

static void session_set(SvcObj *o, IpcReq *r) { configuration = in_u32(r, 4); }
static void session_get(SvcObj *o, IpcReq *r) { out_u32(r, configuration); }

static const Iface iface_session = {"apm:ISession", (const IpcCmd[]){
    {0, session_set, "SetPerformanceConfiguration"},
    {1, session_get, "GetPerformanceConfiguration"},
    {2, ipc_noop, "SetCpuOverclockEnabled"},
    IPC_END}, NULL};

IPC_OPEN(apm_open_session, &iface_session)
static void apm_performance_mode(SvcObj *o, IpcReq *r) { out_u32(r, 1); }
static void apm_false(SvcObj *o, IpcReq *r) { out_u8(r, 0); }

static const Iface iface_apm = {"apm", (const IpcCmd[]){
    {0, apm_open_session, "OpenSession"},
    {1, apm_performance_mode, "GetPerformanceMode"},
    {6, apm_false, "IsEnoughPowerSupplied"},
    IPC_END}, NULL};

static SvcObj *open_apm(const char *name) { return svcobj_new(&iface_apm, NULL, name); }

void apm_register(void) { service_register("apm", open_apm); }
