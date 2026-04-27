// [exceptions.c] — DEBUG BUILD
// Replace your exceptions.c with this temporarily to find the crash location.
// Every step prints to stderr so you can see exactly where it stops.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

// --- EXCEPTION HANDLING RUNTIME ---
jmp_buf quirk_try_stack[256];
int quirk_try_depth = -1;
void* quirk_active_exception = NULL;

extern int quirk_shadow_sp;
int quirk_saved_shadow_sp[256];

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
//   field 3: int     line   (+ 4 bytes natural padding)
//   field 4: String* callee
//   field 5: String* traceback
//   field 6: String* cause_trace
// -------------------------------------------------------

typedef struct {
    String* type;
    String* message;
    String* file;
    int32_t line;
    // 4 bytes of natural C padding here to align next pointer
    String* callee;
    String* traceback;
    String* cause_trace;
} QuirkException;

extern String* make_String(const char* raw);

void quirk_throw_exception(const char* type_name, const char* message) {
    QuirkException* exc = (QuirkException*)GC_malloc(sizeof(QuirkException));
    exc->type        = make_String(type_name);
    exc->message     = make_String(message);
    exc->file        = make_String("");
    exc->line        = 0;
    exc->callee      = make_String("");
    exc->traceback   = make_String("");
    exc->cause_trace = make_String("");

    quirk_active_exception = exc;

    if (quirk_try_depth >= 0) {
        quirk_shadow_sp = quirk_saved_shadow_sp[quirk_try_depth];
        jmp_buf* buf = &quirk_try_stack[quirk_try_depth];
        quirk_try_depth--;
        longjmp(*buf, 1);
    } else {
        fprintf(stderr, "%s: %s\n", type_name, message);
        exit(1);
    }
}