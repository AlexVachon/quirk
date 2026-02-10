#ifndef TYPES_H
#define TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===== PUBLIC API =====

typedef struct {
    char* buffer;  // Index 1
    int length;  // Index 0
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
    int current_idx;
} MapIterator;

typedef struct {
    void* handle;
    int is_open;
} File;

String* make_String(const char* raw);
String* make_String_taking_ownership(char* raw_buffer);

// --- ADD THIS LINE ---
String* String_join(String* self, List* items);

// ===== PRIMITIVE HELPERS =====
String* Int_str(int self);
String* Double_str(double self);

#endif