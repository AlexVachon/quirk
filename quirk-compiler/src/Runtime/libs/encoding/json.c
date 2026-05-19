#include "../../types.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

extern void* GC_malloc(size_t);
extern String* make_String(const char* raw);
extern String* make_String_taking_ownership(char* raw);

// Any* boxing helpers from core/any.c — used to give JSON primitives their
// real Quirk types on the way out of loads(). Without these, every numeric
// or boolean field would come back as a String, forcing callers to do
// Int.parse(m.get("age")) by hand.
extern Any* Core_Primitives_Any_box_int(int32_t v);
extern Any* Core_Primitives_Any_box_double(double v);
extern Any* Core_Primitives_Any_box_bool(int32_t v);
extern Any* Core_Primitives_Any_box_null(void);

// Forward Declaration
void* Encoding_Json__parse_value(const char** cursor);

static void Json__skip_whitespace(const char** cursor) {
    while (isspace(**cursor)) (*cursor)++;
}

// Returns a String* so quirk_opaque_to_string and Json__serialize_any can detect it
static String* Json__parse_raw_string(const char** cursor) {
    (*cursor)++; // Skip opening quote
    const char* start = *cursor;
    while (**cursor && **cursor != '"') {
        if (**cursor == '\\') (*cursor)++;
        (*cursor)++;
    }
    int len = *cursor - start;
    char* buf = (char*)GC_malloc(len + 1);
    strncpy(buf, start, len);
    buf[len] = '\0';
    (*cursor)++; // Skip closing quote
    return make_String_taking_ownership(buf);
}

// Parse a JSON number into a typed Any* — Int if it has no decimal/exponent,
// Double otherwise. Failure to parse yields ANY_INT 0; the cursor is still
// advanced past the malformed run so the outer parser doesn't loop.
static Any* Json__parse_number(const char** cursor) {
    const char* start = *cursor;
    int is_float = 0;
    if (**cursor == '-') (*cursor)++;
    while (isdigit(**cursor) || **cursor == '.' || **cursor == 'e' ||
           **cursor == 'E' || **cursor == '+' || **cursor == '-') {
        if (**cursor == '.' || **cursor == 'e' || **cursor == 'E') is_float = 1;
        (*cursor)++;
    }
    int len = *cursor - start;
    char tmp[64];
    if (len >= (int)sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, start, len);
    tmp[len] = '\0';
    if (is_float) return Core_Primitives_Any_box_double(strtod(tmp, NULL));
    return Core_Primitives_Any_box_int((int32_t)strtol(tmp, NULL, 10));
}

Map* Encoding_Json__parse_object(const char** cursor) {
    (*cursor)++; // Skip '{'
    
    Map* map = (Map*)GC_malloc(sizeof(Map));
    extern void Core_Collections_Map_Map___init(Map*);
    extern void Core_Collections_Map_Map_put(Map*, String*, void*);
    Core_Collections_Map_Map___init(map);

    Json__skip_whitespace(cursor);
    if (**cursor == '}') {
        (*cursor)++;
        return map;
    }

    while (**cursor) {
        Json__skip_whitespace(cursor);
        if (**cursor != '"') break; 
        
        // Map Keys MUST be String Objects
        String* keyObj = Json__parse_raw_string(cursor);
        
        Json__skip_whitespace(cursor);
        if (**cursor == ':') (*cursor)++;
        
        // Map Values are raw C-strings (so 'Any' can auto-box them safely!)
        void* val = Encoding_Json__parse_value(cursor);
        Core_Collections_Map_Map_put(map, keyObj, val); 
        
        Json__skip_whitespace(cursor);
        if (**cursor == ',') {
            (*cursor)++;
        } else if (**cursor == '}') {
            (*cursor)++;
            break;
        }
    }
    return map;
}

List* Encoding_Json__parse_array(const char** cursor) {
    (*cursor)++; // Skip '['
    
    List* list = (List*)GC_malloc(sizeof(List));
    extern void Core_Collections_List_List___init(List*);
    extern void Core_Collections_List_List_append(List*, void*);
    Core_Collections_List_List___init(list);

    Json__skip_whitespace(cursor);
    if (**cursor == ']') {
        (*cursor)++;
        return list;
    }

    while (**cursor) {
        void* val = Encoding_Json__parse_value(cursor);
        Core_Collections_List_List_append(list, val); 
        
        Json__skip_whitespace(cursor);
        if (**cursor == ',') {
            (*cursor)++;
        } else if (**cursor == ']') {
            (*cursor)++;
            break;
        }
    }
    return list;
}

