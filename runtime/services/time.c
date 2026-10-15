/* time: system clocks from the host's clock, a steady clock counting from start-up, and a time zone service with
 * one rule: the host's current UTC offset (the console's tz database lives in a system archive we don't have). */
#include "../ipc.h"
#include <windows.h>
#include <time.h>

static const u8 clock_source_id[16] = {0x6d, 0x6b, 0x38, 0x72, 0x65, 0x63, 0x6f, 0x6d, 0x70, 0x20, 0x73, 0x74, 0x65, 0x61, 0x64, 0x79};
static s64 boot_posix;                           /* host time at start-up */
static s32 utc_offset;                           /* seconds east of UTC */

static s64 now_posix(void) { return (s64)time(NULL); }
static s64 steady_seconds(void) { return (s64)(host_cntpct() / 19200000u); }

/* SteadyClockTimePoint: s64 seconds, u128 source id */
static void out_time_point(IpcReq *r) {
    out_u64(r, (u64)steady_seconds());
    out_data(r, clock_source_id, 16);
}

/* ---------------------------------------------------------------------------------------------- ISystemClock */
static void sc_current_time(SvcObj *o, IpcReq *r) { out_u64(r, (u64)now_posix()); }
/* SystemClockContext: s64 offset (posix - steady), SteadyClockTimePoint */
static void sc_context(SvcObj *o, IpcReq *r) {
    out_u64(r, (u64)(now_posix() - steady_seconds()));
    out_time_point(r);
}
static const Iface iface_system_clock = {"ISystemClock", (const IpcCmd[]){
    {0, sc_current_time, "GetCurrentTime"},
    {1, ipc_noop, "SetCurrentTime"},
    {2, sc_context, "GetSystemClockContext"},
    {3, ipc_noop, "SetSystemClockContext"},
    IPC_END}, NULL};

/* ---------------------------------------------------------------------------------------------- ISteadyClock */
static void st_time_point(SvcObj *o, IpcReq *r) { out_time_point(r); }
static void st_zero64(SvcObj *o, IpcReq *r) { out_u64(r, 0); }
static void st_rtc(SvcObj *o, IpcReq *r) { out_u64(r, (u64)now_posix()); }
static void st_false(SvcObj *o, IpcReq *r) { out_u8(r, 0); }
static void st_zero32(SvcObj *o, IpcReq *r) { out_u32(r, 0); }
static const Iface iface_steady_clock = {"ISteadyClock", (const IpcCmd[]){
    {0, st_time_point, "GetCurrentTimePoint"},
    {2, st_zero64, "GetTestOffset"},
    {3, ipc_noop, "SetTestOffset"},
    {100, st_rtc, "GetRtcValue"},
    {101, st_false, "IsRtcResetDetected"},
    {102, st_zero32, "GetSetupResultValue"},
    {200, st_zero64, "GetInternalOffset"},
    IPC_END}, NULL};

/* ---------------------------------------------------------------------------------------------- ITimeZoneService */
/* CalendarTime: s16 year, s8 month, day, hour, minute, second, pad;
 * CalendarAdditionalInfo: u32 day of week, u32 day of year, char tz name[8], u32 is dst, s32 utc offset */
static void out_calendar(IpcReq *r, s64 t, s32 offset) {
    __time64_t lt = t + offset;
    struct tm tmv;
    _gmtime64_s(&tmv, &lt);
    u8 ct[8];
    s16 year = (s16)(tmv.tm_year + 1900);
    memcpy(ct, &year, 2);
    ct[2] = (u8)(tmv.tm_mon + 1); ct[3] = (u8)tmv.tm_mday; ct[4] = (u8)tmv.tm_hour; ct[5] = (u8)tmv.tm_min;
    ct[6] = (u8)tmv.tm_sec; ct[7] = 0;
    out_data(r, ct, 8);
    u8 info[0x18] = {0};
    u32 wday = (u32)tmv.tm_wday, yday = (u32)tmv.tm_yday;
    memcpy(info, &wday, 4);
    memcpy(info + 4, &yday, 4);
    memcpy(info + 8, offset ? "LOCAL" : "UTC", offset ? 6 : 4);
    memcpy(info + 0x14, &offset, 4);
    out_data(r, info, 0x18);
}

/* our TimeZoneRule (0x4000 bytes, opaque to the game): a tag and the offset */
#define RULE_TAG 0x6c757254u
static s32 rule_offset(IpcReq *r) {
    const IpcBuf *b = buf_in(r, 0);
    if (b && b->size >= 8) {
        u32 tag; s32 off;
        memcpy(&tag, (const void *)(uintptr_t)b->addr, 4);
        memcpy(&off, (const void *)(uintptr_t)(b->addr + 4), 4);
        if (tag == RULE_TAG) return off;
    }
    return utc_offset;
}

