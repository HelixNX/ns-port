/* mii: the console's Mii database, empty here, plus the six default Miis that Get returns for SourceFlag Default
 * (the game takes its player Mii from them). BuildRandom and BuildDefault make Miis that pass the SDK's CharInfo check (nn::mii::detail::CharInfoRaw::Verify): a create id with
 * the RFC 4122 variant, a nickname, and every part within its range. The parts are our own picks: plain faces for
 * the defaults, random ones (seeded per call) for BuildRandom. */
#include "../ipc.h"
#include <windows.h>
#include <bcrypt.h>

#define R_MII_NOT_FOUND 0x27eu                           /* mii 1 */

static void db_false(SvcObj *o, IpcReq *r) { out_u8(r, 0); }
static void db_not_found(SvcObj *o, IpcReq *r) { r->result = R_MII_NOT_FOUND; }
static void db_find_index(SvcObj *o, IpcReq *r) { out_u32(r, 0xffffffffu); }
/* CharInfo (0x58 bytes): create id 0x00, nickname 0x10 (10 UTF-16 + terminator), then one byte per field */
enum {
    CI_FONT = 0x26, CI_FAV_COLOR, CI_GENDER, CI_HEIGHT, CI_BUILD, CI_TYPE, CI_REGION_MOVE, CI_FACE_TYPE, CI_FACE_COLOR,
    CI_FACE_WRINKLE, CI_FACE_MAKE, CI_HAIR_TYPE, CI_HAIR_COLOR, CI_HAIR_FLIP, CI_EYE_TYPE, CI_EYE_COLOR, CI_EYE_SCALE,
    CI_EYE_ASPECT, CI_EYE_ROTATE, CI_EYE_X, CI_EYE_Y, CI_BROW_TYPE, CI_BROW_COLOR, CI_BROW_SCALE, CI_BROW_ASPECT,
    CI_BROW_ROTATE, CI_BROW_X, CI_BROW_Y, CI_NOSE_TYPE, CI_NOSE_SCALE, CI_NOSE_Y, CI_MOUTH_TYPE, CI_MOUTH_COLOR,
    CI_MOUTH_SCALE, CI_MOUTH_ASPECT, CI_MOUTH_Y, CI_BEARD_COLOR, CI_BEARD_TYPE, CI_MUSTACHE_TYPE, CI_MUSTACHE_SCALE,
    CI_MUSTACHE_Y, CI_GLASS_TYPE, CI_GLASS_COLOR, CI_GLASS_SCALE, CI_GLASS_Y, CI_MOLE_TYPE, CI_MOLE_SCALE, CI_MOLE_X,
    CI_MOLE_Y, CI_SIZE = 0x58
};

static u32 rng_state;
static u32 rnd(u32 n) {                                  /* 0..n-1 (xorshift; good enough for faces) */
    u32 x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return n ? x % n : 0;
}

/* a plain face, then gender-specific hair; `random` varies the parts. Default Miis (random false) have fixed
 * create ids, so the same default is the same Mii every time. */
