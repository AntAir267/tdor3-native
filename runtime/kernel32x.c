/* KERNEL32, the rest of what the C runtime DLL (MSVCRT.DLL) and the game ask for: version,
   ticks, times and time zones, locale, environment, system information, console stubs. */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "runtime.h"

/* ------------------------------------------------------------------ version, process */
API(KERNEL32, GetVersionExA, 1)
{
    uint32_t v = ARG(0);
    uint32_t size = R32(v);
    if (size < 148) { set_last_error(122); return 0; }
    memset(gptr(v + 4), 0, size - 4);
    W32(v + 4, 5);                       /* Windows XP 5.1.2600, as GetVersion says */
    W32(v + 8, 1);
    W32(v + 12, 2600);
    W32(v + 16, 2);                      /* VER_PLATFORM_WIN32_NT */
    gstrncpy(v + 20, "Service Pack 3", 128);
    return 1;
}

API(KERNEL32, GetTickCount, 0) { return (uint32_t)(uint64_t)now_ms(); }
API(KERNEL32, SetLastError, 1) { set_last_error(ARG(0)); return 0; }
API(KERNEL32, GetProcessHeap, 0) { return process_heap; }
API(KERNEL32, SetErrorMode, 1) { static uint32_t mode; uint32_t o = mode; mode = ARG(0); return o; }
API(KERNEL32, SetPriorityClass, 2) { return 1; }
API(KERNEL32, Beep, 2) { return 1; }
API(KERNEL32, HeapValidate, 3) { return 1; }
API(KERNEL32, HeapCompact, 2) { return 0x100000; }
API(KERNEL32, HeapWalk, 2) { set_last_error(259); return 0; }   /* ERROR_NO_MORE_ITEMS */
API(KERNEL32, GetExitCodeProcess, 2) { if (ARG(1)) W32(ARG(1), 0); return 0; }
API(KERNEL32, CreateProcessA, 10) { set_last_error(2); return 0; }
API(KERNEL32, CreateProcessW, 10) { set_last_error(2); return 0; }
API(KERNEL32, CreatePipe, 4) { set_last_error(5); return 0; }
API(KERNEL32, PeekNamedPipe, 6) { return 0; }

API(KERNEL32, GetSystemInfo, 1)
{
    uint32_t si = ARG(0);
    memset(gptr(si), 0, 36);
    W32(si + 4, 0x1000);                 /* page size */
    W32(si + 8, 0x10000);                /* lowest application address */
    W32(si + 12, 0x7ffeffff);
    W32(si + 16, 1);                     /* processor mask: one CPU, as the game knew them */
    W32(si + 20, 1);
    W32(si + 24, 586);                   /* PROCESSOR_INTEL_PENTIUM */
    W32(si + 28, 0x10000);               /* allocation granularity */
    W16(si + 32, 6);                     /* level */
    W16(si + 34, 0x0502);
    return 0;
}

/* ------------------------------------------------------------------ console (no console) */
API(KERNEL32, GetConsoleMode, 2) { set_last_error(6); return 0; }
API(KERNEL32, SetConsoleMode, 2) { set_last_error(6); return 0; }
API(KERNEL32, SetConsoleCtrlHandler, 2) { return 1; }
API(KERNEL32, GetNumberOfConsoleInputEvents, 2) { set_last_error(6); return 0; }
API(KERNEL32, PeekConsoleInputA, 4) { set_last_error(6); return 0; }
API(KERNEL32, ReadConsoleInputA, 4) { set_last_error(6); return 0; }
API(KERNEL32, ReadConsoleA, 5) { set_last_error(6); return 0; }
API(KERNEL32, WriteConsoleA, 5)
{
    if (ARG(4)) W32(ARG(4), ARG(2));
    LOG(LOG_ALWAYS, "console: %.*s", (int)ARG(2), gptr(ARG(1)));
    return 1;
}

/* ------------------------------------------------------------------ time */
/* the host's time (fixed with --test, so runs repeat) */
static time_t host_time(void)
{
    return deterministic ? (time_t)946684800 + (time_t)(now_ms() / 1000.0) : time(NULL);
}

