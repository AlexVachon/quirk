#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"

extern String* quirk_opaque_to_string(void* val);

#define SET_INITIAL_CAP   8
#define SET_ORDER_INIT    8
#define SET_LOAD_FACTOR   0.75

static unsigned int set_hash(const char* key) {
    unsigned int h = 2166136261u;
    while (*key) { h ^= (unsigned char)(*key++); h *= 16777619; }
    return h;
}

static SetEntry* Set__find_entry(SetEntry* entries, int cap, const char* key) {
    unsigned int h = set_hash(key);
    int idx = h % cap, start = idx;
    while (1) {
        SetEntry* e = &entries[idx];
        if (!e->is_occupied && !e->is_deleted) return e;
        if (e->is_occupied && strcmp(e->key, key) == 0) return e;
        idx = (idx + 1) % cap;
        if (idx == start) return NULL;
    }
}

static void Set__order_append(Set* self, const char* key) {
    if (self->order_size >= self->order_capacity) {
        self->order_capacity *= 2;
        self->key_order = (char**)realloc(self->key_order, self->order_capacity * sizeof(char*));
    }
    self->key_order[self->order_size++] = strdup(key);
}

static void Set__order_remove(Set* self, const char* key) {
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

// ===== LIFECYCLE =====

void Core_Collections_Set_Set___init(Set* self) {
    self->capacity = SET_INITIAL_CAP;
    self->size = 0;
    self->entries = (SetEntry*)calloc(self->capacity, sizeof(SetEntry));
    self->order_capacity = SET_ORDER_INIT;
    self->order_size = 0;
    self->key_order = (char**)malloc(self->order_capacity * sizeof(char*));
}

void Core_Collections_Set_Set___del(Set* self) {
    if (self->entries) {
        for (int i = 0; i < self->capacity; i++)
            if (self->entries[i].is_occupied && self->entries[i].key)
                free(self->entries[i].key);
        free(self->entries);
        self->entries = NULL;
    }
    if (self->key_order) {
        for (int i = 0; i < self->order_size; i++) free(self->key_order[i]);
        free(self->key_order);
        self->key_order = NULL;
    }
}

static void Set__resize(Set* self) {
    int old_cap = self->capacity;
    SetEntry* old = self->entries;
    self->capacity *= 2;
    self->entries = (SetEntry*)calloc(self->capacity, sizeof(SetEntry));
    self->size = 0;
    for (int i = 0; i < old_cap; i++) {
        if (!old[i].is_occupied) continue;
        SetEntry* dest = Set__find_entry(self->entries, self->capacity, old[i].key);
        dest->key = old[i].key;
        dest->value = old[i].value;
        dest->is_occupied = 1;
        self->size++;
    }
    free(old);
}

// ===== MUTATION =====

void Core_Collections_Set_Set_add(Set* self, void* val) {
    String* s = quirk_opaque_to_string(val);
    const char* key = (s && s->buffer) ? s->buffer : "";
    if ((float)(self->size + 1) / self->capacity > SET_LOAD_FACTOR) Set__resize(self);
    SetEntry* e = Set__find_entry(self->entries, self->capacity, key);
    if (!e) return;
    if (!e->is_occupied) {
        e->key = strdup(key);
        e->value = val;
        e->is_occupied = 1;
        self->size++;
        Set__order_append(self, key);
    } else {
        e->value = val; // update value, keep key
    }
}

void Core_Collections_Set_Set_remove(Set* self, void* val) {
    String* s = quirk_opaque_to_string(val);
    const char* key = (s && s->buffer) ? s->buffer : "";
    SetEntry* e = Set__find_entry(self->entries, self->capacity, key);
    if (!e || !e->is_occupied) return;
    free(e->key);
    e->key = NULL;
    e->value = NULL;
    e->is_occupied = 0;
    e->is_deleted = 1;
    self->size--;
    Set__order_remove(self, key);
}

void Core_Collections_Set_Set_clear(Set* self) {
    for (int i = 0; i < self->capacity; i++) {
        if (self->entries[i].is_occupied && self->entries[i].key)
            free(self->entries[i].key);
        self->entries[i] = (SetEntry){0};
    }
    for (int i = 0; i < self->order_size; i++) free(self->key_order[i]);
    self->order_size = 0;
    self->size = 0;
}

// ===== QUERY =====

int Core_Collections_Set_Set_has(Set* self, void* val) {
    if (!self || self->size == 0) return 0;
    String* s = quirk_opaque_to_string(val);
    const char* key = (s && s->buffer) ? s->buffer : "";
    SetEntry* e = Set__find_entry(self->entries, self->capacity, key);
    return e && e->is_occupied;
}

int Core_Collections_Set_Set_size(Set* self) { return self ? self->size : 0; }
int Core_Collections_Set_Set_is_empty(Set* self) { return !self || self->size == 0; }

// ===== SET OPERATIONS =====

Set* Core_Collections_Set_Set_union(Set* self, Set* other) {
    Set* result = (Set*)malloc(sizeof(Set));
    Core_Collections_Set_Set___init(result);
    for (int i = 0; i < self->order_size; i++) {
        SetEntry* e = Set__find_entry(self->entries, self->capacity, self->key_order[i]);
        if (e && e->is_occupied) Core_Collections_Set_Set_add(result, e->value);
    }
    for (int i = 0; i < other->order_size; i++) {
        SetEntry* e = Set__find_entry(other->entries, other->capacity, other->key_order[i]);
        if (e && e->is_occupied) Core_Collections_Set_Set_add(result, e->value);
    }
    return result;
}

Set* Core_Collections_Set_Set_intersection(Set* self, Set* other) {
    Set* result = (Set*)malloc(sizeof(Set));
    Core_Collections_Set_Set___init(result);
    for (int i = 0; i < self->order_size; i++) {
        if (Core_Collections_Set_Set_has(other, self->entries[i].value))
            Core_Collections_Set_Set_add(result, self->entries[i].value);
    }
    return result;
}

Set* Core_Collections_Set_Set_difference(Set* self, Set* other) {
    Set* result = (Set*)malloc(sizeof(Set));
    Core_Collections_Set_Set___init(result);
    for (int i = 0; i < self->order_size; i++) {
        SetEntry* e = Set__find_entry(self->entries, self->capacity, self->key_order[i]);
        if (e && e->is_occupied && !Core_Collections_Set_Set_has(other, e->value))
            Core_Collections_Set_Set_add(result, e->value);
    }
    return result;
}

// ===== ITERATOR =====

void Core_Collections_Set_SetIterator___init(SetIterator* self, Set* s) {
    self->set_ref = s;
    self->idx = 0;
}

int Core_Collections_Set_SetIterator___has_next(SetIterator* self) {
    return self && self->set_ref && self->idx < self->set_ref->order_size;
}

void* Core_Collections_Set_SetIterator___next(SetIterator* self) {
    const char* key = self->set_ref->key_order[self->idx++];
    SetEntry* e = Set__find_entry(self->set_ref->entries, self->set_ref->capacity, key);
    return e ? e->value : NULL;
}

SetIterator* Core_Collections_Set_Set___iter(Set* self) {
    SetIterator* it = (SetIterator*)malloc(sizeof(SetIterator));
    Core_Collections_Set_SetIterator___init(it, self);
    return it;
}

// ===== STRING =====

String* Core_Collections_Set_Set___str(Set* self) {
    if (!self || self->size == 0) return make_String("{}");
    int cap = 256;
    char* buf = (char*)malloc(cap);
    int len = 0;
    buf[len++] = '{';
    for (int i = 0; i < self->order_size; i++) {
        if (i > 0) {
            if (len + 3 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
            buf[len++] = ','; buf[len++] = ' ';
        }
        const char* k = self->key_order[i];
        int kl = strlen(k);
        while (len + kl + 4 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        memcpy(buf + len, k, kl);
        len += kl;
    }
    if (len + 2 >= cap) { cap += 2; buf = (char*)realloc(buf, cap); }
    buf[len++] = '}'; buf[len] = '\0';
    return make_String(buf);
}
