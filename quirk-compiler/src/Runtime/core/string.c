#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"

// Forward declaration: Core_Primitives_Any_to_string is implemented in any.c
// (included here so append_any / append_formatted can call it)
String* Core_Primitives_Any_to_string(Any* a);
double Core_Primitives_Any_to_float(Any* a);
extern void quirk_throw_exception(const char* type_name, const char* message);

#define MAX_SMALL_INT 0xFFFFF

// ==========================================
//  HELPERS
// ==========================================

// --- Helper: Dynamic Buffer Appender ---
static void buffer_append(char** buf, int* cap, int* len, const char* str) {
    if (!str)
        return;
    int str_len = strlen(str);
    while (*len + str_len >= *cap) {
        *cap *= 2;
        *buf = (char*)realloc(*buf, *cap);
    }
    strcpy(*buf + *len, str);
    *len += str_len;
}

// Remove quirk_ensure_string — no longer needed with Any* boxing
// Core_String_String___add, Core_String_String___eq now trust that values are proper String* objects.

// --- Format helpers: items in the list are now Any* pointers ---

// Determine if a raw void* looks like a valid Any* by checking whether the
// first 4 bytes (the tag field) fall in the known AnyTag range.  On 64-bit
// systems every real heap allocation lives above 0xFFFFFFFF, so a pointer
// value <= 0xFFFFFFFF is a tagged integer (inttoptr i32 -> i8*), not Any*.
static inline int is_valid_any_ptr(void* item) {
    if (!item) return 0;
    if ((uintptr_t)item <= 0xFFFFFFFFUL) return 0;  // tagged integer
    int32_t tag = *(int32_t*)item;
    return tag >= ANY_INT && tag <= ANY_NULL;
}

static void append_any(char** buf, int* cap, int* len, void* item) {
    if (!item) {
        buffer_append(buf, cap, len, "null");
        return;
    }
    // Tagged integer passed as pointer
    if ((uintptr_t)item <= 0xFFFFFFFFUL) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%d", (int)(uintptr_t)item);
        buffer_append(buf, cap, len, tmp);
        return;
    }
    if (is_valid_any_ptr(item)) {
        Any* a = (Any*)item;
        String* s = Core_Primitives_Any_to_string(a);
        if (s && s->buffer)
            buffer_append(buf, cap, len, s->buffer);
    } else {
        // Raw String* (or other heap object) — use buffer directly
        String* s = (String*)item;
        if (s->buffer)
            buffer_append(buf, cap, len, s->buffer);
    }
}

static void append_formatted(char** buf,
                      int* cap,
                      int* len,
                      void* item,
                      const char* fmt_spec) {
    if (!item) {
        buffer_append(buf, cap, len, "(null)");
        return;
    }

    char format_string[32];
    char output_buffer[256];

    // Tagged integer passed as pointer
    if ((uintptr_t)item <= 0xFFFFFFFFUL) {
        int ival = (int)(uintptr_t)item;
        if (fmt_spec && strlen(fmt_spec) > 0) {
            snprintf(format_string, 32, "%%%s", fmt_spec);
            if (strchr(format_string, 's')) strcpy(format_string, "%d");
            snprintf(output_buffer, 256, format_string, ival);
        } else {
            snprintf(output_buffer, 256, "%d", ival);
        }
        buffer_append(buf, cap, len, output_buffer);
        return;
    }

    // Raw String* or other heap object that is NOT a valid Any*
    if (!is_valid_any_ptr(item)) {
        String* s = (String*)item;
        const char* cstr = (s && s->buffer) ? s->buffer : "(null)";
        if (fmt_spec && strlen(fmt_spec) > 0) {
            snprintf(format_string, 32, "%%%s", fmt_spec);
            if (strpbrk(format_string, "dxfge")) strcpy(format_string, "%s");
            snprintf(output_buffer, 256, format_string, cstr);
            buffer_append(buf, cap, len, output_buffer);
        } else {
            buffer_append(buf, cap, len, cstr);
        }
        return;
    }

    Any* a = (Any*)item;

    // Float format spec (%f, %g, %e)?
    if (fmt_spec && (strchr(fmt_spec, 'f') || strchr(fmt_spec, 'g') ||
                     strchr(fmt_spec, 'e'))) {
        snprintf(format_string, 32, "%%%s", fmt_spec);
        double d = Core_Primitives_Any_to_float(a);
        snprintf(output_buffer, 256, format_string, d);
        buffer_append(buf, cap, len, output_buffer);
        return;
    }

    switch (a->tag) {
        case ANY_BOOL: {
            // Always print "true"/"false". Numeric format specifiers (%d, %x)
            // are intentionally ignored — Bools have a textual canonical form.
            buffer_append(buf, cap, len, a->ival ? "true" : "false");
            break;
        }
        case ANY_INT:
        case ANY_CHAR: {
            if (fmt_spec && strlen(fmt_spec) > 0) {
                snprintf(format_string, 32, "%%%s", fmt_spec);
                if (strchr(format_string, 's'))
                    strcpy(format_string, "%d");
                snprintf(output_buffer, 256, format_string, a->ival);
            } else {
                snprintf(output_buffer, 256, "%d", a->ival);
            }
            buffer_append(buf, cap, len, output_buffer);
            break;
        }
        case ANY_DOUBLE: {
            if (fmt_spec && strlen(fmt_spec) > 0) {
                snprintf(format_string, 32, "%%%s", fmt_spec);
                snprintf(output_buffer, 256, format_string, a->dval);
            } else {
                snprintf(output_buffer, 256, "%g", a->dval);
            }
            buffer_append(buf, cap, len, output_buffer);
            break;
        }
        default: {
            // String, List, Map, Ptr, Null — convert to string then format
            String* s = Core_Primitives_Any_to_string(a);
            const char* cstr = (s && s->buffer) ? s->buffer : "(null)";
            if (fmt_spec && strlen(fmt_spec) > 0) {
                snprintf(format_string, 32, "%%%s", fmt_spec);
                if (strpbrk(format_string, "dxfge"))
                    strcpy(format_string, "%s");
                snprintf(output_buffer, 256, format_string, cstr);
                buffer_append(buf, cap, len, output_buffer);
            } else {
                buffer_append(buf, cap, len, cstr);
            }
            break;
        }
    }
}

