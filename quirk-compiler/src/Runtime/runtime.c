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
#include "libs/debug.c"

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

// Coercion-friendly variant of quirk_opaque_to_string used at function-
// argument boundaries: when the value is *literally null* at runtime,
// return the null pointer (cast to String*) so the receiver's
// `s != null` guard keeps working. Other shapes route through the
// standard helper. The stringification-shaped callers (print, debug,
// collection display) still want `null → "null"` and stay on the
// original helper.
String* quirk_opaque_to_string_or_null(void* val) {
    if (!val) return NULL;
    return quirk_opaque_to_string(val);
}

// Coerce an opaque i8* (whatever shape it has) to an int32. Same
// heuristic as quirk_opaque_to_string but for the Int target:
//   null                 → 0       (legit "no value" fallback)
//   uval ≤ 0xFFFFFFFF    → (int32)uval        (tagged int — the common
//                                              case for list[i] of Ints)
//   Any*-tagged ANY_INT  → a->ival
//   Any*-tagged ANY_DOUBLE/CHAR/BOOL → coerce
//   Any*-tagged anything else → 0
// Used by field assignments / typed-walrus when the RHS is opaque.
int32_t quirk_opaque_to_int(void* val) {
    if (!val) return 0;
    uintptr_t u = (uintptr_t)val;
    if (u <= 0xFFFFFFFFUL) return (int32_t)u;
    int32_t possible_tag = *(int32_t*)val;
    if (possible_tag >= ANY_INT && possible_tag <= ANY_NULL) {
        Any* a = (Any*)val;
        switch (a->tag) {
            case ANY_INT:
            case ANY_BOOL:
            case ANY_CHAR:   return a->ival;
            case ANY_DOUBLE: return (int32_t)a->dval;
            default:         return 0;
        }
    }
    return 0;
}

double quirk_opaque_to_double(void* val) {
    if (!val) return 0.0;
    uintptr_t u = (uintptr_t)val;
    if (u <= 0xFFFFFFFFUL) return (double)(int32_t)u;
    int32_t possible_tag = *(int32_t*)val;
    if (possible_tag >= ANY_INT && possible_tag <= ANY_NULL) {
        Any* a = (Any*)val;
        switch (a->tag) {
            case ANY_DOUBLE: return a->dval;
            case ANY_INT:
            case ANY_BOOL:
            case ANY_CHAR:   return (double)a->ival;
            default:         return 0.0;
        }
    }
    return 0.0;
}

// Generic Any → struct* coercion for the non-String collection types
// (List, Map, Tuple, Callable). Throws TypeError when the runtime value
// can't possibly be the expected struct — e.g. an Int laundered through
// `Any` arriving at a `: List` parameter. Without this, the v2.2.7
// String fix's twin hazard surfaced as a SIGSEGV at the first method
// dispatch (the receiver bitcast a tagged-int pointer to a real struct
// pointer and dereferenced address 42 or similar).
//
//   null in                → null out (preserves legit nullability)
//   uval ≤ 0xFFFFFFFF      → tagged int  → throw TypeError
//   Any*-tagged (0..NULL)  → matching tag: unwrap; else: throw TypeError
//   otherwise              → assume direct struct ptr (the common case;
//                            the low 32 bits of a real heap pointer are
//                            ~always above the AnyTag range, so the
//                            heuristic is safe in practice)
//
// `type_name` is purely for the error message — e.g. "List", "Map".
// Untagged variant for struct types without a dedicated AnyTag
// (Set, Queue, File, user-defined structs). Rejects the two
// obviously-wrong opaque shapes (tagged int, Any* heap wrap) but
// trusts the "direct struct ptr" path — the same heuristic the
// String / List / Map / Tuple / Callable unwraps use, minus the
// per-tag validation. Without this, an Any-laundered Int into a
// `: Set` slot used to land at address 42 and SIGSEGV on .length().
void* quirk_opaque_check_struct_or_null(void* val, const char* type_name) {
    if (!val) return NULL;
    uintptr_t u = (uintptr_t)val;
    if (u <= 0xFFFFFFFFUL) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "expected %s but got Int (laundered through Any-typed parameter)",
                 type_name ? type_name : "<struct>");
        quirk_throw_exception("TypeError", buf);
        return NULL;
    }
    int32_t possible_tag = *(int32_t*)val;
    if (possible_tag >= ANY_INT && possible_tag <= ANY_NULL) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "expected %s but got Any-wrapped value",
                 type_name ? type_name : "<struct>");
        quirk_throw_exception("TypeError", buf);
        return NULL;
    }
    return val;
}

