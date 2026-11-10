// Windows implementation of the Process_* runtime (packages/process),
// included by process.c on _WIN32 instead of the POSIX code. Same entry
// points and conventions:
//   - a "pid" is the process HANDLE, a "fd" is a pipe HANDLE (as Int);
//   - exit status: >= 0 the exit code, -N "killed by signal N"
//     (kill / terminate end the process with exit code -N, so a killed
//     program reads back the same as on Linux), PROC_RUNNING on timeout;
//   - every wait releases the task lock, and in a task stays cancellable.
// communicate() reads stdout / stderr on their own threads (anonymous
// pipes can't be polled on Windows); those threads aren't GC-registered,
// so their buffers live on the Win32 heap until copied out.
#include <stdio.h>
#include <string.h>
#include <gc.h>
#include "../types.h"

#define PROC_RUNNING (-1000)

void* quirk_int_to_opaque(int64_t v);   // runtime.c

static List* proc__new_list(void) {
    List* l = (List*)quirk_alloc(QT_LIST, sizeof(List));
    Core_Collections_List_List___init(l);
    return l;
}
static void proc__push_int(List* l, int64_t v) { Core_Collections_List_List_append(l, quirk_int_to_opaque(v)); }

static int proc__cancel_pending(void) {
    QuirkTask* me = (QuirkTask*)quirk_ts->task;
    return me && me->cancel_requested && !me->cancel_raised;
}

// Win32 error -> the errno the Quirk side understands (2 = ENOENT gives
// FileNotFoundError).
static int proc__errno(DWORD e) {
    switch (e) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_BAD_PATHNAME: return 2;
        case ERROR_ACCESS_DENIED: return 13;
        case ERROR_DIRECTORY: return 20;
        default: return 22;
    }
}

// ----- command line / environment -----

typedef struct { char* p; size_t len, cap; } WBuf;   // Win32-heap buffer
static void wbuf_add(WBuf* b, const char* s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char* np = b->p ? (char*)HeapReAlloc(GetProcessHeap(), 0, b->p, cap)
                        : (char*)HeapAlloc(GetProcessHeap(), 0, cap);
        if (!np) return;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}
static void wbuf_free(WBuf* b) { if (b->p) HeapFree(GetProcessHeap(), 0, b->p); b->p = NULL; b->len = b->cap = 0; }

// One argument for CreateProcess (the MSVC CRT's command-line rules).
static void proc__quote(WBuf* b, const char* a) {
    if (*a && !strpbrk(a, " \t\n\v\"")) { wbuf_add(b, a, strlen(a)); return; }
    wbuf_add(b, "\"", 1);
    size_t bs = 0;
    for (const char* c = a; *c; c++) {
        if (*c == '\\') { bs++; continue; }
        if (*c == '"') { for (size_t i = 0; i < bs * 2 + 1; i++) wbuf_add(b, "\\", 1); wbuf_add(b, "\"", 1); bs = 0; continue; }
        for (size_t i = 0; i < bs; i++) wbuf_add(b, "\\", 1);
        bs = 0;
        wbuf_add(b, c, 1);
    }
    for (size_t i = 0; i < bs * 2; i++) wbuf_add(b, "\\", 1);
    wbuf_add(b, "\"", 1);
}

static const char* proc__str(void* v) {
    String* s = quirk_ensure_string((String*)v);
    char* c = make_safe_cstr(s);
    return c ? c : "";
}

// "K=V\0K=V\0\0": `env` entries over (or instead of) this process's.
static char* proc__env_block(List* env, int inherit, WBuf* out) {
    int64_t n = env ? env->size : 0;
    if (inherit) {
        char* cur = GetEnvironmentStringsA();
        for (char* e = cur; e && *e; e += strlen(e) + 1) {
            const char* eq = strchr(e + 1, '=');      // "=C:=C:\\" entries start with '='
            size_t klen = eq ? (size_t)(eq - e) : strlen(e);
            int overridden = 0;
            for (int64_t j = 0; j < n && !overridden; j++) {
                const char* x = proc__str(Core_Collections_List_List___get(env, j));
                overridden = _strnicmp(x, e, klen) == 0 && x[klen] == '=';
            }
            if (!overridden) wbuf_add(out, e, strlen(e) + 1);
        }
        if (cur) FreeEnvironmentStringsA(cur);
    }
    for (int64_t j = 0; j < n; j++) {
        const char* x = proc__str(Core_Collections_List_List___get(env, j));
        wbuf_add(out, x, strlen(x) + 1);
    }
    wbuf_add(out, "\0", 1);
    return out->p;
}

// ----- spawn -----