static void tz_location(SvcObj *o, IpcReq *r) { char n[0x24] = "UTC"; if (utc_offset) strcpy(n, "Local"); out_data(r, n, 0x24); }
static void tz_count(SvcObj *o, IpcReq *r) { out_u32(r, 1); }
static void tz_load_rule(SvcObj *o, IpcReq *r) {
    if (!r->nb || r->b[0].size < 8) return;
    u32 tag = RULE_TAG;
    memcpy((void *)(uintptr_t)r->b[0].addr, &tag, 4);
    memcpy((void *)(uintptr_t)(r->b[0].addr + 4), &utc_offset, 4);
}
static void tz_version(SvcObj *o, IpcReq *r) { u8 v[0x10] = "2018e"; out_data(r, v, 0x10); }
static void tz_to_calendar(SvcObj *o, IpcReq *r) { out_calendar(r, (s64)in_u64(r, 0), rule_offset(r)); }
static void tz_to_calendar_mine(SvcObj *o, IpcReq *r) { out_calendar(r, (s64)in_u64(r, 0), utc_offset); }
static void to_posix(IpcReq *r, s32 offset) {
    const u8 *ct = r->in;
    if (r->in_size < 8) return;
    struct tm tmv = {0};
    s16 year;
    memcpy(&year, ct, 2);
    tmv.tm_year = year - 1900; tmv.tm_mon = ct[2] - 1; tmv.tm_mday = ct[3];
    tmv.tm_hour = ct[4]; tmv.tm_min = ct[5]; tmv.tm_sec = ct[6];
    s64 t = (s64)_mkgmtime64(&tmv) - offset;
    const IpcBuf *b = buf_out(r, 0);
    if (b && b->size >= 8) memcpy((void *)(uintptr_t)b->addr, &t, 8);
    out_u32(r, 1);
}
static void tz_to_posix(SvcObj *o, IpcReq *r) { to_posix(r, rule_offset(r)); }
static void tz_to_posix_mine(SvcObj *o, IpcReq *r) { to_posix(r, utc_offset); }

static const Iface iface_tz = {"ITimeZoneService", (const IpcCmd[]){
    {0, tz_location, "GetDeviceLocationName"},
    {1, ipc_noop, "SetDeviceLocationName"},
    {2, tz_count, "GetTotalLocationNameCount"},
    {4, tz_load_rule, "LoadTimeZoneRule"},
    {5, tz_version, "GetTimeZoneRuleVersion"},
    {100, tz_to_calendar, "ToCalendarTime"},
    {101, tz_to_calendar_mine, "ToCalendarTimeWithMyRule"},
    {201, tz_to_posix, "ToPosixTime"},
    {202, tz_to_posix_mine, "ToPosixTimeWithMyRule"},
    IPC_END}, NULL};

/* ---------------------------------------------------------------------------------------------- IStaticService */
IPC_OPEN(ss_system_clock, &iface_system_clock)
IPC_OPEN(ss_steady_clock, &iface_steady_clock)
IPC_OPEN(ss_tz, &iface_tz)
static void ss_false(SvcObj *o, IpcReq *r) { out_u8(r, 0); }
/* CalculateMonotonicSystemClockBaseTimePoint(SystemClockContext): offset + the steady time point -> posix base */
static void ss_monotonic_base(SvcObj *o, IpcReq *r) { out_u64(r, in_u64(r, 0) + 0); }
/* ClockSnapshot (0xd0): user and network contexts, times, calendars, location, automatic correction, type */
static void ss_snapshot(SvcObj *o, IpcReq *r) {
    if (!r->nb || r->b[0].size < 0xd0) return;
    u8 *s = (u8 *)(uintptr_t)r->b[0].addr;
    memset(s, 0, 0xd0);
    s64 now = now_posix(), steady = steady_seconds(), off = now - steady;
    for (int i = 0; i < 2; i++) {                        /* user, network contexts */
        memcpy(s + i * 0x20, &off, 8);
        memcpy(s + i * 0x20 + 8, &steady, 8);
        memcpy(s + i * 0x20 + 0x10, clock_source_id, 16);
    }
    memcpy(s + 0x40, &now, 8);                            /* user time */
    memcpy(s + 0x48, &now, 8);                            /* network time */
    memcpy(s + 0x50, &steady, 8);                         /* steady time point */
    memcpy(s + 0x58, clock_source_id, 16);
    strcpy((char *)s + 0x68, utc_offset ? "Local" : "UTC");
    s[0xcf] = in_u8(r, 0);
}

static const Iface iface_time = {"time", (const IpcCmd[]){
    {0, ss_system_clock, "GetStandardUserSystemClock"},
    {1, ss_system_clock, "GetStandardNetworkSystemClock"},
    {2, ss_steady_clock, "GetStandardSteadyClock"},
    {3, ss_tz, "GetTimeZoneService"},
    {4, ss_system_clock, "GetStandardLocalSystemClock"},
    {100, ss_false, "IsStandardUserSystemClockAutomaticCorrectionEnabled"},
    {300, ss_monotonic_base, "CalculateMonotonicSystemClockBaseTimePoint"},
    {400, ss_snapshot, "GetClockSnapshot"},
    IPC_END}, NULL};

static SvcObj *open_time(const char *name) { return svcobj_new(&iface_time, NULL, name); }

void time_register(void) {
    boot_posix = now_posix();
    TIME_ZONE_INFORMATION tz;
    DWORD k = GetTimeZoneInformation(&tz);
    LONG bias = tz.Bias + (k == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : k == TIME_ZONE_ID_STANDARD ? tz.StandardBias : 0);
    utc_offset = -bias * 60;
    service_register("time:u", open_time);
    service_register("time:a", open_time);
    service_register("time:s", open_time);
}
