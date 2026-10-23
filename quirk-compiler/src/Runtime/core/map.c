#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"

extern String* quirk_opaque_to_string(void* val);

// Insertion-ordered hash map (see the layout notes on `Map` in types.h).
// `entries` is dense in insertion order, so iteration, keys(), values()
// and str() walk it directly; `index` maps hash slots to entry positions.
// Lookups compare the cached hash before strcmp. The index is kept at
// most 3/4 full counting removed entries, and rebuilt (compacting the
// removed holes) when it would pass that — so a probe always reaches an
// empty slot.

#define INITIAL_CAPACITY 8
#define SLOT_EMPTY    0
#define SLOT_DELETED (-1)

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

// Index slot for `key`: where it lives if present (*found = 1), else
// the slot an insert should use (first deleted slot seen, or the empty
// one that ended the probe).
static int Map__find_slot(Map* self, const char* key, unsigned int hash, int* found) {
    unsigned int mask = (unsigned int)self->capacity - 1;
    unsigned int i = hash & mask;
    int first_deleted = -1;
    while (1) {
        int32_t pos = self->index[i];
        if (pos == SLOT_EMPTY) {
            *found = 0;
            return first_deleted >= 0 ? first_deleted : (int)i;
        }
        if (pos == SLOT_DELETED) {
            if (first_deleted < 0) first_deleted = (int)i;
        } else {
            MapEntry* e = &self->entries[pos - 1];
            if (e->hash == hash && strcmp(e->key, key) == 0) {
                *found = 1;
                return (int)i;
            }
        }
        i = (i + 1) & mask;
    }
}

static MapEntry* Map__lookup(Map* self, const char* key) {
    if (!self || !key || self->size == 0) return NULL;
    int found;
    int slot = Map__find_slot(self, key, hash_str(key), &found);
    return found ? &self->entries[self->index[slot] - 1] : NULL;
}

// Rebuild the index for `new_cap` slots, compacting removed entries out
// of the dense array (order of live entries is preserved).
static void Map__rebuild(Map* self, int new_cap) {
    int live = 0;
    for (int i = 0; i < self->order_size; i++) {
        if (self->entries[i].is_occupied) {
            if (live != i) self->entries[live] = self->entries[i];
            live++;
        }
    }
    self->order_size = live;
    self->capacity = new_cap;
    self->index = (int32_t*)calloc(new_cap, sizeof(int32_t));
    unsigned int mask = (unsigned int)new_cap - 1;
    for (int i = 0; i < live; i++) {
        unsigned int s = self->entries[i].hash & mask;
        while (self->index[s] != SLOT_EMPTY) s = (s + 1) & mask;
        self->index[s] = i + 1;
    }
}

// Make room for one more entry: keep (used entries + 1) <= 3/4 of the
// index. Grows when live entries need it, otherwise just compacts holes.
static void Map__reserve_one(Map* self) {
    if ((self->order_size + 1) * 4 > self->capacity * 3) {
        int new_cap = self->capacity;
        while ((self->size + 1) * 2 > new_cap) new_cap *= 2;   // live <= 1/2 after rebuild
        Map__rebuild(self, new_cap);
    }
    if (self->order_size >= self->order_capacity) {
        self->order_capacity = self->order_capacity ? self->order_capacity * 2 : INITIAL_CAPACITY;
        self->entries = (MapEntry*)realloc(self->entries, self->order_capacity * sizeof(MapEntry));
    }
}

// put() on a raw C-string key (copied on insert).
static void Map__put_raw(Map* self, const char* rawKey, void* value) {
    unsigned int hash = hash_str(rawKey);
    int found;
    int slot = Map__find_slot(self, rawKey, hash, &found);
    if (found) {
        self->entries[self->index[slot] - 1].value = value;
        return;
    }
    int32_t* old_index = self->index;
    Map__reserve_one(self);
    if (self->index != old_index)   // rebuilt: slots moved
        slot = Map__find_slot(self, rawKey, hash, &found);
    MapEntry* e = &self->entries[self->order_size];
    e->key = strdup(rawKey);
    e->value = value;
    e->is_occupied = 1;
    e->hash = hash;
    self->index[slot] = ++self->order_size;
    self->size++;
}

// ==========================================
//  LIFECYCLE
// ==========================================

void Core_Collections_Map_Map___init(Map* self) {
    self->capacity = INITIAL_CAPACITY;
    self->size = 0;
    self->index = (int32_t*)calloc(INITIAL_CAPACITY, sizeof(int32_t));
    self->order_capacity = INITIAL_CAPACITY;
    self->order_size = 0;
    self->entries = (MapEntry*)calloc(INITIAL_CAPACITY, sizeof(MapEntry));
}

void Core_Collections_Map_Map___del(Map* self) {
    // Memory is GC-managed (runtime.c maps malloc/free onto Boehm); just
    // drop the references.
    self->entries = NULL;
    self->index = NULL;
    self->size = self->order_size = self->order_capacity = 0;
}

