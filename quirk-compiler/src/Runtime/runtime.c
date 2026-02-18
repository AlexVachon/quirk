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

#include "types.h"

#include "core/string.c"
#include "core/primitives.c"
#include "core/list.c"
#include "core/map.c"
#include "core/exceptions.c"

#include "libs/file.c"
#include "libs/sys.c"

#include "libs/encoding/json.c"
#include "libs/encoding/base64.c"
#include "libs/encoding/hex.c"


void QuirkRuntime_init(int argc, char** argv) {
    GC_init();             // Initialize Boehm Garbage Collector
    Sys_init(argc, argv);  // Initialize OS Arguments
    
    // As you add more libraries that need startup logic (e.g., Network_init),
    // you simply add them here. No C++ compiler changes required!
}