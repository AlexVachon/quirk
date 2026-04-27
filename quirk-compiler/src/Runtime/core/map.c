#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"

#define INITIAL_CAPACITY 8
#define LOAD_FACTOR 0.75

// FNV-1a Hash Function (Standard, fast string hashing)
static unsigned int hash_str(const char* key) {
    unsigned int hash = 2166136261u;
    while (*key) {
        hash ^= (unsigned char)(*key);
        hash *= 16777619;
        key++;
    }
    return hash;
}

// ... (Lifecycle: Core_Collections_Map_Map___init, Core_Collections_Map_Map___del remain same) ...
void Core_Collections_Map_Map___init(Map* self) {
    self->capacity = INITIAL_CAPACITY;
    self->size = 0;
    self->entries = (MapEntry*)calloc(self->capacity, sizeof(MapEntry));
}

void Core_Collections_Map_Map___del(Map* self) {
    if (self->entries) {
        for (int i = 0; i < self->capacity; i++) {
            if (self->entries[i].is_occupied && self->entries[i].key) {
                free(self->entries[i].key);
            }
        }
        free(self->entries);
        self->entries = NULL;
    }
}

// ... (Map__find_entry remains same, takes const char* raw_key) ...
static MapEntry* Map__find_entry(MapEntry* entries, int capacity, const char* key) {
    unsigned int hash = hash_str(key);
    int idx = hash % capacity;
    int start_idx = idx;

    while (1) {
        MapEntry* entry = &entries[idx];
        if (!entry->is_occupied && !entry->is_deleted)
            return entry;
        if (entry->is_occupied && strcmp(entry->key, key) == 0)
            return entry;
        idx = (idx + 1) % capacity;
        if (idx == start_idx)
            return NULL;
    }
}

// ... (Map__resize remains mostly same) ...
static void Map__resize(Map* self) {
    int old_cap = self->capacity;
    MapEntry* old_entries = self->entries;

    self->capacity *= 2;
    self->entries = (MapEntry*)calloc(self->capacity, sizeof(MapEntry));
    self->size = 0;

    for (int i = 0; i < old_cap; i++) {
        if (old_entries[i].is_occupied) {
            MapEntry* dest = Map__find_entry(self->entries, self->capacity,
                                             old_entries[i].key);
            dest->key = old_entries[i].key;  // Move ownership
            dest->value = old_entries[i].value;
            dest->is_occupied = 1;
            self->size++;
        }
    }
    free(old_entries);
}

// ==========================================
//  PUBLIC METHODS (Updated to use String*)
// ==========================================

void Core_Collections_Map_Map_put(Map* self, String* keyObj, void* value) {
    // Safety check
    if (!keyObj || !keyObj->buffer)
        return;
    char* rawKey = keyObj->buffer;

    if ((float)self->size / self->capacity >= LOAD_FACTOR) {
        Map__resize(self);
    }

    MapEntry* entry = Map__find_entry(self->entries, self->capacity, rawKey);

    if (!entry->is_occupied) {
        entry->key = strdup(rawKey);  // Copy the C-String
        self->size++;
    }

    entry->value = value;
    entry->is_occupied = 1;
    entry->is_deleted = 0;
}

void* Core_Collections_Map_Map_get(Map* self, String* keyObj) {
    if (!keyObj || !keyObj->buffer)
        return NULL;

    MapEntry* entry =
        Map__find_entry(self->entries, self->capacity, keyObj->buffer);
    if (entry && entry->is_occupied) {
        return entry->value;
    }
    return NULL;
}

int Core_Collections_Map_Map_has(Map* self, String* keyObj) {
    if (!keyObj || !keyObj->buffer)
        return 0;

    MapEntry* entry =
        Map__find_entry(self->entries, self->capacity, keyObj->buffer);
    return (entry && entry->is_occupied);
}

void Core_Collections_Map_Map_remove(Map* self, String* keyObj) {
    if (!keyObj || !keyObj->buffer)
        return;

    MapEntry* entry =
        Map__find_entry(self->entries, self->capacity, keyObj->buffer);
    if (entry && entry->is_occupied) {
        entry->is_occupied = 0;
        entry->is_deleted = 1;
        free(entry->key);
        entry->key = NULL;
        self->size--;
    }
}

int Core_Collections_Map_Map_len(Map* self) {
    return self->size;
}

void Core_Collections_Map_Map_clear(Map* self) {
    for (int i = 0; i < self->capacity; i++) {
        if (self->entries[i].is_occupied) {
            free(self->entries[i].key);
            self->entries[i].is_occupied = 0;
        }
        self->entries[i].is_deleted = 0;
    }
    self->size = 0;
}


// ==========================================
//  COLLECTION VIEWS
// ==========================================

// Returns a List of all keys as String* objects.
// This is what Quirk's `map.keys()` compiles to.
List* Core_Collections_Map_Map_keys(Map* self) {
    extern void Core_Collections_List_List___init(List*);
    extern void Core_Collections_List_List_append(List*, void*);

    List* result = (List*)malloc(sizeof(List));
    Core_Collections_List_List___init(result);

    if (!self) return result;
    for (int i = 0; i < self->capacity; i++) {
        if (self->entries[i].is_occupied && self->entries[i].key) {
            String* k = make_String(self->entries[i].key);
            Core_Collections_List_List_append(result, (void*)k);
        }
    }
    return result;
}