// Keys in format lists are always String* objects.
// (handleMapFormat wraps raw key names in String* before adding to the list.)
static int find_key_index(List* keys, const char* key_name) {
    if (!keys || !keys->data)
        return -1;
    for (int i = 0; i < keys->size; i++) {
        String* kObj = (String*)keys->data[i];
        if (kObj && kObj->buffer && strcmp(kObj->buffer, key_name) == 0) {
            return i;
        }
    }
    return -1;
}

// ==========================================
//  LIFECYCLE (Init / Del)
// ==========================================

void Core_String_String___init(String* self, char* raw) {
    uintptr_t val = (uintptr_t)raw;
    if (val <= MAX_SMALL_INT) {
        char temp[32];
        sprintf(temp, "%d", (int)val);
        self->length = strlen(temp);
        self->buffer = strdup(temp);
        return;
    }
    if (!raw || raw[0] == '\0') {
        // Empty literal "" — short-circuit before the Any-tag heuristic below.
        // Reading 4 bytes past a 1-byte zeroinitializer is UB and intermittently
        // picks up adjacent memory that happens to look like a valid AnyTag.
        self->length = 0;
        self->buffer = strdup("");
        return;
    }
    // If raw is actually an Any* (first 4 bytes are a valid AnyTag), extract
    // its string representation instead of treating it as a raw C string.
    int32_t possible_tag = *(int32_t*)raw;
    if (possible_tag >= ANY_INT && possible_tag <= ANY_NULL) {
        Any* a = (Any*)raw;
        String* extracted = Core_Primitives_Any_to_string(a);
        if (extracted && extracted->buffer) {
            self->length = extracted->length;
            self->buffer = strdup(extracted->buffer);
        } else {
            self->length = 0;
            self->buffer = strdup("");
        }
        return;
    }
    self->length = strlen(raw);
    self->buffer = strdup(raw);
}

void Core_String_String___del(String* self) {
    if (self->buffer) {
        free(self->buffer);
        self->buffer = NULL;
    }
}

// ==========================================
//  OPERATORS
// ==========================================

String* make_String_taking_ownership(char* raw_buffer);
String* make_String(const char* raw);

// List/Map store integers as tagged pointers (inttoptr i32 -> i8*).
// Until List storage is migrated to Any*, Core_String_String___add must handle
// the case where a retrieved value is a tiny address, not a real String*.
static inline String* quirk_ensure_string(String* val) {
    uintptr_t v = (uintptr_t)val;
    if (v <= MAX_SMALL_INT) {
        char temp[32];
        sprintf(temp, "%d", (int)v);
        return make_String(temp);
    }
    return val;
}

