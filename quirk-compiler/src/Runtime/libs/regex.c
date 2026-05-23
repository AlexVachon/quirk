#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <gc.h>
#include "../types.h"

extern void quirk_throw_exception(const char* type_name, const char* message);

// ---------------------------------------------------------------------------
//  Regex runtime (POSIX ERE) exported to libs/regex/index.quirk.
//  Naming: Regex_<name>  (matches `linkageName = "Regex_<name>"` from
//  Parser::computeModulePrefix for libs/regex/index.quirk).
//
//  Compiled patterns are returned to Quirk as opaque Any* handles. Quirk
//  stores them in a `Regex` struct's `_handle` field and forwards them to
//  the Regex_* externs that operate on them.
//
//  Limitations vs. PCRE: no backreferences in patterns, no lookahead/behind,
//  no named groups, no \d/\w shorthands (use [0-9] / [[:alnum:]]).
// ---------------------------------------------------------------------------

#define MAX_GROUPS 32

typedef struct {
    regex_t r;
    int compiled;
    // Cache of the most-recent match's group offsets — populated by
    // Regex_find_at(); inspected by Regex_group_start/end(idx).
    regmatch_t last_groups[MAX_GROUPS];
    int last_count;
    int last_origin;  // offset in the original string for translating rm_so/rm_eo
} QkRegex;

// Translate a Quirk-side flag string ("i", "m", "im", ...) into POSIX cflags.
// Defaults to REG_EXTENDED so users get familiar `+`, `?`, `|`, `()` semantics
// without an explicit flag.
static int parse_flags(String* flags) {
    int cflags = REG_EXTENDED;
    if (!flags || !flags->buffer) return cflags;
    for (int i = 0; i < flags->length; i++) {
        char c = flags->buffer[i];
        if (c == 'i' || c == 'I') cflags |= REG_ICASE;
        if (c == 'm' || c == 'M') cflags |= REG_NEWLINE;
    }
    return cflags;
}

// ---------------------------------------------------------------------------
//  Compilation / lifecycle
// ---------------------------------------------------------------------------

void* Regex_compile_raw(String* pattern, String* flags) {
    if (!pattern || !pattern->buffer) {
        quirk_throw_exception("ValueError", "regex.compile: pattern is null");
        return NULL;
    }
    QkRegex* qr = (QkRegex*)GC_malloc(sizeof(QkRegex));
    qr->compiled = 0;
    qr->last_count = 0;
    qr->last_origin = 0;
    char* p = make_safe_cstr(pattern);
    int err = regcomp(&qr->r, p, parse_flags(flags));
    if (err != 0) {
        char errbuf[256];
        regerror(err, &qr->r, errbuf, sizeof(errbuf));
        quirk_throw_exception("ValueError", errbuf);
        return NULL;
    }
    qr->compiled = 1;
    return qr;
}

// ---------------------------------------------------------------------------
//  Single-shot test (no group capture)
// ---------------------------------------------------------------------------

int Regex_test_raw(void* handle, String* s) {
    QkRegex* qr = (QkRegex*)handle;
    if (!qr || !qr->compiled || !s || !s->buffer) return 0;
    char* str = make_safe_cstr(s);
    // POSIX: REG_NOSUB is a *compile* flag — passing it as eflags is invalid.
    return regexec(&qr->r, str, 0, NULL, 0) == 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
//  Find: stores matches in the QkRegex's last_groups cache and returns the
//  start of the full match (or -1 on no match). Quirk then queries
//  group_start/end to assemble a `Match` value.
// ---------------------------------------------------------------------------

int Regex_find_at(void* handle, String* s, int from_offset) {
    QkRegex* qr = (QkRegex*)handle;
    if (!qr || !qr->compiled || !s || !s->buffer) return -1;
    int len = s->length;
    if (from_offset < 0) from_offset = 0;
    if (from_offset > len) return -1;

    int err = regexec(&qr->r, s->buffer + from_offset, MAX_GROUPS, qr->last_groups, 0);
    if (err != 0) {
        qr->last_count = 0;
        return -1;
    }
    qr->last_count = 0;
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (qr->last_groups[i].rm_so == -1) break;
        qr->last_count++;
    }
    qr->last_origin = from_offset;
    return (int)qr->last_groups[0].rm_so + from_offset;
}

int Regex_group_count(void* handle) {
    QkRegex* qr = (QkRegex*)handle;
    return qr ? qr->last_count : 0;
}

int Regex_group_start(void* handle, int idx) {
    QkRegex* qr = (QkRegex*)handle;
    if (!qr || idx < 0 || idx >= qr->last_count) return -1;
    if (qr->last_groups[idx].rm_so < 0) return -1;
    return (int)qr->last_groups[idx].rm_so + qr->last_origin;
}

int Regex_group_end(void* handle, int idx) {
    QkRegex* qr = (QkRegex*)handle;
    if (!qr || idx < 0 || idx >= qr->last_count) return -1;
    if (qr->last_groups[idx].rm_eo < 0) return -1;
    return (int)qr->last_groups[idx].rm_eo + qr->last_origin;
}

