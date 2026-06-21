#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"

extern String* quirk_opaque_to_string(void* val);

#define INITIAL_CAPACITY 8
#define INITIAL_ORDER_CAPACITY 8
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

// Append key to insertion-order list (only for new keys).
static void Map__order_append(Map* self, const char* key) {
    if (self->order_size >= self->order_capacity) {
        self->order_capacity *= 2;
        self->key_order = (char**)realloc(self->key_order, self->order_capacity * sizeof(char*));
    }
    self->key_order[self->order_size++] = strdup(key);
}

// Remove key from insertion-order list (shift remaining entries left).
static void Map__order_remove(Map* self, const char* key) {
    for (int i = 0; i < self->order_size; i++) {
        if (self->key_order[i] && strcmp(self->key_order[i], key) == 0) {
            free(self->key_order[i]);
            memmove(&self->key_order[i], &self->key_order[i + 1],
                    (self->order_size - i - 1) * sizeof(char*));
            self->order_size--;
            return;
        }
    }
}

// ==========================================
//  LIFECYCLE
// ==========================================

void Core_Collections_Map_Map___init(Map* self) {
    self->capacity = INITIAL_CAPACITY;
    self->size = 0;
    self->entries = (MapEntry*)calloc(self->capacity, sizeof(MapEntry));
    self->order_capacity = INITIAL_ORDER_CAPACITY;
    self->order_size = 0;
    self->key_order = (char**)malloc(self->order_capacity * sizeof(char*));
}

void Core_Collections_Map_Map___del(Map* self) {
    if (self->entries) {
        for (int i = 0; i < self->capacity; i++) {
            if (self->entries[i].is_occupied && self->entries[i].key)
                free(self->entries[i].key);
        }
        free(self->entries);
        self->entries = NULL;
    }
    if (self->key_order) {
        for (int i = 0; i < self->order_size; i++)
            free(self->key_order[i]);
        free(self->key_order);
        self->key_order = NULL;
    }
}

// Resize only rehashes the entry table; key_order is unaffected.
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
            dest->key = old_entries[i].key;
            dest->value = old_entries[i].value;
            dest->is_occupied = 1;
            self->size++;
        }
    }
    free(old_entries);
}

// ==========================================
//  PUBLIC METHODS
// ==========================================

void Core_Collections_Map_Map_put(Map* self, String* keyObj, void* value) {
    if (!keyObj || !keyObj->buffer) return;
    char* rawKey = keyObj->buffer;

    if ((float)self->size / self->capacity >= LOAD_FACTOR)
        Map__resize(self);

    MapEntry* entry = Map__find_entry(self->entries, self->capacity, rawKey);

    int is_new = !entry->is_occupied;
    if (is_new) {
        entry->key = strdup(rawKey);
        self->size++;
        Map__order_append(self, rawKey);
    }

    entry->value = value;
    entry->is_occupied = 1;
    entry->is_deleted = 0;
}

void* Core_Collections_Map_Map_get(Map* self, String* keyObj) {
    if (!keyObj || !keyObj->buffer) return NULL;
    MapEntry* entry = Map__find_entry(self->entries, self->capacity, keyObj->buffer);
    return (entry && entry->is_occupied) ? entry->value : NULL;
}

int Core_Collections_Map_Map_has(Map* self, String* keyObj) {
    if (!keyObj || !keyObj->buffer) return 0;
    MapEntry* entry = Map__find_entry(self->entries, self->capacity, keyObj->buffer);
    return (entry && entry->is_occupied);
}

void Core_Collections_Map_Map_remove(Map* self, String* keyObj) {
    if (!keyObj || !keyObj->buffer) return;
    MapEntry* entry = Map__find_entry(self->entries, self->capacity, keyObj->buffer);
    if (entry && entry->is_occupied) {
        Map__order_remove(self, entry->key);
        entry->is_occupied = 0;
        entry->is_deleted = 1;
        free(entry->key);
        entry->key = NULL;
        self->size--;
    }
}

