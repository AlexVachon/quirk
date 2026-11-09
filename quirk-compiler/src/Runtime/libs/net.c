#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <gc.h>

#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #define getcwd _getcwd

    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <netdb.h>
    #include <errno.h>
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif

#ifndef QUIRK_TYPES_H
#include "../types.h"
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>


int64_t Net_socket() {
    return socket(AF_INET, SOCK_STREAM, 0);
}

int64_t Net_bind(int64_t sockfd, String* host, int64_t port) {
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;

    char* safe_host = make_safe_cstr(host);

    serv_addr.sin_addr.s_addr = (safe_host && strlen(safe_host) > 0)
                                ? inet_addr(safe_host)
                                : INADDR_ANY;
    serv_addr.sin_port = htons(port);

    // Allow rebinding while a previous socket on this port is in
    // TIME_WAIT — the standard server-restart case. Without this every
    // test run would have to wait ~60s before the port frees.
    int one = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    return bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
}

int64_t Net_listen(int64_t sockfd, int64_t backlog) {
    return listen(sockfd, backlog);
}

int64_t Net_accept(int64_t sockfd) {
    struct sockaddr_in cli_addr;
    socklen_t clilen = sizeof(cli_addr);
    quirk_wait_io((int)sockfd, 0);   // a task: other tasks run; cancel() interrupts
    quirk_gil_release();   // other tasks run while this waits for a client
    int fd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
    quirk_gil_acquire();
    return fd;
}

// connect(), letting other tasks run meanwhile. In a task it connects
// non-blocking and waits in quirk_wait_io, so task.cancel() interrupts it
// (raising CancelledError); elsewhere it blocks with the lock released.
static int net__connect(int fd, struct sockaddr_in* addr) {
#ifndef _WIN32
    if (quirk_ts->task) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, (struct sockaddr*)addr, sizeof(*addr));
        if (rc < 0 && errno == EINPROGRESS) {
            quirk_wait_io(fd, 1);
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            rc = err == 0 ? 0 : -1;
        }
        fcntl(fd, F_SETFL, flags);
        return rc;
    }
#endif
    quirk_gil_release();
    int rc = connect(fd, (struct sockaddr*)addr, sizeof(*addr));
    quirk_gil_acquire();
    return rc;
}

// Host name → IPv4 address. getaddrinfo is thread-safe (gethostbyname
// returns a shared static buffer), so other tasks run during the lookup.
static int net__resolve_ipv4(const char* host, struct in_addr* out) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    quirk_gil_release();
    int rc = getaddrinfo(host, NULL, &hints, &res);
    quirk_gil_acquire();
    if (rc != 0 || !res) return -1;
    *out = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

int64_t Net_connect(int64_t sockfd, String* host, int64_t port) {
    if (!host || !host->buffer) return -1;
    
    char* safe_host = make_safe_cstr(host);
    if (!safe_host) return -1;

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    if (net__resolve_ipv4(safe_host, &serv_addr.sin_addr) != 0) return -1;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    return net__connect((int)sockfd, &serv_addr);
}
int64_t Net_send(int64_t sockfd, String* data) {
    if (!data || !data->buffer) return 0;
    const char* buf = data->buffer;
    int64_t len = data->length;
    quirk_wait_io((int)sockfd, 1);
    quirk_gil_release();
    int64_t n = send(sockfd, buf, len, 0);
    quirk_gil_acquire();
    return n;
}

String* Net_recv(int64_t sockfd, int64_t size) {
    char* buffer = GC_malloc(size + 1);
    quirk_wait_io((int)sockfd, 0);
    quirk_gil_release();
    int n = recv(sockfd, buffer, size, 0);
    quirk_gil_acquire();
    if (n < 0) return make_String("");
    buffer[n] = '\0';
    return make_String(buffer);
}

void Net_close(int64_t sockfd) {
    #ifdef _WIN32
        closesocket(sockfd);
    #else
        close(sockfd);
    #endif
}

// ===================================================
//  TLS (libssl) — used by the `https://` path in
//  packages/net/http.quirk and any Quirk code that
//  imports net.tls directly.
//
//  Design: Quirk's runtime calling convention can't
//  carry an opaque pointer easily, so we hand the
//  Quirk side an integer "handle" that indexes into a
//  static slot table. Each slot owns its own SSL*
//  plus the underlying TCP fd; the shared SSL_CTX is
//  lazily allocated on the first connect.
//
//  Slot table is fixed-size (256) — a per-process
//  Quirk script that needs more concurrent HTTPS
//  connections than that is well outside what we're
//  trying to support today. Out-of-slots returns -1
//  the same way socket(2) failures do, so the Quirk
//  side already raises SocketError on it.
//
//  Cert verification is ON by default with the
//  system CA bundle. Sema-level opt-out (insecure
//  mode) can be added later if a user actually
//  needs it — better to ship secure by default and
//  weaken it on request than the other way around.
// ===================================================

#define QUIRK_TLS_SLOTS 256

typedef struct {
    SSL* ssl;
    int  fd;
    int  in_use;
} QuirkTlsSlot;