// ---------------------------------------------------------------------------
//  Replace-all: build a fresh String, copying non-match segments verbatim
//  and substituting `replacement` for each match. Replacement supports
//  `\1`, `\2`, ... backrefs to capture groups.
// ---------------------------------------------------------------------------

static void buf_append_n(char** buf, int* cap, int* len, const char* src, int n) {
    while (*len + n + 1 > *cap) {
        int new_cap = *cap ? (*cap * 2) : 64;
        char* grown = (char*)realloc(*buf, new_cap);
        if (!grown) return;   // OOM — leave the buffer at its current size
        *buf = grown;
        *cap = new_cap;
    }
    if (!*buf) return;
    memcpy(*buf + *len, src, n);
    *len += n;
    (*buf)[*len] = '\0';
}

String* Regex_replace_all_raw(void* handle, String* s, String* replacement) {
    QkRegex* qr = (QkRegex*)handle;
    if (!qr || !qr->compiled || !s || !s->buffer) return s;
    const char* repl = (replacement && replacement->buffer) ? replacement->buffer : "";
    int repl_len = replacement ? replacement->length : 0;

    char* out = NULL;
    int cap = 0, len = 0;

    int offset = 0;
    int input_len = s->length;
    regmatch_t groups[MAX_GROUPS];

    while (offset <= input_len) {
        int err = regexec(&qr->r, s->buffer + offset, MAX_GROUPS, groups, 0);
        if (err != 0) {
            buf_append_n(&out, &cap, &len, s->buffer + offset, input_len - offset);
            break;
        }
        int m_start = (int)groups[0].rm_so;
        int m_end   = (int)groups[0].rm_eo;
        // Copy literal segment before this match.
        buf_append_n(&out, &cap, &len, s->buffer + offset, m_start);
        // Expand the replacement, handling \N backrefs.
        for (int i = 0; i < repl_len; i++) {
            char c = repl[i];
            if (c == '\\' && i + 1 < repl_len) {
                char d = repl[i + 1];
                if (d >= '0' && d <= '9') {
                    int gi = d - '0';
                    if (gi < MAX_GROUPS && groups[gi].rm_so >= 0) {
                        int gs = (int)groups[gi].rm_so;
                        int ge = (int)groups[gi].rm_eo;
                        buf_append_n(&out, &cap, &len, s->buffer + offset + gs, ge - gs);
                    }
                    i++;
                    continue;
                }
                // Literal escape (\\, \n already decoded by lexer, but defensive).
                buf_append_n(&out, &cap, &len, &d, 1);
                i++;
                continue;
            }
            buf_append_n(&out, &cap, &len, &c, 1);
        }
        // Advance past the match. If the match was zero-width, step forward
        // one byte to avoid infinite looping.
        if (m_end == m_start) {
            if (offset + m_start < input_len) {
                buf_append_n(&out, &cap, &len, s->buffer + offset + m_start, 1);
            }
            offset += m_start + 1;
        } else {
            offset += m_end;
        }
    }

    if (!out) {
        // No matches AND no input copied (e.g. empty input). Return empty.
        out = (char*)malloc(1);
        out[0] = '\0';
    }
    return make_String_taking_ownership(out);
}

// ---------------------------------------------------------------------------
//  list.c is included before us in the unity build, so List___init and
//  List_append are already in scope without an `extern` declaration.
// ---------------------------------------------------------------------------

static List* regex__new_list(void) {
    List* l = (List*)GC_malloc(sizeof(List));
    Core_Collections_List_List___init(l);
    return l;
}

// Splits `s` on every match of the pattern. Empty matches advance one byte.
// Returns a List<String> of the segments between matches.
List* Regex_split_raw(void* handle, String* s) {
    QkRegex* qr = (QkRegex*)handle;
    List* result = regex__new_list();
    if (!qr || !qr->compiled || !s || !s->buffer) {
        if (s) Core_Collections_List_List_append(result, s);
        return result;
    }
    int offset = 0;
    int input_len = s->length;
    regmatch_t groups[MAX_GROUPS];
    while (offset <= input_len) {
        int err = regexec(&qr->r, s->buffer + offset, 1, groups, 0);
        if (err != 0) {
            // Append remaining tail and stop.
            int remain = input_len - offset;
            char* buf = (char*)GC_malloc(remain + 1);
            memcpy(buf, s->buffer + offset, remain);
            buf[remain] = '\0';
            String* tail = make_String(buf);
            Core_Collections_List_List_append(result, tail);
            break;
        }
        int m_start = (int)groups[0].rm_so;
        int m_end   = (int)groups[0].rm_eo;
        // Append the literal segment before this match.
        char* buf = (char*)GC_malloc(m_start + 1);
        memcpy(buf, s->buffer + offset, m_start);
        buf[m_start] = '\0';
        Core_Collections_List_List_append(result, make_String(buf));

        if (m_end == m_start) {
            // Zero-width match: skip one byte to avoid infinite loop.
            offset += m_start + 1;
        } else {
            offset += m_end;
        }
    }
    return result;
}
