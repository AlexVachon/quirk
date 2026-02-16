// The "Unity Build" approach: Include the C files directly.
// This is the simplest way to link everything into one .so without complex Makefiles.
#include <gc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// --- HIJACK MACROS GO *AFTER* STANDARD INCLUDES ---
#define malloc(x) GC_malloc(x)
#define realloc(x, y) GC_realloc(x, y)
#define calloc(x, y) GC_malloc((x) * (y))
#define free(x)

#include "core/types.h"

#include "core/string.c"
#include "core/primitives.c"
#include "core/list.c"
#include "core/map.c"

#include "core/exceptions.c"

#include "core/file.c"

char* String_add(char* a, char* b) {
    if (!a) a = "";
    if (!b) b = "";
    
    size_t lenA = strlen(a);
    size_t lenB = strlen(b);
    
    char* result = (char*)malloc(lenA + lenB + 1);
    if (!result) {
        fprintf(stderr, "Out of memory in String_add\n");
        exit(1);
    }
    
    strcpy(result, a);
    strcat(result, b);
    
    return result;
}