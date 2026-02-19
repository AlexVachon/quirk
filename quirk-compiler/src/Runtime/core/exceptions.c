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
    fprintf(stderr, "[EXC] quirk_unhandled_exception called!\n");
    exit(1);
}