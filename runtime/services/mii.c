/* mii: the console's Mii database, empty here (the game falls back to its own characters). BuildDefault and
 * BuildRandom need the Mii part tables; they are not written yet and say so. */
#include "../ipc.h"

#define R_MII_NOT_FOUND 0x27eu                           /* mii 1 */

static void db_false(SvcObj *o, IpcReq *r) { out_u8(r, 0); }
static void db_count(SvcObj *o, IpcReq *r) { out_u32(r, 0); }
static void db_not_found(SvcObj *o, IpcReq *r) { r->result = R_MII_NOT_FOUND; }
static void db_find_index(SvcObj *o, IpcReq *r) { out_u32(r, 0xffffffffu); }
static void db_build(SvcObj *o, IpcReq *r) {
    hlog(0, "mii: %s is not written yet (zeroed CharInfo)", r->cmd == 6 ? "BuildRandom" : "BuildDefault");
    u8 info[0x58] = {0};
    out_data(r, info, sizeof info);
}

static const Iface iface_db = {"IDatabaseService", (const IpcCmd[]){
    {0, db_false, "IsUpdated"},
    {1, db_false, "IsFullDatabase"},
    {2, db_count, "GetCount"},
    {3, db_count, "Get"},
    {4, db_count, "Get1"},
    {5, db_not_found, "UpdateLatest"},
    {6, db_build, "BuildRandom"},
    {7, db_build, "BuildDefault"},
    {8, db_count, "Get2"},
    {9, db_count, "Get3"},
    {10, db_not_found, "UpdateLatest1"},
    {11, db_find_index, "FindIndex"},
    {20, db_false, "IsBrokenDatabaseWithClearFlag"},
    {21, db_not_found, "GetIndex"},
    {22, ipc_noop, "SetInterfaceVersion"},
    IPC_END}, NULL};

IPC_OPEN(mii_get_db, &iface_db)

static const Iface iface_mii = {"mii", (const IpcCmd[]){
    {0, mii_get_db, "GetDatabaseService"},
    IPC_END}, NULL};

static SvcObj *open_mii(const char *name) { return svcobj_new(&iface_mii, NULL, name); }

void mii_register(void) {
    service_register("mii:e", open_mii);
    service_register("mii:u", open_mii);
}
