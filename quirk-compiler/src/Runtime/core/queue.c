#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"

extern String* quirk_opaque_to_string(void* val);

#define QUEUE_INITIAL_CAP 8

// ===== LIFECYCLE =====

void Core_Collections_Queue_Queue___init(Queue* self) {
    self->capacity = QUEUE_INITIAL_CAP;
    self->data = (void**)calloc(self->capacity, sizeof(void*));
    self->head = 0;
    self->tail = 0;
    self->size = 0;
}

void Core_Collections_Queue_Queue___del(Queue* self) {
    if (self->data) { free(self->data); self->data = NULL; }
}

static void Queue__grow(Queue* self) {
    int new_cap = self->capacity * 2;
    void** new_data = (void**)malloc(new_cap * sizeof(void*));
    for (int i = 0; i < self->size; i++)
        new_data[i] = self->data[(self->head + i) % self->capacity];
    free(self->data);
    self->data = new_data;
    self->head = 0;
    self->tail = self->size;
    self->capacity = new_cap;
}

// ===== MUTATION =====

void Core_Collections_Queue_Queue_push_back(Queue* self, void* val) {
    if (!self) return;
    if (self->size >= self->capacity) Queue__grow(self);
    self->data[self->tail] = val;
    self->tail = (self->tail + 1) % self->capacity;
    self->size++;
}

void Core_Collections_Queue_Queue_push_front(Queue* self, void* val) {
    if (!self) return;
    if (self->size >= self->capacity) Queue__grow(self);
    self->head = (self->head - 1 + self->capacity) % self->capacity;
    self->data[self->head] = val;
    self->size++;
}

void* Core_Collections_Queue_Queue_pop_front(Queue* self) {
    if (!self || self->size == 0) return NULL;
    void* val = self->data[self->head];
    self->head = (self->head + 1) % self->capacity;
    self->size--;
    return val;
}

void* Core_Collections_Queue_Queue_pop_back(Queue* self) {
    if (!self || self->size == 0) return NULL;
    self->tail = (self->tail - 1 + self->capacity) % self->capacity;
    void* val = self->data[self->tail];
    self->size--;
    return val;
}

// ===== QUERY =====

void* Core_Collections_Queue_Queue_peek_front(Queue* self) {
    return (self && self->size > 0) ? self->data[self->head] : NULL;
}

void* Core_Collections_Queue_Queue_peek_back(Queue* self) {
    if (!self || self->size == 0) return NULL;
    return self->data[(self->tail - 1 + self->capacity) % self->capacity];
}

int Core_Collections_Queue_Queue_size(Queue* self) { return self ? self->size : 0; }
int Core_Collections_Queue_Queue_is_empty(Queue* self) { return !self || self->size == 0; }

void Core_Collections_Queue_Queue_clear(Queue* self) {
    if (!self) return;
    self->head = self->tail = self->size = 0;
}

// ===== ITERATOR (front to back) =====

void Core_Collections_Queue_QueueIterator___init(QueueIterator* self, Queue* q) {
    self->queue_ref = q;
    self->pos = 0;
}

int Core_Collections_Queue_QueueIterator___has_next(QueueIterator* self) {
    return self && self->queue_ref && self->pos < self->queue_ref->size;
}

void* Core_Collections_Queue_QueueIterator___next(QueueIterator* self) {
    Queue* q = self->queue_ref;
    void* val = q->data[(q->head + self->pos) % q->capacity];
    self->pos++;
    return val;
}

QueueIterator* Core_Collections_Queue_Queue___iter(Queue* self) {
    QueueIterator* it = (QueueIterator*)malloc(sizeof(QueueIterator));
    Core_Collections_Queue_QueueIterator___init(it, self);
    return it;
}

// ===== STRING =====

String* Core_Collections_Queue_Queue___str(Queue* self) {
    if (!self || self->size == 0) return make_String("Queue[]");
    int cap = 256;
    char* buf = (char*)malloc(cap);
    int len = 0;
    const char* prefix = "Queue[";
    int pl = strlen(prefix);
    memcpy(buf, prefix, pl); len = pl;
    for (int i = 0; i < self->size; i++) {
        if (i > 0) {
            if (len + 3 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
            buf[len++] = ','; buf[len++] = ' ';
        }
        void* v = self->data[(self->head + i) % self->capacity];
        String* s = quirk_opaque_to_string(v);
        if (s && s->buffer) {
            while (len + s->length + 4 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
            memcpy(buf + len, s->buffer, s->length);
            len += s->length;
        }
    }
    if (len + 2 >= cap) { cap += 2; buf = (char*)realloc(buf, cap); }
    buf[len++] = ']'; buf[len] = '\0';
    return make_String(buf);
}