int Core_Collections_Map_Map_length(Map* self) {
    return self->size;
}

void Core_Collections_Map_Map_clear(Map* self) {
    for (int i = 0; i < self->capacity; i++) {
        if (self->entries[i].is_occupied)
            free(self->entries[i].key);
        self->entries[i].is_occupied = 0;
        self->entries[i].is_deleted = 0;
    }
    for (int i = 0; i < self->order_size; i++)
        free(self->key_order[i]);
    self->order_size = 0;
    self->size = 0;
}

// Copy every entry from `other` into `self`, overwriting same-key
// values with other's. Quirk: `m.put_all(other)`. Returns void —
// Python-`update`-style semantics. Use `__add` (below) for a fresh
// merged Map instead of mutating.
void Core_Collections_Map_Map_put_all(Map* self, Map* other) {
    if (!self || !other) return;
    for (int i = 0; i < other->order_size; i++) {
        const char* rawKey = other->key_order[i];
        if (!rawKey) continue;
        MapEntry* src = Map__find_entry(other->entries, other->capacity, rawKey);
        if (!src || !src->is_occupied) continue;
        // Wrap the C string back into a Quirk String* so the existing
        // put() logic owns its own copy of the key. Avoids any
        // shared-key lifetime entanglement between the two maps.
        String* keyObj = make_String(rawKey);
        Core_Collections_Map_Map_put(self, keyObj, src->value);
    }
}

// `m1 + m2` — fresh Map containing every entry of self, then every
// entry of other (right side wins on key collision — matches the
// `{**m1, **m2}` precedence Python uses). Neither input mutated.
Map* Core_Collections_Map_Map___add(Map* self, Map* other) {
    extern void Core_Collections_Map_Map___init(Map*);
    Map* result = (Map*)GC_malloc(sizeof(Map));
    Core_Collections_Map_Map___init(result);
    if (self)  Core_Collections_Map_Map_put_all(result, self);
    if (other) Core_Collections_Map_Map_put_all(result, other);
    return result;
}

// ==========================================
//  COLLECTION VIEWS
// ==========================================

List* Core_Collections_Map_Map_keys(Map* self) {
    extern void Core_Collections_List_List___init(List*);
    extern void Core_Collections_List_List_append(List*, void*);

    List* result = (List*)malloc(sizeof(List));
    Core_Collections_List_List___init(result);

    if (!self) return result;
    for (int i = 0; i < self->order_size; i++) {
        if (self->key_order[i])
            Core_Collections_List_List_append(result, (void*)make_String(self->key_order[i]));
    }
    return result;
}

List* Core_Collections_Map_Map_values(Map* self) {
    extern void Core_Collections_List_List___init(List*);
    extern void Core_Collections_List_List_append(List*, void*);

    List* result = (List*)malloc(sizeof(List));
    Core_Collections_List_List___init(result);

    if (!self) return result;
    for (int i = 0; i < self->order_size; i++) {
        if (!self->key_order[i]) continue;
        MapEntry* entry = Map__find_entry(self->entries, self->capacity, self->key_order[i]);
        if (entry && entry->is_occupied)
            Core_Collections_List_List_append(result, entry->value);
    }
    return result;
}