List* Process_spawn_raw(List* argv, String* cwd, List* env, int64_t inherit_env,
                        int64_t in_mode, int64_t out_mode, int64_t err_mode) {
    List* res = proc__new_list();
    if (!argv || argv->size == 0) { proc__push_int(res, -1); proc__push_int(res, 22); return res; }
    WBuf cmd = {0}, envb = {0};
    for (int64_t i = 0; i < argv->size; i++) {
        if (i) wbuf_add(&cmd, " ", 1);
        proc__quote(&cmd, proc__str(Core_Collections_List_List___get(argv, i)));
    }
    char* envp = NULL;
    if (env && (env->size > 0 || !inherit_env)) envp = proc__env_block(env, (int)inherit_env, &envb);
    const char* dir = cwd ? make_safe_cstr(cwd) : NULL;
    if (dir && !*dir) dir = NULL;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL, errR = NULL, errW = NULL, nul = INVALID_HANDLE_VALUE;
    int ok = 1;
    if (in_mode == 1)  { ok = ok && CreatePipe(&inR, &inW, &sa, 0); if (ok) SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0); }
    if (out_mode == 1) { ok = ok && CreatePipe(&outR, &outW, &sa, 0); if (ok) SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0); }
    if (err_mode == 1) { ok = ok && CreatePipe(&errR, &errW, &sa, 0); if (ok) SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0); }
    if (in_mode == 2 || out_mode == 2 || err_mode == 2)
        nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = in_mode == 1 ? inR : in_mode == 2 ? nul : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_mode == 1 ? outW : out_mode == 2 ? nul : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = err_mode == 1 ? errW : err_mode == 2 ? nul : err_mode == 3 ? si.hStdOutput
                                         : GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    DWORD err = 0;
    if (ok) {
        quirk_gil_release();
        ok = CreateProcessA(NULL, cmd.p, NULL, NULL, TRUE, 0, envp, dir, &si, &pi);
        if (!ok) err = GetLastError();
        quirk_gil_acquire();
    } else {
        err = GetLastError();
    }
    // The child's ends are the child's now.
    if (inR) CloseHandle(inR);
    if (outW) CloseHandle(outW);
    if (errW) CloseHandle(errW);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    wbuf_free(&cmd);
    wbuf_free(&envb);
    if (!ok) {
        if (inW) CloseHandle(inW);
        if (outR) CloseHandle(outR);
        if (errR) CloseHandle(errR);
        proc__push_int(res, -1);
        proc__push_int(res, proc__errno(err));
        return res;
    }
    CloseHandle(pi.hThread);
    proc__push_int(res, (int64_t)(intptr_t)pi.hProcess);
    proc__push_int(res, inW ? (int64_t)(intptr_t)inW : -1);
    proc__push_int(res, outR ? (int64_t)(intptr_t)outR : -1);
    proc__push_int(res, errR ? (int64_t)(intptr_t)errR : -1);
    return res;
}

String* Process_strerror(int64_t err) {
    return make_String(strerror((int)err));
}

// ----- waiting / signals -----

// Wait up to `ms` (-1 forever) with the lock released; cancel-aware in
// a task. 1 = it exited, 0 = timed out. A pending cancel kills the child
// and raises CancelledError.
static int proc__wait_handle(HANDLE h, int64_t ms) {
    int in_task = quirk_ts->task != NULL;
    int64_t left = ms;
    for (;;) {
        DWORD slice = (ms < 0 || left > 20) ? (in_task ? 20 : (ms < 0 ? INFINITE : (DWORD)left)) : (DWORD)left;
        quirk_gil_release();
        DWORD r = WaitForSingleObject(h, slice);
        quirk_gil_acquire();
        if (r == WAIT_OBJECT_0) return 1;
        if (proc__cancel_pending()) {
            TerminateProcess(h, (UINT)-9);
            WaitForSingleObject(h, INFINITE);
            quirk_task_check_cancel();
        }
        if (ms >= 0) {
            left -= slice;
            if (left <= 0) return 0;
        }
    }
}

static int64_t proc__exit_code(HANDLE h) {
    DWORD code = 0;
    if (!GetExitCodeProcess(h, &code)) return -1;
    return (int64_t)(int32_t)code;     // -9 / -15 from kill / terminate read back as such
}

int64_t Process_wait_raw(int64_t pid, int64_t timeout_ms) {
    HANDLE h = (HANDLE)(intptr_t)pid;
    if (!h || pid <= 0) return -1;
    if (!proc__wait_handle(h, timeout_ms)) return PROC_RUNNING;
    return proc__exit_code(h);
}

// No signals on Windows: every one ends the process, with exit code -sig.
int64_t Process_kill_raw(int64_t pid, int64_t sig) {
    HANDLE h = (HANDLE)(intptr_t)pid;
    if (!h || pid <= 0) return -1;
    return TerminateProcess(h, (UINT)(-(int)sig)) ? 0 : -1;
}

void Process_close_raw(int64_t fd) {
    if (fd > 0) CloseHandle((HANDLE)(intptr_t)fd);
}

// ----- pipes -----

// Blocking read with the lock released; 0 at EOF / broken pipe.
static DWORD proc__read(HANDLE h, char* buf, DWORD n) {
    DWORD got = 0;
    quirk_gil_release();
    BOOL ok = ReadFile(h, buf, n, &got, NULL);
    quirk_gil_acquire();
    return ok ? got : 0;
}