String* Core_String_String___add(String* self, String* other) {
    self = quirk_ensure_string(self);
    other = quirk_ensure_string(other);
    if (!self || !other)
        return make_String("");
    int new_len = self->length + other->length;
    char* raw = (char*)malloc(new_len + 1);
    if (!raw)
        return NULL;
    strcpy(raw, self->buffer);
    strcat(raw, other->buffer);
    return make_String_taking_ownership(raw);
}

int Core_String_String___eq(String* self, String* other) {
    if (!self || !other)
        return self == other;
    if (self->length != other->length)
        return 0;
    return strcmp(self->buffer, other->buffer) == 0;
}

// Lexicographic comparisons. Until these existed, codegen fell back to
// raw pointer comparison for `<` / `<=` / `>` / `>=` on String, which
// produced nonsense (`"5" <= "9"` could return false depending on the
// GC's allocation order).
static int Core_String__cmp(String* self, String* other) {
    if (!self && !other) return 0;
    if (!self) return -1;
    if (!other) return 1;
    return strcmp(self->buffer ? self->buffer : "",
                  other->buffer ? other->buffer : "");
}

int Core_String_String___lt(String* self, String* other) { return Core_String__cmp(self, other) <  0; }
int Core_String_String___le(String* self, String* other) { return Core_String__cmp(self, other) <= 0; }
int Core_String_String___gt(String* self, String* other) { return Core_String__cmp(self, other) >  0; }
int Core_String_String___ge(String* self, String* other) { return Core_String__cmp(self, other) >= 0; }

String* Core_String_String___str(String* self) {
    return self ? self : make_String("");
}

String* Core_String_String___repr(String* self) {
    int len = self->length + 2;
    char* raw = (char*)GC_malloc(len + 1);
    snprintf(raw, len + 1, "\"%s\"", self->buffer);
    return make_String(raw);
}

int Core_String_String___get(String* self, int index) {
    if (!self || !self->buffer) {
        quirk_throw_exception("IndexError", "string index on null string");
    }
    if (index < 0) index += self->length;
    if (index < 0 || index >= self->length) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "string index %d out of range (length: %d)", index, self->length);
        quirk_throw_exception("IndexError", buf);
    }
    return (int)(unsigned char)self->buffer[index];
}

// ==========================================
//  ITERATOR
// ==========================================

void Core_String_StringIterator___init(StringIterator* self, String* s) {
    self->str_ref = s;
    self->idx = 0;
}

int Core_String_StringIterator___has_next(StringIterator* self) {
    if (!self || !self->str_ref)
        return 0;
    return self->idx < self->str_ref->length;
}

String* Core_String_StringIterator___next(StringIterator* self) {
    if (!self || !self->str_ref) return make_String("");
    char c = self->str_ref->buffer[self->idx];
    self->idx++;
    char* raw = (char*)malloc(2);
    raw[0] = c;
    raw[1] = '\0';
    return make_String_taking_ownership(raw);
}

StringIterator* Core_String_String___iter(String* self) {
    StringIterator* iter = (StringIterator*)malloc(sizeof(StringIterator));
    Core_String_StringIterator___init(iter, self);
    return iter;
}

// ==========================================
//  CONSTRUCTORS
// ==========================================

String* make_String_taking_ownership(char* raw_buffer) {
    if (!raw_buffer)
        return NULL;
    String* s = (String*)malloc(sizeof(String));
    s->buffer = raw_buffer;
    s->length = strlen(raw_buffer);
    return s;
}

String* make_String(const char* raw) {
    String* s = (String*)malloc(sizeof(String));
    uintptr_t val = (uintptr_t)raw;
    if (val <= MAX_SMALL_INT) {
        char temp[32];
        sprintf(temp, "%d", (int)val);
        s->length = strlen(temp);
        s->buffer = strdup(temp);
    } else {
        s->length = strlen(raw);
        s->buffer = strdup(raw);
    }
    return s;
}

// ==========================================
//  METHODS
// ==========================================

String* Core_String_String_upper(String* self) {
    if (!self || !self->buffer)
        return make_String("");
    char* temp = (char*)malloc(self->length + 1);
    for (int i = 0; i < self->length; i++)
        temp[i] = toupper((unsigned char)self->buffer[i]);
    temp[self->length] = '\0';
    return make_String_taking_ownership(temp);
}

String* Core_String_String_lower(String* self) {
    if (!self || !self->buffer)
        return make_String("");
    char* temp = (char*)malloc(self->length + 1);
    for (int i = 0; i < self->length; i++)
        temp[i] = tolower((unsigned char)self->buffer[i]);
    temp[self->length] = '\0';
    return make_String_taking_ownership(temp);
}