// Returns a List of all values as void* (the raw stored pointers).
List* Core_Collections_Map_Map_values(Map* self) {
    extern void Core_Collections_List_List___init(List*);
    extern void Core_Collections_List_List_append(List*, void*);

    List* result = (List*)malloc(sizeof(List));
    Core_Collections_List_List___init(result);

    if (!self) return result;
    for (int i = 0; i < self->capacity; i++) {
        if (self->entries[i].is_occupied) {
            Core_Collections_List_List_append(result, self->entries[i].value);
        }
    }
    return result;
}

// ==========================================
//  OPERATORS (Updated to use String*)
// ==========================================

extern void quirk_throw_exception(const char* type_name, const char* message);

void* Core_Collections_Map_Map___get(Map* self, String* keyObj) {
    void* val = Core_Collections_Map_Map_get(self, keyObj);
    if (!val && !Core_Collections_Map_Map_has(self, keyObj)) {
        const char* key = (keyObj && keyObj->buffer) ? keyObj->buffer : "(null)";
        char buf[256];
        snprintf(buf, sizeof(buf), "'%s'", key);
        quirk_throw_exception("KeyError", buf);
    }
    return val;
}

void Core_Collections_Map_Map___set(Map* self, String* keyObj, void* value) {
    Core_Collections_Map_Map_put(self, keyObj, value);
}

String* Core_Collections_Map_Map___str(Map* self) {
    if (!self || self->size == 0) return make_String("{}");

    // Dynamic buffer
    int cap = 256, len = 0;
    char* buf = (char*)malloc(cap);
    buf[len++] = '{';

    int first = 1;
    for (int i = 0; i < self->capacity; i++) {
        if (!self->entries[i].is_occupied) continue;
        const char* k = self->entries[i].key ? self->entries[i].key : "";
        // Value: try to get a printable form via Any_to_string if it is an Any*,
        // otherwise fall back to raw pointer address.
        // For now we emit key: <value> pairs where value is shown as a string if possible.
        const char* v = "(?)";
        // Rough approximation: if the stored pointer looks like a C string
        // (i.e. the first byte is printable ASCII and not a struct field tag),
        // print it directly.
        char* stored = (char*)self->entries[i].value;
        char vbuf[64];
        if (stored && (unsigned char)stored[0] >= 32 && (unsigned char)stored[0] < 127) {
            v = stored;
        } else if (!stored) {
            v = "null";
        } else {
            snprintf(vbuf, sizeof(vbuf), "<ptr:%p>", (void*)stored);
            v = vbuf;
        }

        int needed = (first ? 0 : 2) + 1 + strlen(k) + 3 + strlen(v) + 1;
        while (len + needed + 2 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }

        if (!first) { buf[len++] = ','; buf[len++] = ' '; }
        buf[len++] = '"';
        strcpy(buf + len, k); len += strlen(k);
        buf[len++] = '"'; buf[len++] = ':'; buf[len++] = ' ';
        strcpy(buf + len, v); len += strlen(v);
        first = 0;
    }
    buf[len++] = '}';
    buf[len] = '\0';
    return make_String_taking_ownership(buf);
}
// ==========================================
//  MAP ITERATOR
// ==========================================

void Core_Collections_Map_MapIterator___init(MapIterator* self, Map* m) {
    self->map_ref = m;
    self->idx = 0;
}

int Core_Collections_Map_MapIterator___has_next(MapIterator* self) {
    if (!self || !self->map_ref) return 0;
    // Advance past deleted/empty slots
    while (self->idx < self->map_ref->capacity &&
           !self->map_ref->entries[self->idx].is_occupied) {
        self->idx++;
    }
    return self->idx < self->map_ref->capacity;
}

String* Core_Collections_Map_MapIterator___next(MapIterator* self) {
    if (!self || !self->map_ref) return make_String("");
    // Skip to next occupied slot
    while (self->idx < self->map_ref->capacity &&
           !self->map_ref->entries[self->idx].is_occupied) {
        self->idx++;
    }
    if (self->idx >= self->map_ref->capacity) return make_String("");
    const char* key = self->map_ref->entries[self->idx].key;
    self->idx++;
    return make_String(key ? key : "");
}

MapIterator* Core_Collections_Map_Map___iter(Map* self) {
    MapIterator* iter = (MapIterator*)malloc(sizeof(MapIterator));
    Core_Collections_Map_MapIterator___init(iter, self);
    return iter;
}

// ==========================================
//  FUNCTIONAL METHODS (lambda support)
// ==========================================

typedef void* (*LambdaFn1)(void* env, void* arg);

// each(fn(key: String, value: Any)) — calls cb with key and value for every entry
void Core_Collections_Map_Map_each(Map* self, Callable* cb) {
    LambdaFn2 fn = (LambdaFn2)cb->fn;
    for (int i = 0; i < self->capacity; i++) {
        if (self->entries[i].is_occupied)
            fn(cb->env, make_String(self->entries[i].key), self->entries[i].value);
    }
}

// each_value(fn(value: Any)) — calls cb with only the value
void Core_Collections_Map_Map_each_value(Map* self, Callable* cb) {
    LambdaFn1 fn = (LambdaFn1)cb->fn;
    for (int i = 0; i < self->capacity; i++) {
        if (self->entries[i].is_occupied)
            fn(cb->env, self->entries[i].value);
    }
}

void Core_Collections_Map_Map_each_key(Map* self, Callable* cb) {
    LambdaFn1 fn = (LambdaFn1)cb->fn;
    for (int i = 0; i < self->capacity; i++) {
        if (self->entries[i].is_occupied)
            fn(cb->env, make_String(self->entries[i].key));
    }
}