String* Process_read_raw(int64_t fd, int64_t max) {
    if (fd <= 0 || max <= 0) return make_String("");
    char* buf = (char*)GC_malloc_atomic((size_t)max + 1);
    DWORD n = proc__read((HANDLE)(intptr_t)fd, buf, (DWORD)max);
    if (n == 0) return make_String("");
    buf[n] = '\0';
    return make_String_taking_ownership(buf);
}

List* Process_read_line_raw(int64_t fd) {
    List* res = proc__new_list();
    size_t cap = 128, len = 0;
    char* line = (char*)GC_malloc_atomic(cap);
    int got = 0;
    for (;;) {
        char c;
        if (fd <= 0 || proc__read((HANDLE)(intptr_t)fd, &c, 1) == 0) break;
        got = 1;
        if (c == '\n') break;
        if (len + 2 > cap) {
            char* grown = (char*)GC_malloc_atomic(cap * 2);
            memcpy(grown, line, len);
            line = grown;
            cap *= 2;
        }
        line[len++] = c;
    }
    if (len && line[len - 1] == '\r') len--;
    line[len] = '\0';
    Core_Collections_List_List_append(res, make_String_taking_ownership(line));
    proc__push_int(res, got);
    return res;
}

int64_t Process_write_raw(int64_t fd, String* data) {
    if (fd <= 0 || !data || !data->buffer) return -1;
    const char* p = data->buffer;
    size_t left = (size_t)data->length, done = 0;
    while (left > 0) {
        DWORD w = 0;
        quirk_gil_release();
        BOOL ok = WriteFile((HANDLE)(intptr_t)fd, p + done, (DWORD)left, &w, NULL);
        quirk_gil_acquire();
        if (!ok) return -1;
        done += w;
        left -= w;
    }
    return (int64_t)done;
}

// ----- communicate -----

typedef struct { HANDLE h; WBuf buf; const char* in; size_t in_len; } ProcIO;

static DWORD WINAPI proc__reader(LPVOID arg) {
    ProcIO* io = (ProcIO*)arg;
    char chunk[65536];
    DWORD n;
    while (ReadFile(io->h, chunk, sizeof chunk, &n, NULL) && n > 0) wbuf_add(&io->buf, chunk, n);
    return 0;
}

static DWORD WINAPI proc__writer(LPVOID arg) {
    ProcIO* io = (ProcIO*)arg;
    size_t done = 0;
    while (done < io->in_len) {
        DWORD w = 0;
        if (!WriteFile(io->h, io->in + done, (DWORD)(io->in_len - done), &w, NULL)) break;
        done += w;
    }
    CloseHandle(io->h);
    return 0;
}

static String* proc__wbuf_string(WBuf* b) {
    String* s = make_String(b->p ? b->p : "");
    wbuf_free(b);
    return s;
}

List* Process_communicate_raw(int64_t pid, int64_t in_fd, int64_t out_fd, int64_t err_fd,
                              String* input, int64_t timeout_ms) {
    List* res = proc__new_list();
    HANDLE proc = (HANDLE)(intptr_t)pid;
    ProcIO in = {0}, out = {0}, err = {0};
    HANDLE threads[3];
    int nt = 0;
    // Input: copied to the Win32 heap (the writer thread can't touch GC memory safely).
    char* input_copy = NULL;
    if (in_fd > 0) {
        size_t len = (input && input->buffer) ? (size_t)input->length : 0;
        if (len == 0) {
            CloseHandle((HANDLE)(intptr_t)in_fd);
        } else {
            input_copy = (char*)HeapAlloc(GetProcessHeap(), 0, len);
            memcpy(input_copy, input->buffer, len);
            in.h = (HANDLE)(intptr_t)in_fd; in.in = input_copy; in.in_len = len;
            threads[nt++] = CreateThread(NULL, 0, proc__writer, &in, 0, NULL);
        }
    }
    if (out_fd > 0) { out.h = (HANDLE)(intptr_t)out_fd; threads[nt++] = CreateThread(NULL, 0, proc__reader, &out, 0, NULL); }
    if (err_fd > 0) { err.h = (HANDLE)(intptr_t)err_fd; threads[nt++] = CreateThread(NULL, 0, proc__reader, &err, 0, NULL); }

    int timed_out = !proc__wait_handle(proc, timeout_ms);
    if (timed_out) {
        TerminateProcess(proc, (UINT)-9);
        WaitForSingleObject(proc, INFINITE);
    }
    // The pipes close with the process; let the readers finish.
    quirk_gil_release();
    if (nt) WaitForMultipleObjects((DWORD)nt, threads, TRUE, INFINITE);
    quirk_gil_acquire();
    for (int i = 0; i < nt; i++) CloseHandle(threads[i]);
    if (out.h) CloseHandle(out.h);
    if (err.h) CloseHandle(err.h);
    if (input_copy) HeapFree(GetProcessHeap(), 0, input_copy);

    proc__push_int(res, timed_out ? -9 : proc__exit_code(proc));
    Core_Collections_List_List_append(res, proc__wbuf_string(&out.buf));
    Core_Collections_List_List_append(res, proc__wbuf_string(&err.buf));
    proc__push_int(res, timed_out);
    return res;
}

int64_t Process_pid() {
    return (int64_t)GetCurrentProcessId();
}
