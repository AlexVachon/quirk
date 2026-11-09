// [exceptions.c] — try/catch runtime, C-side throws and traceback formatting.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "../types.h"

// --- EXCEPTION HANDLING RUNTIME ---
// Per-thread state (call stack, try stack, exception in flight): see
// QuirkThreadState in types.h. The main thread's is this static one.
static QuirkThreadState quirk_main_ts = { .try_depth = -1 };
#if defined(__GNUC__) && !defined(__APPLE__)
__thread QuirkThreadState* quirk_ts __attribute__((tls_model("initial-exec"))) = &quirk_main_ts;
#else
__thread QuirkThreadState* quirk_ts = &quirk_main_ts;
#endif

void* quirk_get_jmp_buf() {
    quirk_try_depth++;
    if (quirk_try_depth >= 256) {
        fprintf(stderr, "[EXC] FATAL: Try/Catch stack overflow!\n");
        exit(1);
    }
    quirk_saved_shadow_sp[quirk_try_depth] = quirk_shadow_sp;
    return &quirk_try_stack[quirk_try_depth];
}

void quirk_pop_try() {
    quirk_try_depth--;
}

void quirk_set_exception(void* exc) {
    quirk_active_exception = exc;
}

void* quirk_get_exception() {
    return quirk_active_exception;
}

int quirk_get_try_depth() {
    return quirk_try_depth;
}

void* quirk_get_current_jmp_buf() {
    quirk_shadow_sp = quirk_saved_shadow_sp[quirk_try_depth];
    return &quirk_try_stack[quirk_try_depth];
}

void quirk_unhandled_exception() {
    exit(1);
}

// -------------------------------------------------------
// C-side exception thrower.
// Memory layout mirrors the Quirk Exception struct exactly:
//   field 0: String* type
//   field 1: String* message
//   field 2: String* file
//   field 3: Int     line   (int64_t)
//   field 4: String* callee
//   field 5: String* traceback
//   field 6: String* cause_trace
// -------------------------------------------------------

typedef struct {
    String* type;
    String* message;
    String* file;
    int64_t line;
    String* callee;
    String* traceback;
    String* cause_trace;
} QuirkException;

extern String* make_String(const char* raw);

// --- Source-line cache for tracebacks ---------------------------------
// A caught exception in a loop captures a traceback every iteration; keep
// each file's text in memory so that costs no file I/O after the first.
typedef struct {
    char*  path;
    char*  text;     // whole file, '\n' replaced by '\0'
    char** lines;    // lines[i] = start of line i+1
    int    count;
} SrcFile;
static SrcFile quirk_src_cache[16];
static int quirk_src_cache_n = 0;

static const char* quirk_src_line(const char* path, int line) {
    if (!path || !*path || line <= 0) return NULL;
    SrcFile* sf = NULL;
    for (int i = 0; i < quirk_src_cache_n; i++)
        if (strcmp(quirk_src_cache[i].path, path) == 0) { sf = &quirk_src_cache[i]; break; }
    if (!sf) {
        FILE* f = fopen(path, "rb");
        if (!f) return NULL;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n < 0 || n > (8L << 20)) { fclose(f); return NULL; }
        char* text = (char*)malloc((size_t)n + 1);
        size_t got = fread(text, 1, (size_t)n, f);
        fclose(f);
        text[got] = '\0';
        int count = 1;
        for (size_t i = 0; i < got; i++) if (text[i] == '\n') count++;
        char** lines = (char**)malloc(sizeof(char*) * (size_t)count);
        int li = 0;
        lines[li++] = text;
        for (size_t i = 0; i < got; i++) {
            if (text[i] == '\n') {
                text[i] = '\0';
                if (i > 0 && text[i - 1] == '\r') text[i - 1] = '\0';
                if (li < count) lines[li++] = text + i + 1;
            }
        }
        int slot = quirk_src_cache_n < 16 ? quirk_src_cache_n++ : 15;
        if (slot == 15 && quirk_src_cache[15].path) {
            free(quirk_src_cache[15].path); free(quirk_src_cache[15].text); free(quirk_src_cache[15].lines);
        }
        quirk_src_cache[slot].path  = strdup(path);
        quirk_src_cache[slot].text  = text;
        quirk_src_cache[slot].lines = lines;
        quirk_src_cache[slot].count = li;
        sf = &quirk_src_cache[slot];
    }
    if (line > sf->count) return NULL;
    const char* l = sf->lines[line - 1];
    while (*l == ' ' || *l == '\t') l++;
    return l;
}

static int quirk_format_one(char* buf, int cap, const char* file, int line, const char* fn) {
    if (cap <= 0) return 0;
    int w;
    if (line > 0)
        w = snprintf(buf, (size_t)cap, "  File \"%s\", line %d, in %s\n", file, line, fn);
    else
        w = snprintf(buf, (size_t)cap, "  File \"%s\", in %s\n", file, fn);
    if (w < 0) return 0;
    if (w >= cap) return cap - 1;
    const char* src = quirk_src_line(file, line);
    if (src && *src) {
        int w2 = snprintf(buf + w, (size_t)(cap - w), "    %s\n", src);
        if (w2 > 0) w += (w2 >= cap - w) ? cap - w - 1 : w2;
    }
    return w;
}

