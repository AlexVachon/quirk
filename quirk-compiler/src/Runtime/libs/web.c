// Helpers for packages/web (the web framework) and net.server: byte-level
// work on Strings that may hold binary data (request bodies, files) —
// Quirk's String methods count and index characters (UTF-8), while HTTP
// counts bytes. Naming: Web_<name> (packages/web/index.quirk externs).
#include <time.h>
#include <string.h>

// Bytes in `s` (HTTP Content-Length), not characters.
int64_t Web_byte_length(String* s) {
    return (s && s->buffer) ? s->length : 0;
}

int64_t Net_Server_byte_length(String* s) {
    return Web_byte_length(s);
}

// Byte offset of `needle` in `hay` at or after `from`, or -1.
int64_t Web_byte_find(String* hay, String* needle, int64_t from) {
    if (!hay || !hay->buffer || !needle || !needle->buffer) return -1;
    int64_t n = hay->length, m = needle->length;
    if (from < 0) from = 0;
    if (m == 0) return from <= n ? from : -1;
    for (int64_t i = from; i + m <= n; i++)
        if (hay->buffer[i] == needle->buffer[0] && memcmp(hay->buffer + i, needle->buffer, (size_t)m) == 0)
            return i;
    return -1;
}

// `len` bytes of `s` from byte `start` (clamped; len < 0 = to the end).
String* Web_byte_slice(String* s, int64_t start, int64_t len) {
    int64_t n = (s && s->buffer) ? s->length : 0;
    if (start < 0) start = 0;
    if (start > n) start = n;
    if (len < 0 || start + len > n) len = n - start;
    char* buf = (char*)GC_malloc_atomic((size_t)len + 1);
    if (len > 0) memcpy(buf, s->buffer + start, (size_t)len);
    buf[len] = '\0';
    return make_String_len(buf, len);
}

// An HTTP date (RFC 9110 IMF-fixdate) for Unix time `t`:
// "Sun, 06 Nov 1994 08:49:37 GMT".
String* Web_http_date(int64_t t) {
    static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char buf[40];
    snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[g.tm_wday % 7], g.tm_mday,
             months[g.tm_mon % 12], g.tm_year + 1900, g.tm_hour, g.tm_min, g.tm_sec);
    return make_String(buf);
}

// Seconds since the Unix epoch, now.
int64_t Web_now() {
    return (int64_t)time(NULL);
}

// Read up to `size` bytes from socket `fd`, waiting at most `timeout_ms`
// (other tasks run meanwhile). "" when the peer closed, on error, or on
// timeout — an idle keep-alive connection is then closed.
String* Web_recv_timeout(int64_t fd, int64_t size, int64_t timeout_ms) {
    if (size <= 0) size = 65536;
    char* buf = (char*)GC_malloc_atomic((size_t)size + 1);
    int n = -1;
    quirk_gil_release();
#ifdef _WIN32
    WSAPOLLFD p = { (SOCKET)fd, POLLRDNORM, 0 };
    int rc = WSAPoll(&p, 1, (INT)timeout_ms);
#else
    struct pollfd p = { (int)fd, POLLIN, 0 };
    int rc;
    do { rc = poll(&p, 1, (int)timeout_ms); } while (rc < 0 && errno == EINTR);
#endif
    if (rc > 0) n = (int)recv((int)fd, buf, (int)size, 0);
    quirk_gil_acquire();
    if (n <= 0) return make_String("");
    buf[n] = '\0';
    return make_String_len(buf, n);
}

// Send all of `data` (send() may take only part of a large buffer).
// The bytes sent, or -1 on error.
int64_t Web_send_all(int64_t fd, String* data) {
    if (!data || !data->buffer) return 0;
    int64_t left = data->length, off = 0;
    quirk_gil_release();
    while (left > 0) {
        int chunk = left > (1 << 30) ? (1 << 30) : (int)left;
        int n = (int)send((int)fd, data->buffer + off, chunk, 0);
        if (n < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            break;
        }
        off += n;
        left -= n;
    }
    quirk_gil_acquire();
    return left == 0 ? off : -1;
}

// Milliseconds on a steady clock (request timing).
int64_t Web_ms() {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}
