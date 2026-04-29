#include "../types.h"

// Forward declarations from string.c
int    Core_String_String___eq(String* self, String* other);
int    Core_String_String_to_int(String* self);
double Core_String_String_to_float(String* self);
String* Core_Collections_List_List___str(List* self);
String* Core_Collections_Map_Map___str(Map* self);
String* Core_Collections_Tuple_Tuple___str(Tuple* self);
String* Core_Callable_Callable___str(Callable* self);

// ===================================================
//  BOX — wrap a primitive/struct into Any*
// ===================================================

Any* Core_Primitives_Any_box_int(int32_t v) {
    Any* a = (Any*)malloc(sizeof(Any));
    a->tag = ANY_INT; a->ival = v; a->dval = 0.0; a->ptr = NULL;
    return a;
}

Any* Core_Primitives_Any_box_double(double v) {
    Any* a = (Any*)malloc(sizeof(Any));
    a->tag = ANY_DOUBLE; a->ival = 0; a->dval = v; a->ptr = NULL;
    return a;
}

Any* Core_Primitives_Any_box_bool(int32_t v) {
    Any* a = (Any*)malloc(sizeof(Any));
    a->tag = ANY_BOOL; a->ival = v ? 1 : 0; a->dval = 0.0; a->ptr = NULL;
    return a;
}

Any* Core_Primitives_Any_box_char(int32_t v) {
    Any* a = (Any*)malloc(sizeof(Any));
    a->tag = ANY_CHAR; a->ival = v; a->dval = 0.0; a->ptr = NULL;
    return a;
}

Any* Core_Primitives_Any_box_string(String* v) {
    Any* a = (Any*)malloc(sizeof(Any));
    a->tag = ANY_STRING; a->ival = 0; a->dval = 0.0; a->ptr = v;
    return a;
}

Any* Core_Primitives_Any_box_list(List* v) {
    Any* a = (Any*)malloc(sizeof(Any));
    a->tag = ANY_LIST; a->ival = 0; a->dval = 0.0; a->ptr = v;
    return a;
}

Any* Core_Primitives_Any_box_map(Map* v) {
    Any* a = (Any*)malloc(sizeof(Any));
    a->tag = ANY_MAP; a->ival = 0; a->dval = 0.0; a->ptr = v;
    return a;
}

Any* Core_Primitives_Any_box_ptr(void* v) {
    Any* a = (Any*)malloc(sizeof(Any));
    a->tag = ANY_PTR; a->ival = 0; a->dval = 0.0; a->ptr = v;
    return a;
}

Any* Core_Primitives_Any_box_null(void) {
    Any* a = (Any*)malloc(sizeof(Any));
    a->tag = ANY_NULL; a->ival = 0; a->dval = 0.0; a->ptr = NULL;
    return a;
}

Any* Core_Primitives_Any_box_tuple(Tuple* v) {
    Any* a = (Any*)malloc(sizeof(Any));
    a->tag = ANY_TUPLE; a->ival = 0; a->dval = 0.0; a->ptr = v;
    return a;
}

Any* Core_Primitives_Any_box_callable(Callable* v) {
    Any* a = (Any*)malloc(sizeof(Any));
    a->tag = ANY_CALLABLE; a->ival = 0; a->dval = 0.0; a->ptr = v;
    return a;
}

// ===================================================
//  UNBOX — extract a concrete value from Any*
// ===================================================

int32_t Core_Primitives_Any_to_int(Any* a) {
    if (!a) return 0;
    switch (a->tag) {
        case ANY_INT:    return a->ival;
        case ANY_BOOL:   return a->ival;
        case ANY_CHAR:   return a->ival;
        case ANY_DOUBLE: return (int32_t)a->dval;
        case ANY_STRING: return a->ptr ? Core_String_String_to_int((String*)a->ptr) : 0;
        default:         return 0;
    }
}

double Core_Primitives_Any_to_float(Any* a) {
    if (!a) return 0.0;
    switch (a->tag) {
        case ANY_DOUBLE: return a->dval;
        case ANY_INT:    return (double)a->ival;
        case ANY_BOOL:   return (double)a->ival;
        case ANY_STRING: return a->ptr ? Core_String_String_to_float((String*)a->ptr) : 0.0;
        default:         return 0.0;
    }
}

// ===================================================
//  TYPE INFO
// ===================================================

String* Core_Primitives_Any_get_type(Any* a) {
    if (!a) return make_String("Null");
    switch (a->tag) {
        case ANY_INT:    return make_String("Int");
        case ANY_DOUBLE: return make_String("Double");
        case ANY_BOOL:   return make_String("Bool");
        case ANY_CHAR:   return make_String("Char");
        case ANY_STRING: return make_String("String");
        case ANY_LIST:   return make_String("List");
        case ANY_MAP:    return make_String("Map");
        case ANY_PTR:    return make_String("Ptr");
        case ANY_NULL:   return make_String("Null");
        case ANY_TUPLE:    return make_String("Tuple");
        case ANY_CALLABLE: return make_String("Callable");
        default:           return make_String("Unknown");
    }
}

// isinstance(val, "String") etc.
int Core_Primitives_Any_isinstance(Any* a, String* type_name) {
    if (!a || !type_name) return 0;
    String* t = Core_Primitives_Any_get_type(a);
    return Core_String_String___eq(t, type_name);
}

// ===================================================
//  TO STRING — dispatch by tag
// ===================================================

