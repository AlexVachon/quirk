#include "../../types.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

extern void* GC_malloc(size_t);

// Forward Declaration
void* Json__parse_value(const char** cursor);

void Json__skip_whitespace(const char** cursor) {
    while (isspace(**cursor)) (*cursor)++;
}

// --- FIX: Returns a RAW C-string (used for dynamic Any values) ---
char* Json__parse_raw_string(const char** cursor) {
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
    return buf; 
}

// --- FIX: Returns a RAW C-string ---
char* Json__parse_raw_number(const char** cursor) {
    const char* start = *cursor;
    if (**cursor == '-') (*cursor)++;
    while (isdigit(**cursor) || **cursor == '.' || **cursor == 'e' || 
           **cursor == 'E' || **cursor == '+' || **cursor == '-') {
        (*cursor)++;
    }
    int len = *cursor - start;
    
    char* buf = (char*)GC_malloc(len + 1);
    strncpy(buf, start, len);
    buf[len] = '\0';
    
    return buf; 
}

Map* Json__parse_object(const char** cursor) {
    (*cursor)++; // Skip '{'
    
    Map* map = (Map*)GC_malloc(sizeof(Map));
    extern void Map__init(Map*);
    extern void Map_put(Map*, String*, void*);
    Map__init(map);

    Json__skip_whitespace(cursor);
    if (**cursor == '}') {
        (*cursor)++;
        return map;
    }

    while (**cursor) {
        Json__skip_whitespace(cursor);
        if (**cursor != '"') break; 
        
        // Map Keys MUST be String Objects
        char* raw_key = Json__parse_raw_string(cursor);
        String* keyObj = make_String_taking_ownership(raw_key);
        
        Json__skip_whitespace(cursor);
        if (**cursor == ':') (*cursor)++;
        
        // Map Values are raw C-strings (so 'Any' can auto-box them safely!)
        void* val = Json__parse_value(cursor);
        Map_put(map, keyObj, val); 
        
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

List* Json__parse_array(const char** cursor) {
    (*cursor)++; // Skip '['
    
    List* list = (List*)GC_malloc(sizeof(List));
    extern void List__init(List*, int);
    extern void List_append(List*, void*);
    List__init(list, 4);

    Json__skip_whitespace(cursor);
    if (**cursor == ']') {
        (*cursor)++;
        return list;
    }

    while (**cursor) {
        void* val = Json__parse_value(cursor);
        List_append(list, val); 
        
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

void* Json__parse_value(const char** cursor) {
    Json__skip_whitespace(cursor);
    
    if (**cursor == '"') return Json__parse_raw_string(cursor);
    if (**cursor == '{') return Json__parse_object(cursor);
    if (**cursor == '[') return Json__parse_array(cursor);
    if (isdigit(**cursor) || **cursor == '-') return Json__parse_raw_number(cursor);
    
    if (strncmp(*cursor, "true", 4) == 0) {
        *cursor += 4;
        return (void*)1;
    }
    if (strncmp(*cursor, "false", 5) == 0) {
        *cursor += 5;
        return (void*)0;
    }
    if (strncmp(*cursor, "null", 4) == 0) {
        *cursor += 4;
        return NULL;
    }
    
    (*cursor)++; 
    return NULL;
}

void* Json_parse(String* json_str) {
    if (!json_str || !json_str->buffer) return NULL;
    const char* cursor = json_str->buffer;
    return Json__parse_value(&cursor);
}