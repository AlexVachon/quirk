// ===================================================
//  Debug — interactive breakpoint helper + line stepper.
//
//  Two entry points:
//    Debug_breakpoint(label)    — invoked from user code via
//                                 `debug.breakpoint("label")`. Always pauses
//                                 (unless skipped via env / `s` command).
//    Debug_step_hook(file, line)— emitted by the compiler at every
//                                 statement when `--debug` is passed. Only
//                                 pauses when step mode is armed or a
//                                 breakpoint matches.
//
//  The two share `debug_prompt` so the user gets the same qdb experience
//  whether they arrived via a `breakpoint()` call or a `--debug` step.
//
//  Naming: Debug_<name>  (Parser::computeModulePrefix turns
//  libs/debug/index.quirk into the `Debug_` linkage prefix).
//
//  Commands accepted at the (qdb) prompt:
//    c, continue        — resume; only stop again at a breakpoint
//    n, next            — step over (run until we're back at the same
//                         shadow-stack depth or shallower)
//    s, step            — step into (stop at the very next statement)
//    bt, backtrace      — print the current shadow stack
//    where              — print current file:line
//    b <file:line>      — add a breakpoint at file:line
//    b <line>           — add a breakpoint in the current file
//    b                  — list breakpoints
//    clear <file:line>  — remove that breakpoint
//    clear              — clear all breakpoints
//    skip               — disable every breakpoint() / step hook for the
//                         rest of this run (sticky)
//    p <name>           — print the current value of a local in this frame
//    locals             — list every local registered in this frame
//    q, quit            — abort the program (exit 1)
//    <blank>            — repeat the last navigation command (c/n/s)
//
//  Variable inspection is name-only — `p x` works, `p x + 1` does not.
//  Arbitrary expression eval would need a re-entrant codegen path with
//  access to the active scope's NamedValues; that's a tier-3 problem.
// ===================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"   // ShadowFrame lives here

// Stack storage + counter are defined in sys.c; we only consume them.
extern ShadowFrame quirk_shadow_stack[1024];
extern int quirk_shadow_sp;

// ---------------------------------------------------------------------------
//  JSON event mode — when QUIRK_DBG_JSON=1 (set by the VSCode adapter), qdb
//  emits one machine-parseable event per stderr line instead of the human-
//  friendly banner/prompt. Commands on stdin stay in the text format; no
//  reason to wrap simple verbs ("c", "n", "p x") in JSON.
//
//  Event shape (one line per event):
//     {"event":"stopped","reason":"step","file":"foo.quirk","line":12,"depth":2}
//     {"event":"local","name":"a","value":"10","type":"Int"}
//     {"event":"locals","items":[{"name":"a","value":"10","type":"Int"}, ...]}
//     {"event":"stack","frames":[{"name":"main","file":"foo.quirk","line":12}, ...]}
//     {"event":"breakpointSet","file":"foo.quirk","line":12}
//     {"event":"breakpointCleared","file":"foo.quirk","line":12}
//     {"event":"breakpoints","items":[{"file":"...","line":N}, ...]}
//     {"event":"message","text":"..."}    (info / errors not tied to anything else)
// ---------------------------------------------------------------------------
static int debug_json_mode = 0;
static int debug_json_checked = 0;
static int Debug__json_active(void) {
    if (!debug_json_checked) {
        const char* e = getenv("QUIRK_DBG_JSON");
        debug_json_mode = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        debug_json_checked = 1;
    }
    return debug_json_mode;
}

// Escape a C string into a JSON string literal (including the surrounding
// quotes). Handles backslashes, quotes, and the control chars we actually
// produce. Not full RFC-8259 — no UTF-8 surrogate handling — but more than
// enough for variable names and our generated value strings.
static void Debug__json_str(FILE* fp, const char* s) {
    fputc('"', fp);
    if (s) for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if      (c == '"')  fputs("\\\"", fp);
        else if (c == '\\') fputs("\\\\", fp);
        else if (c == '\n') fputs("\\n",  fp);
        else if (c == '\r') fputs("\\r",  fp);
        else if (c == '\t') fputs("\\t",  fp);
        else if (c < 0x20)  fprintf(fp, "\\u%04x", c);
        else                fputc(c, fp);
    }
    fputc('"', fp);
}

