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
    int length;
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
    void* handle;
    int is_open;
} File;

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
    ANY_PTR    = 7,   // ptr  holds arbitrary struct*
    ANY_NULL   = 8,   // no value
} AnyTag;

typedef struct {
    int32_t tag;   // AnyTag  — offset  0, size 4
    int32_t ival;  // Int/Bool/Char — offset  4, size 4
    double  dval;  // Double  — offset  8, size 8
    void*   ptr;   // String*/List*/Map*/ptr — offset 16, size 8
} Any;

// ===== CONSTRUCTORS =====
String* make_String(const char* raw);
String* make_String_taking_ownership(char* raw_buffer);
String* String_join(String* self, List* items);

// ===== PRIMITIVES =====
String* Int_str(int self);
String* Double_str(double self);

// ===== ANY RUNTIME =====
Any* box_int(int32_t v);
Any* box_double(double v);
Any* box_bool(int32_t v);
Any* box_char(int32_t v);
Any* box_string(String* v);
Any* box_list(List* v);
Any* box_map(Map* v);
Any* box_ptr(void* v);
Any* box_null(void);

String* Any_to_string(Any* a);
String* Any_to_str(Any* a);
String* Any_get_type(Any* a);
int32_t Any_to_int(Any* a);
double  Any_to_float(Any* a);
int     Any_isinstance(Any* a, String* type_name);

#endif