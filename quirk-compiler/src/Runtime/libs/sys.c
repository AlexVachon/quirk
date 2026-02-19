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

// ==========================================
//  HELPER: Defined BEFORE use (Fixes Error)
// ==========================================
char* make_safe_cstr(String* s) {
    if (!s || !s->buffer) return NULL;
    char* safe = GC_malloc(s->length + 1);
    memcpy(safe, s->buffer, s->length);
    safe[s->length] = '\0'; 
    return safe;
}

// ==========================================
//  SYSTEM RUNTIME
// ==========================================

static int quirk_argc = 0;
static char** quirk_argv = NULL;

void Sys_init(int argc, char** argv) {
    quirk_argc = argc;
    quirk_argv = argv;
    #ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    #endif
}
char* gc_strdup(const char* s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char* new_s = GC_malloc(len);
    if (!new_s) {
        return NULL;
    }
    memcpy(new_s, s, len);
    return new_s;
}

char* Sys_srcline(const char* filename, int target_line) {
    if (!filename) {
        return gc_strdup("?");
    }
    FILE* f = fopen(filename, "r");
    if (!f) {
        return gc_strdup("");
    }

    char buffer[1024];
    int current = 1;
    while (fgets(buffer, sizeof(buffer), f)) {
        if (current == target_line) {
            fclose(f);
            char* start = buffer;
            while (*start == ' ' || *start == '\t') start++;
            size_t len = strlen(start);
            if (len > 0 && start[len-1] == '\n') start[len-1] = '\0';
            char* result = gc_strdup(start);
            return result;
        }
        current++;
    }
    fclose(f);
    return gc_strdup("");
}

// --- QUIRK SHADOW STACK ---
typedef struct {
    const char* func_name;
    const char* file_name;
} ShadowFrame;

static ShadowFrame quirk_shadow_stack[1024];
int quirk_shadow_sp = 0;

void quirk_push_frame(const char* func, const char* file) {
    if (quirk_shadow_sp < 1024) {
        quirk_shadow_stack[quirk_shadow_sp].func_name = func;
        quirk_shadow_stack[quirk_shadow_sp].file_name = file;
        quirk_shadow_sp++;
    }
}

void quirk_pop_frame() {
    if (quirk_shadow_sp > 0) quirk_shadow_sp--;
}

int Sys_shadow_size() {
    return quirk_shadow_sp;
}

String* Sys_shadow_frame(int index) {
    if (index < 0 || index >= quirk_shadow_sp) {
        return make_String("");
    }

    char buf[512];
    const char* fn = quirk_shadow_stack[index].func_name ? quirk_shadow_stack[index].func_name : "?";
    const char* fl = quirk_shadow_stack[index].file_name ? quirk_shadow_stack[index].file_name : "?";
    snprintf(buf, sizeof(buf), "%s (%s)", fn, fl);
    return make_String(buf);
}

// --- SYSTEM BUILTINS ---

int Sys_arg_count() {
    return quirk_argc;
}

String* Sys_arg_get(int index) {
    if (index < 0 || index >= quirk_argc) return make_String("");
    return make_String(quirk_argv[index]);
}

String* Sys_prefix() {
    return make_String("/usr/local"); 
}

String* Sys_version() {
    return make_String("1.0.0"); 
}

String* Sys_getenv(String* key) {
    // Safety check here too
    char* safe_key = make_safe_cstr(key);
    if (!safe_key) return make_String("");
    
    char* val = getenv(safe_key);
    return make_String(val ? val : "");
}

int Sys_system(String* cmd) {
    char* safe_cmd = make_safe_cstr(cmd);
    if (!safe_cmd) return -1;
    return system(safe_cmd);
}

void Sys_exit(int code) {
    exit(code);
}

// --- NETWORKING BUILTINS ---

int Sys_net_socket() {
    return socket(AF_INET, SOCK_STREAM, 0);
}

int Sys_net_bind(int sockfd, String* host, int port) {
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    
    char* safe_host = make_safe_cstr(host);
    
    serv_addr.sin_addr.s_addr = (safe_host && strlen(safe_host) > 0) 
                                ? inet_addr(safe_host) 
                                : INADDR_ANY;
    serv_addr.sin_port = htons(port);

    return bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
}

int Sys_net_listen(int sockfd, int backlog) {
    return listen(sockfd, backlog);
}

int Sys_net_accept(int sockfd) {
    struct sockaddr_in cli_addr;
    socklen_t clilen = sizeof(cli_addr);
    return accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
}

int Sys_net_connect(int sockfd, String* host, int port) {
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

    int result = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    if (result < 0) return result;

    // Set a 10-second receive timeout so recv() doesn't block forever
    // when the server is slow to close the connection.
#ifdef _WIN32
    DWORD timeout_ms = 10000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif

    return result;
}
int Sys_net_send(int sockfd, String* data) {
    if (!data || !data->buffer) return 0;
    return send(sockfd, data->buffer, data->length, 0);
}

String* Sys_net_recv(int sockfd, int size) {
    char* buffer = GC_malloc(size + 1);
    int n = recv(sockfd, buffer, size, 0);
    if (n <= 0) {
        // n == 0: connection closed cleanly by peer
        // n <  0: error or timeout (SO_RCVTIMEO expired → EAGAIN/EWOULDBLOCK/WSAETIMEDOUT)
        // Either way, signal end-of-data to the caller so the read loop can break.
        return make_String("");
    }
    buffer[n] = '\0';
    return make_String(buffer);
}

void Sys_net_close(int sockfd) {
    #ifdef _WIN32
        closesocket(sockfd);
    #else
        close(sockfd);
    #endif
}