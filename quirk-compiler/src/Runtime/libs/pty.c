// ---------------------------------------------------------------------------
//  Pseudo-terminals, for packages/pty/index.quirk. Naming: Pty_<name>.
//
//  A program run in a pty sees a terminal: it colours its output, edits
//  lines, draws full-screen (a shell, vim, top). We hold the other side:
//  what it writes (text and escape sequences) comes out of `read`, what
//  we `write` is typed at it, `resize` changes its window size.
//
//    POSIX    posix_openpt + fork: the child gets the pty as its controlling
//             terminal (setsid + TIOCSCTTY) on stdin / stdout / stderr.
//    Windows  ConPTY (Windows 10 1809+): CreatePseudoConsole over two pipes;
//             looked up at run time, so older Windows says "not available".
//
//  A handle is a slot in `qptys`. Reads hand back whole UTF-8 characters
//  only: a character split between two reads waits for its other bytes.
//
//  Included by runtime.c after process.c (shares its proc__* helpers).
// ---------------------------------------------------------------------------

#define QPTY_MAX 32

#ifndef _WIN32
    #include <sys/ioctl.h>
    #include <termios.h>
    // (Declared by <stdlib.h> only with _XOPEN_SOURCE, which this single
    // translation unit can't define this late.)
    extern int posix_openpt(int);
    extern int grantpt(int);
    extern int unlockpt(int);
    extern char* ptsname(int);
#else
    static int64_t proc__now_ms(void) { return (int64_t)GetTickCount64(); }
#endif

typedef struct {
    int used;
#ifdef _WIN32
    HANDLE in_w;            // what we write: the console's input
    HANDLE out_r;           // what we read: the console's output
    HANDLE proc;
    void* hpc;              // HPCON
#else
    int fd;                 // the master side
    pid_t pid;
#endif
    int exited;
    int64_t code;           // exit code (>= 0), -N killed by signal N
    char carry[4];          // the start of a UTF-8 character not yet complete
    int ncarry;
    int eof;
} QPty;

static QPty qptys[QPTY_MAX];
static char qpty_err[256] = "";

static QPty* qpty_get(int64_t h) {
    if (h < 0 || h >= QPTY_MAX || !qptys[h].used) return NULL;
    return &qptys[h];
}

static int qpty_slot(void) {
    for (int i = 0; i < QPTY_MAX; i++) if (!qptys[i].used) return i;
    return -1;
}

String* Pty_error() {
    return make_String(qpty_err);
}

// Bytes [0, n) of buf, with the slot's carried bytes first, as a String of
// whole UTF-8 characters; an incomplete one at the end is carried.
static String* qpty_take(QPty* p, const char* buf, size_t n) {
    char* out = (char*)GC_malloc_atomic(n + (size_t)p->ncarry + 1);
    size_t len = 0;
    for (int i = 0; i < p->ncarry; i++) out[len++] = p->carry[i];
    memcpy(out + len, buf, n);
    len += n;
    p->ncarry = 0;
    // An incomplete character at the end: up to 3 bytes back, a lead byte
    // whose sequence runs past the end.
    size_t k = len, back = 0;
    while (k > 0 && back < 4 && ((unsigned char)out[k - 1] & 0xC0) == 0x80) { k--; back++; }
    if (k > 0) {
        unsigned char lead = (unsigned char)out[k - 1];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        size_t have = len - (k - 1);
        if (need > 1 && have < need) {
            p->ncarry = (int)have;
            memcpy(p->carry, out + k - 1, have);
            len = k - 1;
        }
    }
    out[len] = '\0';
    return make_String_taking_ownership(out);
}

#ifndef _WIN32
// ======================= POSIX =======================

