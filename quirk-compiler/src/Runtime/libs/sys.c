#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <signal.h>
#include <gc.h>

#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #include <io.h>
    #include <conio.h>
    #define getcwd _getcwd
    #define isatty _isatty
    #define fileno _fileno

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

// Print the Quirk shadow-stack trace and abort cleanly on fatal signals.
static void quirk_crash_handler(int sig) {
    const char* signame = (sig == SIGSEGV) ? "SIGSEGV (null dereference or bad memory access)"
                        : (sig == SIGABRT) ? "SIGABRT (abort / assertion failure)"
                        : (sig == SIGBUS)  ? "SIGBUS (bus error)"
                        : (sig == SIGFPE)  ? "SIGFPE (arithmetic error)"
                        :                    "fatal signal";
    fprintf(stderr, "\nQuirk runtime error: %s\n", signame);
    if (quirk_shadow_sp > 0) {
        fprintf(stderr, "Traceback (most recent call last):\n");
        for (int i = 0; i < quirk_shadow_sp; i++) {
            const char* fn = quirk_shadow_stack[i].func_name ? quirk_shadow_stack[i].func_name : "?";
            const char* fl = quirk_shadow_stack[i].file_name ? quirk_shadow_stack[i].file_name : "?";
            int ln = quirk_shadow_stack[i].line;
            if (ln > 0)
                fprintf(stderr, "  [%d] %s  (%s:%d)\n", i, fn, fl, ln);
            else
                fprintf(stderr, "  [%d] %s  (%s)\n", i, fn, fl);
        }
    } else {
        fprintf(stderr, "(no Quirk stack frames recorded)\n");
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void Sys_init(int argc, char** argv) {
    quirk_argc = argc;
    quirk_argv = argv;
    // Install crash handler for common fatal signals.
    // Boehm GC installs its own SIGSEGV handler for heap probing — it saves and
    // restores the previous handler, so our handler fires for genuine crashes only.
    signal(SIGSEGV, quirk_crash_handler);
    signal(SIGABRT, quirk_crash_handler);
    signal(SIGBUS,  quirk_crash_handler);
    signal(SIGFPE,  quirk_crash_handler);
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
#include "../types.h"

ShadowFrame quirk_shadow_stack[1024];
int quirk_shadow_sp = 0;

void quirk_push_frame(const char* func, const char* file, int line) {
    if (quirk_shadow_sp >= 1024) {
        // Stack overflow — print trace and abort with a clear message.
        fprintf(stderr, "\nQuirk runtime error: stack overflow\n");
        fprintf(stderr, "Traceback (most recent call last):\n");
        for (int i = 0; i < quirk_shadow_sp; i++) {
            const char* fn = quirk_shadow_stack[i].func_name ? quirk_shadow_stack[i].func_name : "?";
            const char* fl = quirk_shadow_stack[i].file_name ? quirk_shadow_stack[i].file_name : "?";
            int ln = quirk_shadow_stack[i].line;
            if (ln > 0)
                fprintf(stderr, "  [%d] %s  (%s:%d)\n", i, fn, fl, ln);
            else
                fprintf(stderr, "  [%d] %s  (%s)\n", i, fn, fl);
        }
        fprintf(stderr, "  ... (attempting to call: %s)\n", func ? func : "?");
        abort();
    }
    quirk_shadow_stack[quirk_shadow_sp].func_name = func;
    quirk_shadow_stack[quirk_shadow_sp].file_name = file;
    quirk_shadow_stack[quirk_shadow_sp].line      = line;
    quirk_shadow_sp++;
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
    int ln = quirk_shadow_stack[index].line;
    if (ln > 0)
        snprintf(buf, sizeof(buf), "%s (%s:%d)", fn, fl, ln);
    else
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

// Returns 1 if the given Quirk File wraps a terminal (interactive), 0 otherwise.
// Used by console.quirk to decide whether to emit ANSI color codes.
int Sys_isatty(File* f) {
    if (!f || !f->handle) return 0;
    return isatty(fileno((FILE*)f->handle)) ? 1 : 0;
}

// Returns the ANSI SGR sequence for the named color/effect.
// The Quirk lexer doesn't decode \x1b/\033, so console.quirk goes through
// this helper rather than embedding the escape byte in source files.
String* Sys_ansi(String* name) {
    if (!name || !name->buffer) return make_String("");
    const char* code = "";
    if      (strcmp(name->buffer, "reset")  == 0) code = "\x1b[0m";
    else if (strcmp(name->buffer, "bold")   == 0) code = "\x1b[1m";
    else if (strcmp(name->buffer, "dim")    == 0) code = "\x1b[2m";
    else if (strcmp(name->buffer, "red")    == 0) code = "\x1b[31m";
    else if (strcmp(name->buffer, "green")  == 0) code = "\x1b[32m";
    else if (strcmp(name->buffer, "yellow") == 0) code = "\x1b[33m";
    else if (strcmp(name->buffer, "blue")   == 0) code = "\x1b[34m";
    else if (strcmp(name->buffer, "cyan")   == 0) code = "\x1b[36m";
    else if (strcmp(name->buffer, "gray")   == 0) code = "\x1b[90m";
    else if (strcmp(name->buffer, "clear")  == 0) code = "\x1b[2J\x1b[H";
    return make_String(code);
}

// ---------------------------------------------------------------------------
//  Terminal extras: size, password input, single-key read, monotonic clock
// ---------------------------------------------------------------------------

#ifndef _WIN32
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#endif

// Terminal dimensions. Returns 80/24 fallbacks when stdout isn't a TTY or
// the syscall fails — keeps callers from special-casing piped output.
int Sys_terminal_cols() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 80;
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
#endif
}

int Sys_terminal_rows() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    return 24;
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row;
    return 24;
#endif
}

// Reads one line from stdin with echo disabled. Used for password prompts.
// On non-TTY input falls back to a normal read (so piped passwords still work).
String* Sys_read_password() {
    char buffer[2048];
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode & ~ENABLE_ECHO_INPUT);
    char* got = fgets(buffer, sizeof(buffer), stdin);
    SetConsoleMode(h, mode);
#else
    struct termios old, neu;
    int has_tty = isatty(fileno(stdin)) && tcgetattr(fileno(stdin), &old) == 0;
    if (has_tty) {
        neu = old;
        neu.c_lflag &= ~(unsigned)ECHO;
        tcsetattr(fileno(stdin), TCSANOW, &neu);
    }
    char* got = fgets(buffer, sizeof(buffer), stdin);
    if (has_tty) {
        tcsetattr(fileno(stdin), TCSANOW, &old);
        // Echo a newline since the user's CR didn't display.
        fputc('\n', stdout);
        fflush(stdout);
    }
#endif
    if (!got) return make_String("");
    buffer[strcspn(buffer, "\r\n")] = '\0';
    return make_String(buffer);
}

// Reads ONE keypress from stdin without waiting for Enter. Returns the
// character value as an Int (so callers can compare to e.g. 27 for ESC,
// 13 for Enter, 0x41 for 'A', or special codes from arrow keys via
// subsequent reads — Up = ESC '[' 'A', etc.).
int Sys_read_key() {
#ifdef _WIN32
    return _getch();
#else
    if (!isatty(fileno(stdin))) {
        int c = fgetc(stdin);
        return c == EOF ? -1 : c;
    }
    struct termios old, neu;
    if (tcgetattr(fileno(stdin), &old) != 0) return fgetc(stdin);
    neu = old;
    neu.c_lflag &= ~(unsigned)(ICANON | ECHO);
    neu.c_cc[VMIN]  = 1;
    neu.c_cc[VTIME] = 0;
    tcsetattr(fileno(stdin), TCSANOW, &neu);
    int c = fgetc(stdin);
    tcsetattr(fileno(stdin), TCSANOW, &old);
    return c == EOF ? -1 : c;
#endif
}

// Monotonic clock in milliseconds. Use deltas only — absolute value is
// not relative to the Unix epoch. Suitable for `console.time/time_end`.
int Sys_now_ms() {
#ifdef _WIN32
    return (int)GetTickCount();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    long long ms = (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    return (int)ms;
#endif
}

void Sys_sleep(int ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

// ---------------------------------------------------------------------------
//  Module-level state helpers (group depth, named timers).
//  Quirk doesn't currently support module-level mutable state in `.quirk` files
//  — these C-side static slots stand in for that until the language gains it.
// ---------------------------------------------------------------------------

static int sys_group_depth = 0;

int Sys_group_depth_get()  { return sys_group_depth; }
void Sys_group_depth_inc() { sys_group_depth++; }
void Sys_group_depth_dec() { if (sys_group_depth > 0) sys_group_depth--; }

#define SYS_TIMER_SLOTS 32
static struct { char name[64]; int start_ms; int active; } sys_timers[SYS_TIMER_SLOTS];

void Sys_timer_start(String* label) {
    if (!label || !label->buffer) return;
    int now = Sys_now_ms();
    int free_slot = -1;
    for (int i = 0; i < SYS_TIMER_SLOTS; i++) {
        if (sys_timers[i].active && strncmp(sys_timers[i].name, label->buffer, sizeof(sys_timers[i].name) - 1) == 0) {
            sys_timers[i].start_ms = now;
            return;
        }
        if (!sys_timers[i].active && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return;
    strncpy(sys_timers[free_slot].name, label->buffer, sizeof(sys_timers[free_slot].name) - 1);
    sys_timers[free_slot].name[sizeof(sys_timers[free_slot].name) - 1] = '\0';
    sys_timers[free_slot].start_ms = now;
    sys_timers[free_slot].active = 1;
}

// Returns elapsed ms for the named timer and clears the slot. Returns -1 if
// the timer wasn't started.
int Sys_timer_end(String* label) {
    if (!label || !label->buffer) return -1;
    for (int i = 0; i < SYS_TIMER_SLOTS; i++) {
        if (sys_timers[i].active && strncmp(sys_timers[i].name, label->buffer, sizeof(sys_timers[i].name) - 1) == 0) {
            int elapsed = Sys_now_ms() - sys_timers[i].start_ms;
            sys_timers[i].active = 0;
            sys_timers[i].name[0] = '\0';
            return elapsed;
        }
    }
    return -1;
}