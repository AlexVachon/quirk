#include "../types.h"

// Forward declarations from string.c
int    Core_String_String___eq(String* self, String* other);
int    Core_String_String_to_int(String* self);
double Core_String_String_to_float(String* self);
String* Core_Collections_List_List___str(List* self);
String* Core_Collections_Map_Map___str(Map* self);

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
        default:         return make_String("Unknown");
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
        case ANY_PTR:  return make_String("<ptr>");
        case ANY_NULL: return make_String("null");
        default:       return make_String("?");
    }
}

// Alias — matches the naming convention used in __str dispatch
String* Core_Primitives_Any_to_str(Any* a) { return Core_Primitives_Any_to_string(a); }
String* Core_Primitives_Any___str(Any* a)  { return Core_Primitives_Any_to_string(a); }