// Start `argv` in a new pty of cols x rows; [handle, pid] or [-1, errno].
// `env`: "K=V" strings over this process's environment.
List* Pty_open_raw(List* argv, String* cwd, List* env, int64_t cols, int64_t rows) {
    List* res = proc__new_list();
    int slot = qpty_slot();
    if (!argv || argv->size == 0 || slot < 0) {
        snprintf(qpty_err, sizeof qpty_err, slot < 0 ? "too many terminals (max %d)" : "no program given", QPTY_MAX);
        proc__push_int(res, -1); proc__push_int(res, EINVAL);
        return res;
    }
    char** args = proc__cstr_array(argv);
    char* dir = cwd ? make_safe_cstr(cwd) : NULL;
    if (dir && !*dir) dir = NULL;
    char** extra = proc__cstr_array(env);
    int64_t nExtra = env ? env->size : 0;

    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) {
        int e = errno;
        if (m >= 0) close(m);
        snprintf(qpty_err, sizeof qpty_err, "can't open a pseudo-terminal: %s", strerror(e));
        proc__push_int(res, -1); proc__push_int(res, e);
        return res;
    }
    char* sname = ptsname(m);
    char slave_name[256];
    snprintf(slave_name, sizeof slave_name, "%s", sname ? sname : "");
    fcntl(m, F_SETFD, FD_CLOEXEC);
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_col = (unsigned short)(cols > 0 ? cols : 80);
    ws.ws_row = (unsigned short)(rows > 0 ? rows : 24);

    int execP[2] = {-1, -1};
    if (proc__pipe(execP) != 0) {
        int e = errno;
        close(m);
        proc__push_int(res, -1); proc__push_int(res, e);
        return res;
    }
    proc__ignore_sigpipe();
    pid_t pid = fork();
    if (pid == 0) {
        // Child: only async-signal-safe calls until exec.
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        setsid();
        int s = open(slave_name, O_RDWR);
        if (s < 0) { int e = errno; (void)!write(execP[1], &e, sizeof e); _exit(127); }
#ifdef TIOCSCTTY
        ioctl(s, TIOCSCTTY, 0);
#endif
        ioctl(s, TIOCSWINSZ, &ws);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        close(m);
        for (int64_t j = 0; j < nExtra; j++) putenv(extra[j]);
        if (dir && chdir(dir) != 0) { int e = errno; (void)!write(execP[1], &e, sizeof e); _exit(127); }
        execvp(args[0], args);
        int e = errno;
        (void)!write(execP[1], &e, sizeof e);
        _exit(127);
    }
    close(execP[1]);
    if (pid < 0) {
        int e = errno;
        close(execP[0]); close(m);
        proc__push_int(res, -1); proc__push_int(res, e);
        return res;
    }
    int child_err = 0;
    ssize_t got;
    quirk_gil_release();
    do { got = read(execP[0], &child_err, sizeof child_err); } while (got < 0 && errno == EINTR);
    quirk_gil_acquire();
    close(execP[0]);
    if (got == (ssize_t)sizeof child_err) {
        waitpid(pid, NULL, 0);
        close(m);
        snprintf(qpty_err, sizeof qpty_err, "can't run '%s': %s", args[0], strerror(child_err));
        proc__push_int(res, -1); proc__push_int(res, child_err);
        return res;
    }
    int fl = fcntl(m, F_GETFL);
    fcntl(m, F_SETFL, fl | O_NONBLOCK);
    QPty* p = &qptys[slot];
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->fd = m;
    p->pid = pid;
    proc__push_int(res, slot);
    proc__push_int(res, pid);
    return res;
}

// Its output so far, waiting up to `timeout_ms` for some (0: don't wait,
// -1: until there is). [text, open]: open is 0 once it has ended (the
// program exited and everything it wrote has been read).
List* Pty_read_raw(int64_t h, int64_t timeout_ms, int64_t max) {
    List* res = proc__new_list();
    QPty* p = qpty_get(h);
    if (!p || p->eof) {
        Core_Collections_List_List_append(res, make_String(""));
        proc__push_int(res, 0);
        return res;
    }
    if (max <= 0) max = 65536;
    struct pollfd pf = { p->fd, POLLIN, 0 };
    int r;
    quirk_gil_release();
    do { r = poll(&pf, 1, (int)timeout_ms); } while (r < 0 && errno == EINTR);
    quirk_gil_acquire();
    char* buf = (char*)GC_malloc_atomic((size_t)max);
    ssize_t n = 0;
    if (r > 0) {
        do { n = read(p->fd, buf, (size_t)max); } while (n < 0 && errno == EINTR);
        // EIO: the child side closed (it exited) — Linux's end of output.
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { p->eof = 1; n = 0; }
    }
    Core_Collections_List_List_append(res, qpty_take(p, buf, n > 0 ? (size_t)n : 0));
    proc__push_int(res, p->eof ? 0 : 1);
    return res;
}

