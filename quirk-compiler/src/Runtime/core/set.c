#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"

extern String* quirk_opaque_to_string(void* val);

// Insertion-ordered hash set — the same compact design as Map (see the
// layout notes on `Set` in types.h). Elements are keyed by their string
// form (`quirk_opaque_to_string`), so `1` and `"1"` are the same member.
// The index is kept at most 3/4 full counting removed entries, and
// rebuilt (compacting the holes) when it would pass that.

#define SET_INITIAL_CAP 8
#define SLOT_EMPTY      0
#define SLOT_DELETED  (-1)

static unsigned int set_hash(const char* key) {
    unsigned int h = 2166136261u;
    while (*key) { h ^= (unsigned char)(*key++); h *= 16777619; }
    return h;
}

static const char* Set__key_of(void* val) {
    String* s = quirk_opaque_to_string(val);
    return (s && s->buffer) ? s->buffer : "";
}

// Index slot for `key`: where it lives if present (*found = 1), else the
// slot an insert should use (first deleted slot seen, or the empty one
// that ended the probe).
static int Set__find_slot(Set* self, const char* key, unsigned int hash, int* found) {
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
            SetEntry* e = &self->entries[pos - 1];
            if (e->hash == hash && strcmp(e->key, key) == 0) {
                *found = 1;
                return (int)i;
            }
        }
        i = (i + 1) & mask;
    }
}

