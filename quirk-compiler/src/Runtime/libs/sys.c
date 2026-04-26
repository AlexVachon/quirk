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
//  SYSTEM RUNTIME
//  NOTE: make_safe_cstr is now in types.h
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
static char* gc_strdup(const char* s) {
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


// ==========================================
//  STANDARD STREAMS
// ==========================================

// Wraps a C FILE* handle in a Quirk File struct.
// is_open=2 signals to File_close that it should NOT fclose() a std stream.
static File* make_stream_file(FILE* handle) {
    File* f = (File*)GC_malloc(sizeof(File));
    f->handle = handle;
    f->is_open = 2; // sentinel: open but unmanaged (don't fclose)
    return f;
}

File* Sys_stdin()  { return make_stream_file(stdin);  }
File* Sys_stdout() { return make_stream_file(stdout); }
File* Sys_stderr() { return make_stream_file(stderr); }