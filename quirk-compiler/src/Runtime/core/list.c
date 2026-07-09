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
    // Defensive: selfhost's codegen sometimes forwards a null
    // (unknown-method returning null, uninitialised slot) to
    // `.append()`. Dereferencing self here would segfault
    // silently; a bare return matches the "no-op on missing
    // container" semantics the corpus expects.
    if (!self) return;
    // Amortized O(1) growth. If capacity is 0 (uninitialised
    // struct from selfhost — no runtime __init call ran),
    // `capacity * GROWTH_FACTOR` is also 0, realloc(NULL, 0)
    // is implementation-defined, and the resulting NULL data
    // pointer segfaults on the next index. Seed with a small
    // default (8) so the grow-then-write pattern always ends
    // with a valid allocation.
    // Zero-cap OR unreasonable-cap (uninitialised selfhost
    // struct): start fresh rather than realloc'ing an
    // unknown-provenance pointer. GC_realloc on a bogus
    // pointer segfaults immediately.
    if (self->capacity <= 0 || self->capacity > (1 << 24)) {
        self->capacity = 8;
        self->data = (void**)malloc(sizeof(void*) * 8);
        if (self->size < 0 || self->size > (1 << 24)) self->size = 0;
    } else if (self->size == self->capacity) {
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

extern void quirk_throw_exception(const char* type_name, const char* message);

void* Core_Collections_List_List___get(List* self, int index) {
    // Defensive: selfhost's codegen can hand us a null or
    // uninitialised List (unknown-method fallback, coerced
    // slot never routed through List___init). Return null
    // instead of dereferencing garbage fields.
    if (!self) return NULL;
    if (self->size < 0 || self->size > (1 << 24)) return NULL;
    if (!self->data) return NULL;
    if (index < 0) index += self->size;
    if (index < 0 || index >= self->size) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "list index %d out of range (size: %d)", index, self->size);
        quirk_throw_exception("IndexError", buf);
    }
    return self->data[index];
}

void Core_Collections_List_List___set(List* self, int index, void* item) {
    if (index < 0) index += self->size;
    if (index < 0 || index >= self->size) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "list assignment index %d out of range (size: %d)", index, self->size);
        quirk_throw_exception("IndexError", buf);
    }
    self->data[index] = item;
}

