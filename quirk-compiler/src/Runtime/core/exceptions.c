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

char* quirk_get_source_line(const char* filename, int target_line) {
    FILE* f = fopen(filename, "r");
    if (!f) return strdup("");
    
    char buffer[1024];
    int current = 1;
    while (fgets(buffer, sizeof(buffer), f)) {
        if (current == target_line) {
            fclose(f);
            
            // Trim leading whitespace for clean Python-like formatting
            char* start = buffer;
            while (*start == ' ' || *start == '\t') start++;
            
            // Trim trailing newline
            size_t len = strlen(start);
            if (len > 0 && start[len-1] == '\n') start[len-1] = '\0';
            
            return strdup(start);
        }
        current++;
    }
    fclose(f);
    return strdup("");
}