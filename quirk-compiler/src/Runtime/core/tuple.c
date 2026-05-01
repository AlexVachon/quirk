#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"

// Defined in runtime.c after the include chain
String* quirk_opaque_to_string(void* val);

Tuple* quirk_tuple_new(int size) {
    Tuple* t = (Tuple*)malloc(sizeof(Tuple));
    t->size = size;
    t->data = (void**)malloc(sizeof(void*) * (size > 0 ? size : 1));
    if (t->data) memset(t->data, 0, sizeof(void*) * (size > 0 ? size : 1));
    return t;
}

void quirk_tuple_set(Tuple* t, int i, void* val) {
    if (!t || i < 0 || i >= t->size) return;
    t->data[i] = val;
}

// ===== ITERATOR =====

void Core_Collections_Tuple_TupleIterator___init(TupleIterator* self, Tuple* t) {
    self->tuple_ref = t;
    self->idx = 0;
}

int Core_Collections_Tuple_TupleIterator___has_next(TupleIterator* self) {
    if (!self || !self->tuple_ref) return 0;
    return self->idx < self->tuple_ref->size;
}

void* Core_Collections_Tuple_TupleIterator___next(TupleIterator* self) {
    void* val = self->tuple_ref->data[self->idx];
    self->idx++;
    return val;
}

TupleIterator* Core_Collections_Tuple_Tuple___iter(Tuple* self) {
    TupleIterator* iter = (TupleIterator*)malloc(sizeof(TupleIterator));
    Core_Collections_Tuple_TupleIterator___init(iter, self);
    return iter;
}

// ===== ELEMENT ACCESS =====

void* Core_Collections_Tuple_Tuple___get(Tuple* t, int i) {
    if (!t || i < 0 || i >= t->size) return NULL;
    return t->data[i];
}

int Core_Collections_Tuple_Tuple_length(Tuple* self) {
    return self ? self->size : 0;
}

String* Core_Collections_Tuple_Tuple___str(Tuple* self) {
    if (!self || self->size == 0) return make_String("()");

    int cap = 256;
    char* buf = (char*)malloc(cap);
    int len = 0;

    buf[len++] = '(';
    for (int i = 0; i < self->size; i++) {
        if (i > 0) {
            if (len + 2 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
            buf[len++] = ',';
            buf[len++] = ' ';
        }
        String* s = self->data[i] ? quirk_opaque_to_string(self->data[i]) : make_String("null");
        if (s && s->buffer) {
            while (len + s->length + 4 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
            memcpy(buf + len, s->buffer, s->length);
            len += s->length;
        }
    }
    if (len + 4 >= cap) { cap += 4; buf = (char*)realloc(buf, cap); }
    buf[len++] = ')';
    buf[len] = '\0';
    return make_String(buf);
}