static uint64_t to_filetime(time_t t, long nsec)
{
    return ((uint64_t)t + 11644473600ull) * 10000000ull + (uint64_t)(nsec / 100);
}

static void put_systemtime(uint32_t st, const struct tm *tm, int ms)
{
    W16(st + 0, (uint16_t)(tm->tm_year + 1900));
    W16(st + 2, (uint16_t)(tm->tm_mon + 1));
    W16(st + 4, (uint16_t)tm->tm_wday);
    W16(st + 6, (uint16_t)tm->tm_mday);
    W16(st + 8, (uint16_t)tm->tm_hour);
    W16(st + 10, (uint16_t)tm->tm_min);
    W16(st + 12, (uint16_t)tm->tm_sec);
    W16(st + 14, (uint16_t)ms);
}

static int now_millis(void)
{
    if (deterministic)
        return (int)((uint64_t)now_ms() % 1000);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int)(ts.tv_nsec / 1000000);
}

API(KERNEL32, GetSystemTime, 1)
{
    time_t t = host_time();
    struct tm tm;
    gmtime_r(&t, &tm);
    put_systemtime(ARG(0), &tm, now_millis());
    return 0;
}

API(KERNEL32, GetLocalTime, 1)
{
    time_t t = host_time();
    struct tm tm;
    if (deterministic) gmtime_r(&t, &tm); else localtime_r(&t, &tm);
    put_systemtime(ARG(0), &tm, now_millis());
    return 0;
}

API(KERNEL32, GetSystemTimeAsFileTime, 1)
{
    uint64_t ft = to_filetime(host_time(), (long)now_millis() * 1000000L);
    W32(ARG(0), (uint32_t)ft);
    W32(ARG(0) + 4, (uint32_t)(ft >> 32));
    return 0;
}

API(KERNEL32, SetLocalTime, 1) { set_last_error(1314); return 0; }   /* no privilege */

static long utc_offset_s(void)
{
    if (deterministic)
        return 0;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_gmtoff;
}

API(KERNEL32, FileTimeToSystemTime, 2)
{
    uint64_t ft = R32(ARG(0)) | (uint64_t)R32(ARG(0) + 4) << 32;
    time_t t = (time_t)(ft / 10000000ull) - (time_t)11644473600ll;
    struct tm tm;
    gmtime_r(&t, &tm);
    put_systemtime(ARG(1), &tm, (int)(ft / 10000 % 1000));
    return 1;
}

static uint32_t shift_filetime(uint32_t from, uint32_t to, long sec)
{
    uint64_t ft = R32(from) | (uint64_t)R32(from + 4) << 32;
    ft += (uint64_t)((int64_t)sec * 10000000ll);
    W32(to, (uint32_t)ft);
    W32(to + 4, (uint32_t)(ft >> 32));
    return 1;
}

API(KERNEL32, FileTimeToLocalFileTime, 2) { return shift_filetime(ARG(0), ARG(1), utc_offset_s()); }
API(KERNEL32, LocalFileTimeToFileTime, 2) { return shift_filetime(ARG(0), ARG(1), -utc_offset_s()); }

API(KERNEL32, SystemTimeToFileTime, 2)
{
    uint32_t st = ARG(0);
    struct tm tm = {0};
    tm.tm_year = R16(st) - 1900;
    tm.tm_mon = R16(st + 2) - 1;
    tm.tm_mday = R16(st + 6);
    tm.tm_hour = R16(st + 8);
    tm.tm_min = R16(st + 10);
    tm.tm_sec = R16(st + 12);
    time_t t = timegm(&tm);
    uint64_t ft = to_filetime(t, (long)R16(st + 14) * 1000000L);
    W32(ARG(1), (uint32_t)ft);
    W32(ARG(1) + 4, (uint32_t)(ft >> 32));
    return 1;
}

