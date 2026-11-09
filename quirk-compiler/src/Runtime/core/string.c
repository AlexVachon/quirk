#include <ctype.h>

// ---- UTF-8 ------------------------------------------------------------
// Text is UTF-8. Positions and lengths seen from Quirk count characters
// (code points), as in Python: "héllo".length() is 5, not 6. The String's
// `length` field stays the byte count. ASCII text takes the same paths
// with one byte per character.
static int u8_cont(unsigned char c) { return (c & 0xC0) == 0x80; }
static int u8_bytes(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;   // invalid lead byte: one byte, one character
}
// Characters in the first `nbytes` bytes of `buf`.
static int64_t u8_count(const char* buf, int64_t nbytes) {
    int64_t n = 0;
    for (int64_t i = 0; i < nbytes; i++) if (!u8_cont((unsigned char)buf[i])) n++;
    return n;
}
static int64_t u8_len(const String* s) {
    return (s && s->buffer) ? u8_count(s->buffer, s->length) : 0;
}
// Byte offset of character `ci` (0..len; past the end → the byte length).
static int64_t u8_offset(const String* s, int64_t ci) {
    int64_t b = 0;
    while (ci > 0 && b < s->length) {
        b += u8_bytes((unsigned char)s->buffer[b]);
        ci--;
    }
    return b > s->length ? s->length : b;
}
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
extern String* quirk_opaque_to_string(void* val);

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
    return quirk_is_any(item);
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

// Parse a Python-style format spec into a printf-compatible spec and
// two post-process flags. The Python mini-language allows things printf
// doesn't:
//
//   - `<`, `>`, `^` alignment chars (printf only knows `-` for left).
//     `>` maps to printf's default right-align; `<` becomes `-`.
//     `^` (center) has no printf equivalent, so we post-process the
//     final string by padding equally on both sides.
//
//   - `,` thousand separators (Python: `:,d`, `:,.2f`). printf's `'`
//     flag is locale-dependent and not portable. We strip the comma
//     out of the printf spec and insert commas into the result
//     after the integer portion is formatted.
//
// `out` receives the printf-side spec without the leading `%` (so the
// caller does `"%" + out`). `*center` and `*commas` are set to 1 when
// those post-process steps are required, 0 otherwise.
//
// Specs that don't trigger any Python-specific feature pass through
// verbatim — `.2f` and `05d` and `x` all reach `out` unchanged.
static void translate_python_spec(const char* py,
                                  char* out, size_t out_size,
                                  int* center, int* commas) {
    *center = 0;
    *commas = 0;
    if (!py || !*py || out_size == 0) {
        if (out_size) out[0] = '\0';
        return;
    }
    const char* p = py;
    char align = 0;
    // [[fill]align] — fill char is any non-`}` char immediately
    // followed by an align char. To avoid mis-detecting `+d` etc, we
    // ONLY treat position 0 as a fill candidate if position 1 is one
    // of `<>=^`. Default fill stays implicit (space, or `0` when the
    // 0-flag is set).
    if (p[0] && p[1] && (p[1] == '<' || p[1] == '>' || p[1] == '^')) {
        // Custom fill char would go here; printf only supports ' '
        // and '0' as pad chars, so a non-space-non-zero fill needs
        // manual post-processing. v3.9.0 honours `>` / `<` / `^`
        // with space-fill only — a future release can add custom
        // fill via post-processing.
        align = p[1];
        p += 2;
    } else if (*p == '<' || *p == '>' || *p == '^') {
        align = *p;
        p++;
    }

    size_t oi = 0;
    // printf's `-` flag means "left-align". Python's `<` is left-align,
    // `>` is right-align (printf default), `^` is centred (no printf
    // equivalent — we post-process).
    if (align == '<') {
        if (oi + 1 < out_size) out[oi++] = '-';
    } else if (align == '^') {
        *center = 1;
        // No flag emitted — print right-aligned, then center the
        // padding manually after the fact.
    }
    // `>` is printf's default; no flag needed.

    // Copy the rest of the spec, dropping `,` (thousand separators)
    // and re-emitting everything else. Numeric flags and the type
    // suffix pass straight through.
    while (*p && oi + 1 < out_size) {
        if (*p == ',') { *commas = 1; p++; continue; }
        out[oi++] = *p++;
    }
    out[oi] = '\0';
}