// ==========================================
//  PUBLIC METHODS
// ==========================================

void Core_Collections_Map_Map_put(Map* self, String* keyObj, void* value) {
    if (!keyObj || !keyObj->buffer) return;
    Map__put_raw(self, keyObj->buffer, value);
}

void* Core_Collections_Map_Map_get(Map* self, String* keyObj) {
    if (!keyObj || !keyObj->buffer) return NULL;
    MapEntry* e = Map__lookup(self, keyObj->buffer);
    return e ? e->value : NULL;
}

int Core_Collections_Map_Map_has(Map* self, String* keyObj) {
    if (!keyObj || !keyObj->buffer) return 0;
    return Map__lookup(self, keyObj->buffer) != NULL;
}

void Core_Collections_Map_Map_remove(Map* self, String* keyObj) {
    if (!self || !keyObj || !keyObj->buffer || self->size == 0) return;
    int found;
    int slot = Map__find_slot(self, keyObj->buffer, hash_str(keyObj->buffer), &found);
    if (!found) return;
    MapEntry* e = &self->entries[self->index[slot] - 1];
    e->is_occupied = 0;
    e->key = NULL;
    e->value = NULL;
    self->index[slot] = SLOT_DELETED;
    self->size--;
}

int Core_Collections_Map_Map_length(Map* self) {
    return self->size;
}

void Core_Collections_Map_Map_clear(Map* self) {
    memset(self->index, 0, self->capacity * sizeof(int32_t));
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
        MapEntry* src = &other->entries[i];
        if (src->is_occupied) Map__put_raw(self, src->key, src->value);
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
        if (self->entries[i].is_occupied)
            Core_Collections_List_List_append(result, (void*)make_String(self->entries[i].key));
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
        if (self->entries[i].is_occupied)
            Core_Collections_List_List_append(result, self->entries[i].value);
    }
    return result;
}

// ==========================================
//  OPERATORS
// ==========================================

extern void quirk_throw_exception(const char* type_name, const char* message);

void* Core_Collections_Map_Map___get(Map* self, String* keyObj) {
    MapEntry* e = (keyObj && keyObj->buffer) ? Map__lookup(self, keyObj->buffer) : NULL;
    if (!e) {
        const char* key = (keyObj && keyObj->buffer) ? keyObj->buffer : "(null)";
        char buf[256];
        snprintf(buf, sizeof(buf), "'%s'", key);
        quirk_throw_exception("KeyError", buf);
        return NULL;
    }
    return e->value;
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
        MapEntry* entry = &self->entries[i];
        if (!entry->is_occupied) continue;
        const char* k = entry->key;

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
//
// `idx` walks the dense entry array; removed entries are skipped.

static int Map__skip_removed(Map* m, int idx) {
    while (idx < m->order_size && !m->entries[idx].is_occupied) idx++;
    return idx;
}

void Core_Collections_Map_MapIterator___init(MapIterator* self, Map* m) {
    self->map_ref = m;
    self->idx = 0;
}

int Core_Collections_Map_MapIterator___has_next(MapIterator* self) {
    if (!self || !self->map_ref) return 0;
    self->idx = Map__skip_removed(self->map_ref, self->idx);
    return self->idx < self->map_ref->order_size;
}

String* Core_Collections_Map_MapIterator___next(MapIterator* self) {
    if (!self || !self->map_ref) return make_String("");
    self->idx = Map__skip_removed(self->map_ref, self->idx);
    if (self->idx >= self->map_ref->order_size) return make_String("");
    return make_String(self->map_ref->entries[self->idx++].key);
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
    if (!self || !self->map_ref) return 0;
    self->idx = Map__skip_removed(self->map_ref, self->idx);
    return self->idx < self->map_ref->order_size;
}

String* Core_Collections_Map_MapPairIterator___next(MapPairIterator* self) {
    if (!self || !self->map_ref) { if (self) self->current_value = NULL; return make_String(""); }
    self->idx = Map__skip_removed(self->map_ref, self->idx);
    if (self->idx >= self->map_ref->order_size) {
        self->current_value = NULL;
        return make_String("");
    }
    MapEntry* e = &self->map_ref->entries[self->idx++];
    self->current_value = e->value;
    return make_String(e->key);
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
        MapEntry* e = &self->entries[i];
        if (e->is_occupied) fn(cb->env, make_String(e->key), e->value);
    }
}

void Core_Collections_Map_Map_each_value(Map* self, Callable* cb) {
    LambdaFn1 fn = (LambdaFn1)cb->fn;
    for (int i = 0; i < self->order_size; i++) {
        MapEntry* e = &self->entries[i];
        if (e->is_occupied) fn(cb->env, e->value);
    }
}

void Core_Collections_Map_Map_each_key(Map* self, Callable* cb) {
    LambdaFn1 fn = (LambdaFn1)cb->fn;
    for (int i = 0; i < self->order_size; i++) {
        MapEntry* e = &self->entries[i];
        if (e->is_occupied) fn(cb->env, make_String(e->key));
    }
}
