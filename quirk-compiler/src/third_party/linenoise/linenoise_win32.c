/* Windows stand-in for linenoise (native Windows port,
 * docs/WINDOWS-PORT.md). linenoise.c needs POSIX termios; this is a small
 * console line editor instead: ReadConsoleInputW for keys, VT sequences
 * (Windows 10+) to redraw. Left / Right / Home / End, Backspace / Delete,
 * Ctrl-A / E / K / U / L, Up / Down history (persisted by
 * linenoiseHistorySave / Load). Ctrl-C clears the line, or ends input on
 * an empty one (like linenoise); Ctrl-D on an empty line / Ctrl-Z ends
 * input. When stdin isn't a console (piped, redirected) lines are read
 * plainly with fgets. Built instead of linenoise.c on Windows only. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "linenoise.h"

/* ---- history (UTF-8 lines, oldest first) ---- */
static char **hist = NULL;
static int hist_len = 0;
static int hist_max = 100;

int linenoiseHistoryAdd(const char *line) {
    if (hist_max <= 0 || !line) return 0;
    if (hist_len > 0 && strcmp(hist[hist_len - 1], line) == 0) return 0;
    if (!hist) {
        hist = (char **)calloc((size_t)hist_max, sizeof(char *));
        if (!hist) return 0;
    }
    char *copy = _strdup(line);
    if (!copy) return 0;
    if (hist_len == hist_max) {
        free(hist[0]);
        memmove(hist, hist + 1, sizeof(char *) * (size_t)(hist_max - 1));
        hist_len--;
    }
    hist[hist_len++] = copy;
    return 1;
}

int linenoiseHistorySetMaxLen(int len) {
    if (len < 1) return 0;
    char **n = (char **)calloc((size_t)len, sizeof(char *));
    if (!n) return 0;
    int keep = hist_len < len ? hist_len : len;
    for (int i = 0; i < hist_len - keep; i++) free(hist[i]);
    if (keep) memcpy(n, hist + (hist_len - keep), sizeof(char *) * (size_t)keep);
    free(hist);
    hist = n;
    hist_len = keep;
    hist_max = len;
    return 1;
}

int linenoiseHistorySave(const char *filename) {
    FILE *f = fopen(filename, "wb");
    if (!f) return -1;
    for (int i = 0; i < hist_len; i++) fprintf(f, "%s\n", hist[i]);
    fclose(f);
    return 0;
}

int linenoiseHistoryLoad(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return -1;
    char buf[4096];
    while (fgets(buf, sizeof buf, f)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
        linenoiseHistoryAdd(buf);
    }
    fclose(f);
    return 0;
}

void linenoiseFree(void *ptr) { free(ptr); }

/* ---- plain reading (not a console) ---- */
static char *read_plain(const char *prompt) {
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

/* ---- console editor (line kept as UTF-16) ---- */
#define MAXW 4096
typedef struct {
    HANDLE out;
    const wchar_t *prompt;
    wchar_t buf[MAXW];
    int len, pos;
} Edit;

static void put(HANDLE out, const wchar_t *s, int n) {
    DWORD w;
    if (n > 0) WriteConsoleW(out, s, (DWORD)n, &w, NULL);
}

static void refresh(Edit *e) {
    wchar_t tail[32];
    put(e->out, L"\r", 1);
    put(e->out, e->prompt, (int)wcslen(e->prompt));
    put(e->out, e->buf, e->len);
    put(e->out, L"\x1b[K", 3);
    if (e->len > e->pos) {
        int n = swprintf(tail, 32, L"\x1b[%dD", e->len - e->pos);
        put(e->out, tail, n);
    }
}

static void set_text(Edit *e, const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, e->buf, MAXW - 1);
    e->len = n > 0 ? n - 1 : 0;
    e->pos = e->len;
}

static char *to_utf8(const wchar_t *w, int n) {
    int bytes = WideCharToMultiByte(CP_UTF8, 0, w, n, NULL, 0, NULL, NULL);
    char *s = (char *)malloc((size_t)bytes + 1);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, n, s, bytes, NULL, NULL);
    s[bytes] = '\0';
    return s;
}

