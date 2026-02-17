#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"

#define INITIAL_CAPACITY 8
#define LOAD_FACTOR 0.75

// FNV-1a Hash Function (Standard, fast string hashing)
unsigned int hash_str(const char* key) {
    unsigned int hash = 2166136261u;
    while (*key) {
        hash ^= (unsigned char)(*key);
        hash *= 16777619;
        key++;
    }
    return hash;
}

// ... (Lifecycle: Map__init, Map___del remain same) ...
void Map__init(Map* self) {
    self->capacity = INITIAL_CAPACITY;
    self->size = 0;
    self->entries = (MapEntry*)calloc(self->capacity, sizeof(MapEntry));
}

void Map___del(Map* self) {
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
MapEntry* Map__find_entry(MapEntry* entries, int capacity, const char* key) {
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
void Map__resize(Map* self) {
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

void Map_put(Map* self, String* keyObj, void* value) {
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

void* Map_get(Map* self, String* keyObj) {
    if (!keyObj || !keyObj->buffer)
        return NULL;

    MapEntry* entry =
        Map__find_entry(self->entries, self->capacity, keyObj->buffer);
    if (entry && entry->is_occupied) {
        return entry->value;
    }
    return NULL;
}

int Map_has(Map* self, String* keyObj) {
    if (!keyObj || !keyObj->buffer)
        return 0;

    MapEntry* entry =
        Map__find_entry(self->entries, self->capacity, keyObj->buffer);
    return (entry && entry->is_occupied);
}

void Map_remove(Map* self, String* keyObj) {
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

int Map_len(Map* self) {
    return self->size;
}

void Map_clear(Map* self) {
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
//  OPERATORS (Updated to use String*)
// ==========================================

void* Map___get(Map* self, String* keyObj) {
    void* val = Map_get(self, keyObj);
    // Note: Map_has checks raw buffer, so it's safe to call here
    if (!val && !Map_has(self, keyObj)) {
        printf("KeyError: '%s' not found\n",
               keyObj ? keyObj->buffer : "(null)");
        exit(1);
    }
    return val;
}

void Map___set(Map* self, String* keyObj, void* value) {
    Map_put(self, keyObj, value);
}

String* Map___str(Map* self) {
    return make_String("{Map}");
}