String* Core_String_String_title(String* self) {
    if (!self || !self->buffer)
        return make_String("");
    char* temp = (char*)malloc(self->length + 1);
    int prev_is_cased = 0;
    for (int i = 0; i < self->length; i++) {
        char c = self->buffer[i];
        temp[i] = prev_is_cased ? tolower((unsigned char)c)
                                : toupper((unsigned char)c);
        prev_is_cased = isalpha((unsigned char)c);
    }
    temp[self->length] = '\0';
    return make_String_taking_ownership(temp);
}

String* Core_String_String_capitalize(String* self) {
    if (!self || !self->buffer)
        return make_String("");
    char* temp = (char*)malloc(self->length + 1);
    if (self->length > 0) {
        temp[0] = toupper((unsigned char)self->buffer[0]);
        for (int i = 1; i < self->length; i++)
            temp[i] = tolower((unsigned char)self->buffer[i]);
    }
    temp[self->length] = '\0';
    return make_String_taking_ownership(temp);
}

String* Core_String_String_sentence_case(String* self) {
    if (!self || !self->buffer)
        return make_String("");
    char* temp = (char*)malloc(self->length + 1);
    int expect_capital = 1;
    for (int i = 0; i < self->length; i++) {
        char c = self->buffer[i];
        if (isalpha((unsigned char)c)) {
            temp[i] = expect_capital ? toupper((unsigned char)c)
                                     : tolower((unsigned char)c);
            expect_capital = 0;
        } else {
            temp[i] = c;
            if (c == '.' || c == '!' || c == '?')
                expect_capital = 1;
        }
    }
    temp[self->length] = '\0';
    return make_String_taking_ownership(temp);
}

static int is_url_safe(char c) {
    return isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' ||
           c == '~';
}

String* Core_String_String_encode(String* self) {
    if (!self || !self->buffer)
        return make_String("");
    char* temp = (char*)malloc((self->length * 3) + 1);
    char hex[] = "0123456789ABCDEF";
    int pos = 0;
    for (int i = 0; i < self->length; i++) {
        unsigned char c = (unsigned char)self->buffer[i];
        if (is_url_safe(c)) {
            temp[pos++] = c;
        } else {
            temp[pos++] = '%';
            temp[pos++] = hex[c >> 4];
            temp[pos++] = hex[c & 0x0F];
        }
    }
    temp[pos] = '\0';
    return make_String_taking_ownership(temp);
}

int Core_String_String_count(String* self, String* sub) {
    if (!self || !self->buffer || !sub || !sub->buffer)
        return 0;
    int sub_len = sub->length;
    if (sub_len == 0)
        return self->length + 1;
    int count = 0;
    char* temp = self->buffer;
    while ((temp = strstr(temp, sub->buffer)) != NULL) {
        count++;
        temp += sub_len;
    }
    return count;
}

int Core_String_String_endswith(String* self, String* suffix) {
    if (!self || !self->buffer || !suffix || !suffix->buffer)
        return 0;
    if (suffix->length > self->length)
        return 0;
    return strcmp(self->buffer + (self->length - suffix->length),
                  suffix->buffer) == 0;
}

int Core_String_String_find(String* self, String* sub) {
    if (!self || !self->buffer || !sub || !sub->buffer)
        return -1;
    char* ptr = strstr(self->buffer, sub->buffer);
    return (ptr == NULL) ? -1 : (int)(ptr - self->buffer);
}

int Core_String_String_index(String* self, String* sub) {
    int idx = Core_String_String_find(self, sub);
    if (idx == -1) {
        printf("ValueError: substring not found\n");
        exit(1);
    }
    return idx;
}

String* Core_String_String_trim(String* self) {
    if (!self || !self->buffer)
        return make_String("");
    int start = 0, end = self->length - 1;
    while (start < self->length && isspace((unsigned char)self->buffer[start]))
        start++;
    if (start > end)
        return make_String("");
    while (end > start && isspace((unsigned char)self->buffer[end]))
        end--;
    int new_len = end - start + 1;
    char* temp = (char*)malloc(new_len + 1);
    memcpy(temp, self->buffer + start, new_len);
    temp[new_len] = '\0';
    return make_String_taking_ownership(temp);
}

String* Core_String_String_replace(String* self, String* old_str, String* new_str) {
    if (!self || !self->buffer || !old_str || !new_str)
        return make_String("");
    int count = 0;
    char* tmp = self->buffer;
    int old_len = old_str->length;
    if (old_len == 0)
        return make_String(self->buffer);
    while ((tmp = strstr(tmp, old_str->buffer))) {
        count++;
        tmp += old_len;
    }
    int new_len = self->length + (count * (new_str->length - old_len));
    char* result = (char*)malloc(new_len + 1);
    char* src = self->buffer;
    char* dest = result;
    while (*src) {
        if (strstr(src, old_str->buffer) == src) {
            strcpy(dest, new_str->buffer);
            dest += new_str->length;
            src += old_len;
        } else {
            *dest++ = *src++;
        }
    }
    *dest = '\0';
    return make_String_taking_ownership(result);
}

