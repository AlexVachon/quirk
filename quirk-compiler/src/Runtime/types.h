#ifndef TYPES_H
#define TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ===================================================
//  OBJECT HEADER (runtime type tags, phase 1 — v6.2.0)
// ===================================================
// Every runtime *value* object (String, List, Map, Set, Tuple, Any,
// Callable, iterators, user structs, ...) is allocated by quirk_alloc,
// which puts an 8-byte header in front of the returned pointer:
//
//     [ QuirkHeader @ p-8 ][ object fields @ p ... ]
//
// The object's own layout is unchanged (so `typing` struct sizes and
// every field offset stay the same); untyped code can read the header to
// learn what a pointer is instead of guessing from its first bytes.
// Boehm keeps the block alive through the interior pointer p.
// Internal buffers (character data, entry arrays) carry no header.
typedef struct {
    uint32_t magic;     // QUIRK_HDR_MAGIC — distinguishes headered objects
    uint32_t type_id;   // QuirkTypeId, or QT_STRUCT_BASE + user struct id
} QuirkHeader;

#define QUIRK_HDR_MAGIC 0x4B525551u   /* "QURK" */

enum QuirkTypeId {
    QT_NONE = 0,
    QT_STRING, QT_LIST, QT_MAP, QT_SET, QT_TUPLE, QT_ANY, QT_CALLABLE,
    QT_QUEUE, QT_EXCEPTION, QT_ITERATOR, QT_TYPE, QT_FILE, QT_OTHER,
    QT_CSTR = 14,          // raw NUL-terminated char* (selfhost String values)
    QT_STRUCT_BASE = 100   // user struct: QT_STRUCT_BASE + codegen type id
};

// Allocate `size` bytes of zeroed, GC-managed memory with a header.
void* quirk_alloc(uint32_t type_id, size_t size);

static inline const QuirkHeader* quirk_header_of(const void* p) {
    return (const QuirkHeader*)((const char*)p - sizeof(QuirkHeader));
}

// ===== CORE STRUCTS =====

typedef struct {
    char* buffer;
    int64_t length;
} String;

typedef struct {
    String* str_ref;
    int64_t idx;
} StringIterator;

typedef struct {
    void** data;
    int64_t size;
    int64_t capacity;
} List;

typedef struct {
    List* list_ref;
    int64_t idx;
} ListIterator;

// Map layout (v6.0.6) — an insertion-ordered "compact dict":
// `entries` is a dense array in insertion order (removed entries stay as
// is_occupied == 0 holes until the next rebuild), and `index` is an
// open-addressing table of 1-based positions into it (0 = empty,
// -1 = deleted). The struct is 32 bytes and keeps the offsets json.c's
// shape heuristic reads (entries @0, pow2 capacity @8, size @12, the
// first entry's is_occupied @16): `struct Map` in typing allocates
// exactly this size.
typedef struct {
    char* key;
    void* value;
    int is_occupied;
    unsigned int hash;     // cached FNV-1a of key
} MapEntry;

typedef struct {
    MapEntry* entries;     // dense, insertion order
    int64_t capacity;          // index slots (power of two, >= 8)
    int64_t size;              // live entries
    int32_t* index;        // slot -> entry position + 1; 0 empty, -1 deleted
    int64_t order_size;        // entries used in `entries` (incl. removed holes)
    int64_t order_capacity;    // allocated length of `entries`
} Map;

typedef struct {
    Map* map_ref;
    int64_t idx;  // index into key_order
} MapIterator;

typedef struct {
    Map* map_ref;
    int64_t idx;  // index into key_order
    void* current_value;
} MapPairIterator;

typedef struct {
    void* handle;
    int is_open;
} File;

typedef struct {
    const char* func_name;
    const char* file_name;
    int         line;
} ShadowFrame;

typedef struct {
    void* fn;   // i8* (*fn)(i8* env, i8* arg0, ...)
    void* env;  // captured variable struct, may be NULL
} Callable;

typedef struct {
    void** data;
    int64_t size;
} Tuple;

typedef struct {
    Tuple* tuple_ref;
    int64_t idx;
} TupleIterator;

