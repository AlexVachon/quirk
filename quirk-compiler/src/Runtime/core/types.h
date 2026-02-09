#ifndef TYPES_H
#define TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===== PUBLIC API =====

typedef struct {
    int length;    // Index 0
    char* buffer;  // Index 1
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
    void* handle;
    int is_open;
} File;

// Helper to create objects from C
String* make_String(const char* raw);
String* make_String_taking_ownership(char* raw_buffer);

// ===== PRIMITIVE HELPERS (Optional but recommended) =====
String* Int_str(int self);
String* Double_str(double self);

#endif