String* Core_String_String_remove(String* self, String* sub) {
    if (!self || !self->buffer || !sub || !sub->buffer)
        return make_String("");
    int sub_len = sub->length;
    if (sub_len == 0)
        return make_String(self->buffer);
    int count = 0;
    char* tmp = self->buffer;
    while ((tmp = strstr(tmp, sub->buffer))) {
        count++;
        tmp += sub_len;
    }
    int new_len = self->length - (count * sub_len);
    char* result = (char*)malloc(new_len + 1);
    char* src = self->buffer;
    char* dest = result;
    while (*src) {
        if (strstr(src, sub->buffer) == src)
            src += sub_len;
        else
            *dest++ = *src++;
    }
    *dest = '\0';
    return make_String_taking_ownership(result);
}

String* Core_String_String_repeat(String* self, int n) {
    if (!self || !self->buffer || n <= 0)
        return make_String("");
    int new_len = self->length * n;
    char* result = (char*)malloc(new_len + 1);
    char* ptr = result;
    for (int i = 0; i < n; i++) {
        memcpy(ptr, self->buffer, self->length);
        ptr += self->length;
    }
    *ptr = '\0';
    return make_String_taking_ownership(result);
}

String* Core_String_String_reverse(String* self) {
    if (!self || !self->buffer)
        return make_String("");
    char* result = (char*)malloc(self->length + 1);
    for (int i = 0; i < self->length; i++)
        result[i] = self->buffer[self->length - 1 - i];
    result[self->length] = '\0';
    return make_String_taking_ownership(result);
}

int Core_String_String_startswith(String* self, String* prefix) {
    if (!self || !self->buffer || !prefix || !prefix->buffer)
        return 0;
    if (prefix->length > self->length)
        return 0;
    return strncmp(self->buffer, prefix->buffer, prefix->length) == 0;
}

String* Core_String_String_substring(String* self, int start, int end) {
    if (!self || !self->buffer)
        return make_String("");
    if (start < 0)
        start += self->length;
    if (end < 0)
        end += self->length;
    if (start < 0)
        start = 0;
    if (end > self->length)
        end = self->length;
    if (start >= end)
        return make_String("");
    int new_len = end - start;
    char* temp = (char*)malloc(new_len + 1);
    memcpy(temp, self->buffer + start, new_len);
    temp[new_len] = '\0';
    return make_String_taking_ownership(temp);
}

List* Core_String_String_split(String* self, String* delim) {
    if (!self || !self->buffer) {
        List* empty = (List*)malloc(sizeof(List));
        empty->size = 0;
        empty->capacity = 0;
        empty->data = NULL;
        return empty;
    }

    // Empty delimiter — split into individual characters
    const char* sep = (delim && delim->length > 0) ? delim->buffer : NULL;
    int sep_len = sep ? (int)strlen(sep) : 1;

    // Count parts first
    int count = 1;
    if (sep) {
        char* ptr = self->buffer;
        while ((ptr = strstr(ptr, sep)) != NULL) {
            count++;
            ptr += sep_len;
        }
    } else {
        count = self->length;
    }

    List* list = (List*)malloc(sizeof(List));
    list->size = 0;
    list->capacity = count;
    list->data = (void**)malloc(sizeof(void*) * count);

    if (!sep) {
        // Split into chars
        for (int i = 0; i < self->length; i++) {
            char buf[2] = {self->buffer[i], '\0'};
            list->data[list->size++] = make_String(buf);
        }
        return list;
    }

    char* src = self->buffer;
    char* found;
    while ((found = strstr(src, sep)) != NULL) {
        int part_len = found - src;
        char* part = (char*)malloc(part_len + 1);
        strncpy(part, src, part_len);
        part[part_len] = '\0';
        list->data[list->size++] = make_String_taking_ownership(part);
        src = found + sep_len;
    }
    // Remainder after last delimiter
    list->data[list->size++] = make_String(src);
    return list;
}