// Rebuild the index for `new_cap` slots, compacting removed entries out
// of the dense array (order of live entries is preserved).
static void Set__rebuild(Set* self, int new_cap) {
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
static void Set__reserve_one(Set* self) {
    if ((self->order_size + 1) * 4 > self->capacity * 3) {
        int new_cap = self->capacity;
        while ((self->size + 1) * 2 > new_cap) new_cap *= 2;   // live <= 1/2 after rebuild
        Set__rebuild(self, new_cap);
    }
    if (self->order_size >= self->order_capacity) {
        self->order_capacity = self->order_capacity ? self->order_capacity * 2 : SET_INITIAL_CAP;
        self->entries = (SetEntry*)realloc(self->entries, self->order_capacity * sizeof(SetEntry));
    }
}

// ===== LIFECYCLE =====

void Core_Collections_Set_Set___init(Set* self) {
    self->capacity = SET_INITIAL_CAP;
    self->size = 0;
    self->index = (int32_t*)calloc(SET_INITIAL_CAP, sizeof(int32_t));
    self->order_capacity = SET_INITIAL_CAP;
    self->order_size = 0;
    self->entries = (SetEntry*)calloc(SET_INITIAL_CAP, sizeof(SetEntry));
}

void Core_Collections_Set_Set___del(Set* self) {
    // Memory is GC-managed (runtime.c maps malloc/free onto Boehm); just
    // drop the references.
    self->entries = NULL;
    self->index = NULL;
    self->size = self->order_size = self->order_capacity = 0;
}

// ===== MUTATION =====

void Core_Collections_Set_Set_add(Set* self, void* val) {
    const char* key = Set__key_of(val);
    unsigned int hash = set_hash(key);
    int found;
    int slot = Set__find_slot(self, key, hash, &found);
    if (found) {
        self->entries[self->index[slot] - 1].value = val;   // update value, keep key
        return;
    }
    int32_t* old_index = self->index;
    Set__reserve_one(self);
    if (self->index != old_index)   // rebuilt: slots moved
        slot = Set__find_slot(self, key, hash, &found);
    SetEntry* e = &self->entries[self->order_size];
    e->key = strdup(key);
    e->value = val;
    e->is_occupied = 1;
    e->hash = hash;
    self->index[slot] = ++self->order_size;
    self->size++;
}

void Core_Collections_Set_Set_remove(Set* self, void* val) {
    if (!self || self->size == 0) return;
    const char* key = Set__key_of(val);
    int found;
    int slot = Set__find_slot(self, key, set_hash(key), &found);
    if (!found) return;
    SetEntry* e = &self->entries[self->index[slot] - 1];
    e->is_occupied = 0;
    e->key = NULL;
    e->value = NULL;
    self->index[slot] = SLOT_DELETED;
    self->size--;
}

void Core_Collections_Set_Set_clear(Set* self) {
    memset(self->index, 0, self->capacity * sizeof(int32_t));
    self->order_size = 0;
    self->size = 0;
}

// ===== QUERY =====

int Core_Collections_Set_Set_has(Set* self, void* val) {
    if (!self || self->size == 0) return 0;
    const char* key = Set__key_of(val);
    int found;
    Set__find_slot(self, key, set_hash(key), &found);
    return found;
}

int Core_Collections_Set_Set_size(Set* self) { return self ? self->size : 0; }
int Core_Collections_Set_Set_is_empty(Set* self) { return !self || self->size == 0; }

// ===== SET OPERATIONS =====
//
// All walk the dense entry array, so results keep insertion order
// (self's elements first, then other's new ones for union).

Set* Core_Collections_Set_Set_union(Set* self, Set* other) {
    Set* result = (Set*)quirk_alloc(QT_SET, sizeof(Set));
    Core_Collections_Set_Set___init(result);
    for (int i = 0; i < self->order_size; i++)
        if (self->entries[i].is_occupied) Core_Collections_Set_Set_add(result, self->entries[i].value);
    for (int i = 0; i < other->order_size; i++)
        if (other->entries[i].is_occupied) Core_Collections_Set_Set_add(result, other->entries[i].value);
    return result;
}

Set* Core_Collections_Set_Set_intersection(Set* self, Set* other) {
    Set* result = (Set*)quirk_alloc(QT_SET, sizeof(Set));
    Core_Collections_Set_Set___init(result);
    for (int i = 0; i < self->order_size; i++) {
        SetEntry* e = &self->entries[i];
        if (e->is_occupied && Core_Collections_Set_Set_has(other, e->value))
            Core_Collections_Set_Set_add(result, e->value);
    }
    return result;
}

Set* Core_Collections_Set_Set_difference(Set* self, Set* other) {
    Set* result = (Set*)quirk_alloc(QT_SET, sizeof(Set));
    Core_Collections_Set_Set___init(result);
    for (int i = 0; i < self->order_size; i++) {
        SetEntry* e = &self->entries[i];
        if (e->is_occupied && !Core_Collections_Set_Set_has(other, e->value))
            Core_Collections_Set_Set_add(result, e->value);
    }
    return result;
}

// Materialise the set's elements into a fresh List in insertion
// order. Useful when downstream code wants to index/sort/iterate
// with a stable order — List operations (`__get`, `sort`, `slice`)
// require a List receiver.
List* Core_Collections_Set_Set_to_list(Set* self) {
    extern void Core_Collections_List_List___init(List*);
    extern void Core_Collections_List_List_append(List*, void*);
    List* result = (List*)quirk_alloc(QT_LIST, sizeof(List));
    Core_Collections_List_List___init(result);
    if (!self) return result;
    for (int i = 0; i < self->order_size; i++)
        if (self->entries[i].is_occupied)
            Core_Collections_List_List_append(result, self->entries[i].value);
    return result;
}

// ===== ITERATOR =====
//
// `idx` walks the dense entry array; removed entries are skipped.

static int Set__skip_removed(Set* s, int idx) {
    while (idx < s->order_size && !s->entries[idx].is_occupied) idx++;
    return idx;
}

void Core_Collections_Set_SetIterator___init(SetIterator* self, Set* s) {
    self->set_ref = s;
    self->idx = 0;
}

int Core_Collections_Set_SetIterator___has_next(SetIterator* self) {
    if (!self || !self->set_ref) return 0;
    self->idx = Set__skip_removed(self->set_ref, self->idx);
    return self->idx < self->set_ref->order_size;
}

void* Core_Collections_Set_SetIterator___next(SetIterator* self) {
    if (!self || !self->set_ref) return NULL;
    self->idx = Set__skip_removed(self->set_ref, self->idx);
    if (self->idx >= self->set_ref->order_size) return NULL;
    return self->set_ref->entries[self->idx++].value;
}

SetIterator* Core_Collections_Set_Set___iter(Set* self) {
    SetIterator* it = (SetIterator*)quirk_alloc(QT_ITERATOR, sizeof(SetIterator));
    Core_Collections_Set_SetIterator___init(it, self);
    return it;
}

// ===== STRING =====

String* Core_Collections_Set_Set___str(Set* self) {
    if (!self || self->size == 0) return make_String("{}");
    int cap = 256;
    char* buf = (char*)malloc(cap);
    int len = 0;
    int first = 1;
    buf[len++] = '{';
    for (int i = 0; i < self->order_size; i++) {
        if (!self->entries[i].is_occupied) continue;
        if (!first) {
            if (len + 3 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
            buf[len++] = ','; buf[len++] = ' ';
        }
        first = 0;
        const char* k = self->entries[i].key;
        int kl = strlen(k);
        while (len + kl + 4 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        memcpy(buf + len, k, kl);
        len += kl;
    }
    if (len + 2 >= cap) { cap += 2; buf = (char*)realloc(buf, cap); }
    buf[len++] = '}'; buf[len] = '\0';
    return make_String(buf);
}