API(KERNEL32, FileTimeToDosDateTime, 3)
{
    uint64_t ft = R32(ARG(0)) | (uint64_t)R32(ARG(0) + 4) << 32;
    time_t t = (time_t)(ft / 10000000ull) - (time_t)11644473600ll;
    struct tm tm;
    gmtime_r(&t, &tm);
    if (ARG(1)) W16(ARG(1), (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday));
    if (ARG(2)) W16(ARG(2), (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2)));
    return 1;
}

API(KERNEL32, GetTimeZoneInformation, 1)
{
    uint32_t z = ARG(0);
    memset(gptr(z), 0, 172);
    W32(z, (uint32_t)(int32_t)(-utc_offset_s() / 60));   /* Bias: UTC = local + bias */
    return 0;                                            /* TIME_ZONE_ID_UNKNOWN: no DST rules */
}

/* ------------------------------------------------------------------ locale (English, US) */
API(KERNEL32, GetUserDefaultLCID, 0) { return 0x409; }
API(KERNEL32, IsValidCodePage, 1) { return ARG(0) == 1252 || ARG(0) == 437 || ARG(0) == 850; }
API(KERNEL32, IsValidLocale, 2) { return (ARG(0) & 0x3ff) == 0x09; }
API(KERNEL32, EnumSystemLocalesA, 2) { return 1; }

static const char *locale_value(uint32_t type)
{
    switch (type & 0xffff) {
    case 0x01: return "0409";            /* LOCALE_ILANGUAGE */
    case 0x02: return "English (United States)";
    case 0x03: return "ENU";             /* LOCALE_SABBREVLANGNAME */
    case 0x07: return "ENU";
    case 0x0b: return "437";             /* LOCALE_IDEFAULTCODEPAGE */
    case 0x1004: return "1252";          /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x0e: return ".";               /* LOCALE_SDECIMAL */
    case 0x0f: return ",";               /* LOCALE_STHOUSAND */
    case 0x10: return "3;0";             /* LOCALE_SGROUPING */
    case 0x14: return "$";
    case 0x15: return "$";
    case 0x16: return ".";
    case 0x17: return ",";
    case 0x18: return "3;0";
    case 0x19: return "2";
    case 0x1a: return "2";
    case 0x1b: return "0";
    case 0x1c: return "0";
    case 0x1d: return "/";               /* LOCALE_SDATE */
    case 0x1e: return ":";               /* LOCALE_STIME */
    case 0x1f: return "M/d/yyyy";
    case 0x20: return "dddd, MMMM dd, yyyy";
    case 0x21: return "0";
    case 0x23: return "0";
    case 0x24: return "0";               /* LOCALE_ICENTURY */
    case 0x25: return "0";
    case 0x26: return "0";
    case 0x28: return "AM";
    case 0x29: return "PM";
    case 0x50: return "";
    case 0x51: return "-";
    case 0x1001: return "English";
    case 0x1002: return "United States";
    case 0x1003: return "h:mm:ss tt";
    case 0x59: return "en";
    case 0x5a: return "US";
    case 0x1007: return "USA";
    case 0x1009: return "0";
    case 0x100b: return "1";
    default: return NULL;
    }
}

API(KERNEL32, GetLocaleInfoA, 4)
{
    const char *v = locale_value(ARG(1));
    if (!v) { set_last_error(1004); LOG(LOG_API, "GetLocaleInfoA(%x, %x): not known", ARG(0), ARG(1)); return 0; }
    uint32_t n = (uint32_t)strlen(v) + 1;
    if (!ARG(3)) return n;
    if (ARG(3) < n) { set_last_error(122); return 0; }
    memcpy(gptr(ARG(2)), v, n);
    return n;
}

API(KERNEL32, GetLocaleInfoW, 4)
{
    const char *v = locale_value(ARG(1));
    if (!v) { set_last_error(1004); return 0; }
    uint32_t n = (uint32_t)strlen(v) + 1;
    if (!ARG(3)) return n;
    if (ARG(3) < n) { set_last_error(122); return 0; }
    for (uint32_t i = 0; i < n; i++)
        W16(ARG(2) + 2 * i, (uint8_t)v[i]);
    return n;
}