String* Core_String_String_join(String* self, List* items) {
    if (!items || !self || !self->buffer)
        return make_String("");
    int total_len = 0;
    for (int i = 0; i < items->size; i++) {
        void* item = items->data[i];
        uintptr_t val = (uintptr_t)item;
        if (val <= MAX_SMALL_INT) {
            char temp[32];
            total_len += sprintf(temp, "%d", (int)val);
        } else {
            String* s = (String*)item;
            if (s && s->buffer)
                total_len += s->length;
        }
        if (i < items->size - 1)
            total_len += self->length;
    }
    char* result = (char*)malloc(total_len + 1);
    char* ptr = result;
    *ptr = '\0';
    for (int i = 0; i < items->size; i++) {
        void* item = items->data[i];
        uintptr_t val = (uintptr_t)item;
        if (val <= MAX_SMALL_INT) {
            ptr += sprintf(ptr, "%d", (int)val);
        } else {
            String* s = (String*)item;
            if (s && s->buffer) {
                strcpy(ptr, s->buffer);
                ptr += s->length;
            }
        }
        if (i < items->size - 1) {
            memcpy(ptr, self->buffer, self->length);
            ptr += self->length;
        }
    }
    *ptr = '\0';
    return make_String_taking_ownership(result);
}

String* Core_String_String_zfill(String* self, int width) {
    if (!self || !self->buffer)
        return make_String("");
    if (self->length >= width)
        return make_String(self->buffer);
    int padding = width - self->length;
    char* result = (char*)malloc(width + 1);
    memset(result, '0', padding);
    memcpy(result + padding, self->buffer, self->length);
    result[width] = '\0';
    return make_String_taking_ownership(result);
}

String* Core_String_String_ljust(String* self, int width, String* pad) {
    if (!self || !self->buffer)
        return make_String("");
    char pad_char = (pad && pad->length > 0) ? pad->buffer[0] : ' ';
    if (self->length >= width)
        return make_String(self->buffer);
    int padding = width - self->length;
    char* result = (char*)malloc(width + 1);
    memcpy(result, self->buffer, self->length);
    memset(result + self->length, pad_char, padding);
    result[width] = '\0';
    return make_String_taking_ownership(result);
}

String* Core_String_String_rjust(String* self, int width, String* pad) {
    if (!self || !self->buffer)
        return make_String("");
    char pad_char = (pad && pad->length > 0) ? pad->buffer[0] : ' ';
    if (self->length >= width)
        return make_String(self->buffer);
    int padding = width - self->length;
    char* result = (char*)malloc(width + 1);
    memset(result, pad_char, padding);
    memcpy(result + padding, self->buffer, self->length);
    result[width] = '\0';
    return make_String_taking_ownership(result);
}

String* Core_String_String_lstrip(String* self) {
    return Core_String_String_trim(self);
}
String* Core_String_String_rstrip(String* self) {
    return Core_String_String_trim(self);
}

String* Core_String_String_swapcase(String* self) {
    if (!self || !self->buffer)
        return make_String("");
    char* temp = (char*)malloc(self->length + 1);
    for (int i = 0; i < self->length; i++) {
        char c = self->buffer[i];
        if (isupper((unsigned char)c))
            temp[i] = tolower((unsigned char)c);
        else if (islower((unsigned char)c))
            temp[i] = toupper((unsigned char)c);
        else
            temp[i] = c;
    }
    temp[self->length] = '\0';
    return make_String_taking_ownership(temp);
}

int Core_String_String_is_space(String* self) {
    if (!self || self->length == 0)
        return 0;
    for (int i = 0; i < self->length; i++)
        if (!isspace((unsigned char)self->buffer[i]))
            return 0;
    return 1;
}

int Core_String_String_contains(String* self, String* sub) {
    return Core_String_String_find(self, sub) != -1;
}

String* Core_String_String_center(String* self, int width, String* pad) {
    if (!self || !self->buffer)
        return make_String("");
    char pad_char = (pad && pad->length > 0) ? pad->buffer[0] : ' ';
    if (self->length >= width)
        return make_String(self->buffer);
    int total_padding = width - self->length;
    int left = total_padding / 2;
    int right = total_padding - left;
    char* result = (char*)malloc(width + 1);
    memset(result, pad_char, left);
    memcpy(result + left, self->buffer, self->length);
    memset(result + left + self->length, pad_char, right);
    result[width] = '\0';
    return make_String_taking_ownership(result);
}

int Core_String_String_is_digit(String* self) {
    if (!self || self->length == 0)
        return 0;
    for (int i = 0; i < self->length; i++)
        if (!isdigit((unsigned char)self->buffer[i]))
            return 0;
    return 1;
}