// Type `data` at it; bytes written, or -1.
int64_t Pty_write_raw(int64_t h, String* data) {
    QPty* p = qpty_get(h);
    if (!p || !data) return -1;
    const char* s = make_safe_cstr(data);
    size_t len = s ? strlen(s) : 0, done = 0;
    while (done < len) {
        ssize_t n = write(p->fd, s + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = { p->fd, POLLOUT, 0 };
            quirk_gil_release();
            poll(&pf, 1, 100);
            quirk_gil_acquire();
            continue;
        }
        if (n <= 0) return done ? (int64_t)done : -1;
        done += (size_t)n;
    }
    return (int64_t)done;
}

void Pty_resize_raw(int64_t h, int64_t cols, int64_t rows) {
    QPty* p = qpty_get(h);
    if (!p || cols <= 0 || rows <= 0) return;
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ioctl(p->fd, TIOCSWINSZ, &ws);     // (the kernel sends the program SIGWINCH)
}

// Wait up to `timeout_ms` (-1: forever) for it to end: its exit status, or
// PROC_RUNNING.
int64_t Pty_wait_raw(int64_t h, int64_t timeout_ms) {
    QPty* p = qpty_get(h);
    if (!p) return PROC_RUNNING;
    if (p->exited) return p->code;
    int64_t until = timeout_ms < 0 ? -1 : proc__now_ms() + timeout_ms;
    for (;;) {
        int st = 0;
        pid_t r = waitpid(p->pid, &st, WNOHANG);
        if (r == p->pid) {
            p->exited = 1;
            p->code = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? -WTERMSIG(st) : 0;
            return p->code;
        }
        if (r < 0 && errno != EINTR) { p->exited = 1; p->code = 0; return 0; }
        if (until >= 0 && proc__now_ms() >= until) return PROC_RUNNING;
        quirk_gil_release();
        struct timespec ts = { 0, 10 * 1000000 };
        nanosleep(&ts, NULL);
        quirk_gil_acquire();
    }
}

// Signal it (SIGHUP: the terminal went away; SIGKILL; SIGINT...).
void Pty_kill_raw(int64_t h, int64_t sig) {
    QPty* p = qpty_get(h);
    if (!p || p->exited) return;
    kill(p->pid, (int)sig);
}

// Close it: the program gets SIGHUP (and is reaped); the slot is freed.
void Pty_close_raw(int64_t h) {
    QPty* p = qpty_get(h);
    if (!p) return;
    if (!p->exited) {
        kill(p->pid, SIGHUP);
        if (Pty_wait_raw(h, 300) == PROC_RUNNING) {
            kill(p->pid, SIGKILL);
            Pty_wait_raw(h, -1);
        }
    }
    close(p->fd);
    p->used = 0;
}

int64_t Pty_available_raw() {
    return 1;
}

#else
// ======================= Windows (ConPTY) =======================

typedef HRESULT (WINAPI *QCreatePC)(COORD, HANDLE, HANDLE, DWORD, void**);
typedef HRESULT (WINAPI *QResizePC)(void*, COORD);
typedef void (WINAPI *QClosePC)(void*);
static QCreatePC qpty_create;
static QResizePC qpty_resize;
static QClosePC qpty_closepc;
static int qpty_looked = 0;

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

static int qpty_load(void) {
    if (!qpty_looked) {
        qpty_looked = 1;
        HMODULE k = GetModuleHandleA("kernel32.dll");
        if (k) {
            qpty_create = (QCreatePC)(void*)GetProcAddress(k, "CreatePseudoConsole");
            qpty_resize = (QResizePC)(void*)GetProcAddress(k, "ResizePseudoConsole");
            qpty_closepc = (QClosePC)(void*)GetProcAddress(k, "ClosePseudoConsole");
        }
    }
    return qpty_create && qpty_resize && qpty_closepc;
}

