#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"

// Constants for optimization
#define DEFAULT_CAPACITY 8
#define GROWTH_FACTOR 2

// ==========================================
//  LIST ITERATOR
// ==========================================

void Core_Collections_List_ListIterator___init(ListIterator* self, List* l) {
    self->list_ref = l;
    self->idx = 0;
}

int Core_Collections_List_ListIterator___has_next(ListIterator* self) {
    // Fast fail checks
    if (!self || !self->list_ref) return 0;
    return self->idx < self->list_ref->size;
}

void* Core_Collections_List_ListIterator___next(ListIterator* self) {
    // Unsafe access (faster) - assumes has_next() was checked
    // or add safety check back if stability is preferred over raw speed
    void* val = self->list_ref->data[self->idx];
    self->idx++;
    return val;
}

// ==========================================
//  LIST LIFECYCLE
// ==========================================

// OPTIMIZED: Removed 'initial_cap' argument to support 'List()' constructor calls
// from the compiler. Sets a sensible default capacity.
void Core_Collections_List_List___init(List* self) {
    self->size = 0;
    self->capacity = DEFAULT_CAPACITY;
    self->data = (void**)malloc(sizeof(void*) * self->capacity);
    
    if (!self->data) {
        fprintf(stderr, "Fatal: Out of memory in List initialization\n");
        exit(1);
    }
}

void Core_Collections_List_List___del(List* self) {
    if (self->data) {
        free(self->data);
        self->data = NULL;
    }
    self->size = 0;
    self->capacity = 0;
}

// ==========================================
//  CORE METHODS
// ==========================================

// Internal helper for resizing
static void List__resize(List* self, int new_cap) {
    void** new_data = (void**)realloc(self->data, sizeof(void*) * new_cap);
    if (!new_data) {
        fprintf(stderr, "Fatal: Out of memory during List resize\n");
        exit(1);
    }
    self->data = new_data;
    self->capacity = new_cap;
}

// NEW: Manual optimization hook.
// Can be exposed to Quirk as `list.reserve(1000)`
void Core_Collections_List_List_ensure_capacity(List* self, int min_capacity) {
    if (min_capacity > self->capacity) {
        List__resize(self, min_capacity);
    }
}

void Core_Collections_List_List_append(List* self, void* item) {
    // Amortized O(1) growth
    if (self->size == self->capacity) {
        List__resize(self, self->capacity * GROWTH_FACTOR);
    }
    self->data[self->size++] = item;
}

void* Core_Collections_List_List_pop(List* self) {
    if (self->size == 0) return 0;
    
    self->size--;
    return self->data[self->size];
    // Note: We do NOT shrink the array here.
    // Keeping the capacity high makes future appends faster.
}

// Inline candidate (if LTO enabled)
int Core_Collections_List_List_length(List* self) {
    return self->size;
}

void Core_Collections_List_List_clear(List* self) {
    // Fast clear: just reset length. O(1).
    self->size = 0;
}

int Core_Collections_List_List_is_empty(List* self) {
    return self->size == 0;
}

// ==========================================
//  OPERATOR OVERLOADING
// ==========================================

ListIterator* Core_Collections_List_List___iter(List* self) {
    ListIterator* iter = (ListIterator*)malloc(sizeof(ListIterator));
    if (!iter) exit(1);
    Core_Collections_List_ListIterator___init(iter, self);
    return iter;
}

void* Core_Collections_List_List___get(List* self, int index) {
    // Handle negative indexing (Python-style)
    if (index < 0) index += self->size;

    // Bounds check
    if (index < 0 || index >= self->size) {
        printf("IndexError: list index out of range (index: %d, len: %d)\n", index, self->size);
        exit(1);
    }
    return self->data[index];
}

void Core_Collections_List_List___set(List* self, int index, void* item) {
    if (index < 0) index += self->size;

    if (index < 0 || index >= self->size) {
        printf("IndexError: list assignment index out of range\n");
        exit(1);
    }
    self->data[index] = item;
}

// ==========================================
//  STRING REPRESENTATION
// ==========================================

// Helper required to bridge C strings to Quirk strings
extern String* make_String_taking_ownership(char*);
extern String* make_String(const char*);

String* Core_Collections_List_List___repr(List* self) {
    if (self->size == 0) {
        return make_String("[]");
    }

    // 1. Create the separator
    // Optimization: Stack allocate small constant strings if make_String supports it, 
    // otherwise generic allocation.
    String* sep = make_String(", ");

    // 2. Join the list items
    // This assumes Core_String_String_join is efficient
    String* content = Core_String_String_join(sep, self);

    // 3. Fast concatenation
    // We calculate exact size to do 1 malloc instead of multiple
    // Format: "[" + content + "]" + null_terminator
    int total_len = 1 + content->length + 1;
    char* raw = (char*)malloc(total_len + 1);
    
    if (!raw) exit(1);

    raw[0] = '[';
    // content->buffer assumed to be null-terminated char*
    memcpy(raw + 1, content->buffer, content->length);
    raw[total_len - 1] = ']';
    raw[total_len] = '\0';

    // Cleanup
    // (Assuming String struct needs freeing but buffer is managed by runtime/GC)
    // free(sep); 
    // free(content); 

    return make_String_taking_ownership(raw);
}

String* Core_Collections_List_List___str(List* self) {
    return Core_Collections_List_List___repr(self);
}