// Set layout (v6.0.7) — same insertion-ordered compact design as Map:
// dense `entries` in insertion order (removed ones are is_occupied == 0
// holes until the next rebuild) plus an open-addressing `index` of
// 1-based positions (0 = empty, -1 = deleted). 32 bytes, with the
// offsets any.c's shape heuristic reads (entries @0, pow2 capacity @8,
// size @12); `struct Set` in typing allocates exactly this size.
typedef struct {
    char* key;       // string representation used for hashing/equality
    void* value;     // original boxed value
    int is_occupied;
    unsigned int hash;     // cached FNV-1a of key
} SetEntry;

typedef struct {
    SetEntry* entries;     // dense, insertion order
    int64_t capacity;          // index slots (power of two, >= 8)
    int64_t size;              // live entries
    int32_t* index;        // slot -> entry position + 1; 0 empty, -1 deleted
    int64_t order_size;        // entries used in `entries` (incl. removed holes)
    int64_t order_capacity;    // allocated length of `entries`
} Set;

typedef struct {
    Set* set_ref;
    int64_t idx;
} SetIterator;

typedef struct {
    void** data;
    int64_t head;       // index of front element
    int64_t tail;       // index past last element
    int64_t capacity;
    int64_t size;
} Queue;

typedef struct {
    Queue* queue_ref;
    int64_t pos;        // current iteration index (0-based)
} QueueIterator;

// ===================================================
//  Any — Tagged Union for dynamic typing
//
//  LLVM struct layout: { i32 tag, i64 ival, double dval, i8* ptr }
//  Offsets: 0, 8, 16, 24  —  total 32 bytes
// ===================================================

typedef enum {
    ANY_INT    = 0,   // ival holds int64
    ANY_DOUBLE = 1,   // dval holds double
    ANY_BOOL   = 2,   // ival holds 0 or 1
    ANY_CHAR   = 3,   // ival holds char code
    ANY_STRING = 4,   // ptr  holds String*
    ANY_LIST   = 5,   // ptr  holds List*
    ANY_MAP    = 6,   // ptr  holds Map*
    ANY_PTR      = 7,   // ptr  holds arbitrary struct*
    ANY_NULL     = 8,   // no value
    ANY_TUPLE    = 9,   // ptr  holds Tuple*
    ANY_CALLABLE = 10,  // ptr  holds Callable*
} AnyTag;

typedef struct {
    int32_t tag;   // AnyTag  — offset  0, size 4
    int64_t ival;  // Int/Bool/Char — offset  8, size 8
    double  dval;  // Double  — offset 16, size 8
    void*   ptr;   // String*/List*/Map*/ptr — offset 24, size 8
} Any;

// Type id from a value's QuirkHeader, or QT_NONE when it has none (tagged
// int, null, or an object not allocated by quirk_alloc — e.g. selfhost's
// raw C strings). Headered objects always sit at p ≡ 8 (mod 16): Boehm
// blocks are 16-aligned and the header is the first 8 bytes. So any other
// pointer is rejected without being dereferenced, and for p ≡ 8 the header
// lies in the same 16-byte granule as p — reading it can't fault.
static inline uint32_t quirk_type_of(const void* p) {
    uintptr_t u = (uintptr_t)p;
    if (u <= 0xFFFFFFFFUL || (u & 15) != 8) return QT_NONE;
    const QuirkHeader* h = quirk_header_of(p);
    return h->magic == QUIRK_HDR_MAGIC ? h->type_id : QT_NONE;
}

// Is this untyped value an Any* box? Decided by the header when there is
// one. Only header-less values fall back to the old guess (first 4 bytes
// in the AnyTag range), which misfires on any struct whose first field is
// a small int.
static inline int quirk_is_any(const void* p) {
    if (!p || (uintptr_t)p <= 0xFFFFFFFFUL) return 0;
    uint32_t t = quirk_type_of(p);
    if (t != QT_NONE) return t == QT_ANY;
    int32_t tag = *(const int32_t*)p;
    return tag >= ANY_INT && tag <= ANY_CALLABLE;
}


// ===================================================
//  UTILITY: Safe C-string extraction from String*
//  Defined here so all C modules can use it without
//  depending on sys.c include order.
// ===================================================

#include <gc.h>

static inline char* make_safe_cstr(String* s) {
    if (!s || !s->buffer) return NULL;
    char* safe = (char*)GC_malloc(s->length + 1);
    memcpy(safe, s->buffer, s->length);
    safe[s->length] = '\0';
    return safe;
}

#endif