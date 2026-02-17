#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

// --- EXCEPTION HANDLING RUNTIME ---
jmp_buf quirk_try_stack[256];
int quirk_try_depth = -1;
void* quirk_active_exception = NULL;

void* quirk_get_jmp_buf() {
    quirk_try_depth++;
    if (quirk_try_depth >= 256) {
        printf("Fatal: Try/Catch stack overflow!\n");
        exit(1);
    }
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
    // Return current buffer, then pop it so we don't infinitely loop
    return &quirk_try_stack[quirk_try_depth--];
}

void quirk_unhandled_exception() {
    printf("Fatal: Unhandled Exception!\n");
    exit(1);
}