List* Core_Collections_List_List_slice(List* self, int start, int end) {
    if (!self) return NULL;
    int len = self->size;
    if (start < 0) start += len;
    if (end   < 0) end   += len;
    if (start < 0) start = 0;
    if (end > len) end = len;
    List* out = (List*)GC_malloc(sizeof(List));
    out->size = 0;
    out->capacity = (end > start) ? end - start : 0;
    out->data = out->capacity > 0
        ? (void**)GC_malloc(sizeof(void*) * out->capacity)
        : NULL;
    for (int i = start; i < end; i++)
        out->data[out->size++] = self->data[i];
    return out;
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

// ==========================================
//  MEMBERSHIP TEST
// ==========================================

// `elem in list` — works for String* (by value) and other pointer types (by identity).
// Tagged-integer i8* values are <= 0xFFFFFFFF on 64-bit systems; heap pointers are above.
int Core_Collections_List_List_contains(List* self, void* elem) {
    for (int i = 0; i < self->size; i++) {
        void* item = self->data[i];
        if (item == elem) return 1;
        if ((uintptr_t)item > 0xFFFFFFFFUL && (uintptr_t)elem > 0xFFFFFFFFUL) {
            String* s1 = (String*)item;
            String* s2 = (String*)elem;
            if (s1->buffer && s2->buffer && s1->length == s2->length &&
                memcmp(s1->buffer, s2->buffer, s1->length) == 0) return 1;
        }
    }
    return 0;
}

// ==========================================
//  FUNCTIONAL METHODS (lambda support)
// ==========================================

typedef void* (*LambdaFn1)(void* env, void* arg);
typedef void* (*LambdaFn2)(void* env, void* acc, void* arg);

// `xs * n` — fresh List with self's elements repeated n times.
// `[0] * 5` → `[0, 0, 0, 0, 0]`. n<=0 returns the empty list.
// Element values are shared (not deep-copied) — same shape as
// Python's list-repeat operator.
List* Core_Collections_List_List___mul(List* self, int n) {
    List* result = (List*)GC_malloc(sizeof(List));
    Core_Collections_List_List___init(result);
    if (!self || n <= 0 || self->size == 0) return result;
    Core_Collections_List_List_ensure_capacity(result, self->size * n);
    for (int rep = 0; rep < n; rep++) {
        for (int i = 0; i < self->size; i++)
            Core_Collections_List_List_append(result, self->data[i]);
    }
    return result;
}

// Append every element of `other` to `self`, mutating in place.
// Quirk: `xs.extend(ys)`. Returns void — Pythonic semantics, not
// a fluent chain. Use `__add` (below) if you want a fresh List.
void Core_Collections_List_List_append_all(List* self, List* other) {
    if (!self || !other) return;
    Core_Collections_List_List_ensure_capacity(self, self->size + other->size);
    for (int i = 0; i < other->size; i++)
        Core_Collections_List_List_append(self, other->data[i]);
}

// `xs + ys` — fresh List containing every element of self followed
// by every element of other. Neither input is mutated. Sema picks
// up the call via the `__add` dunder on the receiver type.
List* Core_Collections_List_List___add(List* self, List* other) {
    List* result = (List*)GC_malloc(sizeof(List));
    Core_Collections_List_List___init(result);
    if (!self && !other) return result;
    int total = (self ? self->size : 0) + (other ? other->size : 0);
    Core_Collections_List_List_ensure_capacity(result, total);
    if (self) {
        for (int i = 0; i < self->size; i++)
            Core_Collections_List_List_append(result, self->data[i]);
    }
    if (other) {
        for (int i = 0; i < other->size; i++)
            Core_Collections_List_List_append(result, other->data[i]);
    }
    return result;
}

List* Core_Collections_List_List_map(List* self, Callable* cb) {
    LambdaFn1 fn = (LambdaFn1)cb->fn;
    List* result = (List*)GC_malloc(sizeof(List));
    Core_Collections_List_List___init(result);
    for (int i = 0; i < self->size; i++)
        Core_Collections_List_List_append(result, fn(cb->env, self->data[i]));
    return result;
}

List* Core_Collections_List_List_filter(List* self, Callable* cb) {
    LambdaFn1 fn = (LambdaFn1)cb->fn;
    List* result = (List*)GC_malloc(sizeof(List));
    Core_Collections_List_List___init(result);
    for (int i = 0; i < self->size; i++)
        if (fn(cb->env, self->data[i]) != NULL)
            Core_Collections_List_List_append(result, self->data[i]);
    return result;
}

void Core_Collections_List_List_each(List* self, Callable* cb) {
    LambdaFn1 fn = (LambdaFn1)cb->fn;
    for (int i = 0; i < self->size; i++)
        fn(cb->env, self->data[i]);
}

void* Core_Collections_List_List_reduce(List* self, void* initial, Callable* cb) {
    LambdaFn2 fn = (LambdaFn2)cb->fn;
    void* acc = initial;
    for (int i = 0; i < self->size; i++)
        acc = fn(cb->env, acc, self->data[i]);
    return acc;
}

int Core_Collections_List_List_any(List* self, Callable* cb) {
    LambdaFn1 fn = (LambdaFn1)cb->fn;
    for (int i = 0; i < self->size; i++)
        if (fn(cb->env, self->data[i]) != NULL) return 1;
    return 0;
}

int Core_Collections_List_List_all(List* self, Callable* cb) {
    LambdaFn1 fn = (LambdaFn1)cb->fn;
    for (int i = 0; i < self->size; i++)
        if (fn(cb->env, self->data[i]) == NULL) return 0;
    return 1;
}

void* Core_Collections_List_List_find(List* self, Callable* cb) {
    LambdaFn1 fn = (LambdaFn1)cb->fn;
    for (int i = 0; i < self->size; i++)
        if (fn(cb->env, self->data[i]) != NULL) return self->data[i];
    return NULL;
}