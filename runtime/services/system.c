/* Small system services: csrng (random bytes from the host's CSPRNG) and btm (Bluetooth manager, offline: no
 * devices; its events never fire). Command ids are from the SDK proxies. */
#include "../ipc.h"
#include <windows.h>
#include <bcrypt.h>

/* ---------------------------------------------------------------------------------------------- csrng */
static void csrng_bytes(SvcObj *o, IpcReq *r) {
    (void)o;
    const IpcBuf *b = buf_out(r, 0);
    if (b && b->size) BCryptGenRandom(NULL, (u8 *)(uintptr_t)b->addr, (ULONG)b->size, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}
static const Iface iface_csrng = {"csrng", (const IpcCmd[]){{0, csrng_bytes, "GenerateRandomBytes"}, IPC_END}, NULL};
static SvcObj *open_csrng(const char *name) { return svcobj_new(&iface_csrng, NULL, name); }

/* ---------------------------------------------------------------------------------------------- btm */
static KEvent *ble_event;                       /* one event for all of them: nothing ever happens */
static void btm_event(SvcObj *o, IpcReq *r) { (void)o; out_event(r, ble_event); out_u8(r, 1); }
static void btm_false(SvcObj *o, IpcReq *r) { (void)o; out_u8(r, 0); }
static void btm_zero(SvcObj *o, IpcReq *r) { (void)o; out_u32(r, 0); }
static const Iface iface_btm_core = {"IBtmUserCore", (const IpcCmd[]){
    {0, btm_event, "AcquireBleScanEvent"},
    {7, btm_false, "IsBleScanEnabled"},
    {17, btm_event, "AcquireBleConnectionEvent"},
    {20, btm_zero, "GetBleConnectionInfoList"},
    {26, btm_event, "AcquireBleServiceDiscoveryEvent"},
    {33, btm_event, "AcquireBleMtuConfigEvent"},
    IPC_END}, NULL};
IPC_OPEN(btm_get_core, &iface_btm_core)
static const Iface iface_btm = {"btm:u", (const IpcCmd[]){{0, btm_get_core, "GetCore"}, IPC_END}, NULL};
static SvcObj *open_btm(const char *name) { return svcobj_new(&iface_btm, NULL, name); }

void system_register(void) {
    ble_event = event_create();
    service_register("csrng", open_csrng);
    service_register("btm:u", open_btm);
}
