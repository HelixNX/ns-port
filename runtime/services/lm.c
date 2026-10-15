/* lm: the log service. ILogger::Log gets a packet (header + TLV chunks); the message text goes to our log. */
#include "../ipc.h"

/* a packet: u64 pid, u64 thread id, u8 flags, u8 pad, u8 severity, u8 verbosity, u32 payload size, payload */
static void logger_log(SvcObj *o, IpcReq *r) {
    const IpcBuf *b = buf_in(r, 0);
    if (!b || b->size < 0x18) return;
    const u8 *p = (const u8 *)(uintptr_t)b->addr;
    u32 len;
    memcpy(&len, p + 0x14, 4);
    const u8 *q = p + 0x18, *end = q + len;
    if (end > p + b->size) end = p + b->size;
    char msg[1024] = "", func[128] = "", file[160] = "";
    u32 line = 0;
    while (q < end) {
        u8 key = *q++;
        u32 n = 0, shift = 0;
        while (q < end) {                       /* ULEB128 length */
            u8 c = *q++;
            n |= (u32)(c & 0x7f) << shift;
            shift += 7;
            if (!(c & 0x80)) break;
        }
        if (q + n > end) break;
        if (key == 2) snprintf(msg + strlen(msg), sizeof msg - strlen(msg), "%.*s", (int)n, q);
        else if (key == 3 && n == 4) memcpy(&line, q, 4);
        else if (key == 4) snprintf(file, sizeof file, "%.*s", (int)n, q);
        else if (key == 5) snprintf(func, sizeof func, "%.*s", (int)n, q);
        q += n;
    }
    size_t l = strlen(msg);
    while (l && (msg[l - 1] == '\n' || msg[l - 1] == '\r')) msg[--l] = 0;
    if (l) hlog(0, "lm: %s%s%s", msg, func[0] ? "  @ " : "", func);
}

static const Iface iface_logger = {"ILogger", (const IpcCmd[]){
    {0, logger_log, "Log"},
    {1, ipc_noop, "SetDestination"},
    IPC_END}, NULL};

IPC_OPEN(lm_open_logger, &iface_logger)

static const Iface iface_lm = {"lm", (const IpcCmd[]){
    {0, lm_open_logger, "OpenLogger"},
    IPC_END}, NULL};

static SvcObj *open_lm(const char *name) { return svcobj_new(&iface_lm, NULL, name); }

void lm_register(void) { service_register("lm", open_lm); }