String* Core_Primitives_Any_to_string(Any* a) {
    if (!a) return make_String("null");
    switch (a->tag) {
        case ANY_INT:  return Core_Primitives_Int_str(a->ival);
        case ANY_DOUBLE: return Core_Primitives_Double_str(a->dval);
        case ANY_BOOL: return make_String(a->ival ? "true" : "false");
        case ANY_CHAR: {
            char buf[2] = {(char)a->ival, '\0'};
            return make_String(buf);
        }
        case ANY_STRING: {
            if (!a->ptr) return make_String("");
            return (String*)a->ptr;
        }
        case ANY_LIST: {
            if (!a->ptr) return make_String("[]");
            String* s = Core_Collections_List_List___str((List*)a->ptr);
            return s ? s : make_String("[]");
        }
        case ANY_MAP: {
            if (!a->ptr) return make_String("{}");
            String* s = Core_Collections_Map_Map___str((Map*)a->ptr);
            return s ? s : make_String("{}");
        }
        case ANY_TUPLE: {
            if (!a->ptr) return make_String("()");
            String* s = Core_Collections_Tuple_Tuple___str((Tuple*)a->ptr);
            return s ? s : make_String("()");
        }
        case ANY_CALLABLE: {
            if (!a->ptr) return make_String("<Callable>");
            return Core_Callable_Callable___str((Callable*)a->ptr);
        }
        case ANY_PTR:  return make_String("<ptr>");
        case ANY_NULL: return make_String("null");
        default:       return make_String("?");
    }
}

// Alias — matches the naming convention used in __str dispatch
String* Core_Primitives_Any_to_str(Any* a) { return Core_Primitives_Any_to_string(a); }
String* Core_Primitives_Any___str(Any* a)  { return Core_Primitives_Any_to_string(a); }
// ===================================================
//  STRUCTURAL isinstance — works for raw pointers
//
//  Core_Primitives_Any_isinstance takes Any* which needs
//  a tag field. This version uses structural heuristics
//  identical to json.c so `val is Map`, `val is String`
//  etc. work when val is a raw Quirk struct pointer.
// ===================================================
extern int32_t Core_String_String___eq_cstr(String* s, const char* cstr);

static int _is_pow2_cap(int v) {
    return v >= 8 && v <= 65536 && (v & (v-1)) == 0;
}

int Core_Primitives_Quirk_isinstance(void* val, String* type_str) {
    if (!val || !type_str || !type_str->buffer) return 0;
    const char* t = type_str->buffer;

    // Any* check (tag 0-10, inclusive of ANY_CALLABLE)
    int32_t first_i32 = *((int32_t*)val);
    if (first_i32 >= 0 && first_i32 <= 10) {
        Any* a = (Any*)val;
        String* actual = Core_Primitives_Any_get_type(a);
        return Core_String_String___eq(actual, type_str);
    }

    void* inner = *((void**)val);
    if (!inner) return strcmp(t, "Null") == 0;

    // String*: [char* @0][i32 len @8], len 0-4096, buf[len]=='\0'
    if (strcmp(t, "String") == 0) {
        int32_t slen = *((int32_t*)((char*)val + 8));
        if (slen >= 0 && slen <= 4096) {
            const char* buf = (const char*)inner;
            return buf[slen] == '\0';
        }
        return 0;
    }

    // Map*: [ptr @0][cap i32 @8][size i32 @12]
    if (strcmp(t, "Map") == 0) {
        int32_t cap  = *((int32_t*)((char*)val + 8));
        int32_t size = *((int32_t*)((char*)val + 12));
        return _is_pow2_cap(cap) && size >= 0 && size <= cap;
    }

    // List*: [ptr @0][size i32 @8][cap i32 @12]
    if (strcmp(t, "List") == 0) {
        int32_t size = *((int32_t*)((char*)val + 8));
        int32_t cap  = *((int32_t*)((char*)val + 12));
        return _is_pow2_cap(cap) && size >= 0 && size <= cap;
    }

    // For user-defined struct types and ISerializable, we cannot determine
    // the type without RTTI. Return 0 (unknown).
    return 0;
}

// ===================================================
//  ISerializable vtable registry
//
//  Since Quirk structs have no vtable, we maintain a
//  simple hash map from (void* instance) -> to_json_fn.
//  Structs that inherit ISerializable call
//  Quirk_register_serializable(self, their_to_json_fn)
//  inside __init. ISerializable_to_json does a lookup.
// ===================================================
#include <stdlib.h>

typedef String* (*ToJsonFn)(void*);

#define SERIAL_REGISTRY_CAP 256
typedef struct { void* key; ToJsonFn fn; } SerialEntry;
static SerialEntry _serial_registry[SERIAL_REGISTRY_CAP];
static int _serial_count = 0;

void Quirk_register_serializable(void* instance, ToJsonFn fn) {
    if (_serial_count < SERIAL_REGISTRY_CAP) {
        _serial_registry[_serial_count].key = instance;
        _serial_registry[_serial_count].fn  = fn;
        _serial_count++;
    }
}

static ToJsonFn Quirk_lookup_to_json(void* instance) {
    for (int i = 0; i < _serial_count; i++) {
        if (_serial_registry[i].key == instance)
            return _serial_registry[i].fn;
    }
    return NULL;
}

// Called from ISerializable_to_json in the IR — does vtable lookup
String* Quirk_ISerializable_to_json(void* self) {
    if (!self) return make_String("null");
    ToJsonFn fn = Quirk_lookup_to_json(self);
    if (fn) return fn(self);
    return make_String("null");
}

// Also used by Encoding_Json_dumps for ISerializable dispatch
int Quirk_is_serializable(void* self) {
    if (!self) return 0;
    return Quirk_lookup_to_json(self) != NULL;
}