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

typedef struct {
    char* key;
    void* value;
    int is_occupied;
    int is_deleted;
} MapEntry;

typedef struct {
    MapEntry* entries;
    int capacity;
    int size;
} Map;

typedef struct {
    Map* map_ref;
    int idx;
} MapIterator;

typedef struct {
    Map* map_ref;
    int idx;
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