// Insert commas into the integer portion of a freshly-formatted
// numeric string. Handles a leading sign or `-` and stops at the
// decimal point. Called only when the spec used Python `,` and the
// underlying type is numeric.
static void insert_thousand_commas(char* buf, size_t buf_size) {
    if (!buf || !*buf) return;
    // Locate the int part: skip optional sign, then count digits up
    // to a `.` or end-of-string.
    char* start = buf;
    if (*start == '+' || *start == '-' || *start == ' ') start++;
    char* digits_end = start;
    while (*digits_end && *digits_end >= '0' && *digits_end <= '9') digits_end++;
    size_t int_len = (size_t)(digits_end - start);
    if (int_len <= 3) return;
    size_t commas_to_add = (int_len - 1) / 3;
    size_t total = strlen(buf);
    if (total + commas_to_add + 1 > buf_size) return;
    // Slide everything from digits_end onward to the right by
    // `commas_to_add` bytes, then insert commas from the right.
    memmove(digits_end + commas_to_add, digits_end,
            strlen(digits_end) + 1);
    char* src = digits_end - 1;
    char* dst = digits_end + commas_to_add - 1;
    size_t since_comma = 0;
    while (src >= start) {
        *dst-- = *src--;
        since_comma++;
        if (since_comma == 3 && src >= start) {
            *dst-- = ',';
            since_comma = 0;
        }
    }
}

// Centre a printf-right-aligned result in a field of `width` chars by
// shifting up to half of the padding to the trailing side. The buffer
// already has the requested width (printf padded on the left), so we
// only need to redistribute the leading spaces.
static void center_in_field(char* buf, size_t buf_size, int width) {
    if (!buf || width <= 0) return;
    size_t cur = strlen(buf);
    // If the formatted text already exceeds the field, printf didn't
    // pad — nothing to redistribute.
    if ((int)cur > width) return;
    int lead = 0;
    while (buf[lead] == ' ') lead++;
    if (lead == 0) return;
    int trail = lead / 2;
    int new_lead = lead - trail;
    // Move the non-pad text left by `trail`, then pad the tail.
    size_t text_len = cur - lead;
    memmove(buf + new_lead, buf + lead, text_len);
    for (int i = 0; i < trail && (size_t)(new_lead + text_len + i) < buf_size - 1; i++) {
        buf[new_lead + text_len + i] = ' ';
    }
    if ((size_t)(new_lead + text_len + trail) < buf_size) {
        buf[new_lead + text_len + trail] = '\0';
    }
}

// Ensure a printf spec ends in a recognised type char. Quirk's user-
// facing format syntax lets people write `{x:>10}` (alignment only,
// no type) which Python interprets as "default the type to the value's
// repr." Printf has no such default — we'd snprintf("%10", x) which is
// undefined behaviour. This helper appends `default_type` when the
// spec lacks a type suffix so the snprintf is well-formed.
static void ensure_type_suffix(char* fmt, size_t fmt_size, char default_type) {
    if (!fmt || !*fmt) return;
    size_t n = strlen(fmt);
    char last = fmt[n - 1];
    // Already a recognised type char? Leave it.
    if (last == 'd' || last == 'i' || last == 'u' || last == 'x' ||
        last == 'X' || last == 'o' || last == 'b' || last == 'f' ||
        last == 'F' || last == 'g' || last == 'G' || last == 'e' ||
        last == 'E' || last == 's' || last == 'c') {
        return;
    }
    if (n + 2 > fmt_size) return;
    fmt[n]     = default_type;
    fmt[n + 1] = '\0';
}

// For a 64-bit Int argument: put the `ll` length modifier before an
// integer conversion char (`%5d` → `%5lld`, `%x` → `%llx`).
static void ensure_ll_length(char* fmt, size_t fmt_size) {
    size_t n = fmt ? strlen(fmt) : 0;
    if (n == 0 || !strchr("diuxXo", fmt[n - 1])) return;
    if (n >= 2 && fmt[n - 2] == 'l') return;
    if (n + 3 > fmt_size) return;
    fmt[n + 1] = fmt[n - 1];
    fmt[n - 1] = 'l'; fmt[n] = 'l';
    fmt[n + 2] = '\0';
}

// Pull the explicit width number out of a printf spec (the digits
// between the alignment flag and the type char or precision dot).
// Returns 0 if there's no width specified. Used to size the centring
// pass.
static int extract_width(const char* spec) {
    if (!spec) return 0;
    const char* p = spec;
    while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') p++;
    int w = 0;
    while (*p >= '0' && *p <= '9') { w = w * 10 + (*p - '0'); p++; }
    return w;
}