static const char* Debug__tag_name(int tag) {
    switch (tag) {
        case 0: return "Int";
        case 1: return "Int64";
        case 2: return "Double";
        case 3: return "Pointer";
        case 4: return "Bool";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------

// One global skip flag — once the user types `skip` at a prompt, every
// subsequent breakpoint() / step hook short-circuits silently. Useful when a
// breakpoint is in a tight loop.
static int debug_skip_all = 0;

typedef enum {
    STEP_OFF  = 0,   // free run; pause only when a breakpoint matches
    STEP_INTO = 1,   // pause at every step hook
    STEP_NEXT = 2,   // pause only when shadow_sp <= debug_step_sp
} StepMode;
static StepMode debug_step_mode = STEP_OFF;
static int debug_step_sp = 0;   // shadow stack depth captured for STEP_NEXT

// First Debug_step_hook call arms STEP_INTO so `quirk --debug foo.quirk`
// stops at the first user statement without the user having to do anything.
static int debug_first_hit = 1;

// Track the last command so a blank line repeats the previous navigation
// (gdb / pdb convention — saves a lot of typing while stepping).
static char debug_last_nav = '\0';

// Last (file, line) seen — used by `where`, `b <line>` (defaults to current
// file), and the prompt banner.
static char debug_last_file[256] = "<unknown>";
static int  debug_last_line = 0;

// Breakpoint table — small fixed array is plenty for interactive use.
#define DEBUG_MAX_BPS 256
typedef struct {
    char file[256];
    int  line;
} DebugBP;
static DebugBP debug_bps[DEBUG_MAX_BPS];
static int     debug_bp_count = 0;

// ---------------------------------------------------------------------------
//  Locals registry — populated by the compiler at every alloca site so the
//  `p <name>` and `locals` commands can read the current value back out.
//
//  Tag values match the encoder in VariableGen.hpp::emitDebugRegister:
//     0 = i32                1 = i64
//     2 = double             3 = pointer (String*/Any*/etc.)
//     4 = i1 (bool)          5 = anything else (printed via fallback)
//
//  `frame_depth` is the shadow-stack depth captured when the alloca was
//  registered. When the stack pops below that depth, the entry is stale
//  (the stack slot is dead memory). We prune lazily on every register call.
// ---------------------------------------------------------------------------
enum {
    DBG_TAG_I32     = 0,
    DBG_TAG_I64     = 1,
    DBG_TAG_DOUBLE  = 2,
    DBG_TAG_POINTER = 3,
    DBG_TAG_BOOL    = 4,
    DBG_TAG_OTHER   = 5,
};

typedef struct {
    const char* name;     // global string constant — points into the JIT module
    void*       addr;     // pointer to the alloca slot
    int         tag;
    int         frame_depth;
} DebugLocal;

#define DEBUG_MAX_LOCALS 1024
static DebugLocal debug_locals[DEBUG_MAX_LOCALS];
static int        debug_locals_count = 0;

// Forward decl — defined in runtime.c (unity build), prints/converts any
// boxed i8* to a String for display. Lets us format pointer locals without
// knowing whether they're String*, Any*, or tagged-int.
extern String* quirk_opaque_to_string(void* val);

static int Debug__should_skip(void);  // defined below; needed by register_local

static void debug_prune_dead_locals(void) {
    // Drop any local whose owning frame has already returned. The next time
    // someone registers under the same name we'd just shadow it anyway, but
    // pruning keeps `locals` output honest.
    int out = 0;
    for (int i = 0; i < debug_locals_count; i++) {
        if (debug_locals[i].frame_depth <= quirk_shadow_sp)
            debug_locals[out++] = debug_locals[i];
    }
    debug_locals_count = out;
}

void Debug_register_local(const char* name, void* addr, int tag) {
    if (Debug__should_skip()) return;
    debug_prune_dead_locals();
    // Replace an existing same-name entry at the current depth (re-binding /
    // reassignment with type change) — keeps the table from growing.
    for (int i = 0; i < debug_locals_count; i++) {
        if (debug_locals[i].frame_depth == quirk_shadow_sp &&
            debug_locals[i].name && name && strcmp(debug_locals[i].name, name) == 0) {
            debug_locals[i].addr = addr;
            debug_locals[i].tag  = tag;
            return;
        }
    }
    if (debug_locals_count >= DEBUG_MAX_LOCALS) {
        // One-shot warning — without this the user sees a partial locals
        // panel with no hint why some names are missing. Once is plenty;
        // emitting per drop would flood the debug console.
        static int warned = 0;
        if (!warned) {
            warned = 1;
            if (Debug__json_active())
                fprintf(stderr, "{\"event\":\"message\",\"text\":\"locals table full (%d) — some variables omitted\"}\n",
                        DEBUG_MAX_LOCALS);
            else
                fprintf(stderr, "  [debug] locals table full (%d) — some variables will not appear\n",
                        DEBUG_MAX_LOCALS);
        }
        return;
    }
    debug_locals[debug_locals_count].name        = name;
    debug_locals[debug_locals_count].addr        = addr;
    debug_locals[debug_locals_count].tag         = tag;
    debug_locals[debug_locals_count].frame_depth = quirk_shadow_sp;
    debug_locals_count++;
}

// Format a local's current value into `out` (size `out_sz`). Returns 0 if
// the entry is missing, 1 if formatted successfully. Shared by both the
// human pretty-printer and the JSON emitter so the two can never diverge.
static int debug_format_local_value(const DebugLocal* d, char* out, size_t out_sz) {
    if (!d || !d->addr) { snprintf(out, out_sz, "<no-addr>"); return 0; }
    switch (d->tag) {
        case DBG_TAG_I32:    snprintf(out, out_sz, "%d",   *(int32_t*)d->addr); break;
        case DBG_TAG_I64:    snprintf(out, out_sz, "%lld", (long long)*(int64_t*)d->addr); break;
        case DBG_TAG_DOUBLE: snprintf(out, out_sz, "%g",   *(double*)d->addr); break;
        case DBG_TAG_BOOL: {
            unsigned char b = *(unsigned char*)d->addr;
            snprintf(out, out_sz, "%s", (b & 1) ? "true" : "false");
            break;
        }
        case DBG_TAG_POINTER: {
            void* val = *(void**)d->addr;
            if (!val) { snprintf(out, out_sz, "null"); break; }
            String* s = quirk_opaque_to_string(val);
            snprintf(out, out_sz, "%s", (s && s->buffer) ? s->buffer : "<obj>");
            break;
        }
        default:
            snprintf(out, out_sz, "<opaque tag=%d addr=%p>", d->tag, d->addr);
    }
    return 1;
}

// Print one local. Returns 1 if found, 0 if not (so the caller can complain).
// Looks up the innermost frame first (mirrors lexical scoping).
static int debug_print_local(const char* name) {
    debug_prune_dead_locals();
    int best = -1;
    int best_depth = -1;
    for (int i = 0; i < debug_locals_count; i++) {
        if (!debug_locals[i].name) continue;
        if (strcmp(debug_locals[i].name, name) != 0) continue;
        if (debug_locals[i].frame_depth > best_depth) {
            best = i;
            best_depth = debug_locals[i].frame_depth;
        }
    }
    if (best < 0) return 0;
    DebugLocal* d = &debug_locals[best];
    char buf[256];
    debug_format_local_value(d, buf, sizeof(buf));
    if (Debug__json_active()) {
        fprintf(stderr, "{\"event\":\"local\",\"name\":");
        Debug__json_str(stderr, name);
        fprintf(stderr, ",\"value\":");
        Debug__json_str(stderr, buf);
        fprintf(stderr, ",\"type\":\"%s\"}\n", Debug__tag_name(d->tag));
    } else {
        fprintf(stderr, "  %s = %s\n", name, buf);
    }
    return 1;
}

static void debug_list_locals(void) {
    debug_prune_dead_locals();
    int top_depth = quirk_shadow_sp;
    if (Debug__json_active()) {
        // Build a single JSON array so the adapter parses one event for
        // the whole frame instead of N individual `local` events.
        fprintf(stderr, "{\"event\":\"locals\",\"items\":[");
        int first = 1;
        for (int i = 0; i < debug_locals_count; i++) {
            DebugLocal* d = &debug_locals[i];
            if (d->frame_depth != top_depth || !d->name) continue;
            char buf[256];
            debug_format_local_value(d, buf, sizeof(buf));
            if (!first) fputc(',', stderr);
            first = 0;
            fputs("{\"name\":", stderr);
            Debug__json_str(stderr, d->name);
            fputs(",\"value\":", stderr);
            Debug__json_str(stderr, buf);
            fprintf(stderr, ",\"type\":\"%s\"}", Debug__tag_name(d->tag));
        }
        fprintf(stderr, "]}\n");
        return;
    }
    int shown = 0;
    for (int i = 0; i < debug_locals_count; i++) {
        if (debug_locals[i].frame_depth == top_depth && debug_locals[i].name) {
            debug_print_local(debug_locals[i].name);
            shown++;
        }
    }
    if (!shown) fprintf(stderr, "  (no locals registered in this frame)\n");
}

// ---------------------------------------------------------------------------
//  Env var: QUIRK_DEBUG_SKIP=1 turns every breakpoint into a no-op for the
//  whole process. The `quirk test` runner sets this so a stray
//  `debug.breakpoint()` in source can't hang the suite.
// ---------------------------------------------------------------------------
static int debug_env_checked = 0;
static int debug_env_skip = 0;
static int Debug__should_skip(void) {
    if (debug_skip_all) return 1;
    if (!debug_env_checked) {
        const char* e = getenv("QUIRK_DEBUG_SKIP");
        debug_env_skip = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        debug_env_checked = 1;
    }
    return debug_env_skip;
}

// ---------------------------------------------------------------------------
//  Breakpoint table helpers
// ---------------------------------------------------------------------------

// Compare files by basename — the compiler hands us paths like
// "tests/foo.quirk" while the user usually types `b foo.quirk:12`. We match
// on the trailing component so both spellings work.
static const char* basename_only(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int debug_file_match(const char* a, const char* b) {
    if (strcmp(a, b) == 0) return 1;
    return strcmp(basename_only(a), basename_only(b)) == 0;
}

static int debug_match_bp(const char* file, int line) {
    for (int i = 0; i < debug_bp_count; i++) {
        if (debug_bps[i].line == line && debug_file_match(debug_bps[i].file, file))
            return 1;
    }
    return 0;
}

static void debug_add_bp(const char* file, int line) {
    if (debug_bp_count >= DEBUG_MAX_BPS) {
        if (Debug__json_active())
            fprintf(stderr, "{\"event\":\"message\",\"text\":\"breakpoint table full\"}\n");
        else
            fprintf(stderr, "  breakpoint table full (%d)\n", DEBUG_MAX_BPS);
        return;
    }
    if (debug_match_bp(file, line)) {
        if (Debug__json_active()) {
            fprintf(stderr, "{\"event\":\"breakpointSet\",\"file\":");
            Debug__json_str(stderr, file);
            fprintf(stderr, ",\"line\":%d,\"duplicate\":true}\n", line);
        } else {
            fprintf(stderr, "  already set: %s:%d\n", file, line);
        }
        return;
    }
    strncpy(debug_bps[debug_bp_count].file, file,
            sizeof(debug_bps[debug_bp_count].file) - 1);
    debug_bps[debug_bp_count].file[sizeof(debug_bps[debug_bp_count].file) - 1] = '\0';
    debug_bps[debug_bp_count].line = line;
    debug_bp_count++;
    if (Debug__json_active()) {
        fprintf(stderr, "{\"event\":\"breakpointSet\",\"file\":");
        Debug__json_str(stderr, file);
        fprintf(stderr, ",\"line\":%d}\n", line);
    } else {
        fprintf(stderr, "  + %s:%d\n", file, line);
    }
}

static void debug_clear_bp(const char* file, int line) {
    int removed = 0;
    for (int i = 0; i < debug_bp_count; ) {
        if (debug_bps[i].line == line && debug_file_match(debug_bps[i].file, file)) {
            debug_bps[i] = debug_bps[debug_bp_count - 1];
            debug_bp_count--;
            removed++;
        } else {
            i++;
        }
    }
    if (Debug__json_active()) {
        fprintf(stderr, "{\"event\":\"breakpointCleared\",\"file\":");
        Debug__json_str(stderr, file);
        fprintf(stderr, ",\"line\":%d,\"removed\":%d}\n", line, removed);
    } else {
        fprintf(stderr, removed ? "  - %s:%d\n" : "  no match: %s:%d\n", file, line);
    }
}

static void debug_list_bps(void) {
    if (Debug__json_active()) {
        fprintf(stderr, "{\"event\":\"breakpoints\",\"items\":[");
        for (int i = 0; i < debug_bp_count; i++) {
            if (i) fputc(',', stderr);
            fputs("{\"file\":", stderr);
            Debug__json_str(stderr, debug_bps[i].file);
            fprintf(stderr, ",\"line\":%d}", debug_bps[i].line);
        }
        fprintf(stderr, "]}\n");
        return;
    }
    if (debug_bp_count == 0) {
        fprintf(stderr, "  (no breakpoints)\n");
        return;
    }
    for (int i = 0; i < debug_bp_count; i++)
        fprintf(stderr, "    [%d] %s:%d\n", i, debug_bps[i].file, debug_bps[i].line);
}

// ---------------------------------------------------------------------------
//  Banner + backtrace
// ---------------------------------------------------------------------------

static void Debug__emit_stopped(const char* reason, const char* file, int line, const char* label_buf) {
    if (Debug__json_active()) {
        fprintf(stderr, "{\"event\":\"stopped\",\"reason\":\"%s\",\"file\":", reason);
        Debug__json_str(stderr, file ? file : "");
        fprintf(stderr, ",\"line\":%d,\"depth\":%d", line, quirk_shadow_sp);
        if (label_buf && *label_buf) {
            fprintf(stderr, ",\"label\":");
            Debug__json_str(stderr, label_buf);
        }
        fprintf(stderr, "}\n");
        return;
    }
    fprintf(stderr, "\n\033[1;33m── pause ──\033[0m\n");
    if (label_buf && *label_buf)
        fprintf(stderr, "  label: %s\n", label_buf);
    if (file && line > 0)
        fprintf(stderr, "  at %s:%d\n", file, line);
    if (quirk_shadow_sp > 0) {
        ShadowFrame top = quirk_shadow_stack[quirk_shadow_sp - 1];
        const char* fn = top.func_name ? top.func_name : "?";
        fprintf(stderr, "  in %s()  (depth=%d)\n", fn, quirk_shadow_sp);
    }
}

static void Debug__print_backtrace(void) {
    if (Debug__json_active()) {
        fprintf(stderr, "{\"event\":\"stack\",\"frames\":[");
        // DAP wants most-recent-first, frame 0 = top. We iterate the shadow
        // stack in reverse to match that convention so the adapter doesn't
        // have to flip the array.
        int first = 1;
        for (int i = quirk_shadow_sp - 1; i >= 0; i--) {
            if (!first) fputc(',', stderr);
            first = 0;
            fputs("{\"name\":", stderr);
            Debug__json_str(stderr,
                quirk_shadow_stack[i].func_name ? quirk_shadow_stack[i].func_name : "?");
            fputs(",\"file\":", stderr);
            Debug__json_str(stderr,
                quirk_shadow_stack[i].file_name ? quirk_shadow_stack[i].file_name : "?");
            fprintf(stderr, ",\"line\":%d}", quirk_shadow_stack[i].line);
        }
        fprintf(stderr, "]}\n");
        return;
    }
    fprintf(stderr, "  Traceback (most recent call last):\n");
    for (int i = 0; i < quirk_shadow_sp; i++) {
        const char* fn = quirk_shadow_stack[i].func_name ? quirk_shadow_stack[i].func_name : "?";
        const char* fl = quirk_shadow_stack[i].file_name ? quirk_shadow_stack[i].file_name : "?";
        int ln = quirk_shadow_stack[i].line;
        if (ln > 0)
            fprintf(stderr, "    [%d] %s  (%s:%d)\n", i, fn, fl, ln);
        else
            fprintf(stderr, "    [%d] %s  (%s)\n", i, fn, fl);
    }
}

// ---------------------------------------------------------------------------
//  Shared prompt loop
//
//  Returns to the caller once the user picks a navigation command (c/n/s)
//  or hits EOF on stdin. `q` exits the process.
// ---------------------------------------------------------------------------

static void debug_prompt(const char* file, int line, const char* label_buf, const char* reason) {
    Debug__emit_stopped(reason ? reason : "pause", file, line, label_buf);
    int json = Debug__json_active();
    if (!json) {
        fprintf(stderr, "  \033[2m[c]ontinue [n]ext [s]tep   p <name>/locals   b[t]/where   b/clear   skip/q\033[0m\n");
    }

    char buf[512];
    for (;;) {
        if (!json) fprintf(stderr, "(qdb) ");
        fflush(stderr);
        if (!fgets(buf, sizeof(buf), stdin)) {
            // EOF (`echo c |` style) — treat as continue.
            fputc('\n', stderr);
            debug_step_mode = STEP_OFF;
            return;
        }
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';

        // Blank: repeat last navigation (gdb/pdb convention).
        if (n == 0) {
            if (debug_last_nav == 'c' || debug_last_nav == 'n' || debug_last_nav == 's')
                buf[0] = debug_last_nav, buf[1] = '\0', n = 1;
            else { continue; }
        }

        // continue
        if (strcmp(buf, "c") == 0 || strcmp(buf, "continue") == 0) {
            debug_step_mode = STEP_OFF;
            debug_last_nav = 'c';
            return;
        }
        // next (step over)
        if (strcmp(buf, "n") == 0 || strcmp(buf, "next") == 0) {
            debug_step_mode = STEP_NEXT;
            debug_step_sp   = quirk_shadow_sp;
            debug_last_nav = 'n';
            return;
        }
        // step (step into)
        if (strcmp(buf, "s") == 0 || strcmp(buf, "step") == 0) {
            debug_step_mode = STEP_INTO;
            debug_last_nav = 's';
            return;
        }
        // quit
        if (strcmp(buf, "q") == 0 || strcmp(buf, "quit") == 0) {
            if (!Debug__json_active())
                fprintf(stderr, "  aborted at qdb\n");
            exit(1);
        }
        // skip-all
        if (strcmp(buf, "skip") == 0) {
            debug_skip_all = 1;
            debug_step_mode = STEP_OFF;
            return;
        }
        // backtrace
        if (strcmp(buf, "bt") == 0 || strcmp(buf, "backtrace") == 0) {
            Debug__print_backtrace();
            continue;
        }
        // p <name> — print a single local. Bare `p` complains so the user
        // doesn't confuse it with the (not-implemented) expression eval.
        if (buf[0] == 'p' && (buf[1] == '\0' || buf[1] == ' ')) {
            const char* arg = (buf[1] == '\0') ? "" : buf + 2;
            while (*arg == ' ') arg++;
            if (*arg == '\0') {
                if (Debug__json_active())
                    fprintf(stderr, "{\"event\":\"message\",\"text\":\"p requires a name\"}\n");
                else
                    fprintf(stderr, "  p <name>   (expression eval not supported — name lookup only)\n");
                continue;
            }
            if (!debug_print_local(arg)) {
                if (Debug__json_active()) {
                    fprintf(stderr, "{\"event\":\"local\",\"name\":");
                    Debug__json_str(stderr, arg);
                    fprintf(stderr, ",\"missing\":true}\n");
                } else {
                    fprintf(stderr, "  no local named '%s' in this frame\n", arg);
                }
            }
            continue;
        }
        // locals — list every registered local in the innermost frame.
        if (strcmp(buf, "locals") == 0 || strcmp(buf, "l") == 0) {
            debug_list_locals();
            continue;
        }
        // where
        if (strcmp(buf, "where") == 0 || strcmp(buf, "w") == 0) {
            if (Debug__json_active()) {
                fprintf(stderr, "{\"event\":\"where\",\"file\":");
                Debug__json_str(stderr, file ? file : "");
                fprintf(stderr, ",\"line\":%d}\n", line);
            } else if (file && line > 0) {
                fprintf(stderr, "  %s:%d\n", file, line);
            } else {
                fprintf(stderr, "  (unknown location)\n");
            }
            continue;
        }
        // b (list / add)
        if (buf[0] == 'b' && (buf[1] == '\0' || buf[1] == ' ')) {
            const char* arg = (buf[1] == '\0') ? "" : buf + 2;
            while (*arg == ' ') arg++;
            if (*arg == '\0') { debug_list_bps(); continue; }
            const char* colon = strchr(arg, ':');
            if (colon) {
                // file:line
                char fbuf[256] = {0};
                size_t flen = (size_t)(colon - arg);
                if (flen >= sizeof(fbuf)) flen = sizeof(fbuf) - 1;
                memcpy(fbuf, arg, flen);
                int ln = atoi(colon + 1);
                if (ln <= 0) { fprintf(stderr, "  bad line: %s\n", colon + 1); continue; }
                debug_add_bp(fbuf, ln);
            } else {
                // bare line → current file
                int ln = atoi(arg);
                if (ln <= 0) { fprintf(stderr, "  bad line: %s\n", arg); continue; }
                debug_add_bp(debug_last_file, ln);
            }
            continue;
        }
        // clear (all / one)
        if (strcmp(buf, "clear") == 0) {
            debug_bp_count = 0;
            fprintf(stderr, "  cleared all\n");
            continue;
        }
        if (strncmp(buf, "clear ", 6) == 0) {
            const char* arg = buf + 6;
            while (*arg == ' ') arg++;
            const char* colon = strchr(arg, ':');
            if (colon) {
                char fbuf[256] = {0};
                size_t flen = (size_t)(colon - arg);
                if (flen >= sizeof(fbuf)) flen = sizeof(fbuf) - 1;
                memcpy(fbuf, arg, flen);
                int ln = atoi(colon + 1);
                if (ln > 0) debug_clear_bp(fbuf, ln);
                else fprintf(stderr, "  bad line: %s\n", colon + 1);
            } else {
                int ln = atoi(arg);
                if (ln > 0) debug_clear_bp(debug_last_file, ln);
                else fprintf(stderr, "  bad line: %s\n", arg);
            }
            continue;
        }

        if (Debug__json_active()) {
            fprintf(stderr, "{\"event\":\"message\",\"text\":\"unknown command\"}\n");
        } else {
            fprintf(stderr, "  ?  c | n | s | p <name> | locals | bt | where | b [file:]line | clear [file:]line | skip | q\n");
        }
    }
}

// ---------------------------------------------------------------------------
//  Public entry points
// ---------------------------------------------------------------------------

// Per-statement step hook emitted by the compiler when --debug is set.
void Debug_step_hook(const char* file, int line) {
    if (Debug__should_skip()) return;

    if (file) {
        strncpy(debug_last_file, file, sizeof(debug_last_file) - 1);
        debug_last_file[sizeof(debug_last_file) - 1] = '\0';
    }
    debug_last_line = line;

    // First hook call ever: arm STEP_INTO so the user breaks at the first
    // statement of main without having to set a breakpoint up-front.
    if (debug_first_hit) {
        debug_first_hit = 0;
        debug_step_mode = STEP_INTO;
    }

    // Distinguish step-derived stops from breakpoint hits so the DAP
    // adapter can show the right `reason` on the Stopped event. (VSCode's
    // UI treats them differently in the call-stack panel.)
    int stop = 0;
    const char* reason = "step";
    switch (debug_step_mode) {
        case STEP_INTO: stop = 1; reason = "step"; break;
        case STEP_NEXT: stop = (quirk_shadow_sp <= debug_step_sp); reason = "step"; break;
        case STEP_OFF:  stop = 0; break;
    }
    if (!stop && debug_match_bp(file, line)) { stop = 1; reason = "breakpoint"; }
    if (!stop) return;

    debug_prompt(file ? file : "<unknown>", line, "", reason);
}

// Exposed to Quirk as `debug.breakpoint(label)`. The Quirk-side wrapper
// supplies "" when the caller omits the label; we still accept null in case
// a direct extern caller passes through.
void Debug_breakpoint(String* label) {
    if (Debug__should_skip()) return;
    const char* label_buf = (label && label->buffer) ? label->buffer : "";

    // Pull the current location off the shadow stack so the banner says
    // something useful even without --debug step context.
    const char* file = debug_last_file;
    int line = debug_last_line;
    if (quirk_shadow_sp > 0) {
        ShadowFrame top = quirk_shadow_stack[quirk_shadow_sp - 1];
        if (top.file_name) file = top.file_name;
        if (top.line > 0)  line = top.line;
    }

    debug_prompt(file, line, label_buf, "userBreakpoint");
}
