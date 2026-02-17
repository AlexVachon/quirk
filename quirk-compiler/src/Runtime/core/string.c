#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"

#define MAX_SMALL_INT 0xFFFFF

// ==========================================
//  HELPERS (Must be defined before use)
// ==========================================

// --- Helper: Dynamic Buffer Appender ---
void buffer_append(char** buf, int* cap, int* len, const char* str) {
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

// --- Helper: Convert Any (Int/String) to Text ---
void append_any(char** buf, int* cap, int* len, void* item) {
    uintptr_t val = (uintptr_t)item;
    if (val <= MAX_SMALL_INT) {
        char temp[32];
        sprintf(temp, "%d", (int)val);
        buffer_append(buf, cap, len, temp);
    } else {
        char* s = (char*)item;
        if (s)
            buffer_append(buf, cap, len, s);
    }
}

// ==========================================
//  LIFECYCLE (Init / Del)
// ==========================================

void String__init(String* self, char* raw) {
    uintptr_t val = (uintptr_t)raw;

    // 1. Safety Check: Is this actually a Small Integer?
    if (val <= MAX_SMALL_INT) {
        char temp[32];
        sprintf(temp, "%d", (int)val);
        self->length = strlen(temp);
        self->buffer = strdup(temp);
        return;
    }

    // 2. Standard String Initialization
    if (!raw) {
        self->length = 0;
        self->buffer = strdup("");
    } else {
        self->length = strlen(raw);
        self->buffer = strdup(raw);
    }
}

void String___del(String* self) {
    if (self->buffer) {
        free(self->buffer);
        self->buffer = NULL;
    }
}

// ==========================================
//  OPERATORS (__add, __eq, __str)
// ==========================================

// char* String_add(char* a, char* b) {
//     if (!a) a = "";
//     if (!b) b = "";
    
//     size_t lenA = strlen(a);
//     size_t lenB = strlen(b);
    
//     char* result = (char*)malloc(lenA + lenB + 1);
//     if (!result) {
//         fprintf(stderr, "Out of memory in String_add\n");
//         exit(1);
//     }
    
//     strcpy(result, a);
//     strcat(result, b);
    
//     return result;
// }

String* String___add(String* self, String* other) {
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

int String___eq(String* self, String* other) {
    if (self->length != other->length)
        return 0;
    return strcmp(self->buffer, other->buffer) == 0;
}

char* String___str(String* self) {
    return self->buffer;
}

char* String___repr(String* self) {
    // Returns "content" (with quotes)
    int len = self->length + 2;  // +2 for quotes
    char* raw = (char*)malloc(len + 1);
    snprintf(raw, len + 1, "\"%s\"", self->buffer);
    return raw;
}

// ==========================================
//  ITERATOR
// ==========================================

void StringIterator__init(StringIterator* self, String* s) {
    self->str_ref = s;
    self->idx = 0;
}

int StringIterator___has_next(StringIterator* self) {
    if (!self || !self->str_ref)
        return 0;
    return self->idx < self->str_ref->length;
}

char StringIterator___next(StringIterator* self) {
    if (!self || !self->str_ref)
        return '\0';
    char c = self->str_ref->buffer[self->idx];
    self->idx++;
    return c;
}

StringIterator* String___iter(String* self) {
    StringIterator* iter = (StringIterator*)malloc(sizeof(StringIterator));
    StringIterator__init(iter, self);
    return iter;
}

String* make_String_taking_ownership(char* raw_buffer) {
    if (!raw_buffer)
        return NULL;

    String* s = (String*)malloc(sizeof(String));
    s->buffer = raw_buffer;  // Direct assignment (No copy)
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
        s->buffer = (char*)malloc(s->length + 1);
        strcpy(s->buffer, raw);
    }
    return s;
}

int find_key_index(List* keys, const char* key_name) {
    if (!keys || !keys->data)
        return -1;
    for (int i = 0; i < keys->length; i++) {
        char* k = (char*)keys->data[i];
        if (k && strcmp(k, key_name) == 0)
            return i;
    }
    return -1;
}

String* String_upper(String* self) {
    if (!self || !self->buffer)
        return make_String("");

    char* temp = (char*)malloc(self->length + 1);
    if (!temp)
        return NULL;

    for (int i = 0; i < self->length; i++) {
        temp[i] = toupper((unsigned char)self->buffer[i]);
    }
    temp[self->length] = '\0';

    return make_String_taking_ownership(temp);
}

String* String_lower(String* self) {
    if (!self || !self->buffer) {
        return make_String("");
    }

    char* temp = (char*)malloc(self->length + 1);
    if (!temp)
        return NULL;

    for (int i = 0; i < self->length; i++) {
        temp[i] = tolower((unsigned char)self->buffer[i]);
    }
    temp[self->length] = '\0';

    return make_String_taking_ownership(temp);
}

String* String_title(String* self) {
    if (!self || !self->buffer)
        return make_String("");

    char* temp = (char*)malloc(self->length + 1);
    if (!temp)
        return NULL;

    int prev_is_cased = 0;  // Treat start of string as a boundary

    for (int i = 0; i < self->length; i++) {
        char c = self->buffer[i];
        if (!prev_is_cased) {
            temp[i] = toupper((unsigned char)c);
        } else {
            temp[i] = tolower((unsigned char)c);
        }
        prev_is_cased = isalpha((unsigned char)c);
    }
    temp[self->length] = '\0';

    return make_String_taking_ownership(temp);
}

String* String_capitalize(String* self) {
    if (!self || !self->buffer)
        return make_String("");

    char* temp = (char*)malloc(self->length + 1);
    if (!temp)
        return NULL;

    if (self->length > 0) {
        temp[0] = toupper((unsigned char)self->buffer[0]);
        for (int i = 1; i < self->length; i++) {
            temp[i] = tolower((unsigned char)self->buffer[i]);
        }
    }
    temp[self->length] = '\0';

    return make_String_taking_ownership(temp);
}

String* String_sentence_case(String* self) {
    if (!self || !self->buffer)
        return make_String("");

    char* temp = (char*)malloc(self->length + 1);
    if (!temp)
        return NULL;

    int expect_capital = 1;  // Start true for the very first letter

    for (int i = 0; i < self->length; i++) {
        char c = self->buffer[i];

        if (isalpha((unsigned char)c)) {
            if (expect_capital) {
                temp[i] = toupper((unsigned char)c);
                expect_capital = 0;  // We just capitalized, so stop expecting
            } else {
                temp[i] = tolower((unsigned char)c);
            }
        } else {
            // Copy non-letters (spaces, punctuation) exactly as is
            temp[i] = c;

            // If we hit a terminator, reset the flag to expect a capital next
            if (c == '.' || c == '!' || c == '?') {
                expect_capital = 1;
            }
        }
    }
    temp[self->length] = '\0';

    return make_String_taking_ownership(temp);
}

int is_url_safe(char c) {
    return isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' ||
           c == '~';
}

String* String_encode(String* self) {
    if (!self || !self->buffer)
        return make_String("");

    char* temp = (char*)malloc((self->length * 3) + 1);
    if (!temp)
        return NULL;

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

int String_count(String* self, String* sub) {
    if (!self || !self->buffer || !sub || !sub->buffer) {
        return 0;
    }

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

int String_endswith(String* self, String* suffix) {
    if (!self || !self->buffer || !suffix || !suffix->buffer)
        return 0;

    if (suffix->length > self->length)
        return 0;

    char* offset = self->buffer + (self->length - suffix->length);

    return strcmp(offset, suffix->buffer) == 0;
}

int String_find(String* self, String* sub) {
    if (!self || !self->buffer || !sub || !sub->buffer)
        return -1;

    char* ptr = strstr(self->buffer, sub->buffer);

    if (ptr == NULL)
        return -1;

    return (int)(ptr - self->buffer);
}

int String_index(String* self, String* sub) {
    int idx = String_find(self, sub);

    if (idx == -1) {
        printf("ValueError: substring not found\n");
        exit(1);
    }
    return idx;
}

String* String_trim(String* self) {
    if (!self || !self->buffer)
        return make_String("");

    int start = 0;
    int end = self->length - 1;

    while (start < self->length &&
           isspace((unsigned char)self->buffer[start])) {
        start++;
    }

    if (start > end)
        return make_String("");

    while (end > start && isspace((unsigned char)self->buffer[end])) {
        end--;
    }

    int new_len = end - start + 1;
    char* temp = (char*)malloc(new_len + 1);
    if (!temp)
        return NULL;

    for (int i = 0; i < new_len; i++) {
        temp[i] = self->buffer[start + i];
    }
    temp[new_len] = '\0';

    return make_String_taking_ownership(temp);
}

String* String_replace(String* self, String* old_str, String* new_str) {
    if (!self || !self->buffer || !old_str || !new_str)
        return make_String("");

    // 1. Count occurrences of old_str
    int count = 0;
    char* tmp = self->buffer;
    int old_len = old_str->length;
    int new_len = new_str->length;

    // Avoid infinite loop if old_str is empty
    if (old_len == 0)
        return make_String(self->buffer);

    while ((tmp = strstr(tmp, old_str->buffer))) {
        count++;
        tmp += old_len;
    }

    // 2. Calculate new size
    // new_size = original + (count * (new_len - old_len))
    int new_total_len = self->length + (count * (new_len - old_len));
    char* result = (char*)malloc(new_total_len + 1);
    if (!result)
        return NULL;

    // 3. Construct new string
    char* src = self->buffer;
    char* dest = result;

    while (*src) {
        // Check if we match 'old_str' at current position
        if (strstr(src, old_str->buffer) == src) {
            strcpy(dest, new_str->buffer);  // Copy new string
            dest += new_len;
            src += old_len;  // Skip over old string
        } else {
            *dest++ = *src++;  // Copy normal character
        }
    }
    *dest = '\0';

    return make_String_taking_ownership(result);
}

String* String_remove(String* self, String* sub) {
    if (!self || !self->buffer || !sub || !sub->buffer)
        return make_String("");

    int sub_len = sub->length;
    if (sub_len == 0)
        return make_String(self->buffer);  // Nothing to remove

    // 1. Count occurrences
    int count = 0;
    char* tmp = self->buffer;
    while ((tmp = strstr(tmp, sub->buffer))) {
        count++;
        tmp += sub_len;
    }

    if (count == 0)
        return make_String(self->buffer);

    // 2. Calculate new size
    // New length = Original - (Count * Substring Length)
    int new_len = self->length - (count * sub_len);
    char* result = (char*)malloc(new_len + 1);
    if (!result)
        return NULL;

    // 3. Build string skipping the 'sub'
    char* src = self->buffer;
    char* dest = result;

    while (*src) {
        if (strstr(src, sub->buffer) == src) {
            src += sub_len;  // Skip the substring
        } else {
            *dest++ = *src++;  // Copy normal char
        }
    }
    *dest = '\0';

    return make_String_taking_ownership(result);
}

String* String_repeat(String* self, int n) {
    if (!self || !self->buffer)
        return make_String("");
    if (n <= 0)
        return make_String("");

    // 1. Calculate new length
    int new_len = self->length * n;
    char* result = (char*)malloc(new_len + 1);
    if (!result)
        return NULL;

    // 2. Copy n times
    char* ptr = result;
    for (int i = 0; i < n; i++) {
        memcpy(ptr, self->buffer, self->length);
        ptr += self->length;
    }
    *ptr = '\0';

    return make_String_taking_ownership(result);
}

String* String_reverse(String* self) {
    if (!self || !self->buffer)
        return make_String("");

    char* result = (char*)malloc(self->length + 1);
    if (!result)
        return NULL;

    // Copy and swap
    for (int i = 0; i < self->length; i++) {
        result[i] = self->buffer[self->length - 1 - i];
    }
    result[self->length] = '\0';

    return make_String_taking_ownership(result);
}

int String_startswith(String* self, String* prefix) {
    if (!self || !self->buffer || !prefix || !prefix->buffer)
        return 0;
    if (prefix->length > self->length)
        return 0;

    // Compare first 'n' bytes
    return strncmp(self->buffer, prefix->buffer, prefix->length) == 0;
}

String* String_substring(String* self, int start, int end) {
    if (!self || !self->buffer)
        return make_String("");

    // Handle negative indices (Python style: -1 is last char)
    if (start < 0)
        start += self->length;
    if (end < 0)
        end += self->length;

    // Clamping
    if (start < 0)
        start = 0;
    if (end > self->length)
        end = self->length;
    if (start >= end)
        return make_String("");

    int new_len = end - start;
    char* temp = (char*)malloc(new_len + 1);

    // Copy segment
    memcpy(temp, self->buffer + start, new_len);
    temp[new_len] = '\0';

    return make_String_taking_ownership(temp);
}

String* String_join(String* self, List* items) {
    if (!items || !self || !self->buffer)
        return make_String("");

    // --- PASS 1: Calculate total length ---
    int total_len = 0;

    for (int i = 0; i < items->length; i++) {
        void* item = items->data[i];
        uintptr_t val = (uintptr_t)item;

        if (val <= MAX_SMALL_INT) {
            char temp[32];
            total_len += sprintf(temp, "%d", (int)val);
        } else {
            // FIX: Assume it is a String Object, not a raw char*
            String* s = (String*)item;
            if (s && s->buffer) {
                total_len += s->length;
            }
        }

        if (i < items->length - 1) {
            total_len += self->length;
        }
    }

    // Allocate
    char* result = (char*)malloc(total_len + 1);
    if (!result)
        return NULL;

    char* ptr = result;
    *ptr = '\0';

    // --- PASS 2: Build the string ---
    for (int i = 0; i < items->length; i++) {
        void* item = items->data[i];
        uintptr_t val = (uintptr_t)item;

        if (val <= MAX_SMALL_INT) {
            ptr += sprintf(ptr, "%d", (int)val);
        } else {
            // FIX: Unwrap String Object
            String* s = (String*)item;
            if (s && s->buffer) {
                strcpy(ptr, s->buffer);
                ptr += s->length;
            }
        }

        if (i < items->length - 1) {
            memcpy(ptr, self->buffer, self->length);
            ptr += self->length;
        }
    }
    *ptr = '\0';

    return make_String_taking_ownership(result);
}

String* String_zfill(String* self, int width) {
    if (!self || !self->buffer)
        return make_String("");

    if (self->length >= width) {
        return make_String(self->buffer);  // Return copy if already long enough
    }

    int padding = width - self->length;
    char* result = (char*)malloc(width + 1);
    if (!result)
        return NULL;

    // 1. Fill start with '0'
    memset(result, '0', padding);

    // 2. Copy original string after zeros
    memcpy(result + padding, self->buffer, self->length);

    result[width] = '\0';
    return make_String_taking_ownership(result);
}

String* String_ljust(String* self, int width, String* pad) {
    if (!self || !self->buffer)
        return make_String("");

    // Default to space if pad is empty/null
    char pad_char = (pad && pad->length > 0) ? pad->buffer[0] : ' ';

    if (self->length >= width)
        return make_String(self->buffer);

    int padding = width - self->length;
    char* result = (char*)malloc(width + 1);
    if (!result)
        return NULL;

    // 1. Copy string to start
    memcpy(result, self->buffer, self->length);

    // 2. Fill the rest
    memset(result + self->length, pad_char, padding);

    result[width] = '\0';
    return make_String_taking_ownership(result);
}

String* String_rjust(String* self, int width, String* pad) {
    if (!self || !self->buffer)
        return make_String("");

    char pad_char = (pad && pad->length > 0) ? pad->buffer[0] : ' ';

    if (self->length >= width)
        return make_String(self->buffer);

    int padding = width - self->length;
    char* result = (char*)malloc(width + 1);
    if (!result)
        return NULL;

    // 1. Fill start with pad_char
    memset(result, pad_char, padding);

    // 2. Copy string to end
    memcpy(result + padding, self->buffer, self->length);

    result[width] = '\0';
    return make_String_taking_ownership(result);
}

String* String_lstrip(String* self) {
    if (!self || !self->buffer)
        return make_String("");

    int start = 0;
    while (start < self->length &&
           isspace((unsigned char)self->buffer[start])) {
        start++;
    }

    // If all spaces
    if (start == self->length)
        return make_String("");

    // Allocate only what remains
    int new_len = self->length - start;
    char* temp = (char*)malloc(new_len + 1);

    memcpy(temp, self->buffer + start, new_len);
    temp[new_len] = '\0';

    return make_String_taking_ownership(temp);
}

String* String_rstrip(String* self) {
    if (!self || !self->buffer)
        return make_String("");

    int end = self->length - 1;
    while (end >= 0 && isspace((unsigned char)self->buffer[end])) {
        end--;
    }

    // If all spaces (end became -1)
    if (end < 0)
        return make_String("");

    int new_len = end + 1;
    char* temp = (char*)malloc(new_len + 1);

    memcpy(temp, self->buffer, new_len);
    temp[new_len] = '\0';

    return make_String_taking_ownership(temp);
}

String* String_swapcase(String* self) {
    if (!self || !self->buffer)
        return make_String("");

    char* temp = (char*)malloc(self->length + 1);
    if (!temp)
        return NULL;

    for (int i = 0; i < self->length; i++) {
        unsigned char c = (unsigned char)self->buffer[i];
        if (isupper(c)) {
            temp[i] = tolower(c);
        } else if (islower(c)) {
            temp[i] = toupper(c);
        } else {
            temp[i] = c;
        }
    }
    temp[self->length] = '\0';

    return make_String_taking_ownership(temp);
}

int String_is_space(String* self) {
    if (!self || self->length == 0)
        return 0;  // Empty string is usually false in Python

    for (int i = 0; i < self->length; i++) {
        if (!isspace((unsigned char)self->buffer[i]))
            return 0;
    }
    return 1;
}

int String_contains(String* self, String* sub) {
    return String_find(self, sub) != -1;
}

String* String_center(String* self, int width, String* pad) {
    if (!self || !self->buffer)
        return make_String("");

    // Default to space if pad is missing
    char pad_char = (pad && pad->length > 0) ? pad->buffer[0] : ' ';

    if (self->length >= width)
        return make_String(self->buffer);

    int total_padding = width - self->length;
    int left_padding = total_padding / 2;
    // Right padding gets the extra char if odd
    int right_padding = total_padding - left_padding;

    char* result = (char*)malloc(width + 1);
    if (!result)
        return NULL;

    // 1. Fill Left
    memset(result, pad_char, left_padding);

    // 2. Copy String
    memcpy(result + left_padding, self->buffer, self->length);

    // 3. Fill Right
    memset(result + left_padding + self->length, pad_char, right_padding);

    result[width] = '\0';
    return make_String_taking_ownership(result);
}

int String_is_digit(String* self) {
    if (!self || self->length == 0)
        return 0;
    for (int i = 0; i < self->length; i++) {
        if (!isdigit((unsigned char)self->buffer[i]))
            return 0;
    }
    return 1;
}

int String_is_alpha(String* self) {
    if (!self || self->length == 0)
        return 0;
    for (int i = 0; i < self->length; i++) {
        if (!isalpha((unsigned char)self->buffer[i]))
            return 0;
    }
    return 1;
}

// --- Helper: Safely cast void* bits back to double ---
double bits_to_double(void* ptr) {
    union {
        void* p;
        double d;
    } u;
    u.p = ptr;
    return u.d;
}

// --- Helper: Append with Format Specifier ---
// [src/Runtime/core/string.c]

void append_formatted(char** buf,
                      int* cap,
                      int* len,
                      void* item,
                      const char* fmt_spec) {
    uintptr_t val = (uintptr_t)item;
    char format_string[32];
    char output_buffer[128];

    // 1. Check if user wants a Float (%f, %g, %e)
    if (fmt_spec && (strchr(fmt_spec, 'f') || strchr(fmt_spec, 'g') ||
                     strchr(fmt_spec, 'e'))) {
        snprintf(format_string, 32, "%%%s", fmt_spec);
        double d_val = 0.0;

        if (val <= MAX_SMALL_INT) {
            d_val = (double)((int)val);
        } else {
            // It is a String pointer (guaranteed by our new compiler logic)
            char* s = (char*)item;
            if (s)
                d_val = strtod(s, NULL);  // Parse "3.14" -> 3.14
        }

        snprintf(output_buffer, 128, format_string, d_val);
        buffer_append(buf, cap, len, output_buffer);
        return;
    }

    // 2. Handle Small Integers
    if (val <= MAX_SMALL_INT) {
        if (fmt_spec && strlen(fmt_spec) > 0) {
            snprintf(format_string, 32, "%%%s", fmt_spec);
            if (strchr(format_string, 's'))
                strcpy(format_string, "%d");
        } else {
            strcpy(format_string, "%d");
        }
        snprintf(output_buffer, 128, format_string, (int)val);
        buffer_append(buf, cap, len, output_buffer);
    }
    // 3. Handle Strings (Everything else is now a String!)
    else {
        char* s = (char*)item;
        if (!s)
            s = "(null)";

        if (fmt_spec && strlen(fmt_spec) > 0) {
            snprintf(format_string, 32, "%%%s", fmt_spec);
            if (strpbrk(format_string, "dxfge"))
                strcpy(format_string, "%s");
            snprintf(output_buffer, 128, format_string, s);
        } else {
            buffer_append(buf, cap, len, s);
        }
    }
}

char* _float_to_str(double val) {
    char* buf = (char*)malloc(64);
    snprintf(buf, 64, "%g", val);
    return buf;
}

// --------------------------------------------------------
//  New "f-string" style Formatter for Map (Named)
//  Supports: "Hello {name}" and "Pi: {val % .2f}"
// --------------------------------------------------------
String* String_format_map(String* self, List* keys, List* values) {
    if (!self || !self->buffer)
        return make_String("");

    int cap = 2048;
    int len = 0;
    char* res = (char*)malloc(cap);
    res[0] = '\0';

    char* ptr = self->buffer;

    while (*ptr) {
        if (*ptr == '{') {
            // Escape "{{" -> "{"
            if (*(ptr + 1) == '{') {
                buffer_append(&res, &cap, &len, "{");
                ptr += 2;
                continue;
            }

            char* close_ptr = strchr(ptr, '}');
            if (close_ptr) {
                // --- Look for PERCENT '%' Separator ---
                char* sep_ptr = strchr(ptr, '%');

                char* fmt_spec = NULL;
                char spec_buffer[16];
                char* key_end = close_ptr;

                // Verify separator is inside the braces
                if (sep_ptr && sep_ptr < close_ptr) {
                    // Found specifier!
                    key_end = sep_ptr;

                    // Skip the '%' and any space after it
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

                // Extract Key Name
                int key_len = key_end - (ptr + 1);
                // Trim trailing spaces from key name
                while (key_len > 0 && ptr[1 + key_len - 1] == ' ')
                    key_len--;

                char* key_name = (char*)malloc(key_len + 1);
                strncpy(key_name, ptr + 1, key_len);
                key_name[key_len] = '\0';

                // Lookup Value
                int idx = find_key_index(keys, key_name);
                if (idx != -1 && idx < values->length) {
                    append_formatted(&res, &cap, &len, values->data[idx],
                                     fmt_spec);
                } else {
                    // Keep original if not found: "{unknown}"
                    buffer_append(&res, &cap, &len, "{");
                    buffer_append(&res, &cap, &len, key_name);
                    buffer_append(&res, &cap, &len, "}");
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

// --------------------------------------------------------
//  New "f-string" style Formatter for List (Positional)
//  Supports: "Hello {}" and "Pi: { % .2f }"
// --------------------------------------------------------
String* String_format_list(String* self, List* args) {
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
                // --- Look for PERCENT '%' Separator ---
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

                if (arg_idx < args->length) {
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

List* String_split(String* strObj, String* delimObj) {
    if (!strObj || !strObj->buffer)
        return NULL;
    char* text = strObj->buffer;

    if (!delimObj || !delimObj->buffer)
        return NULL;
    char* delimiter = delimObj->buffer;

    if (strlen(delimiter) == 0)
        return NULL;

    int delim_len = strlen(delimiter);
    int count = 0;

    char* temp = text;
    while ((temp = strstr(temp, delimiter)) != NULL) {
        count++;
        temp += delim_len;
    }
    count++;

    List* list = (List*)malloc(sizeof(List));
    list->length = count;
    list->capacity = count;
    list->data = (void**)malloc(sizeof(void*) * count);

    int idx = 0;
    char* start = text;
    char* end;

    while ((end = strstr(start, delimiter)) != NULL) {
        int segment_len = end - start;
        char* segment = (char*)malloc(segment_len + 1);
        strncpy(segment, start, segment_len);
        segment[segment_len] = '\0';

        // We use strdup here because the List needs to own its string elements
        list->data[idx++] = strdup(segment);

        free(segment);
        start = end + delim_len;
    }
    list->data[idx] = strdup(start);

    return list;
}

// [Add to src/Runtime/core/string.c]

// ==========================================
//  PARSING TOOLS
// ==========================================

int String_to_int(String* self) {
    if (!self || !self->buffer)
        return 0;
    // Base 10 conversion
    return (int)strtol(self->buffer, NULL, 10);
}

double String_to_float(String* self) {
    if (!self || !self->buffer)
        return 0.0;
    return strtod(self->buffer, NULL);
}

// ==========================================
//  SMART LINE SPLITTER
// ==========================================

List* String_lines(String* self) {
    if (!self || !self->buffer)
        return NULL;

    // First pass: Count lines
    int count = 1;
    char* ptr = self->buffer;
    while (*ptr) {
        if (*ptr == '\n')
            count++;
        ptr++;
    }

    List* list = (List*)malloc(sizeof(List));
    list->length = 0;        // Will increment as we add
    list->capacity = count;  // Approximation
    list->data = (void**)malloc(sizeof(void*) * count);

    char* start = self->buffer;
    ptr = self->buffer;

    while (*ptr) {
        // Handle \r\n (Windows) or \n (Unix)
        if (*ptr == '\n' || *ptr == '\r') {
            int len = ptr - start;

            // Extract line
            char* line = (char*)malloc(len + 1);
            strncpy(line, start, len);
            line[len] = '\0';

            // Add to list
            list->data[list->length++] = make_String_taking_ownership(line);

            // Skip newline char(s)
            if (*ptr == '\r' && *(ptr + 1) == '\n')
                ptr++;  // Skip \n in \r\n

            start = ptr + 1;
        }
        ptr++;
    }

    // Add last line
    if (start <= ptr) {
        int len = ptr - start;
        char* line = (char*)malloc(len + 1);
        strncpy(line, start, len);
        line[len] = '\0';
        list->data[list->length++] = make_String_taking_ownership(line);
    }

    return list;
}

// ==========================================
//  FUZZY MATCHING (Levenshtein)
// ==========================================

int min3(int a, int b, int c) {
    int m = a;
    if (b < m)
        m = b;
    if (c < m)
        m = c;
    return m;
}

int String_distance(String* self, String* other) {
    if (!self || !other)
        return 0;

    int len1 = self->length;
    int len2 = other->length;

    // Allocation: (len1 + 1) * (len2 + 1) matrix
    // Optimization: We could use 2 rows, but a full matrix is easier to read
    // for now
    int* matrix = (int*)malloc((len1 + 1) * (len2 + 1) * sizeof(int));

// Access macro
#define M(r, c) matrix[(r) * (len2 + 1) + (c)]

    // Init first row/col
    for (int i = 0; i <= len1; i++)
        M(i, 0) = i;
    for (int j = 0; j <= len2; j++)
        M(0, j) = j;

    for (int i = 1; i <= len1; i++) {
        for (int j = 1; j <= len2; j++) {
            int cost = (self->buffer[i - 1] == other->buffer[j - 1]) ? 0 : 1;

            M(i, j) = min3(M(i - 1, j) + 1,        // Deletion
                           M(i, j - 1) + 1,        // Insertion
                           M(i - 1, j - 1) + cost  // Substitution
            );
        }
    }

    int result = M(len1, len2);
    free(matrix);
#undef M
    return result;
}