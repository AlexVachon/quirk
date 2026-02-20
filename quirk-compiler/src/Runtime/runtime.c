// [runtime.c]
// The "Unity Build" approach: Include the C files directly.
#include <gc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// --- HIJACK MACROS ---
#define malloc(x) GC_malloc(x)
#define realloc(x, y) GC_realloc(x, y)
#define calloc(x, y) GC_malloc((x) * (y))
#define free(x)

#include "types.h"

// Note: Ensure these paths match your actual directory structure
#include "core/string.c"
#include "core/primitives.c"
#include "core/list.c"
#include "core/map.c"
#include "core/any.c"
#include "core/exceptions.c"

#include "libs/file.c"
#include "libs/sys.c" 

#include "libs/encoding/json.c"
#include "libs/encoding/base64.c"
#include "libs/encoding/hex.c"

void QuirkRuntime_init(int argc, char** argv) {
    GC_INIT();             // <--- USE MACRO: Handles stack base detection automatically
    Sys_init(argc, argv);  
}