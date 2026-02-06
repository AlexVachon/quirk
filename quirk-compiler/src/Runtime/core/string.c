#include "types.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

#define MAX_SMALL_INT 0xFFFFF

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
    s->length = strlen(raw);
    s->buffer = (char*)malloc(s->length + 1);
    strcpy(s->buffer, raw);
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
            // It's likely an Integer: Count digits
            // snprintf(NULL, 0, ...) returns the length without writing
            char temp[32];
            total_len += sprintf(temp, "%d", (int)val);
        } else {
            // It's likely a String (char*): Add strlen
            char* s = (char*)item;
            if (s)
                total_len += strlen(s);
        }

        // Add separator length (if not last item)
        if (i < items->length - 1) {
            total_len += self->length;
        }
    }

    // Allocate buffer (+1 for null terminator)
    char* result = (char*)malloc(total_len + 1);
    if (!result)
        return NULL;

    char* ptr = result;
    *ptr = '\0';  // Start empty

    // --- PASS 2: Build the string ---
    for (int i = 0; i < items->length; i++) {
        void* item = items->data[i];
        uintptr_t val = (uintptr_t)item;

        if (val <= MAX_SMALL_INT) {
            // Write Integer
            ptr += sprintf(ptr, "%d", (int)val);
        } else {
            // Write String
            char* s = (char*)item;
            if (s) {
                strcpy(ptr, s);
                ptr += strlen(s);
            }
        }

        // Add separator
        if (i < items->length - 1) {
            memcpy(ptr, self->buffer, self->length);
            ptr += self->length;
        }
    }
    *ptr = '\0';  // Ensure null termination

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

int String_isdigit(String* self) {
    if (!self || self->length == 0)
        return 0;
    for (int i = 0; i < self->length; i++) {
        if (!isdigit((unsigned char)self->buffer[i]))
            return 0;
    }
    return 1;
}

int String_isalpha(String* self) {
    if (!self || self->length == 0)
        return 0;
    for (int i = 0; i < self->length; i++) {
        if (!isalpha((unsigned char)self->buffer[i]))
            return 0;
    }
    return 1;
}

String* String_format_map(String* self, List* keys, List* values) {
    if (!self || !self->buffer)
        return make_String("");

    int buffer_cap = 2048;
    char* result_buf = (char*)malloc(buffer_cap);
    if (!result_buf)
        return NULL;
    result_buf[0] = '\0';

    int i = 0;
    while (i < self->length) {
        if (self->buffer[i] == '%' && (i + 1 < self->length) &&
            self->buffer[i + 1] == '{') {
            char* close_ptr = strchr(self->buffer + i, '}');
            if (close_ptr) {
                int key_start = i + 2;
                int key_len = close_ptr - (self->buffer + key_start);

                char* key = (char*)malloc(key_len + 1);
                strncpy(key, self->buffer + key_start, key_len);
                key[key_len] = '\0';

                int idx = find_key_index(keys, key);
                char* val_str = "?";
                if (idx != -1 && idx < values->length) {
                    val_str = (char*)values->data[idx];
                }

                if (strlen(result_buf) + strlen(val_str) >= buffer_cap) {
                    buffer_cap = (buffer_cap * 2) + strlen(val_str);
                    result_buf = (char*)realloc(result_buf, buffer_cap);
                }
                strcat(result_buf, val_str);
                free(key);

                i = (close_ptr - self->buffer) + 1;
                continue;
            }
        }

        int len = strlen(result_buf);
        if (len + 1 >= buffer_cap) {
            buffer_cap *= 2;
            result_buf = (char*)realloc(result_buf, buffer_cap);
        }
        result_buf[len] = self->buffer[i];
        result_buf[len + 1] = '\0';
        i++;
    }

    return make_String_taking_ownership(result_buf);
}

String* String_format_list(String* self, List* args) {
    if (!self || !self->buffer)
        return make_String("");

    int buffer_cap = 2048;
    char* result_buf = (char*)malloc(buffer_cap);
    if (!result_buf)
        return NULL;
    result_buf[0] = '\0';

    int arg_idx = 0;
    int i = 0;

    while (i < self->length) {
        if (self->buffer[i] == '%' && (i + 1 < self->length)) {
            char specifier = self->buffer[i + 1];

            if (specifier == '%') {
                int len = strlen(result_buf);
                if (len + 1 >= buffer_cap) {
                    buffer_cap *= 2;
                    result_buf = (char*)realloc(result_buf, buffer_cap);
                }
                result_buf[len] = '%';
                result_buf[len + 1] = '\0';
                i += 2;
                continue;
            }

            if (arg_idx < args->length) {
                char* val_str = (char*)args->data[arg_idx];
                arg_idx++;

                if (strlen(result_buf) + strlen(val_str) >= buffer_cap) {
                    buffer_cap = (buffer_cap * 2) + strlen(val_str);
                    result_buf = (char*)realloc(result_buf, buffer_cap);
                }
                strcat(result_buf, val_str);
                i += 2;
                continue;
            }
        }

        int len = strlen(result_buf);
        if (len + 1 >= buffer_cap) {
            buffer_cap *= 2;
            result_buf = (char*)realloc(result_buf, buffer_cap);
        }
        result_buf[len] = self->buffer[i];
        result_buf[len + 1] = '\0';
        i++;
    }

    return make_String_taking_ownership(result_buf);
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