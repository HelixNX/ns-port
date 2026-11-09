/* bcat: data delivery (news, distributions), offline: the services open, the delivery cache is empty and nothing
 * syncs. Command ids are from the SDK proxies (IServiceCreator, IBcatService, IDeliveryCacheStorageService,
 * IDeliveryCacheFileService, IDeliveryCacheDirectoryService). */
#include "../ipc.h"

#define BCAT_NOT_FOUND ((2u << 9) | 122u)

static void not_found(SvcObj *o, IpcReq *r) { (void)o; r->result = BCAT_NOT_FOUND; }
static void zero_u32(SvcObj *o, IpcReq *r) { (void)o; out_u32(r, 0); }
static void zero_u64(SvcObj *o, IpcReq *r) { (void)o; out_u64(r, 0); }

static const Iface iface_file = {"IDeliveryCacheFileService", (const IpcCmd[]){
    {0, not_found, "Open"},
    {1, zero_u64, "Read"},
    {2, zero_u64, "GetSize"},
    {3, not_found, "GetDigest"},
    IPC_END}, NULL};

static const Iface iface_dir = {"IDeliveryCacheDirectoryService", (const IpcCmd[]){
    {0, not_found, "Open"},
    {1, zero_u32, "Read"},
    {2, zero_u32, "GetCount"},
    IPC_END}, NULL};

IPC_OPEN(open_file_service, &iface_file)
IPC_OPEN(open_dir_service, &iface_dir)
static const Iface iface_storage = {"IDeliveryCacheStorageService", (const IpcCmd[]){
    {0, open_file_service, "CreateFileService"},
    {1, open_dir_service, "CreateDirectoryService"},
    {10, zero_u32, "EnumerateDeliveryCacheDirectory"},
    IPC_END}, NULL};

/* syncing needs the network, which this console does not have (nifm) */
static const Iface iface_bcat = {"IBcatService", (const IpcCmd[]){
    {10100, not_found, "RequestSyncDeliveryCache"},
    {10101, not_found, "RequestSyncDeliveryCacheWithDirectoryName"},
    {10200, ipc_noop, "CancelSyncDeliveryCacheRequest"},
    IPC_END}, NULL};

IPC_OPEN(create_bcat, &iface_bcat)
IPC_OPEN(create_storage, &iface_storage)
static const Iface iface_creator = {"bcat", (const IpcCmd[]){
    {0, create_bcat, "CreateBcatService"},
    {1, create_storage, "CreateDeliveryCacheStorageService"},
    {2, create_storage, "CreateDeliveryCacheStorageServiceWithApplicationId"},
    IPC_END}, NULL};

static SvcObj *open_bcat(const char *name) { return svcobj_new(&iface_creator, NULL, name); }

void bcat_register(void) {
    service_register("bcat:u", open_bcat);
    service_register("bcat:a", open_bcat);
    service_register("bcat:m", open_bcat);
    service_register("bcat:s", open_bcat);
}