void* Encoding_Json__parse_value(const char** cursor) {
    Json__skip_whitespace(cursor);
    
    if (**cursor == '"') return Json__parse_raw_string(cursor);
    if (**cursor == '{') return Encoding_Json__parse_object(cursor);
    if (**cursor == '[') return Encoding_Json__parse_array(cursor);
    if (isdigit(**cursor) || **cursor == '-') return Json__parse_number(cursor);

    // Booleans and null become real Any* values so callers get typed
    // results without an Int.parse / String.eq dance.
    if (strncmp(*cursor, "true", 4) == 0)  { *cursor += 4; return Core_Primitives_Any_box_bool(1); }
    if (strncmp(*cursor, "false", 5) == 0) { *cursor += 5; return Core_Primitives_Any_box_bool(0); }
    if (strncmp(*cursor, "null", 4) == 0)  { *cursor += 4; return Core_Primitives_Any_box_null(); }

    (*cursor)++;
    return NULL;
}

void* Encoding_Json_parse(String* json_str) {
    if (!json_str || !json_str->buffer) return NULL;
    const char* cursor = json_str->buffer;
    return Encoding_Json__parse_value(&cursor);
}
// ===================================================
//  JSON SERIALIZATION
//  Any-aware: handles Map, List, String, Int, Double,
//  Bool, Null, and ISerializable structs recursively.
// ===================================================

// Forward declarations.
// `indent` is the per-level width in spaces. 0 means compact (no
// newlines / no spacing); >0 means pretty-print, with `depth` tracking
// the current nesting level.
static void Json__serialize_any(void* val, AnyTag hint, char** buf, int* cap, int* len, int indent, int depth);
static void Json__buf_append(char** buf, int* cap, int* len, const char* s);
static void Json__buf_append_escaped(char** buf, int* cap, int* len, const char* s);

static void Json__buf_newline_indent(char** buf, int* cap, int* len, int indent, int depth) {
    if (indent <= 0) return;
    int spaces = indent * depth;
    int needed = *len + 1 + spaces + 1;
    if (needed >= *cap) {
        while (needed >= *cap) *cap *= 2;
        *buf = (char*)realloc(*buf, *cap);
    }
    (*buf)[(*len)++] = '\n';
    for (int i = 0; i < spaces; i++) (*buf)[(*len)++] = ' ';
    (*buf)[*len] = '\0';
}

static void Json__buf_ensure(char** buf, int* cap, int needed) {
    while (needed >= *cap) *cap *= 2;
    *buf = (char*)realloc(*buf, *cap);
}