static QuirkTlsSlot     g_tls_slots[QUIRK_TLS_SLOTS];
static SSL_CTX*         g_tls_ctx = NULL;
static int              g_tls_init_done = 0;

static void quirk_tls_lazy_init(void) {
    if (g_tls_init_done) return;
    g_tls_init_done = 1;
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    g_tls_ctx = SSL_CTX_new(TLS_client_method());
    if (g_tls_ctx) {
        // Disable SSLv2/SSLv3/TLSv1.0/TLSv1.1 — only modern TLS.
        SSL_CTX_set_min_proto_version(g_tls_ctx, TLS1_2_VERSION);
        SSL_CTX_set_verify(g_tls_ctx, SSL_VERIFY_PEER, NULL);
        SSL_CTX_set_default_verify_paths(g_tls_ctx);
    }
}

static int quirk_tls_alloc_slot(void) {
    for (int i = 0; i < QUIRK_TLS_SLOTS; i++) {
        if (!g_tls_slots[i].in_use) {
            g_tls_slots[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static void quirk_tls_free_slot(int h) {
    if (h < 0 || h >= QUIRK_TLS_SLOTS) return;
    g_tls_slots[h].ssl = NULL;
    g_tls_slots[h].fd  = -1;
    g_tls_slots[h].in_use = 0;
}

// Returns a handle (>=0) on success, -1 on any failure.
// `host` is used for both DNS resolution and SNI / cert hostname check.
int64_t Net_tls_connect(String* host, int64_t port) {
    quirk_tls_lazy_init();
    if (!g_tls_ctx || !host || !host->buffer) return -1;

    char* safe_host = make_safe_cstr(host);
    if (!safe_host) return -1;

    struct in_addr resolved;
    if (net__resolve_ipv4(safe_host, &resolved) != 0) return -1;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr = resolved;
    serv_addr.sin_port = htons(port);

    int crc = net__connect(sockfd, &serv_addr);
    if (crc < 0) {
        close(sockfd);
        return -1;
    }

    SSL* ssl = SSL_new(g_tls_ctx);
    if (!ssl) {
        close(sockfd);
        return -1;
    }

    // SNI — required by most modern hosts that serve multiple
    // certs on one IP. Without this many servers return a
    // fallback cert that fails verification.
    SSL_set_tlsext_host_name(ssl, safe_host);
    // Hostname verification against the cert's SAN/CN.
    SSL_set1_host(ssl, safe_host);

    SSL_set_fd(ssl, sockfd);
    quirk_gil_release();
    int hs = SSL_connect(ssl);
    quirk_gil_acquire();
    if (hs != 1) {
        SSL_free(ssl);
        close(sockfd);
        return -1;
    }

    int h = quirk_tls_alloc_slot();
    if (h < 0) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(sockfd);
        return -1;
    }
    g_tls_slots[h].ssl = ssl;
    g_tls_slots[h].fd  = sockfd;
    return h;
}

int64_t Net_tls_send(int64_t handle, String* data) {
    if (handle < 0 || handle >= QUIRK_TLS_SLOTS) return -1;
    if (!g_tls_slots[handle].in_use) return -1;
    if (!data || !data->buffer) return 0;
    SSL* wssl = g_tls_slots[handle].ssl;
    quirk_wait_io(SSL_get_fd(wssl), 1);
    quirk_gil_release();
    int64_t n = SSL_write(wssl, data->buffer, data->length);
    quirk_gil_acquire();
    return n;
}

String* Net_tls_recv(int64_t handle, int64_t size) {
    if (handle < 0 || handle >= QUIRK_TLS_SLOTS) return make_String("");
    if (!g_tls_slots[handle].in_use) return make_String("");
    if (size <= 0) return make_String("");
    char* buffer = (char*)GC_malloc(size + 1);
    SSL* rssl = g_tls_slots[handle].ssl;
    // Already-decrypted bytes are buffered in the SSL object: no wait then.
    if (SSL_pending(rssl) <= 0) quirk_wait_io(SSL_get_fd(rssl), 0);
    quirk_gil_release();
    int64_t n = SSL_read(rssl, buffer, size);
    quirk_gil_acquire();
    if (n <= 0) {
        // n=0 → clean close; n<0 → error or want-read. Either way
        // the caller's recv loop reads "no more bytes" the same as
        // plain TCP and stops.
        return make_String("");
    }
    buffer[n] = '\0';
    return make_String(buffer);
}

void Net_tls_close(int64_t handle) {
    if (handle < 0 || handle >= QUIRK_TLS_SLOTS) return;
    if (!g_tls_slots[handle].in_use) return;
    if (g_tls_slots[handle].ssl) {
        SSL_shutdown(g_tls_slots[handle].ssl);
        SSL_free(g_tls_slots[handle].ssl);
    }
    if (g_tls_slots[handle].fd >= 0) {
        #ifdef _WIN32
            closesocket(g_tls_slots[handle].fd);
        #else
            close(g_tls_slots[handle].fd);
        #endif
    }
    quirk_tls_free_slot(handle);
}
