#include <stdio.h>
#include <stdlib.h>
#include "../types.h"

// ==========================================
//  LIST ITERATOR
// ==========================================

void Core_Collections_List_ListIterator___init(ListIterator* self, List* l) {
    self->list_ref = l;
    self->idx = 0;
}

int Core_Collections_List_ListIterator___has_next(ListIterator* self) {
    if (!self || !self->list_ref)
        return 0;
    return self->idx < self->list_ref->length;
}

void* Core_Collections_List_ListIterator___next(ListIterator* self) {
    if (!self || !self->list_ref)
        return 0;
    void* val = self->list_ref->data[self->idx];
    self->idx++;
    return val;
}

// ==========================================
//  LIST LIFECYCLE
// ==========================================

void Core_Collections_List_List___init(List* self, int initial_cap) {
    if (initial_cap < 1)
        initial_cap = 1;
    self->capacity = initial_cap;
    self->length = 0;
    self->data = (void**)malloc(sizeof(void*) * initial_cap);
}

void Core_Collections_List_List___del(List* self) {
    if (self->data) {
        // Note: We do NOT free the items inside, because we don't know their
        // type. This is standard behavior for generic containers (shallow
        // delete).
        free(self->data);
        self->data = NULL;
    }
}

// ==========================================
//  CORE METHODS
// ==========================================

static void List__resize(List* self, int new_cap) {
    self->capacity = new_cap;
    self->data = (void**)realloc(self->data, sizeof(void*) * new_cap);
}

void Core_Collections_List_List_append(List* self, void* item) {
    if (self->length == self->capacity) {
        List__resize(self, self->capacity * 2);
    }
    self->data[self->length++] = item;
}

void* Core_Collections_List_List_pop(List* self) {
    if (self->length == 0)
        return 0;
    self->length--;
    return self->data[self->length];
}

int Core_Collections_List_List_length(List* self) {
    return self->length;
}

void Core_Collections_List_List_clear(List* self) {
    self->length = 0;
}

int Core_Collections_List_List_is_empty(List* self) {
    return self->length == 0;
}

// ==========================================
//  OPERATOR OVERLOADING
// ==========================================

ListIterator* Core_Collections_List_List___iter(List* self) {
    ListIterator* iter = (ListIterator*)malloc(sizeof(ListIterator));
    Core_Collections_List_ListIterator___init(iter, self);
    return iter;
}

void* Core_Collections_List_List___get(List* self, int index) {
    // Handle negative indexing
    if (index < 0)
        index = self->length + index;

    if (index < 0 || index >= self->length) {
        printf("IndexError: list index out of range (index: %d, len: %d)\n",
               index, self->length);
        exit(1);
    }
    return self->data[index];
}

void Core_Collections_List_List___set(List* self, int index, void* item) {
    if (index < 0)
        index = self->length + index;

    if (index < 0 || index >= self->length) {
        printf("IndexError: list assignment index out of range\n");
        exit(1);
    }
    self->data[index] = item;
}

// ==========================================
//  STRING REPRESENTATION
// ==========================================

// Uses Core_String_String_join from string.c to create "[a, b, c]"
String* Core_Collections_List_List___repr(List* self) {
    // 1. Create the separator ", "
    String* sep = make_String(", ");

    // 2. Join the list items
    String* content = Core_String_String_join(sep, self);

    // 3. Wrap in brackets
    String* open = make_String("[");
    String* close = make_String("]");

    // Manual concatenation (simulating open + content + close)
    // We assume String___add is available or use raw buffers

    // Fast path:
    int total_len = 1 + content->length + 1;
    char* raw = (char*)malloc(total_len + 1);
    strcpy(raw, "[");
    strcat(raw, content->buffer);
    strcat(raw, "]");

    // Cleanup temporary strings
    free(sep->buffer);
    free(sep);
    free(open->buffer);
    free(open);
    free(close->buffer);
    free(close);
    // Note: 'content' is wrapped in 'raw', but we need to free the struct
    // (Actual memory management depends on your GC strategy, for now we leak
    // small wrapper structs)

    // Helper from primitives.c or types.h
    extern String* make_String_taking_ownership(char*);
    return make_String_taking_ownership(raw);
}

String* Core_Collections_List_List___str(List* self) {
    return Core_Collections_List_List___repr(self);
}