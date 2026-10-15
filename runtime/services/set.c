/* set: system settings as an application sees them: language English (US), region Americas. */
#include "../ipc.h"

/* language codes by Language index, as u64 strings */
static const char *const languages[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru",
                                        "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
#define NLANG 18
#define LANG_COUNT_4X 15                         /* the first 15: the list before 4.0's additions */
static const u32 my_language = 1, my_region = 1; /* en-US, Americas */

static u64 code(u32 i) { u64 c = 0; memcpy(&c, languages[i], strlen(languages[i])); return c; }

static void set_language_code(SvcObj *o, IpcReq *r) { out_u64(r, code(my_language)); }
static void list_codes(IpcReq *r, u32 max) {
    const IpcBuf *b = buf_out(r, 0);
    u32 n = 0;
    for (; b && n < max && (n + 1) * 8 <= b->size; n++) ST64(b->addr + 8 * n, code(n));
    out_u32(r, n);
}
static void set_available(SvcObj *o, IpcReq *r) { list_codes(r, LANG_COUNT_4X); }
static void set_available2(SvcObj *o, IpcReq *r) { list_codes(r, NLANG); }
static void set_make_code(SvcObj *o, IpcReq *r) {
    u32 i = in_u32(r, 0);
    if (i >= NLANG) { r->result = 0x3c69; return; }               /* settings: invalid language */
    out_u64(r, code(i));
}
static void set_count(SvcObj *o, IpcReq *r) { out_u32(r, LANG_COUNT_4X); }
static void set_count2(SvcObj *o, IpcReq *r) { out_u32(r, NLANG); }
static void set_region(SvcObj *o, IpcReq *r) { out_u32(r, my_region); }

static const Iface iface_set = {"set", (const IpcCmd[]){
    {0, set_language_code, "GetLanguageCode"},
    {1, set_available, "GetAvailableLanguageCodes"},
    {2, set_make_code, "MakeLanguageCode"},
    {3, set_count, "GetAvailableLanguageCodeCount"},
    {4, set_region, "GetRegionCode"},
    {5, set_available2, "GetAvailableLanguageCodes2"},
    {6, set_count2, "GetAvailableLanguageCodeCount2"},
    IPC_END}, NULL};

static SvcObj *open_set(const char *name) { return svcobj_new(&iface_set, NULL, name); }

void set_register(void) { service_register("set", open_set); }
