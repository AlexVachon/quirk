#ifndef TYPES_H
#define TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ===== CORE STRUCTS =====

typedef struct {
    char* buffer;
    int length;
} String;

typedef struct {
    String* str_ref;
    int idx;
} StringIterator;

typedef struct {
    void** data;
    int size;
    int capacity;
} List;

typedef struct {
    List* list_ref;
    int idx;
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
    int capacity;          // index slots (power of two, >= 8)
    int size;              // live entries
    int32_t* index;        // slot -> entry position + 1; 0 empty, -1 deleted
    int order_size;        // entries used in `entries` (incl. removed holes)
    int order_capacity;    // allocated length of `entries`
} Map;

typedef struct {
    Map* map_ref;
    int idx;  // index into key_order
} MapIterator;

typedef struct {
    Map* map_ref;
    int idx;  // index into key_order
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
    int size;
} Tuple;

typedef struct {
    Tuple* tuple_ref;
    int idx;
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
    int capacity;          // index slots (power of two, >= 8)
    int size;              // live entries
    int32_t* index;        // slot -> entry position + 1; 0 empty, -1 deleted
    int order_size;        // entries used in `entries` (incl. removed holes)
    int order_capacity;    // allocated length of `entries`
} Set;

typedef struct {
    Set* set_ref;
    int idx;
} SetIterator;

typedef struct {
    void** data;
    int head;       // index of front element
    int tail;       // index past last element
    int capacity;
    int size;
} Queue;

typedef struct {
    Queue* queue_ref;
    int pos;        // current iteration index (0-based)
} QueueIterator;

// ===================================================
//  Any — Tagged Union for dynamic typing
//
//  LLVM struct layout: { i32 tag, i32 ival, double dval, i8* ptr }
//  Offsets: 0, 4, 8, 16  —  total 24 bytes
// ===================================================

typedef enum {
    ANY_INT    = 0,   // ival holds int32
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
    int32_t ival;  // Int/Bool/Char — offset  4, size 4
    double  dval;  // Double  — offset  8, size 8
    void*   ptr;   // String*/List*/Map*/ptr — offset 16, size 8
} Any;


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