// `${s:>8}` pads with printf's `%8s`, which counts bytes: a String with
// multi-byte characters came out short. Top up to `width` characters on
// the padding side (left unless the spec has '-').
static void utf8_fix_width(char* out, size_t size, const char* spec) {
    int w = extract_width(spec);
    int64_t chars = u8_count(out, (int64_t)strlen(out));
    if (w <= 0 || chars >= w) return;
    size_t missing = (size_t)(w - chars), n = strlen(out);
    if (n + missing + 1 > size) return;
    if (strchr(spec, '-')) {
        memset(out + n, ' ', missing);
    } else {
        memmove(out + missing, out, n);
        memset(out, ' ', missing);
    }
    out[n + missing] = '\0';
}

// `${s:^9}`: centre `text` in `width` characters (extra space on the
// right, as Python). Done here rather than through printf + re-centring,
// which counted bytes.
static void utf8_center(char* out, size_t size, const char* text, int width) {
    int64_t n = (int64_t)strlen(text), chars = u8_count(text, n);
    int64_t total = width > chars ? width - chars : 0, left = total / 2, right = total - left;
    if ((size_t)(left + n + right + 1) > size) { snprintf(out, size, "%s", text); return; }
    memset(out, ' ', left);
    memcpy(out + left, text, n);
    memset(out + left + n, ' ', right);
    out[left + n + right] = '\0';
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

    char format_string[80];
    char output_buffer[256];

    // Pre-translate any Python-only features (alignment chars, `,`
    // thousand separators) into a printf-clean spec plus two post-
    // process flags. Empty spec falls through to per-branch defaults.
    char translated[64];
    int  center = 0;
    int  commas = 0;
    translated[0] = '\0';
    if (fmt_spec && strlen(fmt_spec) > 0) {
        translate_python_spec(fmt_spec, translated, sizeof(translated),
                              &center, &commas);
    }

    // Tagged integer passed as pointer
    if ((uintptr_t)item <= 0xFFFFFFFFUL) {
        int ival = (int)(uintptr_t)item;
        if (translated[0]) {
            snprintf(format_string, sizeof(format_string), "%%%s", translated);
            if (strchr(format_string, 's')) strcpy(format_string, "%d");
            ensure_type_suffix(format_string, sizeof(format_string), 'd');
            snprintf(output_buffer, 256, format_string, ival);
        } else {
            snprintf(output_buffer, 256, "%d", ival);
        }
        if (commas) insert_thousand_commas(output_buffer, sizeof(output_buffer));
        if (center) center_in_field(output_buffer, sizeof(output_buffer), extract_width(translated));
        buffer_append(buf, cap, len, output_buffer);
        return;
    }

    // Raw String* or other heap object that is NOT a valid Any*
    if (!is_valid_any_ptr(item)) {
        String* s = (String*)item;
        const char* cstr = (s && s->buffer) ? s->buffer : "(null)";
        if (translated[0]) {
            snprintf(format_string, sizeof(format_string), "%%%s", translated);
            if (strpbrk(format_string, "dxfge")) strcpy(format_string, "%s");
            ensure_type_suffix(format_string, sizeof(format_string), 's');
            snprintf(output_buffer, 256, format_string, cstr);
            if (center) utf8_center(output_buffer, sizeof(output_buffer), cstr, extract_width(translated));
            else utf8_fix_width(output_buffer, sizeof(output_buffer), translated);
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
        snprintf(format_string, sizeof(format_string), "%%%s", translated[0] ? translated : fmt_spec);
        ensure_type_suffix(format_string, sizeof(format_string), 'f');
        double d = Core_Primitives_Any_to_float(a);
        snprintf(output_buffer, 256, format_string, d);
        if (commas) insert_thousand_commas(output_buffer, sizeof(output_buffer));
        if (center) center_in_field(output_buffer, sizeof(output_buffer), extract_width(translated));
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
            if (translated[0]) {
                snprintf(format_string, sizeof(format_string), "%%%s", translated);
                if (strchr(format_string, 's'))
                    strcpy(format_string, "%d");
                ensure_type_suffix(format_string, sizeof(format_string), 'd');
                ensure_ll_length(format_string, sizeof(format_string));
                snprintf(output_buffer, 256, format_string, (long long)a->ival);
            } else {
                snprintf(output_buffer, 256, "%lld", (long long)a->ival);
            }
            if (commas) insert_thousand_commas(output_buffer, sizeof(output_buffer));
            if (center) center_in_field(output_buffer, sizeof(output_buffer), extract_width(translated));
            buffer_append(buf, cap, len, output_buffer);
            break;
        }
        case ANY_DOUBLE: {
            if (translated[0]) {
                snprintf(format_string, sizeof(format_string), "%%%s", translated);
                ensure_type_suffix(format_string, sizeof(format_string), 'g');
                snprintf(output_buffer, 256, format_string, a->dval);
            } else {
                quirk_format_double(a->dval, output_buffer, 256);
            }
            if (commas) insert_thousand_commas(output_buffer, sizeof(output_buffer));
            if (center) center_in_field(output_buffer, sizeof(output_buffer), extract_width(translated));
            buffer_append(buf, cap, len, output_buffer);
            break;
        }
        default: {
            // String, List, Map, Ptr, Null — convert to string then format
            String* s = Core_Primitives_Any_to_string(a);
            const char* cstr = (s && s->buffer) ? s->buffer : "(null)";
            if (translated[0]) {
                snprintf(format_string, sizeof(format_string), "%%%s", translated);
                if (strpbrk(format_string, "dxfge"))
                    strcpy(format_string, "%s");
                ensure_type_suffix(format_string, sizeof(format_string), 's');
                snprintf(output_buffer, 256, format_string, cstr);
                if (center) utf8_center(output_buffer, sizeof(output_buffer), cstr, extract_width(translated));
                else utf8_fix_width(output_buffer, sizeof(output_buffer), translated);
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
    if (quirk_is_any(raw)) {
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
    // No-op: memory is GC-managed (runtime.c maps free onto nothing), and
    // string literals are shared static Strings — clearing `buffer` here
    // would blank every other use of the same literal.
    (void)self;
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
    // Null String (explicit null, Map.get miss) concatenates as "null",
    // matching print — not as tagged-int 0.
    if (!val) return make_String("null");
    uintptr_t v = (uintptr_t)val;
    if (v <= MAX_SMALL_INT) {
        char temp[32];
        sprintf(temp, "%d", (int)v);
        return make_String(temp);
    }
    return val;
}

// `s * n` — fresh String with self repeated n times. `n <= 0`
// returns the empty string (Python semantics). Called by the
// `__mul` magic method dispatch on String * Int.
String* Core_String_String___mul(String* self, int64_t n) {
    if (!self || !self->buffer || n <= 0) return make_String("");
    size_t len = strlen(self->buffer);
    if (len == 0) return make_String("");
    size_t total = len * (size_t)n;
    char* buf = (char*)GC_malloc(total + 1);
    for (int i = 0; i < n; i++) {
        memcpy(buf + i * len, self->buffer, len);
    }
    buf[total] = '\0';
    return make_String_taking_ownership(buf);
}

String* Core_String_String___add(String* self, String* other) {
    self = quirk_ensure_string(self);
    other = quirk_ensure_string(other);
    if (!self || !other)
        return make_String("");
    int64_t new_len = self->length + other->length;
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
    int64_t len = self->length + 2;
    char* raw = (char*)GC_malloc(len + 1);
    snprintf(raw, len + 1, "\"%s\"", self->buffer);
    return make_String(raw);
}

// `s[i]` — the character at `i` as a one-character String (negative `i`
// counts from the end). Sema types `s[i]` as String; codegen used to
// return the character code as an Int, which crashed wherever the result
// was used as a String.
String* quirk_string_char_at(String* self, int64_t index) {
    if (!self || !self->buffer) {
        quirk_throw_exception("IndexError", "string index on null string");
    }
    int64_t clen = u8_len(self);
    if (index < 0) index += clen;
    if (index < 0 || index >= clen) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "string index %lld out of range (length: %lld)", (long long)index, (long long)clen);
        quirk_throw_exception("IndexError", buf);
    }
    int64_t off = u8_offset(self, index);
    int nb = u8_bytes((unsigned char)self->buffer[off]);
    if (off + nb > self->length) nb = (int)(self->length - off);
    char* raw = (char*)GC_malloc(nb + 1);
    memcpy(raw, self->buffer + off, nb);
    raw[nb] = '\0';
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
                 "string index %d out of range (length: %lld)", index, (long long)self->length);
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
    // One character (all its UTF-8 bytes); idx is a byte position.
    String* str = self->str_ref;
    int nb = u8_bytes((unsigned char)str->buffer[self->idx]);
    if (self->idx + nb > str->length) nb = (int)(str->length - self->idx);
    char* raw = (char*)malloc(nb + 1);
    memcpy(raw, str->buffer + self->idx, nb);
    raw[nb] = '\0';
    self->idx += nb;
    return make_String_taking_ownership(raw);
}

StringIterator* Core_String_String___iter(String* self) {
    StringIterator* iter = (StringIterator*)quirk_alloc(QT_ITERATOR, sizeof(StringIterator));
    Core_String_StringIterator___init(iter, self);
    return iter;
}

// ==========================================
//  CONSTRUCTORS
// ==========================================

String* make_String_taking_ownership(char* raw_buffer) {
    if (!raw_buffer)
        return NULL;
    String* s = (String*)quirk_alloc(QT_STRING, sizeof(String));
    s->buffer = raw_buffer;
    s->length = strlen(raw_buffer);
    return s;
}

String* make_String(const char* raw) {
    String* s = (String*)quirk_alloc(QT_STRING, sizeof(String));
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

// ---- Case mapping -----------------------------------------------------
// Simple (one-to-one) mappings for ASCII, Latin-1, Latin Extended-A, Greek
// and Cyrillic — enough for most European text, without locale tables
// (C's towupper only maps ASCII in the "C" locale). ß upper-cases to "SS".
static uint32_t u8_decode(const unsigned char* p, int64_t avail, int* used) {
    int n = u8_bytes(p[0]);
    if (n > avail) n = 1;
    for (int i = 1; i < n; i++) if (!u8_cont(p[i])) { n = 1; break; }
    *used = n;
    switch (n) {
        case 2: return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        case 3: return ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        case 4: return ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12)
                     | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        default: return p[0];
    }
}
static int u8_encode(uint32_t cp, char* out) {
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
// Latin Extended-A: alternating upper/lower pairs.
static int cp_pair_upper_even(uint32_t c) {
    return (c >= 0x0100 && c <= 0x0137 && c != 0x0130 && c != 0x0131) ||
           (c >= 0x014A && c <= 0x0177) || (c >= 0x0460 && c <= 0x0481) ||
           (c >= 0x048A && c <= 0x04BF) || (c >= 0x04D0 && c <= 0x04FF);
}
static int cp_pair_upper_odd(uint32_t c) {
    return (c >= 0x0139 && c <= 0x0148) || (c >= 0x0179 && c <= 0x017E) ||
           (c >= 0x04C1 && c <= 0x04CE);
}
static uint32_t cp_upper(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c == 0xFF) return 0x0178;
    if (c == 0xB5) return 0x039C;
    if (cp_pair_upper_even(c)) return (c & 1) ? c - 1 : c;
    if (cp_pair_upper_odd(c))  return (c & 1) ? c : c - 1;
    if (c == 0x017F) return 'S';
    if (c >= 0x03B1 && c <= 0x03C9) return c == 0x03C2 ? 0x03A3 : c - 32;   // final sigma → Σ
    if (c == 0x03AC) return 0x0386;
    if (c >= 0x03AD && c <= 0x03AF) return c - 37;
    if (c == 0x03CC) return 0x038C;
    if (c == 0x03CD || c == 0x03CE) return c - 63;
    if (c >= 0x0430 && c <= 0x044F) return c - 32;
    if (c >= 0x0450 && c <= 0x045F) return c - 80;
    return c;
}
static uint32_t cp_lower(uint32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    if (c == 0x0178) return 0xFF;
    if (cp_pair_upper_even(c)) return (c & 1) ? c : c + 1;
    if (cp_pair_upper_odd(c))  return (c & 1) ? c + 1 : c;
    if (c >= 0x0391 && c <= 0x03A9 && c != 0x03A2) return c + 32;
    if (c == 0x0386) return 0x03AC;
    if (c >= 0x0388 && c <= 0x038A) return c + 37;
    if (c == 0x038C) return 0x03CC;
    if (c == 0x038E || c == 0x038F) return c + 63;
    if (c >= 0x0410 && c <= 0x042F) return c + 32;
    if (c >= 0x0400 && c <= 0x040F) return c + 80;
    return c;
}
static int cp_is_upper(uint32_t c) { return cp_lower(c) != c; }
static int cp_is_lower(uint32_t c) { return cp_upper(c) != c || c == 0xDF || c == 0x0138 || c == 0x03C2; }
static int cp_is_alpha(uint32_t c) {
    if (c < 0x80) return isalpha((int)c) != 0;
    if (cp_is_upper(c) || cp_is_lower(c)) return 1;
    return c == 0xAA || c == 0xBA ||
           (c >= 0x0370 && c <= 0x03FF && c != 0x037E && c != 0x0387) ||   // Greek
           (c >= 0x0400 && c <= 0x052F && !(c >= 0x0482 && c <= 0x0489)) ||   // Cyrillic
           (c >= 0x05D0 && c <= 0x05EA) || (c >= 0x0620 && c <= 0x064A) ||   // Hebrew, Arabic
           (c >= 0x3041 && c <= 0x30FF && c != 0x30FB) ||                       // kana
           (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7A3);        // CJK, Hangul
}

enum { CASE_UPPER, CASE_LOWER, CASE_SWAP, CASE_TITLE, CASE_CAPITALIZE, CASE_SENTENCE };
static String* u8_case(String* self, int mode) {
    if (!self || !self->buffer) return make_String("");
    const unsigned char* p = (const unsigned char*)self->buffer;
    int64_t n = self->length;
    char* out = (char*)malloc((size_t)n * 2 + 4);   // mappings here never grow past 2×
    int64_t o = 0;
    int prev_cased = 0, first = 1, expect_capital = 1;
    for (int64_t i = 0; i < n; ) {
        int used;
        uint32_t c = u8_decode(p + i, n - i, &used);
        uint32_t r = c;
        int alpha = cp_is_alpha(c);
        int up = 0;   // this character is being upper-cased
        switch (mode) {
            case CASE_UPPER: up = 1; break;
            case CASE_LOWER: break;
            case CASE_SWAP:  up = !cp_is_upper(c); break;
            case CASE_TITLE: up = !prev_cased; prev_cased = alpha; break;
            case CASE_CAPITALIZE: up = first; break;
            case CASE_SENTENCE:
                if (alpha) { up = expect_capital; expect_capital = 0; }
                else if (c == '.' || c == '!' || c == '?') expect_capital = 1;
                break;
        }
        if (mode != CASE_SENTENCE || alpha) r = up ? cp_upper(c) : cp_lower(c);
        // ß has no one-letter capital: "SS" in upper case, "Ss" as the
        // first letter of a title-cased word (as in Python).
        if (c == 0xDF && up) {
            int whole = mode == CASE_UPPER || mode == CASE_SWAP;
            out[o++] = 'S';
            out[o++] = whole ? 'S' : 's';
            i += used;
            first = 0;
            continue;
        }
        if (r == c) { memcpy(out + o, p + i, (size_t)used); o += used; }
        else o += u8_encode(r, out + o);
        i += used;
        first = 0;
    }
    out[o] = '\0';
    return make_String_taking_ownership(out);
}

// ==========================================
//  METHODS
// ==========================================

String* Core_String_String_upper(String* self) { return u8_case(self, CASE_UPPER); }

String* Core_String_String_lower(String* self) { return u8_case(self, CASE_LOWER); }

String* Core_String_String_title(String* self) { return u8_case(self, CASE_TITLE); }

String* Core_String_String_capitalize(String* self) { return u8_case(self, CASE_CAPITALIZE); }

String* Core_String_String_sentence_case(String* self) { return u8_case(self, CASE_SENTENCE); }

static int is_url_safe(char c) {
    return isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' ||
           c == '~';
}

String* Core_String_String_encode(String* self) {
    if (!self || !self->buffer)
        return make_String("");
    // Sanity: length may be garbage from an uninitialised struct.
    // Fall back to computing from the buffer's actual NUL terminator.
    if (self->length < 0 || self->length > (1 << 24)) {
        self->length = (int)strlen(self->buffer);
    }
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

int64_t Core_String_String_count(String* self, String* sub) {
    if (!self || !self->buffer || !sub || !sub->buffer)
        return 0;
    int64_t sub_len = sub->length;
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

int64_t Core_String_String_find(String* self, String* sub) {
    if (!self || !self->buffer || !sub || !sub->buffer)
        return -1;
    // Sanity: length fields on either String may be garbage
    // from an uninitialised struct. Bail if they don't look
    // like plausible c-string lengths — otherwise strstr may
    // read past a truncated buffer into unmapped memory.
    if (self->length < 0 || self->length > (1 << 24)) return -1;
    if (sub->length < 0 || sub->length > (1 << 24)) return -1;
    char* ptr = strstr(self->buffer, sub->buffer);
    return (ptr == NULL) ? -1 : u8_count(self->buffer, ptr - self->buffer);   // a character position
}

int64_t Core_String_String_index(String* self, String* sub) {
    int64_t idx = Core_String_String_find(self, sub);
    if (idx == -1) {
        printf("ValueError: substring not found\n");
        exit(1);
    }
    return idx;
}

String* Core_String_String_trim(String* self) {
    if (!self || !self->buffer)
        return make_String("");
    int64_t start = 0, end = self->length - 1;
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
    int64_t old_len = old_str->length;
    if (old_len == 0)
        return make_String(self->buffer);
    while ((tmp = strstr(tmp, old_str->buffer))) {
        count++;
        tmp += old_len;
    }
    int64_t new_len = self->length + (count * (new_str->length - old_len));
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
    int64_t sub_len = sub->length;
    if (sub_len == 0)
        return make_String(self->buffer);
    int count = 0;
    char* tmp = self->buffer;
    while ((tmp = strstr(tmp, sub->buffer))) {
        count++;
        tmp += sub_len;
    }
    int64_t new_len = self->length - (count * sub_len);
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

String* Core_String_String_repeat(String* self, int64_t n) {
    if (!self || !self->buffer || n <= 0)
        return make_String("");
    int64_t new_len = self->length * n;
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
    // Reverse the characters, keeping each one's UTF-8 bytes in order.
    char* result = (char*)malloc(self->length + 1);
    int64_t out = self->length;
    for (int64_t i = 0; i < self->length;) {
        int nb = u8_bytes((unsigned char)self->buffer[i]);
        if (i + nb > self->length) nb = (int)(self->length - i);
        out -= nb;
        memcpy(result + out, self->buffer + i, nb);
        i += nb;
    }
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

String* Core_String_String_substring(String* self, int64_t start, int64_t end) {
    if (!self || !self->buffer)
        return make_String("");
    // start / end are character positions.
    int64_t clen = u8_len(self);
    if (start < 0)
        start += clen;
    if (end < 0)
        end += clen;
    if (start < 0)
        start = 0;
    if (end > clen)
        end = clen;
    if (start >= end)
        return make_String("");
    start = u8_offset(self, start);
    end = u8_offset(self, end);
    int64_t new_len = end - start;
    char* temp = (char*)malloc(new_len + 1);
    memcpy(temp, self->buffer + start, new_len);
    temp[new_len] = '\0';
    return make_String_taking_ownership(temp);
}

List* Core_String_String_split(String* self, String* delim) {
    if (!self || !self->buffer) {
        List* empty = (List*)quirk_alloc(QT_LIST, sizeof(List));
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

    List* list = (List*)quirk_alloc(QT_LIST, sizeof(List));
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
    // Route every element through `quirk_opaque_to_string` so we
    // correctly stringify each of the three opaque shapes:
    //   - tagged-Int (low <= 0xFFFFFFFF)
    //   - heap Any* (tag in ANY_INT..ANY_NULL) — needed for
    //     `[1.5, 2.5].join(...)` and any list of boxed primitives
    //   - real String* / other struct ptr
    // The old fast-path inlined the tagged-Int case and assumed
    // every non-Int item was a String*, which SIGSEGV'd on heap-
    // tagged Doubles passed to `print([1.5, 2.5])`.
    String** rendered = (String**)malloc(items->size * sizeof(String*));
    int total_len = 0;
    for (int i = 0; i < items->size; i++) {
        rendered[i] = quirk_opaque_to_string(items->data[i]);
        if (rendered[i] && rendered[i]->buffer)
            total_len += rendered[i]->length;
        if (i < items->size - 1)
            total_len += self->length;
    }
    char* result = (char*)malloc(total_len + 1);
    char* ptr = result;
    *ptr = '\0';
    for (int i = 0; i < items->size; i++) {
        String* s = rendered[i];
        if (s && s->buffer) {
            memcpy(ptr, s->buffer, s->length);
            ptr += s->length;
        }
        if (i < items->size - 1) {
            memcpy(ptr, self->buffer, self->length);
            ptr += self->length;
        }
    }
    *ptr = '\0';
    free(rendered);
    return make_String_taking_ownership(result);
}

// Pad `self` to `width` characters with the first character of `pad`
// (a space when empty): `side` 'l' = text on the left (ljust), 'r' = on
// the right (rjust / zfill), 'c' = centred.
static String* u8_pad(String* self, int64_t width, String* pad, char side) {
    if (!self || !self->buffer)
        return make_String("");
    const char* pc = (pad && pad->buffer && pad->length > 0) ? pad->buffer : " ";
    int pb = u8_bytes((unsigned char)pc[0]);
    int64_t clen = u8_len(self);
    if (clen >= width)
        return make_String(self->buffer);
    int64_t total = width - clen;
    int64_t left = side == 'l' ? 0 : side == 'r' ? total : total / 2;
    int64_t right = total - left;
    char* result = (char*)malloc((left + right) * pb + self->length + 1);
    char* w = result;
    for (int64_t k = 0; k < left; k++) { memcpy(w, pc, pb); w += pb; }
    memcpy(w, self->buffer, self->length); w += self->length;
    for (int64_t k = 0; k < right; k++) { memcpy(w, pc, pb); w += pb; }
    *w = '\0';
    return make_String_taking_ownership(result);
}

String* Core_String_String_zfill(String* self, int64_t width) {
    String zero = {0};
    zero.buffer = (char*)"0";
    zero.length = 1;
    return u8_pad(self, width, &zero, 'r');
}

String* Core_String_String_ljust(String* self, int64_t width, String* pad) {
    return u8_pad(self, width, pad, 'l');
}

String* Core_String_String_rjust(String* self, int64_t width, String* pad) {
    return u8_pad(self, width, pad, 'r');
}

String* Core_String_String_lstrip(String* self) {
    return Core_String_String_trim(self);
}
String* Core_String_String_rstrip(String* self) {
    return Core_String_String_trim(self);
}

String* Core_String_String_swapcase(String* self) { return u8_case(self, CASE_SWAP); }

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

String* Core_String_String_center(String* self, int64_t width, String* pad) {
    return u8_pad(self, width, pad, 'c');
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
    if (!self || !self->buffer || self->length == 0) return 0;
    const unsigned char* p = (const unsigned char*)self->buffer;
    int saw_letter = 0;
    for (int64_t i = 0; i < self->length; ) {
        int used;
        uint32_t c = u8_decode(p + i, self->length - i, &used);
        i += used;
        if (cp_is_alpha(c)) {
            saw_letter = 1;
            if (!cp_is_upper(c)) return 0;
        }
    }
    return saw_letter;
}

int Core_String_String_is_lower(String* self) {
    if (!self || !self->buffer || self->length == 0) return 0;
    const unsigned char* p = (const unsigned char*)self->buffer;
    int saw_letter = 0;
    for (int64_t i = 0; i < self->length; ) {
        int used;
        uint32_t c = u8_decode(p + i, self->length - i, &used);
        i += used;
        if (cp_is_alpha(c)) {
            saw_letter = 1;
            if (!cp_is_lower(c)) return 0;
        }
    }
    return saw_letter;
}

int Core_String_String_is_alpha(String* self) {
    if (!self || !self->buffer || self->length == 0) return 0;
    const unsigned char* p = (const unsigned char*)self->buffer;
    for (int64_t i = 0; i < self->length; ) {
        int used;
        uint32_t c = u8_decode(p + i, self->length - i, &used);
        i += used;
        if (!cp_is_alpha(c)) return 0;
    }
    return 1;
}

extern void quirk_throw_exception(const char* type_name, const char* message);

int64_t Core_String_String_to_int(String* self) {
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
    quirk_format_double(val, buf, 64);
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
    List* list = (List*)quirk_alloc(QT_LIST, sizeof(List));
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

int64_t Core_String_String_distance(String* self, String* other) {
    if (!self || !other)
        return 0;
    int64_t len1 = self->length;
    int64_t len2 = other->length;
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

int64_t Core_String_String_length(String* self) {
    return u8_len(self);
}

// _str_join(parts: List<String>) -> String — single-pass
// concatenation. Used by selfhost's codegen to flatten its
// `out: List<String>` accumulator without the O(N²) growth
// of repeated `+`. Walks the list twice: once to sum
// lengths, once to copy each piece via strcat into a
// freshly-allocated buffer.
extern int64_t Core_Collections_List_List_length(List* self);
extern void* Core_Collections_List_List___get(List* self, int64_t index);

String* Core_String__str_join(List* parts) {
    if (!parts) return make_String("");
    int64_t n = Core_Collections_List_List_length(parts);
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        String* s = (String*)Core_Collections_List_List___get(parts, i);
        if (s && s->buffer) total += strlen(s->buffer);
    }
    char* buf = (char*)GC_malloc(total + 1);
    if (!buf) return make_String("");
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        String* s = (String*)Core_Collections_List_List___get(parts, i);
        if (s && s->buffer) strcat(buf, s->buffer);
    }
    return make_String_taking_ownership(buf);
}