int Core_String_String_is_upper(String* self) {
    if (!self || self->length == 0) return 0;
    int saw_letter = 0;
    for (int i = 0; i < self->length; i++) {
        unsigned char c = (unsigned char)self->buffer[i];
        if (isalpha(c)) {
            saw_letter = 1;
            if (!isupper(c)) return 0;
        }
    }
    return saw_letter;
}

int Core_String_String_is_lower(String* self) {
    if (!self || self->length == 0) return 0;
    int saw_letter = 0;
    for (int i = 0; i < self->length; i++) {
        unsigned char c = (unsigned char)self->buffer[i];
        if (isalpha(c)) {
            saw_letter = 1;
            if (!islower(c)) return 0;
        }
    }
    return saw_letter;
}

int Core_String_String_is_alpha(String* self) {
    if (!self || self->length == 0)
        return 0;
    for (int i = 0; i < self->length; i++)
        if (!isalpha((unsigned char)self->buffer[i]))
            return 0;
    return 1;
}

extern void quirk_throw_exception(const char* type_name, const char* message);

int Core_String_String_to_int(String* self) {
    if (!self || !self->buffer || self->length == 0) {
        quirk_throw_exception("ValueError", "cannot convert empty string to Int");
        return 0;
    }
    char* end;
    long val = strtol(self->buffer, &end, 10);
    if (end == self->buffer || *end != '\0') {
        char msg[256];
        snprintf(msg, sizeof(msg), "invalid literal for Int: '%s'", self->buffer);
        quirk_throw_exception("ValueError", msg);
        return 0;
    }
    return (int)val;
}

double Core_String_String_to_float(String* self) {
    if (!self || !self->buffer || self->length == 0) {
        quirk_throw_exception("ValueError", "cannot convert empty string to Double");
        return 0.0;
    }
    char* end;
    double val = strtod(self->buffer, &end);
    if (end == self->buffer || *end != '\0') {
        char msg[256];
        snprintf(msg, sizeof(msg), "invalid literal for Double: '%s'", self->buffer);
        quirk_throw_exception("ValueError", msg);
        return 0.0;
    }
    return val;
}

int8_t Core_String_String_to_bool(String* self) {
    if (!self || !self->buffer) {
        quirk_throw_exception("ValueError", "cannot convert null string to Bool");
        return 0;
    }
    if (strcmp(self->buffer, "true") == 0)  return 1;
    if (strcmp(self->buffer, "false") == 0) return 0;
    char msg[256];
    snprintf(msg, sizeof(msg),
             "invalid literal for Bool: '%s' (expected 'true' or 'false')", self->buffer);
    quirk_throw_exception("ValueError", msg);
    return 0;
}

char Core_String_String_to_char(String* self) {
    if (!self || !self->buffer || self->length == 0) {
        quirk_throw_exception("ValueError", "cannot convert empty string to Char");
        return '\0';
    }
    if (self->length != 1) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "invalid conversion to Char: expected single character, got '%s'", self->buffer);
        quirk_throw_exception("ValueError", msg);
        return '\0';
    }
    return self->buffer[0];
}

char* Core_String_float_to_str(double val) {
    char* buf = (char*)malloc(64);
    snprintf(buf, 64, "%g", val);
    return buf;
}

List* Core_String_String_lines(String* self) {
    if (!self || !self->buffer)
        return NULL;
    int count = 1;
    char* ptr = self->buffer;
    while (*ptr) {
        if (*ptr == '\n')
            count++;
        ptr++;
    }
    List* list = (List*)malloc(sizeof(List));
    list->size = 0;
    list->capacity = count;
    list->data = (void**)malloc(sizeof(void*) * count);
    char* start = self->buffer;
    ptr = self->buffer;
    while (*ptr) {
        if (*ptr == '\n' || *ptr == '\r') {
            int len = ptr - start;
            char* line = (char*)malloc(len + 1);
            strncpy(line, start, len);
            line[len] = '\0';
            list->data[list->size++] = make_String_taking_ownership(line);
            if (*ptr == '\r' && *(ptr + 1) == '\n')
                ptr++;
            start = ptr + 1;
        }
        ptr++;
    }
    if (start <= ptr) {
        int len = ptr - start;
        char* line = (char*)malloc(len + 1);
        strncpy(line, start, len);
        line[len] = '\0';
        list->data[list->size++] = make_String_taking_ownership(line);
    }
    return list;
}

static int min3(int a, int b, int c) {
    int m = a;
    if (b < m)
        m = b;
    if (c < m)
        m = c;
    return m;
}

