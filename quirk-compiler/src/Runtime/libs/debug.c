// ===================================================
//  Debug — interactive breakpoint helper.
//
//  Naming: Debug_<name>  (Parser::computeModulePrefix turns
//  libs/debug/index.quirk into the `Debug_` linkage prefix).
//
//  The breakpoint loop reads from stdin so it works under a TTY but
//  also under piped input (test harnesses just send `c\n`). Commands:
//    c, continue   — resume execution
//    q, quit       — abort the program (exit 1)
//    bt, backtrace — print the current shadow stack
//    s, skip       — set a global flag that silently skips every
//                    breakpoint() call for the rest of the run
//    <anything>    — re-prompt
//
//  This is the simplest interactive debugger that buys something over
//  print() — you get the location, the label, and a backtrace without
//  recompiling. No expression-eval yet (that would need a sub-compile
//  with access to the current scope's NamedValues).
// ===================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../types.h"   // ShadowFrame lives here

// Stack storage + counter are defined in sys.c; we only consume them.
extern ShadowFrame quirk_shadow_stack[1024];
extern int quirk_shadow_sp;

// One global skip flag — once the user types `s` at a breakpoint, every
// subsequent breakpoint() call short-circuits and prints nothing. Useful
// when a breakpoint is in a tight loop.
static int debug_skip_all = 0;

static void Debug__print_banner(const char* label_buf) {
    fprintf(stderr, "\n\033[1;33m── breakpoint hit ──\033[0m\n");
    if (label_buf && *label_buf) {
        fprintf(stderr, "  label: %s\n", label_buf);
    }
    if (quirk_shadow_sp > 0) {
        ShadowFrame top = quirk_shadow_stack[quirk_shadow_sp - 1];
        const char* fn = top.func_name ? top.func_name : "?";
        const char* fl = top.file_name ? top.file_name : "?";
        if (top.line > 0)
            fprintf(stderr, "  at %s  (%s:%d)\n", fn, fl, top.line);
        else
            fprintf(stderr, "  at %s  (%s)\n", fn, fl);
    }
    fprintf(stderr, "  \033[2m[c]ontinue  [q]uit  [bt]acktrace  [s]kip-rest\033[0m\n");
}

static void Debug__print_backtrace(void) {
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

// Honour the QUIRK_DEBUG_SKIP env var so CI / production runs can keep
// `debug.breakpoint()` calls in the source without ever pausing. Checked
// on first hit; the result is sticky for the rest of the process.
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

// Exposed to Quirk as `debug.breakpoint(label)`. The Quirk-side wrapper
// supplies "" when the caller omits the label; we still accept null in
// case a direct extern caller passes through.
void Debug_breakpoint(String* label) {
    if (Debug__should_skip()) return;
    const char* label_buf = (label && label->buffer) ? label->buffer : "";

    Debug__print_banner(label_buf);

    char line[256];
    for (;;) {
        fprintf(stderr, "(qdb) ");
        fflush(stderr);
        if (!fgets(line, sizeof(line), stdin)) {
            // EOF — treat as continue. Lets piped input like `echo c |`
            // exit cleanly.
            fputc('\n', stderr);
            return;
        }
        // Strip trailing newline.
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        if (n == 0 || strcmp(line, "c") == 0 || strcmp(line, "continue") == 0) {
            return;
        }
        if (strcmp(line, "q") == 0 || strcmp(line, "quit") == 0) {
            fprintf(stderr, "  aborted at breakpoint\n");
            exit(1);
        }
        if (strcmp(line, "bt") == 0 || strcmp(line, "backtrace") == 0) {
            Debug__print_backtrace();
            continue;
        }
        if (strcmp(line, "s") == 0 || strcmp(line, "skip") == 0) {
            debug_skip_all = 1;
            return;
        }
        fprintf(stderr, "  ?  c | q | bt | s\n");
    }
}