int64_t Pty_available_raw() {
    return qpty_load();
}

static COORD qpty_size(int64_t cols, int64_t rows) {
    COORD c;
    c.X = (SHORT)(cols > 0 ? cols : 80);
    c.Y = (SHORT)(rows > 0 ? rows : 24);
    return c;
}

List* Pty_open_raw(List* argv, String* cwd, List* env, int64_t cols, int64_t rows) {
    List* res = proc__new_list();
    int slot = qpty_slot();
    if (!qpty_load()) {
        snprintf(qpty_err, sizeof qpty_err, "pseudo-terminals need Windows 10 1809 or later (ConPTY)");
        proc__push_int(res, -1); proc__push_int(res, 38);
        return res;
    }
    if (!argv || argv->size == 0 || slot < 0) {
        snprintf(qpty_err, sizeof qpty_err, slot < 0 ? "too many terminals (max %d)" : "no program given", QPTY_MAX);
        proc__push_int(res, -1); proc__push_int(res, 22);
        return res;
    }
    WBuf cmd = {0}, envb = {0};
    for (int64_t i = 0; i < argv->size; i++) {
        if (i) wbuf_add(&cmd, " ", 1);
        proc__quote(&cmd, proc__str(Core_Collections_List_List___get(argv, i)));
    }
    char* envp = (env && env->size > 0) ? proc__env_block(env, 1, &envb) : NULL;
    const char* dir = cwd ? make_safe_cstr(cwd) : NULL;
    if (dir && !*dir) dir = NULL;

    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL;
    void* hpc = NULL;
    DWORD err = 0;
    int ok = CreatePipe(&inR, &inW, NULL, 0) && CreatePipe(&outR, &outW, NULL, 0);
    if (ok && FAILED(qpty_create(qpty_size(cols, rows), inR, outW, 0, &hpc))) { ok = 0; err = GetLastError(); }
    // The console has its own copies now.
    if (inR) CloseHandle(inR);
    if (outW) CloseHandle(outW);

    STARTUPINFOEXA si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    SIZE_T asz = 0;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    if (ok) {
        InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
        si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, asz);
        ok = si.lpAttributeList && InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &asz) &&
             UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc,
                                       sizeof(hpc), NULL, NULL);
        if (ok) {
            quirk_gil_release();
            ok = CreateProcessA(NULL, cmd.p, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, envp, dir,
                                &si.StartupInfo, &pi);
            if (!ok) err = GetLastError();
            quirk_gil_acquire();
        } else {
            err = GetLastError();
        }
        if (si.lpAttributeList) {
            DeleteProcThreadAttributeList(si.lpAttributeList);
            HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
        }
    } else if (!err) {
        err = GetLastError();
    }
    if (!ok) {
        snprintf(qpty_err, sizeof qpty_err, "can't run '%s' (Windows error %lu)", cmd.p ? cmd.p : "", (unsigned long)err);
        wbuf_free(&cmd);
        wbuf_free(&envb);
        if (hpc) qpty_closepc(hpc);
        if (inW) CloseHandle(inW);
        if (outR) CloseHandle(outR);
        proc__push_int(res, -1);
        proc__push_int(res, proc__errno(err));
        return res;
    }
    wbuf_free(&cmd);
    wbuf_free(&envb);
    CloseHandle(pi.hThread);
    QPty* p = &qptys[slot];
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->in_w = inW;
    p->out_r = outR;
    p->proc = pi.hProcess;
    p->hpc = hpc;
    proc__push_int(res, slot);
    proc__push_int(res, (int64_t)pi.dwProcessId);
    return res;
}