int Core_String_String_distance(String* self, String* other) {
    if (!self || !other)
        return 0;
    int len1 = self->length;
    int len2 = other->length;
    int* matrix = (int*)malloc((len1 + 1) * (len2 + 1) * sizeof(int));
#define M(r, c) matrix[(r) * (len2 + 1) + (c)]
    for (int i = 0; i <= len1; i++)
        M(i, 0) = i;
    for (int j = 0; j <= len2; j++)
        M(0, j) = j;
    for (int i = 1; i <= len1; i++) {
        for (int j = 1; j <= len2; j++) {
            int cost = (self->buffer[i - 1] == other->buffer[j - 1]) ? 0 : 1;
            M(i, j) =
                min3(M(i - 1, j) + 1, M(i, j - 1) + 1, M(i - 1, j - 1) + cost);
        }
    }
    int result = M(len1, len2);
    free(matrix);
#undef M
    return result;
}

String* Core_String_String_format_map(String* self, List* keys, List* values) {
    if (!self || !self->buffer)
        return make_String("");
    int cap = 2048;
    int len = 0;
    char* res = (char*)malloc(cap);
    res[0] = '\0';
    char* ptr = self->buffer;
    while (*ptr) {
        if (*ptr == '{') {
            if (*(ptr + 1) == '{') {
                buffer_append(&res, &cap, &len, "{");
                ptr += 2;
                continue;
            }
            char* close_ptr = strchr(ptr, '}');
            if (close_ptr) {
                char* sep_ptr = strchr(ptr, '%');
                char* fmt_spec = NULL;
                char spec_buffer[16];
                char* key_end = close_ptr;
                if (sep_ptr && sep_ptr < close_ptr) {
                    key_end = sep_ptr;
                    char* spec_start = sep_ptr + 1;
                    while (*spec_start == ' ')
                        spec_start++;
                    int spec_len = close_ptr - spec_start;
                    if (spec_len < 15 && spec_len > 0) {
                        strncpy(spec_buffer, spec_start, spec_len);
                        spec_buffer[spec_len] = '\0';
                        fmt_spec = spec_buffer;
                    }
                }
                int key_len = key_end - (ptr + 1);
                while (key_len > 0 && ptr[1 + key_len - 1] == ' ')
                    key_len--;
                char* key_name = (char*)malloc(key_len + 1);
                strncpy(key_name, ptr + 1, key_len);
                key_name[key_len] = '\0';
                int idx = find_key_index(keys, key_name);
                if (idx != -1 && idx < values->size) {
                    append_formatted(&res, &cap, &len, values->data[idx], fmt_spec);
                }
                free(key_name);
                ptr = close_ptr + 1;
                continue;
            }
        }
        char c[2] = {*ptr, '\0'};
        buffer_append(&res, &cap, &len, c);
        ptr++;
    }
    return make_String_taking_ownership(res);
}

String* Core_String_String_format_list(String* self, List* args) {
    if (!self || !self->buffer)
        return make_String("");
    int cap = 2048;
    int len = 0;
    char* res = (char*)malloc(cap);
    res[0] = '\0';
    int arg_idx = 0;
    char* ptr = self->buffer;
    while (*ptr) {
        if (*ptr == '{') {
            if (*(ptr + 1) == '{') {
                buffer_append(&res, &cap, &len, "{");
                ptr += 2;
                continue;
            }
            char* close_ptr = strchr(ptr, '}');
            if (close_ptr) {
                char* sep_ptr = strchr(ptr, '%');
                char* fmt_spec = NULL;
                char spec_buffer[16];
                if (sep_ptr && sep_ptr < close_ptr) {
                    char* spec_start = sep_ptr + 1;
                    while (*spec_start == ' ')
                        spec_start++;
                    int spec_len = close_ptr - spec_start;
                    if (spec_len < 15 && spec_len > 0) {
                        strncpy(spec_buffer, spec_start, spec_len);
                        spec_buffer[spec_len] = '\0';
                        fmt_spec = spec_buffer;
                    }
                }
                if (arg_idx < args->size) {
                    append_formatted(&res, &cap, &len, args->data[arg_idx++],
                                     fmt_spec);
                }
                ptr = close_ptr + 1;
                continue;
            }
        }
        if (*ptr == '}' && *(ptr + 1) == '}') {
            buffer_append(&res, &cap, &len, "}");
            ptr += 2;
            continue;
        }
        char c[2] = {*ptr, '\0'};
        buffer_append(&res, &cap, &len, c);
        ptr++;
    }
    return make_String_taking_ownership(res);
}

String* Core_String_String_format(String* self, List* args) {
    return Core_String_String_format_list(self, args);
}

int Core_String_String_length(String* self) {
    if (!self) return 0;
    return self->length;
}