static void Json__buf_append(char** buf, int* cap, int* len, const char* s) {
    if (!s) return;
    int slen = (int)strlen(s);
    if (*len + slen + 1 >= *cap) Json__buf_ensure(buf, cap, *len + slen + 1);
    memcpy(*buf + *len, s, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}

static void Json__buf_append_char(char** buf, int* cap, int* len, char c) {
    if (*len + 2 >= *cap) Json__buf_ensure(buf, cap, *len + 2);
    (*buf)[(*len)++] = c;
    (*buf)[*len] = '\0';
}

static void Json__buf_append_escaped(char** buf, int* cap, int* len, const char* s) {
    if (!s) { Json__buf_append(buf, cap, len, "null"); return; }
    Json__buf_append_char(buf, cap, len, '"');
    while (*s) {
        char c = *s++;
        switch (c) {
            case '"':  Json__buf_append(buf, cap, len, "\\\""); break;
            case '\\': Json__buf_append(buf, cap, len, "\\\\"); break;
            case '\n': Json__buf_append(buf, cap, len, "\\n");  break;
            case '\r': Json__buf_append(buf, cap, len, "\\r");  break;
            case '\t': Json__buf_append(buf, cap, len, "\\t");  break;
            default:    Json__buf_append_char(buf, cap, len, c);  break;
        }
    }
    Json__buf_append_char(buf, cap, len, '"');
}

static void Json__serialize_map(Map* map, char** buf, int* cap, int* len, int indent, int depth) {
    Json__buf_append_char(buf, cap, len, '{');
    int first = 1;
    for (int i = 0; i < map->capacity; i++) {
        if (!map->entries[i].is_occupied) continue;
        if (!first) Json__buf_append_char(buf, cap, len, ',');
        Json__buf_newline_indent(buf, cap, len, indent, depth + 1);
        first = 0;
        Json__buf_append_escaped(buf, cap, len, map->entries[i].key);
        Json__buf_append_char(buf, cap, len, ':');
        if (indent > 0) Json__buf_append_char(buf, cap, len, ' ');
        // Value — stored as void*, could be anything. Since map values
        // don't carry an AnyTag, dispatch via ANY_PTR and let the
        // heuristic in Json__serialize_any figure it out.
        Json__serialize_any(map->entries[i].value, ANY_PTR, buf, cap, len, indent, depth + 1);
    }
    if (!first) Json__buf_newline_indent(buf, cap, len, indent, depth);
    Json__buf_append_char(buf, cap, len, '}');
}

static void Json__serialize_list(List* list, char** buf, int* cap, int* len, int indent, int depth) {
    Json__buf_append_char(buf, cap, len, '[');
    for (int i = 0; i < list->size; i++) {
        if (i > 0) Json__buf_append_char(buf, cap, len, ',');
        Json__buf_newline_indent(buf, cap, len, indent, depth + 1);
        Json__serialize_any(list->data[i], ANY_PTR, buf, cap, len, indent, depth + 1);
    }
    if (list->size > 0) Json__buf_newline_indent(buf, cap, len, indent, depth);
    Json__buf_append_char(buf, cap, len, ']');
}

// Serialize a value whose dynamic type we must infer heuristically.
// When hint is ANY_PTR (i.e. stored in a Map or List as void*), we
// distinguish by the stored pointer's first-byte pattern:
//   - NULL              → null
//   - Any* (tag 0..8)   → dispatch on tag
//   - Map* / List*      → structural check via size/capacity fields
//   - Otherwise         → treat as raw C-string (the json.c parser always
//                         stores string/number values as char* directly)
static void Json__serialize_any(void* val, AnyTag hint, char** buf, int* cap, int* len, int indent, int depth) {
    if (!val) { Json__buf_append(buf, cap, len, "null"); return; }

    // If we have an explicit Any* (hint == ANY_INT etc.), dispatch directly.
    if (hint != ANY_PTR) {
        Any* a = (Any*)val;
        char numbuf[64];
        switch (hint) {
            case ANY_INT:    snprintf(numbuf, 64, "%d", a->ival);  Json__buf_append(buf, cap, len, numbuf); return;
            case ANY_DOUBLE: snprintf(numbuf, 64, "%g", a->dval);  Json__buf_append(buf, cap, len, numbuf); return;
            case ANY_BOOL:   Json__buf_append(buf, cap, len, a->ival ? "true" : "false"); return;
            case ANY_CHAR: {
                char cb[3] = {'"', (char)a->ival, '"'}; cb[3] = '\0';
                Json__buf_append(buf, cap, len, cb); return;
            }
            case ANY_STRING: Json__buf_append_escaped(buf, cap, len, a->ptr ? ((String*)a->ptr)->buffer : NULL); return;
            case ANY_LIST:   Json__serialize_list((List*)a->ptr, buf, cap, len, indent, depth); return;
            case ANY_MAP:    Json__serialize_map((Map*)a->ptr, buf, cap, len, indent, depth); return;
            case ANY_NULL:   Json__buf_append(buf, cap, len, "null"); return;
            default: break;
        }
    }

    // Heuristic dispatch for untagged void* (Quirk codegen stores Map*/List*/String* as void*)
    //
    // Layout reference (64-bit little-endian):
    //   Any*    — [i32 tag @0]                            tag is 0..8
    //   String* — [char* buf @0][i32 len @8]              GC_malloc(16)
    //   Map*    — [MapEntry* @0][i32 cap @8][i32 sz @12]  cap is pow2 >=8
    //   List*   — [void** @0][i32 sz @8][i32 cap @12]     cap >=1 (compiler may use 2)
    //   char*   — raw C-string (json.c parser scalars)

    // Detection order:
    //   0. ISerializable — checked FIRST, using only the safe first 4 bytes
    //   1. Any*     — tag 0..8, checked BEFORE inner_ptr dereference (prevents crash
    //                 when tag value is a small int like 6 that would be dereferenced)
    //   2. String*  — inner_ptr[0] >= 32 (printable), len 0-4096, buf[len]=='\0'
    //   3. Map*     — pow2 cap>=8, entries[0].is_occupied <= 1
    //   4. List*    — cap 1-65536, size <= cap
    //   5. char*    — raw C-string fallback from json parser
    //
    // ISerializable is checked FIRST because user structs (e.g. Point{i32,i32})
    // may be smaller than 16 bytes — reading inner_ptr or offset-8 fields would
    // go out of bounds. The registry check only reads the pointer itself (safe).
    // GC reuse false-positives are prevented because dumps() is always called
    // immediately after __init + Quirk_register_serializable, before GC can
    // reclaim the address for a Map/List/String.

    // 0. ISerializable registry — must come before any offset reads
    {
        extern int Quirk_is_serializable(void*);
        extern String* Quirk_ISerializable_to_json(void*);
        if (Quirk_is_serializable(val)) {
            String* r = Quirk_ISerializable_to_json(val);
            Json__buf_append(buf, cap, len, (r && r->buffer) ? r->buffer : "null");
            return;
        }
    }

    // 1. Any* — check BEFORE reading inner_ptr to prevent crash when tag is a small int
    //    (e.g. Any*{tag=6} means inner_ptr=6, dereferencing it → SIGSEGV)
    {
        int32_t possible_tag = *((int32_t*)val);
        if (possible_tag >= ANY_INT && possible_tag <= ANY_NULL) {
            Any* a = (Any*)val;
            Json__serialize_any(val, (AnyTag)a->tag, buf, cap, len, indent, depth);
            return;
        }
    }

    // All remaining types are at least 16 bytes, so reading inner_ptr and
    // offset-8/12 fields is safe. Guard against null/small invalid pointers.
    void* inner_ptr = *((void**)val);
    if (!inner_ptr || (uintptr_t)inner_ptr <= 0xFFFFUL) {
        Json__buf_append(buf, cap, len, "null");
        return;
    }

    // 2. String* — [char* buf @0][i32 len @8]
    //    buf[0] >= 32: printable ASCII, never confused with Map (is_occupied=0/1)
    {
        int32_t str_len = *((int32_t*)((char*)val + 8));
        unsigned char buf_first = *((unsigned char*)inner_ptr);
        if (str_len >= 0 && str_len <= 4096 && buf_first >= 32) {
            const char* str_buf = (const char*)inner_ptr;
            if (str_buf[str_len] == '\0') {
                Json__buf_append_escaped(buf, cap, len, str_buf);
                return;
            }
        }
    }

    // 3. Map* — [entries* @0][cap i32 @8][size i32 @12]
    //    MapEntry layout: { char* key @0, void* value @8, int is_occupied @16, int is_deleted @20 }
    //    is_occupied is always exactly 0 or 1 (set explicitly by Map_put/Map_remove).
    //    We must read offset 16, NOT offset 0 (key) — key is a non-null heap pointer
    //    when slot 0 is occupied, making key[0] > 1 and breaking the old check.
    {
        int32_t map_cap  = *((int32_t*)((char*)val + 8));
        int32_t map_size = *((int32_t*)((char*)val + 12));
        int32_t is_occupied_0 = *((int32_t*)((char*)inner_ptr + 16));
        if (map_cap >= 8 && (map_cap & (map_cap-1)) == 0 &&
            map_size >= 0 && map_size <= map_cap &&
            (is_occupied_0 == 0 || is_occupied_0 == 1)) {
            Json__serialize_map((Map*)val, buf, cap, len, indent, depth);
            return;
        }
    }

    // 4. List* — [data* @0][size i32 @8][cap i32 @12]
    //    cap >= 1 (compiler-emitted lists may have cap=2).
    {
        int32_t list_size = *((int32_t*)((char*)val + 8));
        int32_t list_cap  = *((int32_t*)((char*)val + 12));
        if (list_cap >= 1 && list_cap <= 65536 &&
            list_size >= 0 && list_size <= list_cap) {
            Json__serialize_list((List*)val, buf, cap, len, indent, depth);
            return;
        }
    }

    // 5. Raw C-string (json.c parser scalars — printable ASCII)
    const char* s = (const char*)val;
    unsigned char first = (unsigned char)s[0];
    if (first >= 32 && first < 128) {
        int is_num = (first == '-' || (first >= '0' && first <= '9'));
        if (is_num || strcmp(s,"true")==0 || strcmp(s,"false")==0 || strcmp(s,"null")==0)
            Json__buf_append(buf, cap, len, s);
        else
            Json__buf_append_escaped(buf, cap, len, s);
    } else {
        Json__buf_append(buf, cap, len, "null");
    }
}

// ===================================================
//  PUBLIC SERIALIZATION API
// ===================================================

// Serialize any value stored in an Any* to a JSON string.
String* Encoding_Json_serialize_any(Any* val) {
    if (!val) return make_String("null");
    int cap = 256, len = 0;
    char* buf = (char*)malloc(cap);
    buf[0] = '\0';
    Json__serialize_any(val, (AnyTag)val->tag, &buf, &cap, &len, 0, 0);
    String* _r = make_String(buf); free(buf); return _r;
}

// Serialize a Map* directly to a JSON object string.
// This is what Quirk's core/json.quirk calls for map_to_json().
String* Encoding_Json_map_to_json(Map* map) {
    if (!map) return make_String("null");
    int cap = 256, len = 0;
    char* buf = (char*)malloc(cap);
    buf[0] = '\0';
    Json__serialize_map(map, &buf, &cap, &len, 0, 0);
    String* _r = make_String(buf); free(buf); return _r;
}

// Serialize a List* directly to a JSON array string.
String* Encoding_Json_list_to_json(List* list) {
    if (!list) return make_String("null");
    int cap = 256, len = 0;
    char* buf = (char*)malloc(cap);
    buf[0] = '\0';
    Json__serialize_list(list, &buf, &cap, &len, 0, 0);
    String* _r = make_String(buf); free(buf); return _r;
}
// ===================================================
//  dumps — public entry point from Quirk
//
//  Accepts any value as void* and serializes it to JSON.
//  Uses the same Any-tag heuristic as Json__serialize_any.
//  This avoids needing `is` type-check codegen in Quirk.
// ===================================================
extern int    Quirk_is_serializable(void* self);
extern String* Quirk_ISerializable_to_json(void* self);

String* Encoding_Json_dumps(void* val) {
    if (!val) return make_String("null");
    // Do NOT call Quirk_is_serializable here: GC can reuse a freed struct's
    // address for a Map/List/String, causing false positives. The registry
    // check is step 5 inside Json__serialize_any, after structural checks.
    int cap = 512, len = 0;
    char* buf = (char*)malloc(cap);
    buf[0] = '\0';
    Json__serialize_any(val, ANY_PTR, &buf, &cap, &len, 0, 0);
    String* _r = make_String(buf); free(buf); return _r;
}

// Pretty-print variant: emits newlines and `indent` spaces per nesting
// level. `indent <= 0` falls back to compact output (matches dumps).
// Quirk's dumps() wrapper picks this entry when the caller passes
// `pretty: true` or a non-zero `indent`.
String* Encoding_Json_dumps_indent(void* val, int32_t indent) {
    if (!val) return make_String("null");
    if (indent < 0) indent = 0;
    int cap = 512, len = 0;
    char* buf = (char*)malloc(cap);
    buf[0] = '\0';
    Json__serialize_any(val, ANY_PTR, &buf, &cap, &len, (int)indent, 0);
    String* _r = make_String(buf); free(buf); return _r;
}

// ===================================================
//  SYMBOL ALIASES — old IR / json.quirk extern compatibility
// ===================================================
String* Encoding_Json__json_serialize_map(Map* map)   { return Encoding_Json_map_to_json(map); }
String* Encoding_Json__json_serialize_list(List* list) { return Encoding_Json_list_to_json(list); }
void*   Encoding_Json_loads(String* s)                 { return Encoding_Json_parse(s); }