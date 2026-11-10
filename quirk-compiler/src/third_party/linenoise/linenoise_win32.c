/* Windows stand-in for linenoise (native Windows port,
 * docs/WINDOWS-PORT.md): the REPL's line editor uses POSIX termios, so on
 * Windows lines are read plainly from the console (which already gives
 * basic editing and up-arrow history within a session). The history
 * functions keep their contract but store nothing. Built instead of
 * linenoise.c on Windows only. */
#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "linenoise.h"

char *linenoise(const char *prompt) {
    char buf[4096];
    fputs(prompt, stdout);
    fflush(stdout);
    if (!fgets(buf, sizeof buf, stdin)) return NULL;
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    char *out = (char *)malloc(n + 1);
    if (out) memcpy(out, buf, n + 1);
    return out;
}

void linenoiseFree(void *ptr) { free(ptr); }
int linenoiseHistoryAdd(const char *line) { (void)line; return 1; }
int linenoiseHistorySetMaxLen(int len) { (void)len; return 1; }
int linenoiseHistorySave(const char *filename) { (void)filename; return 0; }
int linenoiseHistoryLoad(const char *filename) { (void)filename; return 0; }
#endif
