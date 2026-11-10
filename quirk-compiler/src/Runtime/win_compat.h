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

// The C library functions generated code calls directly (printf, fopen,
// malloc, ...). The JIT binds those names to these wrappers on Windows
// (Compiler.cpp): the UCRT exports no `printf` symbol, so a plain lookup
// found the legacy msvcrt.dll — a second CRT with its own stdout buffer,
// and print()'s pieces came out of order. Defined before runtime.c's
// malloc -> GC_malloc macros, so these are the real CRT functions.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
__declspec(dllexport) int quirk_crt_printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vprintf(fmt, ap); va_end(ap); return r;
}
__declspec(dllexport) int quirk_crt_sprintf(char* buf, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsprintf(buf, fmt, ap); va_end(ap); return r;
}
__declspec(dllexport) FILE* quirk_crt_fopen(const char* p, const char* m) { return fopen(p, m); }
__declspec(dllexport) int quirk_crt_fclose(FILE* f) { return fclose(f); }
__declspec(dllexport) size_t quirk_crt_fwrite(const void* p, size_t s, size_t n, FILE* f) { return fwrite(p, s, n, f); }
__declspec(dllexport) size_t quirk_crt_fread(void* p, size_t s, size_t n, FILE* f) { return fread(p, s, n, f); }
__declspec(dllexport) int quirk_crt_fseek(FILE* f, long off, int whence) { return fseek(f, off, whence); }
__declspec(dllexport) long quirk_crt_ftell(FILE* f) { return ftell(f); }
__declspec(dllexport) size_t quirk_crt_strlen(const char* s) { return strlen(s); }
__declspec(dllexport) void* quirk_crt_malloc(size_t n) { return malloc(n); }
__declspec(dllexport) void quirk_crt_free(void* p) { free(p); }
__declspec(dllexport) void quirk_crt_exit(int code) { fflush(NULL); exit(code); }
__declspec(dllexport) int quirk_crt_write(int fd, const void* buf, unsigned n) {
    if (fd == 1 || fd == 2) fflush(fd == 1 ? stdout : stderr);
    return _write(fd, buf, n);
}

#endif  // _WIN32
#endif  // QUIRK_WIN_COMPAT_H
