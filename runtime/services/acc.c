/* acc: one local user, preselected at launch (am passes it as the PreselectedUser launch parameter). No network
 * service account: the online parts report it unavailable. */
#include "../ipc.h"
#include <time.h>

const u64 acc_user_id[2] = {0x6d6b38726563ull, 0x0000000000000001ull};   /* u128, low then high */
static const char nickname[] = "Player";

#define R_ACC_NSA_UNAVAILABLE 0x24a7cu                 /* account 0x5213... : network service account unavailable */

static bool is_user(IpcReq *r, u32 off) { return in_u64(r, off) == acc_user_id[0] && in_u64(r, off + 8) == acc_user_id[1]; }

/* ProfileBase: uid, u64 last edit time, char nickname[0x20] */
static void out_profile_base(IpcReq *r) {
    u8 b[0x38] = {0};
    memcpy(b, acc_user_id, 16);
    u64 t = 1500000000;
    memcpy(b + 0x10, &t, 8);
    memcpy(b + 0x18, nickname, sizeof nickname);
    out_data(r, b, 0x38);
}

/* ---------------------------------------------------------------------------------------------- IProfile */
static void pr_get(SvcObj *o, IpcReq *r) {
    const IpcBuf *b = buf_out(r, 0);
    if (b && b->size) memset((void *)(uintptr_t)b->addr, 0, b->size);   /* UserData */
    out_profile_base(r);
}
static void pr_get_base(SvcObj *o, IpcReq *r) { out_profile_base(r); }
static void pr_image_size(SvcObj *o, IpcReq *r) { out_u32(r, 0); }
static void pr_load_image(SvcObj *o, IpcReq *r) { out_u32(r, 0); }
static const Iface iface_profile = {"IProfile", (const IpcCmd[]){
    {0, pr_get, "Get"},
    {1, pr_get_base, "GetBase"},
    {10, pr_image_size, "GetImageSize"},
    {11, pr_load_image, "LoadImage"},
    IPC_END}, NULL};

/* ---------------------------------------------------------------------------------------------- IManagerForApplication */
static void mg_unavailable(SvcObj *o, IpcReq *r) { r->result = R_ACC_NSA_UNAVAILABLE; }
static void mg_account_id(SvcObj *o, IpcReq *r) { out_u64(r, acc_user_id[0]); }
static const Iface iface_manager = {"IManagerForApplication", (const IpcCmd[]){
    {0, mg_unavailable, "CheckAvailability"},
    {1, mg_account_id, "GetAccountId"},
    {2, mg_unavailable, "EnsureIdTokenCacheAsync"},
    {3, mg_unavailable, "LoadIdTokenCache"},
    IPC_END}, NULL};

/* ---------------------------------------------------------------------------------------------- IAccountServiceForApplication */
static void acc_user_count(SvcObj *o, IpcReq *r) { out_u32(r, 1); }
static void acc_user_exists(SvcObj *o, IpcReq *r) { out_u8(r, is_user(r, 0)); }
static void acc_list_users(SvcObj *o, IpcReq *r) {
    const IpcBuf *b = buf_out(r, 0);
    if (!b || b->size < 16) return;
    memset((void *)(uintptr_t)b->addr, 0, b->size);
    memcpy((void *)(uintptr_t)b->addr, acc_user_id, 16);
}
static void acc_last_user(SvcObj *o, IpcReq *r) { out_data(r, acc_user_id, 16); }
static void acc_profile(SvcObj *o, IpcReq *r) {
    if (!is_user(r, 0)) { r->result = 0x13a7c; return; }           /* account: user not found */
    out_object(r, svcobj_new(&iface_profile, NULL, NULL));
}
static void acc_false(SvcObj *o, IpcReq *r) { out_u8(r, 0); }
static void acc_select(SvcObj *o, IpcReq *r) { out_data(r, acc_user_id, 16); }
static void acc_manager(SvcObj *o, IpcReq *r) { out_object(r, svcobj_new(&iface_manager, NULL, NULL)); }

static const Iface iface_acc = {"acc:u0", (const IpcCmd[]){
    {0, acc_user_count, "GetUserCount"},
    {1, acc_user_exists, "GetUserExistence"},
    {2, acc_list_users, "ListAllUsers"},
    {3, acc_list_users, "ListOpenUsers"},
    {4, acc_last_user, "GetLastOpenedUser"},
    {5, acc_profile, "GetProfile"},
    {50, acc_false, "IsUserRegistrationRequestPermitted"},
    {51, acc_select, "TrySelectUserWithoutInteraction"},
    {60, acc_list_users, "ListOpenContextStoredUsers"},
    {100, ipc_noop, "InitializeApplicationInfo"},
    {101, acc_manager, "GetBaasAccountManagerForApplication"},
    {110, ipc_noop, "StoreSaveDataThumbnail"},
    {111, ipc_noop, "ClearSaveDataThumbnail"},
    IPC_END}, NULL};

static SvcObj *open_acc(const char *name) { return svcobj_new(&iface_acc, NULL, name); }

void acc_register(void) {
    service_register("acc:u0", open_acc);
    service_register("acc:u1", open_acc);
}
