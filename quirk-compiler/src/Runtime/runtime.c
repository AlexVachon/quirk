// [runtime.c]
// The "Unity Build" approach: Include the C files directly.
#include <gc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// --- HIJACK MACROS ---
#define malloc(x) GC_malloc(x)
#define realloc(x, y) GC_realloc(x, y)
// GC_malloc does not zero-initialize; use a helper to preserve calloc semantics
static void* __gc_calloc(size_t n, size_t s) {
    void* p = GC_malloc(n * s);
    if (p) memset(p, 0, n * s);
    return p;
}
#define calloc(x, y) __gc_calloc((x), (y))
#define free(x)
// `strdup` lives in libc and bypasses Boehm — each call is a permanent
// leak when paired with our macro-no-op `free`. Route it through GC by
// copying via GC_malloc.
static char* __gc_strdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* out = (char*)GC_malloc(n + 1);
    if (out) memcpy(out, s, n + 1);
    return out;
}
#define strdup(s) __gc_strdup(s)

#include "types.h"  // NOTE: make_safe_cstr is defined inline in types.h
                    //       sys.c no longer needs its own definition.

// Note: Ensure these paths match your actual directory structure
#include "core/string.c"
#include "core/primitives.c"
#include "core/list.c"
#include "core/map.c"
#include "core/any.c"
#include "core/tuple.c"
#include "core/set.c"
#include "core/queue.c"
#include "core/callable.c"
#include "core/exceptions.c"

#include "libs/file.c"
#include "libs/sys.c"
#include "libs/net.c"
#include "libs/math.c"
#include "libs/fs.c"
#include "libs/time.c"
#include "libs/regex.c"
#include "libs/crypto.c"
#include "libs/random.c"

#include "libs/encoding/json.c"
#include "libs/encoding/base64.c"
#include "libs/encoding/hex.c"

extern void Random_init(void);

void QuirkRuntime_init(int argc, char** argv) {
    GC_INIT();             // <--- USE MACRO: Handles stack base detection automatically
    Sys_init(argc, argv);
    Random_init();          // seed the RNG from time+pid so each run differs
}

// Convert a boxed opaque value to String*.
// Handles three cases:
//   1. Tagged integer: pointer value <= 0xFFFFFFFF (inttoptr i32 -> i8*)
//   2. Any*: first 4 bytes are a valid AnyTag (0..ANY_NULL=8)
//   3. String*: first field is a heap pointer to char buffer
String* quirk_opaque_to_string(void* val) {
    if (!val) return make_String("null");
    uintptr_t uval = (uintptr_t)val;
    // Case 1: tagged integer
    if (uval <= 0xFFFFFFFFUL) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", (int)uval);
        return make_String(buf);
    }
    // Case 2: Any* — first 4 bytes are tag in 0..ANY_NULL
    int32_t possible_tag = *(int32_t*)val;
    if (possible_tag >= ANY_INT && possible_tag <= ANY_NULL) {
        return Core_Primitives_Any_to_string((Any*)val);
    }
    // Case 3: String* — use buffer directly
    String* s = (String*)val;
    return (s->buffer) ? s : make_String("null");
}

// Unbox an opaque i8* / Any* / tagged-int to a C boolean (0 or 1).
// Used by toBool() for Callable return values and any opaque condition.
int32_t quirk_any_as_bool(void* val) {
    if (!val) return 0;
    uintptr_t uval = (uintptr_t)val;
    if (uval <= 0xFFFFFFFFUL) return uval != 0;  // tagged int: 0 = false
    int32_t possible_tag = *(int32_t*)val;
    if (possible_tag >= ANY_INT && possible_tag <= ANY_NULL) {
        Any* a = (Any*)val;
        switch (a->tag) {
            case ANY_BOOL:   return a->ival != 0;
            case ANY_INT:    return a->ival != 0;
            case ANY_DOUBLE: return a->dval != 0.0;
            case ANY_NULL:   return 0;
            default:         return a->ptr != NULL;
        }
    }
    return 1;  // non-null non-Any pointer = truthy
}

// Print a boxed opaque value (i8* returned from Any-typed methods like map.get).
void quirk_print_opaque(void* val) {
    String* s = quirk_opaque_to_string(val);
    printf("%s\n", s->buffer ? s->buffer : "");
}

// Return the type name of an opaque i8* value — safe version of Core_Primitives_Any_get_type
// that handles tagged integers without dereferencing invalid addresses.
String* quirk_opaque_get_type(void* val) {
    if (!val) return make_String("Null");
    uintptr_t uval = (uintptr_t)val;
    if (uval <= 0xFFFFFFFFUL) return make_String("Int");
    int32_t possible_tag = *(int32_t*)val;
    if (possible_tag >= ANY_INT && possible_tag <= ANY_NULL)
        return Core_Primitives_Any_get_type((Any*)val);
    // Assume String* (most common non-Any heap value in opaque slots)
    return make_String("String");
}