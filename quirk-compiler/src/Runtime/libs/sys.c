#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdint.h>

#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #define getcwd _getcwd
#else
    #include <unistd.h>
#endif

#include "../types.h"

static int quirk_argc = 0;
static char** quirk_argv = NULL;
static int current_recursion_limit = 1000;

// Called by LLVM at the very beginning of the program
void Sys_init(int argc, char** argv) {
    quirk_argc = argc;
    quirk_argv = argv;
}

// --- COMMAND LINE ARGS ---
int Sys_arg_count() { return quirk_argc; }
String* Sys_arg_get(int index) {
    if (index < 0 || index >= quirk_argc) return make_String("");
    return make_String(quirk_argv[index]);
}

// --- ENVIRONMENT & SYSTEM ---
String* Sys_getenv(String* name) {
    if (!name || !name->buffer) return make_String("");
    char* val = getenv(name->buffer);
    return make_String(val ? val : "");
}
int Sys_system(String* cmd) {
    if (!cmd || !cmd->buffer) return -1;
    return system(cmd->buffer);
}
void Sys_exit(int code) { exit(code); }
void Sys_sleep(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

// --- PYTHON-LIKE SYS FEATURES ---

String* Sys_byteorder() {
    uint32_t num = 1;
    if (*(uint8_t *)&num == 1) return make_String("little");
    return make_String("big");
}

String* Sys_copyright() {
    return make_String("Copyright (c) 2026 Quirk Contributors.");
}

String* Sys_executable() {
    // Returns the path used to invoke the binary
    if (quirk_argc > 0) return make_String(quirk_argv[0]);
    return make_String("");
}

String* Sys_getdefaultencoding() {
    return make_String("utf-8"); // Standard for modern environments
}

int Sys_getrecursionlimit() {
    return current_recursion_limit;
}

void Sys_setrecursionlimit(int limit) {
    current_recursion_limit = limit;
}

int Sys_getsizeof(void* obj) {
    if (!obj) return 0;
    // Magic Trick: The Boehm GC knows exactly how big this memory block is!
    extern size_t GC_size(const void*);
    return (int)GC_size(obj);
}

int Sys_maxsize() {
    // Quirk's 'Int' is 32-bit (i32 in LLVM), so maxsize is INT_MAX
    return INT_MAX; 
}

String* Sys_platform() {
#if defined(_WIN32)
    return make_String("win32");
#elif defined(__APPLE__)
    return make_String("darwin");
#elif defined(__linux__)
    return make_String("linux");
#else
    return make_String("unknown");
#endif
}

String* Sys_version() {
    return make_String("1.0.0 (Quirk LLVM Backend)");
}

String* Sys_prefix() {
    char* env_home = getenv("QUIRK_HOME");
    return make_String(env_home ? env_home : "/usr/local/lib/quirk");
}

// --- STANDARD STREAMS ---
File* Sys_stdin() {
    File* f = (File*)malloc(sizeof(File));
    f->handle = stdin;
    f->is_open = 1;
    return f;
}
File* Sys_stdout() {
    File* f = (File*)malloc(sizeof(File));
    f->handle = stdout;
    f->is_open = 1;
    return f;
}
File* Sys_stderr() {
    File* f = (File*)malloc(sizeof(File));
    f->handle = stderr;
    f->is_open = 1;
    return f;
}

// --- EXCEPTION INTERFACE ---
void* Sys_exc_info() {
    extern void* quirk_get_exception();
    return quirk_get_exception();
}

// --- UTILS ---
char* Sys_srcline(const char* filename, int target_line) {
    FILE* f = fopen(filename, "r");
    if (!f) return strdup("");
    char buffer[1024];
    int current = 1;
    while (fgets(buffer, sizeof(buffer), f)) {
        if (current == target_line) {
            fclose(f);
            char* start = buffer;
            while (*start == ' ' || *start == '\t') start++;
            size_t len = strlen(start);
            if (len > 0 && start[len-1] == '\n') start[len-1] = '\0';
            return strdup(start);
        }
        current++;
    }
    fclose(f);
    return strdup("");
}