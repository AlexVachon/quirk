// Windows compatibility for the runtime's unity build (native Windows
// port, docs/WINDOWS-PORT.md). Included first by runtime.c on _WIN32 only;
// Linux and macOS never see it.
#ifndef QUIRK_WIN_COMPAT_H
#define QUIRK_WIN_COMPAT_H
#ifdef _WIN32

// winsock2.h must come before windows.h (which otherwise pulls in the old
// winsock.h and every socket type gets defined twice).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <time.h>
#include <stdint.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// clock_gettime for the two clocks the runtime uses.
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
static int quirk_win_clock_gettime(int clk, struct timespec* ts) {
    if (clk == CLOCK_MONOTONIC) {
        static LARGE_INTEGER freq;
        LARGE_INTEGER now;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        ts->tv_sec  = (time_t)(now.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)((now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
        return 0;
    }
    return timespec_get(ts, TIME_UTC) == TIME_UTC ? 0 : -1;
}
#define clock_gettime(clk, ts) quirk_win_clock_gettime((clk), (ts))
#endif

// nanosleep (task.c, sys.sleep outside a task): millisecond Sleep.
static int quirk_win_nanosleep(const struct timespec* req, struct timespec* rem) {
    (void)rem;
    DWORD ms = (DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
    Sleep(ms);
    return 0;
}
#define nanosleep(req, rem) quirk_win_nanosleep((req), (rem))

#endif  // _WIN32
#endif  // QUIRK_WIN_COMPAT_H