static void make_mii(u8 *ci, u32 gender, bool random, u32 index) {
    memset(ci, 0, CI_SIZE);
    if (random) BCryptGenRandom(NULL, ci, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    else memcpy(ci, "mk8recomp-dflt-", 15), ci[15] = (u8)index;
    ci[6] = (u8)(ci[6] & 0x0f) | 0x40;                   /* UUID version 4 */
    ci[8] = (u8)(ci[8] & 0x3f) | 0x80;                   /* RFC 4122 variant: what CreateId::IsValid checks */
    static const char name[] = "no name";
    for (int i = 0; name[i]; i++) ci[0x10 + 2 * i] = (u8)name[i];
    if (gender > 1) gender = rnd(2);
    ci[CI_GENDER] = (u8)gender;
    ci[CI_FAV_COLOR] = (u8)(random ? rnd(12) : 0);
    ci[CI_HEIGHT] = (u8)(random ? 32 + rnd(64) : 64);
    ci[CI_BUILD] = (u8)(random ? 32 + rnd(64) : 64);
    ci[CI_FACE_TYPE] = (u8)(random ? rnd(12) : 0);
    ci[CI_FACE_COLOR] = (u8)(random ? rnd(6) : 0);
    ci[CI_HAIR_TYPE] = (u8)(random ? rnd(132) : gender ? 12 : 33);
    ci[CI_HAIR_COLOR] = (u8)(random ? rnd(8) : index / 2);
    ci[CI_HAIR_FLIP] = (u8)(random ? rnd(2) : 0);
    ci[CI_EYE_TYPE] = (u8)(random ? rnd(60) : gender ? 4 : 2);
    ci[CI_EYE_COLOR] = (u8)(random ? rnd(6) : 0);
    ci[CI_EYE_SCALE] = 4; ci[CI_EYE_ASPECT] = 3; ci[CI_EYE_ROTATE] = (u8)(gender ? 3 : 4);
    ci[CI_EYE_X] = 2; ci[CI_EYE_Y] = 12;
    ci[CI_BROW_TYPE] = (u8)(random ? rnd(24) : gender ? 0 : 6);
    ci[CI_BROW_COLOR] = ci[CI_HAIR_COLOR];
    ci[CI_BROW_SCALE] = 4; ci[CI_BROW_ASPECT] = 3; ci[CI_BROW_ROTATE] = 6; ci[CI_BROW_X] = 2; ci[CI_BROW_Y] = 10;
    ci[CI_NOSE_TYPE] = (u8)(random ? rnd(18) : 1);
    ci[CI_NOSE_SCALE] = 4; ci[CI_NOSE_Y] = 9;
    ci[CI_MOUTH_TYPE] = (u8)(random ? rnd(36) : 23);
    ci[CI_MOUTH_COLOR] = (u8)(random ? rnd(5) : 0);
    ci[CI_MOUTH_SCALE] = 4; ci[CI_MOUTH_ASPECT] = 3; ci[CI_MOUTH_Y] = 13;
    ci[CI_BEARD_COLOR] = ci[CI_HAIR_COLOR];
    ci[CI_MUSTACHE_SCALE] = 4; ci[CI_MUSTACHE_Y] = 10;
    ci[CI_GLASS_SCALE] = 4; ci[CI_GLASS_Y] = 10;
    ci[CI_MOLE_SCALE] = 4; ci[CI_MOLE_X] = 2; ci[CI_MOLE_Y] = 20;
}

/* BuildRandom(age, gender, race): age and race only steer looks on the console */
static void db_build_random(SvcObj *o, IpcReq *r) {
    u8 ci[CI_SIZE];
    BCryptGenRandom(NULL, (u8 *)&rng_state, 4, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    rng_state |= 1;
    make_mii(ci, in_u32(r, 4), true, 0);
    out_data(r, ci, sizeof ci);
}

/* BuildDefault(index): 0-5, alternating male and female */
#define NDEFAULT 6
static void db_build_default(SvcObj *o, IpcReq *r) {
    u32 i = in_u32(r, 0);
    if (i >= NDEFAULT) { r->result = 0x67eu; return; }   /* mii 3: invalid argument */
    u8 ci[CI_SIZE];
    make_mii(ci, i & 1, false, i);
    out_data(r, ci, sizeof ci);
}

/* SourceFlag: 1 the database (empty), 2 the default Miis */
static u32 count_for(u32 flags) { return flags & 2 ? NDEFAULT : 0; }
static void db_get_count(SvcObj *o, IpcReq *r) { out_u32(r, count_for(in_u32(r, 0))); }

/* Get: CharInfoElement {CharInfo, u32 source (1 = default)}; Get1: bare CharInfo */
static void db_get_charinfo(SvcObj *o, IpcReq *r) {
    u32 n = count_for(in_u32(r, 0)), stride = r->cmd == 3 ? CI_SIZE + 4 : CI_SIZE, i = 0;
    const IpcBuf *b = buf_out(r, 0);
    for (; b && i < n && (i + 1) * stride <= b->size; i++) {
        u8 e[CI_SIZE + 4];
        make_mii(e, i & 1, false, i);
        u32 src = 1;
        memcpy(e + CI_SIZE, &src, 4);
        memcpy((void *)(uintptr_t)(b->addr + i * stride), e, stride);
    }
    out_u32(r, i);
}

/* Get2/Get3 return StoreData (with checksums): not written; only the empty database */
static void db_get_storedata(SvcObj *o, IpcReq *r) {
    if (count_for(in_u32(r, 0))) hlog(0, "mii: %s of the default Miis is not written yet (none returned)",
                                      r->cmd == 8 ? "Get2" : "Get3");
    out_u32(r, 0);
}

static const Iface iface_db = {"IDatabaseService", (const IpcCmd[]){
    {0, db_false, "IsUpdated"},
    {1, db_false, "IsFullDatabase"},
    {2, db_get_count, "GetCount"},
    {3, db_get_charinfo, "Get"},
    {4, db_get_charinfo, "Get1"},
    {5, db_not_found, "UpdateLatest"},
    {6, db_build_random, "BuildRandom"},
    {7, db_build_default, "BuildDefault"},
    {8, db_get_storedata, "Get2"},
    {9, db_get_storedata, "Get3"},
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