void* quirk_opaque_unwrap_or_null(void* val, int32_t expected_tag,
                                  const char* type_name) {
    if (!val) return NULL;
    uintptr_t u = (uintptr_t)val;
    if (u <= 0xFFFFFFFFUL) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "expected %s but got Int (laundered through Any-typed parameter)",
                 type_name ? type_name : "<struct>");
        quirk_throw_exception("TypeError", buf);
        return NULL;  // unreachable
    }
    int32_t possible_tag = *(int32_t*)val;
    if (possible_tag >= ANY_INT && possible_tag <= ANY_NULL) {
        Any* a = (Any*)val;
        if (a->tag == expected_tag) return a->ptr;
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "expected %s but got differently-tagged Any value",
                 type_name ? type_name : "<struct>");
        quirk_throw_exception("TypeError", buf);
        return NULL;  // unreachable
    }
    return val;
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

// ============================================================
//  Backed Enum lookup / value getter helpers.
//
//  `enum Gender(String) { Male, Female = "F", Other }` compiles to:
//   - the existing i32-ordinal codegen (Male=0, Female=1, Other=2)
//   - a packed values blob:  "Male\0F\0Other\0"
//   - a count: 3
//   - a name: "Gender" (used in error messages)
//
//  `Gender("F")` emits a call to quirk_enum_lookup_str — returns 1
//  (Female), or throws ValueError if no variant matches.
//
//  `g.value` (where g: Gender) emits a call to quirk_enum_value_str —
//  given the ordinal, returns the variant's backing String*.
//
//  Same shape for Int-backed enums (`enum Status(Int) { OK=200,
//  NotFound=404 }`) — packed is a plain int32_t[].
// ============================================================

// Walk the packed null-separated list, returning the index of the
// matching entry, or -1 if none. `packed` is laid out as
// "<v0>\0<v1>\0...<vN-1>\0".
static int32_t enum_str_index_of(const char* needle, const char* packed, int32_t count) {
    if (!needle || !packed) return -1;
    const char* p = packed;
    for (int32_t i = 0; i < count; i++) {
        if (strcmp(needle, p) == 0) return i;
        p += strlen(p) + 1;
    }
    return -1;
}

extern void quirk_throw_exception(const char* type_name, const char* message);

int32_t quirk_enum_lookup_str(String* query, const char* packed,
                              int32_t count, const char* enum_name) {
    const char* q = (query && query->buffer) ? query->buffer : "";
    int32_t idx = enum_str_index_of(q, packed, count);
    if (idx >= 0) return idx;
    char buf[256];
    snprintf(buf, sizeof(buf), "'%s' is not a valid %s",
             q, enum_name ? enum_name : "enum");
    quirk_throw_exception("ValueError", buf);
    return -1;  // unreachable
}

int32_t quirk_enum_lookup_int(int32_t query, const int32_t* packed,
                              int32_t count, const char* enum_name) {
    for (int32_t i = 0; i < count; i++) {
        if (packed[i] == query) return i;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%d is not a valid %s",
             query, enum_name ? enum_name : "enum");
    quirk_throw_exception("ValueError", buf);
    return -1;
}

// `ordinal` is in-range by construction (it came from typed enum codegen).
// `packed` holds the variant's backing values; this just extracts the
// right slice. Out-of-range returns a safe default rather than UB.
String* quirk_enum_value_str(int32_t ordinal, const char* packed, int32_t count) {
    if (ordinal < 0 || ordinal >= count || !packed) return make_String("");
    const char* p = packed;
    for (int32_t i = 0; i < ordinal; i++) p += strlen(p) + 1;
    return make_String(p);
}

int32_t quirk_enum_value_int(int32_t ordinal, const int32_t* packed, int32_t count) {
    if (ordinal < 0 || ordinal >= count || !packed) return 0;
    return packed[ordinal];
}