char *linenoise(const char *prompt) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD inMode, outMode;
    if (!GetConsoleMode(in, &inMode) || !GetConsoleMode(out, &outMode)) return read_plain(prompt);
    fflush(stdout);

    static Edit e;
    wchar_t wprompt[512];
    if (MultiByteToWideChar(CP_UTF8, 0, prompt, -1, wprompt, 512) <= 0) wprompt[0] = 0;
    e.out = out;
    e.prompt = wprompt;
    e.len = e.pos = 0;

    SetConsoleMode(out, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
    SetConsoleMode(in, (inMode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT))
                       | ENABLE_EXTENDED_FLAGS);
    refresh(&e);

    int hidx = hist_len;          /* hist_len = the line being typed */
    char *saved = NULL;           /* that line, while browsing history */
    char *result = NULL;
    int done = 0, eof = 0;
    while (!done) {
        INPUT_RECORD rec;
        DWORD got = 0;
        if (!ReadConsoleInputW(in, &rec, 1, &got) || got == 0) { eof = 1; break; }
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
        KEY_EVENT_RECORD *k = &rec.Event.KeyEvent;
        wchar_t ch = k->uChar.UnicodeChar;
        int ctrl = (k->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        for (WORD rep = 0; rep < (k->wRepeatCount ? k->wRepeatCount : 1) && !done; rep++) {
            switch (k->wVirtualKeyCode) {
            case VK_RETURN: done = 1; continue;
            case VK_BACK:
                if (e.pos > 0) {
                    memmove(e.buf + e.pos - 1, e.buf + e.pos, sizeof(wchar_t) * (size_t)(e.len - e.pos));
                    e.pos--; e.len--;
                }
                break;
            case VK_DELETE:
                if (e.pos < e.len) {
                    memmove(e.buf + e.pos, e.buf + e.pos + 1, sizeof(wchar_t) * (size_t)(e.len - e.pos - 1));
                    e.len--;
                }
                break;
            case VK_LEFT:  if (e.pos > 0) e.pos--; break;
            case VK_RIGHT: if (e.pos < e.len) e.pos++; break;
            case VK_HOME:  e.pos = 0; break;
            case VK_END:   e.pos = e.len; break;
            case VK_UP:
            case VK_DOWN: {
                int next = hidx + (k->wVirtualKeyCode == VK_UP ? -1 : 1);
                if (next < 0 || next > hist_len) break;
                if (hidx == hist_len) { free(saved); saved = to_utf8(e.buf, e.len); }
                hidx = next;
                set_text(&e, hidx == hist_len ? (saved ? saved : "") : hist[hidx]);
                break;
            }
            default:
                if (ctrl && (ch == 3)) {                     /* Ctrl-C */
                    if (e.len == 0) { eof = 1; done = 1; continue; }
                    e.len = e.pos = 0;
                } else if (ctrl && (ch == 4 || ch == 26)) {  /* Ctrl-D / Ctrl-Z */
                    if (e.len == 0) { eof = 1; done = 1; continue; }
                    if (ch == 4 && e.pos < e.len) {
                        memmove(e.buf + e.pos, e.buf + e.pos + 1, sizeof(wchar_t) * (size_t)(e.len - e.pos - 1));
                        e.len--;
                    }
                } else if (ctrl && ch == 1) e.pos = 0;       /* Ctrl-A */
                else if (ctrl && ch == 5) e.pos = e.len;     /* Ctrl-E */
                else if (ctrl && ch == 11) e.len = e.pos;    /* Ctrl-K */
                else if (ctrl && ch == 21) {                 /* Ctrl-U */
                    memmove(e.buf, e.buf + e.pos, sizeof(wchar_t) * (size_t)(e.len - e.pos));
                    e.len -= e.pos; e.pos = 0;
                } else if (ctrl && ch == 12) {               /* Ctrl-L */
                    put(out, L"\x1b[H\x1b[2J", 7);
                } else if (ch == L'\t') {
                    for (int i = 0; i < 4 && e.len < MAXW - 1; i++) {
                        memmove(e.buf + e.pos + 1, e.buf + e.pos, sizeof(wchar_t) * (size_t)(e.len - e.pos));
                        e.buf[e.pos++] = L' '; e.len++;
                    }
                } else if (ch >= 32 && e.len < MAXW - 1) {
                    memmove(e.buf + e.pos + 1, e.buf + e.pos, sizeof(wchar_t) * (size_t)(e.len - e.pos));
                    e.buf[e.pos++] = ch; e.len++;
                } else {
                    continue;
                }
            }
            refresh(&e);
        }
    }
    e.pos = e.len;
    refresh(&e);
    put(out, L"\r\n", 2);
    SetConsoleMode(in, inMode);
    SetConsoleMode(out, outMode);
    free(saved);
    if (!eof) result = to_utf8(e.buf, e.len);
    return result;
}
#endif