// (Anonymous pipes can't be polled: PeekNamedPipe until something's there.)
List* Pty_read_raw(int64_t h, int64_t timeout_ms, int64_t max) {
    List* res = proc__new_list();
    QPty* p = qpty_get(h);
    if (!p || p->eof) {
        Core_Collections_List_List_append(res, make_String(""));
        proc__push_int(res, 0);
        return res;
    }
    if (max <= 0) max = 65536;
    int64_t until = timeout_ms < 0 ? -1 : proc__now_ms() + timeout_ms;
    DWORD avail = 0;
    for (;;) {
        if (!PeekNamedPipe(p->out_r, NULL, 0, NULL, &avail, NULL)) { p->eof = 1; avail = 0; break; }
        if (avail > 0) break;
        // The program ended and the console said all it had: done. (The
        // console keeps the pipe open until it's closed — see Pty_close_raw.)
        if (WaitForSingleObject(p->proc, 0) == WAIT_OBJECT_0 && p->exited >= 0) {
            if (p->exited == 0) p->exited = -1;       // first time: give it a moment to flush
            else { p->eof = 1; break; }
        }
        if (until >= 0 && proc__now_ms() >= until) break;
        quirk_gil_release();
        Sleep(5);
        quirk_gil_acquire();
    }
    if (p->exited == -1 && avail == 0 && WaitForSingleObject(p->proc, 0) == WAIT_OBJECT_0) {
        p->exited = 0;
        p->eof = 1;
    }
    char* buf = (char*)GC_malloc_atomic((size_t)max);
    DWORD n = 0;
    if (avail > 0) {
        DWORD want = avail < (DWORD)max ? avail : (DWORD)max;
        if (!ReadFile(p->out_r, buf, want, &n, NULL)) { p->eof = 1; n = 0; }
    }
    Core_Collections_List_List_append(res, qpty_take(p, buf, n));
    proc__push_int(res, p->eof ? 0 : 1);
    return res;
}

int64_t Pty_write_raw(int64_t h, String* data) {
    QPty* p = qpty_get(h);
    if (!p || !data) return -1;
    const char* s = make_safe_cstr(data);
    DWORD len = s ? (DWORD)strlen(s) : 0, n = 0;
    if (!len) return 0;
    quirk_gil_release();
    BOOL ok = WriteFile(p->in_w, s, len, &n, NULL);
    quirk_gil_acquire();
    return ok ? (int64_t)n : -1;
}

void Pty_resize_raw(int64_t h, int64_t cols, int64_t rows) {
    QPty* p = qpty_get(h);
    if (!p || cols <= 0 || rows <= 0) return;
    qpty_resize(p->hpc, qpty_size(cols, rows));
}

int64_t Pty_wait_raw(int64_t h, int64_t timeout_ms) {
    QPty* p = qpty_get(h);
    if (!p) return PROC_RUNNING;
    DWORD ms = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
    quirk_gil_release();
    DWORD r = WaitForSingleObject(p->proc, ms);
    quirk_gil_acquire();
    if (r != WAIT_OBJECT_0) return PROC_RUNNING;
    DWORD code = 0;
    GetExitCodeProcess(p->proc, &code);
    p->code = (int64_t)(int32_t)code;
    return p->code;
}

void Pty_kill_raw(int64_t h, int64_t sig) {
    QPty* p = qpty_get(h);
    if (!p) return;
    if (sig == 2) {                              // SIGINT: what Ctrl+C would do
        Pty_write_raw(h, make_String("\x03"));
        return;
    }
    TerminateProcess(p->proc, (UINT)(-(int)sig));
}

// ClosePseudoConsole waits until its output is read (on older Windows 10):
// drain it on a thread meanwhile.
static DWORD WINAPI qpty_drain(LPVOID arg) {
    char buf[4096];
    DWORD n;
    while (ReadFile((HANDLE)arg, buf, sizeof buf, &n, NULL) && n > 0) {}
    return 0;
}

void Pty_close_raw(int64_t h) {
    QPty* p = qpty_get(h);
    if (!p) return;
    if (WaitForSingleObject(p->proc, 0) != WAIT_OBJECT_0) TerminateProcess(p->proc, 1);
    HANDLE t = CreateThread(NULL, 0, qpty_drain, p->out_r, 0, NULL);
    quirk_gil_release();
    qpty_closepc(p->hpc);
    if (t) { WaitForSingleObject(t, 2000); CloseHandle(t); }
    quirk_gil_acquire();
    CloseHandle(p->in_w);
    CloseHandle(p->out_r);
    CloseHandle(p->proc);
    p->used = 0;
}
#endif
