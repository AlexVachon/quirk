// [sys.c] — DEBUG BUILD (add fprintf calls to track execution)
// Replace your sys.c temporarily with this to find the crash.

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
#else
    #include <unistd.h>
#endif

#ifndef QUIRK_TYPES_H
#include "../types.h"
#endif

static int quirk_argc = 0;
static char** quirk_argv = NULL;

void Sys_init(int argc, char** argv) {
    quirk_argc = argc;
    quirk_argv = argv;
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
    // Modify this if you want a specific installation prefix
    return make_String("/usr/local"); 
}

String* Sys_version() {
    // Your Quirk version
    return make_String("1.0.0"); 
}

String* Sys_getenv(String* key) {
    if (!key || !key->buffer) return make_String("");
    char* val = getenv((char*)key->buffer);
    return make_String(val ? val : "");
}

int Sys_system(String* cmd) {
    if (!cmd || !cmd->buffer) return -1;
    return system((char*)cmd->buffer);
}

void Sys_exit(int code) {
    exit(code);
}