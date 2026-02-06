#ifndef TYPES_H
#define TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===== PUBLIC API (Simple and clean) =====
typedef struct {
    int length;
    char* buffer;
} String;

typedef struct {
    void** data;
    int length;
    int capacity;
} List;

typedef struct {
    void* handle;
    int is_open;
} File;

String* make_String(const char* raw);

#endif