static int cmp_strings(const uint8_t *a, int na, const uint8_t *b, int nb, int nocase)
{
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++) {
        int x = a[i], y = b[i];
        if (nocase) { x = tolower(x); y = tolower(y); }
        if (x != y) return x < y ? 1 : 3;
    }
    return na == nb ? 2 : na < nb ? 1 : 3;     /* CSTR_LESS_THAN / EQUAL / GREATER_THAN */
}

API(KERNEL32, CompareStringA, 6)
{
    const uint8_t *a = (const uint8_t *)gptr(ARG(2)), *b = (const uint8_t *)gptr(ARG(4));
    int na = (int)ARG(3), nb = (int)ARG(5);
    if (na < 0) na = (int)strlen((const char *)a);
    if (nb < 0) nb = (int)strlen((const char *)b);
    return (uint32_t)cmp_strings(a, na, b, nb, ARG(1) & 1);   /* NORM_IGNORECASE */
}

API(KERNEL32, CompareStringW, 6)
{
    uint32_t a = ARG(2), b = ARG(4);
    int na = (int)ARG(3), nb = (int)ARG(5);
    if (na < 0) for (na = 0; R16(a + 2 * na); na++) {}
    if (nb < 0) for (nb = 0; R16(b + 2 * nb); nb++) {}
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++) {
        uint32_t x = R16(a + 2 * i), y = R16(b + 2 * i);
        if ((ARG(1) & 1) && x < 128 && y < 128) { x = (uint32_t)tolower((int)x); y = (uint32_t)tolower((int)y); }
        if (x != y) return x < y ? 1 : 3;
    }
    return na == nb ? 2 : na < nb ? 1 : 3;
}

API(KERNEL32, lstrcmpiA, 2) { return (uint32_t)strcasecmp(gstr(ARG(0)) ? gstr(ARG(0)) : "", gstr(ARG(1)) ? gstr(ARG(1)) : ""); }

API(KERNEL32, lstrcpynA, 3)
{
    if (!ARG(0) || !ARG(2)) return ARG(0);
    gstrncpy(ARG(0), gstr(ARG(1)) ? gstr(ARG(1)) : "", ARG(2));
    return ARG(0);
}

/* ------------------------------------------------------------------ environment */
typedef struct { char name[64], value[256]; } EnvVar;
static EnvVar env[32];
static int nenv;

API(KERNEL32, GetEnvironmentVariableA, 3)
{
    const char *n = gstr(ARG(0));
    for (int i = 0; n && i < nenv; i++)
        if (!strcasecmp(env[i].name, n)) {
            uint32_t len = (uint32_t)strlen(env[i].value);
            if (ARG(2) <= len) return len + 1;
            gstrncpy(ARG(1), env[i].value, ARG(2));
            return len;
        }
    LOG(LOG_API, "GetEnvironmentVariableA(\"%s\"): not set", n ? n : "");
    set_last_error(203);                 /* ERROR_ENVVAR_NOT_FOUND */
    return 0;
}

API(KERNEL32, SetEnvironmentVariableA, 2)
{
    const char *n = gstr(ARG(0)), *v = gstr(ARG(1));
    if (!n) return 0;
    for (int i = 0; i < nenv; i++)
        if (!strcasecmp(env[i].name, n)) {
            if (!v) { env[i] = env[--nenv]; return 1; }
            snprintf(env[i].value, sizeof env[i].value, "%s", v);
            return 1;
        }
    if (v && nenv < 32) {
        snprintf(env[nenv].name, sizeof env[nenv].name, "%s", n);
        snprintf(env[nenv].value, sizeof env[nenv].value, "%s", v);
        nenv++;
    }
    return 1;
}

API(KERNEL32, GetCommandLineW, 0)
{
    static uint32_t w;
    if (!w) {
        char b[600];
        snprintf(b, sizeof b, "\"%s\\TDOR3.exe\"", virtual_game_root());
        uint32_t n = (uint32_t)strlen(b) + 1;
        w = galloc(2 * n);
        for (uint32_t i = 0; i < n; i++)
            W16(w + 2 * i, (uint8_t)b[i]);
    }
    return w;
}
