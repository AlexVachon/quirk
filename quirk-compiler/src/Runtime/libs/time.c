#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gc.h>
#include "../types.h"

#ifdef _WIN32
    #include <windows.h>
    // Win lacks timegm; substitute via _mkgmtime.
    #define timegm _mkgmtime
#endif

// ---------------------------------------------------------------------------
//  Time runtime functions exported to libs/time/index.quirk.
//  Naming: Time_<name>  (matches `linkageName = "Time_<name>"` from
//  Parser::computeModulePrefix for libs/time/index.quirk).
//
//  All epoch values are Unix seconds (signed). Quirk Int is 32-bit so the
//  representable range runs out in 2038 (Y2038); fixing that requires
//  widening Int. Below the year 1970 this works via negative epochs.
//
//  Month is 1-12 (1=January) at the Quirk boundary even though the C side
//  uses 0-11 internally. Weekday is 0-6 (0=Sunday). Day-of-year is 1-366.
// ---------------------------------------------------------------------------

// Decode `epoch` into a struct tm, in UTC (`utc != 0`) or local time.
// Returns 1 on success, 0 if the host couldn't decode (extreme epoch).
static int time__break_down(int epoch, int utc, struct tm* out) {
    time_t t = (time_t)epoch;
#ifdef _WIN32
    // gmtime_s / localtime_s have args swapped from POSIX *_r.
    if (utc) {
        return gmtime_s(out, &t) == 0 ? 1 : 0;
    } else {
        return localtime_s(out, &t) == 0 ? 1 : 0;
    }
#else
    if (utc) {
        return gmtime_r(&t, out) ? 1 : 0;
    } else {
        return localtime_r(&t, out) ? 1 : 0;
    }
#endif
}

// Current Unix epoch, seconds. -1 if the host clock is unavailable.
int Time_unix_now() {
    time_t t = time(NULL);
    if (t == (time_t)-1) return -1;
    return (int)t;
}

int Time_year(int epoch, int utc) {
    struct tm tm;
    if (!time__break_down(epoch, utc, &tm)) return 0;
    return 1900 + tm.tm_year;
}

int Time_month(int epoch, int utc) {
    struct tm tm;
    if (!time__break_down(epoch, utc, &tm)) return 0;
    return tm.tm_mon + 1;
}

int Time_day(int epoch, int utc) {
    struct tm tm;
    if (!time__break_down(epoch, utc, &tm)) return 0;
    return tm.tm_mday;
}

int Time_hour(int epoch, int utc) {
    struct tm tm;
    if (!time__break_down(epoch, utc, &tm)) return 0;
    return tm.tm_hour;
}

int Time_minute(int epoch, int utc) {
    struct tm tm;
    if (!time__break_down(epoch, utc, &tm)) return 0;
    return tm.tm_min;
}

int Time_second(int epoch, int utc) {
    struct tm tm;
    if (!time__break_down(epoch, utc, &tm)) return 0;
    return tm.tm_sec;
}

int Time_weekday(int epoch, int utc) {
    struct tm tm;
    if (!time__break_down(epoch, utc, &tm)) return 0;
    return tm.tm_wday;  // 0 = Sunday, 6 = Saturday
}

int Time_yearday(int epoch, int utc) {
    struct tm tm;
    if (!time__break_down(epoch, utc, &tm)) return 0;
    return tm.tm_yday + 1;  // 1-366
}

// Build an epoch from broken-down components. Pass utc=1 to interpret the
// inputs as UTC, utc=0 for the host's local time zone (DST handled by libc).
// Returns -1 if mktime/timegm rejects the inputs.
int Time_to_unix(int year, int month, int day, int hour, int minute, int second, int utc) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = year - 1900;
    tm.tm_mon  = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min  = minute;
    tm.tm_sec  = second;
    tm.tm_isdst = -1;  // let libc decide (mktime only)
    time_t t;
    if (utc) {
        t = timegm(&tm);
    } else {
        t = mktime(&tm);
    }
    if (t == (time_t)-1) return -1;
    return (int)t;
}

// Format `epoch` using a strftime-style spec. Falls back to ISO-8601 when
// `fmt` is empty.
String* Time_format_at(int epoch, String* fmt, int utc) {
    struct tm tm;
    if (!time__break_down(epoch, utc, &tm)) return make_String("");
    const char* spec = (fmt && fmt->buffer && fmt->length > 0)
        ? fmt->buffer
        : "%Y-%m-%dT%H:%M:%S";
    char out[256];
    size_t n = strftime(out, sizeof(out), spec, &tm);
    if (n == 0) return make_String("");
    return make_String(out);
}

// Pre-formatted ISO-8601: "2026-01-15T12:34:56Z" (UTC) or "...+/-HH:MM" local.
String* Time_iso_at(int epoch, int utc) {
    struct tm tm;
    if (!time__break_down(epoch, utc, &tm)) return make_String("");
    char out[64];
    if (utc) {
        snprintf(out, sizeof(out), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 1900 + tm.tm_year, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        // Local time — compute offset from UTC for display.
        time_t t = (time_t)epoch;
        struct tm utc_tm;
#ifdef _WIN32
        gmtime_s(&utc_tm, &t);
#else
        gmtime_r(&t, &utc_tm);
#endif
        // Offset = local - UTC (in seconds). We approximate via timegm.
        // On 32-bit boundaries this is the simplest robust path.
        time_t local_as_utc = timegm(&tm);
        time_t utc_as_utc = timegm(&utc_tm);
        int offset_s = (int)(local_as_utc - utc_as_utc);
        char sign = offset_s >= 0 ? '+' : '-';
        if (offset_s < 0) offset_s = -offset_s;
        int oh = offset_s / 3600;
        int om = (offset_s / 60) % 60;
        snprintf(out, sizeof(out), "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
                 1900 + tm.tm_year, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec,
                 sign, oh, om);
    }
    return make_String(out);
}

// Parse an ISO-8601 string of the form "YYYY-MM-DD[Thh:mm:ss[Z]]" into an
// epoch. Returns -1 on parse failure. Trailing "Z" → UTC; absent timezone
// or " " separator → local (DST as the host sees it).
int Time_parse_iso(String* s) {
    if (!s || !s->buffer) return -1;
    int Y = 0, M = 0, D = 0, h = 0, mn = 0, sec = 0;
    int n;
    int has_time = 0;
    int is_utc = 0;
    n = sscanf(s->buffer, "%4d-%2d-%2d", &Y, &M, &D);
    if (n != 3) return -1;
    // Optional time portion separated by 'T' or ' '.
    const char* p = s->buffer;
    while (*p && *p != 'T' && *p != ' ') p++;
    if (*p == 'T' || *p == ' ') {
        if (sscanf(p + 1, "%2d:%2d:%2d", &h, &mn, &sec) >= 2) {
            has_time = 1;
            const char* q = p + 1;
            while (*q && *q != 'Z' && *q != '+' && *q != '-') q++;
            if (*q == 'Z') is_utc = 1;
            // Numeric offsets like +05:30 are not yet supported — treat as
            // local for now. Users wanting strict UTC should pass a Z suffix.
        }
        (void)has_time;
    }
    return Time_to_unix(Y, M, D, h, mn, sec, is_utc);
}