int quirk_format_frames(char* buf, int cap, int from, int to) {
    int pos = 0;
    if (cap > 0) buf[0] = '\0';
    if (from < 0) from = 0;
    if (to > quirk_shadow_sp) to = quirk_shadow_sp;
    for (int i = from; i < to && pos < cap - 1; i++) {
        ShadowFrame* f = &quirk_shadow_stack[i];
        int ln = f->cur_line > 0 ? f->cur_line : f->line;
        pos += quirk_format_one(buf + pos, cap - pos,
                                f->file_name ? f->file_name : "?", ln,
                                f->func_name ? f->func_name : "?");
    }
    return pos;
}

// Frames [0, to), eliding the middle of deep stacks (runaway recursion):
// up to 20 are shown in full, otherwise the oldest 3 and the newest 8.
int quirk_format_frames_elided(char* buf, int cap, int to) {
    if (to <= 20) return quirk_format_frames(buf, cap, 0, to);
    int pos = quirk_format_frames(buf, cap, 0, 3);
    int w = snprintf(buf + pos, (size_t)(cap - pos), "  ... %d more frames ...\n", to - 11);
    if (w > 0 && w < cap - pos) pos += w;
    pos += quirk_format_frames(buf + pos, cap - pos, to - 8, to);
    return pos;
}

// The traceback string holds every frame *except* the innermost one,
// oldest first; the exception's own file / line / callee describe the
// innermost frame (that is how Exception.print_traceback lays it out).
void quirk_capture_traceback(void* exc_ptr) {
    QuirkException* exc = (QuirkException*)exc_ptr;
    char buf[16384];
    quirk_format_frames_elided(buf, sizeof(buf), quirk_shadow_sp - 1);
    exc->traceback = make_String(buf);
    // Name the throwing function as tracebacks show it (`P.label`), not by
    // its LLVM symbol (`Mod$P_label`).
    if (quirk_shadow_sp > 0 && quirk_shadow_stack[quirk_shadow_sp - 1].func_name)
        exc->callee = make_String(quirk_shadow_stack[quirk_shadow_sp - 1].func_name);
}

static const char* qs_cstr(String* s) { return (s && s->buffer) ? s->buffer : ""; }

// `Exception.print_traceback()` (typing's exceptions/base.quirk): to stderr
// like an uncaught exception's, after anything already printed to stdout.
void Core_Exceptions_Base__eprint(String* text) {
    fflush(stdout);
    fputs(text && text->buffer ? text->buffer : "", stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

void quirk_print_uncaught(void* exc_ptr) {
    QuirkException* exc = (QuirkException*)exc_ptr;
    fflush(stdout);
    if (!exc) { fprintf(stderr, "Uncaught exception\n"); return; }
    if (exc->cause_trace) fputs(qs_cstr(exc->cause_trace), stderr);
    const char* file = qs_cstr(exc->file);
    const char* tb   = qs_cstr(exc->traceback);
    if (*tb || *file) {
        fputs("Traceback (most recent call last):\n", stderr);
        fputs(tb, stderr);
        if (*file) {
            char one[2048];
            const char* callee = qs_cstr(exc->callee);
            quirk_format_one(one, sizeof(one), file, (int)exc->line, *callee ? callee : "?");
            fputs(one, stderr);
        }
    }
    fprintf(stderr, "%s: %s\n", qs_cstr(exc->type), qs_cstr(exc->message));
}

void quirk_throw_exception(const char* type_name, const char* message) {
    QuirkException* exc = (QuirkException*)quirk_alloc(QT_EXCEPTION, sizeof(QuirkException));
    exc->type        = make_String(type_name);
    exc->message     = make_String(message);
    // The innermost Quirk frame is the code that called into the runtime.
    ShadowFrame* top = quirk_shadow_sp > 0 ? &quirk_shadow_stack[quirk_shadow_sp - 1] : NULL;
    exc->file        = make_String(top && top->file_name ? top->file_name : "");
    exc->line        = top ? (top->cur_line > 0 ? top->cur_line : top->line) : 0;
    exc->callee      = make_String(top && top->func_name ? top->func_name : "");
    exc->traceback   = make_String("");
    exc->cause_trace = make_String("");

    quirk_capture_traceback(exc);
    quirk_active_exception = exc;

    if (quirk_try_depth >= 0) {
        quirk_shadow_sp = quirk_saved_shadow_sp[quirk_try_depth];
        jmp_buf* buf = &quirk_try_stack[quirk_try_depth];
        quirk_try_depth--;
        longjmp(*buf, 1);
    } else {
        quirk_print_uncaught(exc);
        exit(1);
    }
}