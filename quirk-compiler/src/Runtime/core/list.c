#include "types.h"

void List_append(List* l, void* item) {
    if (l->length >= l->capacity) {
        l->capacity *= 2;
        l->data = realloc(l->data, sizeof(void*) * l->capacity);
    }
    l->data[l->length++] = item;
}

void* List_get(List* l, int index) {
    if (!l || !l->data)
        return 0;
    if (index < 0 || index >= l->length)
        return 0;
    return l->data[index];
}