// ==========================================
//  OPERATORS
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

    int cap = 256, len = 0;
    char* buf = (char*)malloc(cap);
    buf[len++] = '{';

    int first = 1;
    for (int i = 0; i < self->order_size; i++) {
        const char* k = self->key_order[i];
        if (!k) continue;
        MapEntry* entry = Map__find_entry(self->entries, self->capacity, k);
        if (!entry || !entry->is_occupied) continue;

        String* vs = quirk_opaque_to_string(entry->value);
        const char* vstr = (vs && vs->buffer) ? vs->buffer : "null";
        int is_int_val = entry->value && (uintptr_t)entry->value <= 0xFFFFFFFFUL;
        int vquote = !is_int_val;

        int needed = (first ? 0 : 2) + 1 + strlen(k) + 3 + (vquote ? 2 : 0) + strlen(vstr) + 1;
        while (len + needed + 2 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }

        if (!first) { buf[len++] = ','; buf[len++] = ' '; }
        buf[len++] = '"';
        strcpy(buf + len, k); len += strlen(k);
        buf[len++] = '"'; buf[len++] = ':'; buf[len++] = ' ';
        if (vquote) buf[len++] = '"';
        strcpy(buf + len, vstr); len += strlen(vstr);
        if (vquote) buf[len++] = '"';
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
    return self && self->map_ref && self->idx < self->map_ref->order_size;
}

String* Core_Collections_Map_MapIterator___next(MapIterator* self) {
    if (!self || !self->map_ref || self->idx >= self->map_ref->order_size)
        return make_String("");
    const char* key = self->map_ref->key_order[self->idx++];
    return make_String(key ? key : "");
}

MapIterator* Core_Collections_Map_Map___iter(Map* self) {
    MapIterator* iter = (MapIterator*)malloc(sizeof(MapIterator));
    Core_Collections_Map_MapIterator___init(iter, self);
    return iter;
}

// ==========================================
//  MAP PAIR ITERATOR (for k, v in map)
// ==========================================

static void MapPairIterator__init(MapPairIterator* self, Map* m) {
    self->map_ref = m;
    self->idx = 0;
    self->current_value = NULL;
}

int Core_Collections_Map_MapPairIterator___has_next(MapPairIterator* self) {
    return self && self->map_ref && self->idx < self->map_ref->order_size;
}

String* Core_Collections_Map_MapPairIterator___next(MapPairIterator* self) {
    if (!self || !self->map_ref || self->idx >= self->map_ref->order_size) {
        self->current_value = NULL;
        return make_String("");
    }
    const char* key = self->map_ref->key_order[self->idx++];
    MapEntry* entry = key ? Map__find_entry(self->map_ref->entries, self->map_ref->capacity, key) : NULL;
    self->current_value = (entry && entry->is_occupied) ? entry->value : NULL;
    return make_String(key ? key : "");
}

void* Core_Collections_Map_MapPairIterator___current_value(MapPairIterator* self) {
    return self ? self->current_value : NULL;
}

MapPairIterator* Core_Collections_Map_Map___iter_pairs(Map* self) {
    MapPairIterator* iter = (MapPairIterator*)malloc(sizeof(MapPairIterator));
    MapPairIterator__init(iter, self);
    return iter;
}

// ==========================================
//  FUNCTIONAL METHODS (lambda support)
// ==========================================

typedef void* (*LambdaFn1)(void* env, void* arg);

void Core_Collections_Map_Map_each(Map* self, Callable* cb) {
    LambdaFn2 fn = (LambdaFn2)cb->fn;
    for (int i = 0; i < self->order_size; i++) {
        const char* k = self->key_order[i];
        if (!k) continue;
        MapEntry* entry = Map__find_entry(self->entries, self->capacity, k);
        if (entry && entry->is_occupied)
            fn(cb->env, make_String(k), entry->value);
    }
}

void Core_Collections_Map_Map_each_value(Map* self, Callable* cb) {
    LambdaFn1 fn = (LambdaFn1)cb->fn;
    for (int i = 0; i < self->order_size; i++) {
        const char* k = self->key_order[i];
        if (!k) continue;
        MapEntry* entry = Map__find_entry(self->entries, self->capacity, k);
        if (entry && entry->is_occupied)
            fn(cb->env, entry->value);
    }
}

void Core_Collections_Map_Map_each_key(Map* self, Callable* cb) {
    LambdaFn1 fn = (LambdaFn1)cb->fn;
    for (int i = 0; i < self->order_size; i++) {
        const char* k = self->key_order[i];
        if (k) fn(cb->env, make_String(k));
    }
}
