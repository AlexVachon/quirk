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


int Net_socket() {
    return socket(AF_INET, SOCK_STREAM, 0);
}

int Net_bind(int sockfd, String* host, int port) {
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

int Net_listen(int sockfd, int backlog) {
    return listen(sockfd, backlog);
}

int Net_accept(int sockfd) {
    struct sockaddr_in cli_addr;
    socklen_t clilen = sizeof(cli_addr);
    return accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
}

int Net_connect(int sockfd, String* host, int port) {
    if (!host || !host->buffer) return -1;
    
    char* safe_host = make_safe_cstr(host);
    if (!safe_host) return -1;

    struct sockaddr_in serv_addr;
    struct hostent *server = gethostbyname(safe_host); 
    
    if (server == NULL) return -1;

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(port);

    return connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
}
int Net_send(int sockfd, String* data) {
    if (!data || !data->buffer) return 0;
    return send(sockfd, data->buffer, data->length, 0);
}

String* Net_recv(int sockfd, int size) {
    char* buffer = GC_malloc(size + 1);
    int n = recv(sockfd, buffer, size, 0);
    if (n < 0) return make_String("");
    buffer[n] = '\0';
    return make_String(buffer);
}

void Net_close(int sockfd) {
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
int Net_tls_connect(String* host, int port) {
    quirk_tls_lazy_init();
    if (!g_tls_ctx || !host || !host->buffer) return -1;

    char* safe_host = make_safe_cstr(host);
    if (!safe_host) return -1;

    struct hostent* server = gethostbyname(safe_host);
    if (!server) return -1;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(port);

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
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
    if (SSL_connect(ssl) != 1) {
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

int Net_tls_send(int handle, String* data) {
    if (handle < 0 || handle >= QUIRK_TLS_SLOTS) return -1;
    if (!g_tls_slots[handle].in_use) return -1;
    if (!data || !data->buffer) return 0;
    int n = SSL_write(g_tls_slots[handle].ssl, data->buffer, data->length);
    return n;
}

String* Net_tls_recv(int handle, int size) {
    if (handle < 0 || handle >= QUIRK_TLS_SLOTS) return make_String("");
    if (!g_tls_slots[handle].in_use) return make_String("");
    if (size <= 0) return make_String("");
    char* buffer = (char*)GC_malloc(size + 1);
    int n = SSL_read(g_tls_slots[handle].ssl, buffer, size);
    if (n <= 0) {
        // n=0 → clean close; n<0 → error or want-read. Either way
        // the caller's recv loop reads "no more bytes" the same as
        // plain TCP and stops.
        return make_String("");
    }
    buffer[n] = '\0';
    return make_String(buffer);
}

void Net_tls_close(int handle) {
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
