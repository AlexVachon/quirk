#ifdef _WIN32
// Windows: CreateProcess / pipes / threads — the same Process_* entry points.
#include "process_win32.c"
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gc.h>
#include "../types.h"

#ifndef _WIN32
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <poll.h>
    #include <signal.h>
    #include <time.h>
    #include <sys/types.h>
    #include <sys/wait.h>
#endif

// ---------------------------------------------------------------------------
//  Child processes, for packages/process/index.quirk. Naming: Process_<name>.
//
//  Spawning is fork + exec with a close-on-exec "exec failed" pipe, so a
//  missing program is reported to the parent as an errno instead of a
//  child that exits 127. All pipe ends the parent keeps are close-on-exec,
//  so one child doesn't inherit another's pipes.
//
//  Every wait (reading, writing, waitpid) runs with the task lock
//  released; inside a task it also watches the task's wake pipe, so
//  task.cancel() interrupts it — the child is killed and reaped, then
//  CancelledError is raised.
//
//  Exit status encoding (Process_wait_raw, Process_communicate_raw):
//    >= 0          exit code
//    -N            killed by signal N
//    PROC_RUNNING  not finished (a timed-out wait)
// ---------------------------------------------------------------------------

#define PROC_RUNNING (-1000)

void* quirk_int_to_opaque(int64_t v);   // runtime.c

static List* proc__new_list(void) {
    List* l = (List*)quirk_alloc(QT_LIST, sizeof(List));
    Core_Collections_List_List___init(l);
    return l;
}

static void proc__push_int(List* l, int64_t v) {
    Core_Collections_List_List_append(l, quirk_int_to_opaque(v));
}

static int64_t proc__now_ms(void) {
#ifndef _WIN32
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
    return 0;
#endif
}

#ifndef _WIN32

// Writing to a child that has exited must be an error (EPIPE), not a
// SIGPIPE that kills the whole program. Only when nobody set a handler.
static void proc__ignore_sigpipe(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    struct sigaction sa;
    if (sigaction(SIGPIPE, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL) {
        signal(SIGPIPE, SIG_IGN);
    }
}

// Both ends close-on-exec. (Not atomic like pipe2, but every fork here
// happens under the task lock, so no other spawn can race in between.)
static int proc__pipe(int fds[2]) {
    if (pipe(fds) != 0) return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
}

static int proc__status(int st) {
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return -WTERMSIG(st);
    return PROC_RUNNING;
}

// The task's wake pipe (created on first use), or -1 outside a task.
static int proc__wake_fd(void) {
    QuirkTask* me = (QuirkTask*)quirk_ts->task;
    if (!me || !quirk_gil_on) return -1;
    if (!me->has_wake) {
        if (pipe(me->wake) != 0) return -1;
        fcntl(me->wake[0], F_SETFL, O_NONBLOCK);
        fcntl(me->wake[1], F_SETFL, O_NONBLOCK);
        me->has_wake = 1;
    }
    return me->wake[0];
}

static int proc__cancel_pending(void) {
    QuirkTask* me = (QuirkTask*)quirk_ts->task;
    return me && me->cancel_requested && !me->cancel_raised;
}

static void proc__drain_wake(int wake) {
    if (wake < 0) return;
    char b[16];
    while (read(wake, b, sizeof b) > 0) {}
}

static void proc__kill_and_reap(int64_t pid) {
    if (pid <= 0) return;
    kill((pid_t)pid, SIGKILL);
    int st;
    while (waitpid((pid_t)pid, &st, 0) < 0 && errno == EINTR) {}
}

#endif  // !_WIN32

// A NULL-terminated char* array from a List of Strings (GC-allocated).
static char** proc__cstr_array(List* l) {
    int64_t n = l ? l->size : 0;
    char** out = (char**)GC_malloc(sizeof(char*) * (size_t)(n + 1));
    for (int64_t i = 0; i < n; i++) {
        String* s = quirk_ensure_string((String*)Core_Collections_List_List___get(l, i));
        out[i] = make_safe_cstr(s);
        if (!out[i]) out[i] = "";
    }
    out[n] = NULL;
    return out;
}

// Start `argv[0]` with arguments `argv`. `cwd` "" = this process's.
// `env`: "KEY=VALUE" strings; with `inherit_env` they're added to (and
// override) this process's environment, otherwise they're all of it.
// Modes per stream: 0 inherit, 1 pipe, 2 /dev/null, 3 (stderr only) into
// stdout. Returns [pid, stdin_fd, stdout_fd, stderr_fd] (fd -1 when not
// piped), or [-1, errno] when the program couldn't be started.
List* Process_spawn_raw(List* argv, String* cwd, List* env, int64_t inherit_env,
                        int64_t in_mode, int64_t out_mode, int64_t err_mode) {
    List* res = proc__new_list();
#ifdef _WIN32
    proc__push_int(res, -1); proc__push_int(res, 38 /* ENOSYS */);
    return res;
#else
    proc__ignore_sigpipe();
    if (!argv || argv->size == 0) {
        proc__push_int(res, -1); proc__push_int(res, EINVAL);
        return res;
    }
    char** args = proc__cstr_array(argv);
    char* dir = cwd ? make_safe_cstr(cwd) : NULL;
    if (dir && !*dir) dir = NULL;

    // The child's environment, built before fork (no allocation after).
    char** envp = NULL;
    if (env && (env->size > 0 || !inherit_env)) {
        char** extra = proc__cstr_array(env);
        int64_t nExtra = env->size, nBase = 0;
        extern char** environ;
        if (inherit_env) while (environ[nBase]) nBase++;
        envp = (char**)GC_malloc(sizeof(char*) * (size_t)(nBase + nExtra + 1));
        int64_t k = 0;
        for (int64_t i = 0; i < nBase; i++) {
            const char* eq = strchr(environ[i], '=');
            size_t klen = eq ? (size_t)(eq - environ[i]) : strlen(environ[i]);
            int overridden = 0;
            for (int64_t j = 0; j < nExtra && !overridden; j++)
                overridden = strncmp(extra[j], environ[i], klen) == 0 && extra[j][klen] == '=';
            if (!overridden) envp[k++] = environ[i];
        }
        for (int64_t j = 0; j < nExtra; j++) envp[k++] = extra[j];
        envp[k] = NULL;
    }

    int inP[2] = {-1, -1}, outP[2] = {-1, -1}, errP[2] = {-1, -1}, execP[2] = {-1, -1};
    int ok = proc__pipe(execP) == 0;
    if (ok && in_mode == 1)  ok = proc__pipe(inP) == 0;
    if (ok && out_mode == 1) ok = proc__pipe(outP) == 0;
    if (ok && err_mode == 1) ok = proc__pipe(errP) == 0;
    if (!ok) {
        int e = errno;
        int* all[] = {inP, outP, errP, execP};
        for (int i = 0; i < 4; i++) { if (all[i][0] >= 0) close(all[i][0]); if (all[i][1] >= 0) close(all[i][1]); }
        proc__push_int(res, -1); proc__push_int(res, e);
        return res;
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Child: only async-signal-safe calls from here to exec.
        signal(SIGPIPE, SIG_DFL);
        int devnull = -1;
        if (in_mode == 2 || out_mode == 2 || err_mode == 2) devnull = open("/dev/null", O_RDWR);
        if (in_mode == 1) dup2(inP[0], 0);
        else if (in_mode == 2 && devnull >= 0) dup2(devnull, 0);
        if (out_mode == 1) dup2(outP[1], 1);
        else if (out_mode == 2 && devnull >= 0) dup2(devnull, 1);
        if (err_mode == 1) dup2(errP[1], 2);
        else if (err_mode == 2 && devnull >= 0) dup2(devnull, 2);
        else if (err_mode == 3) dup2(1, 2);
        if (dir && chdir(dir) != 0) {
            int e = errno;
            (void)!write(execP[1], &e, sizeof e);
            _exit(127);
        }
        if (envp) { extern char** environ; environ = envp; }
        execvp(args[0], args);
        int e = errno;
        (void)!write(execP[1], &e, sizeof e);
        _exit(127);
    }
    int forkErr = errno;
    close(execP[1]);
    if (inP[0] >= 0) close(inP[0]);
    if (outP[1] >= 0) close(outP[1]);
    if (errP[1] >= 0) close(errP[1]);
    if (pid < 0) {
        close(execP[0]);
        if (inP[1] >= 0) close(inP[1]);
        if (outP[0] >= 0) close(outP[0]);
        if (errP[0] >= 0) close(errP[0]);
        proc__push_int(res, -1); proc__push_int(res, forkErr);
        return res;
    }
    // exec succeeded iff the pipe closes with nothing written.
    int childErr = 0;
    ssize_t n;
    quirk_gil_release();
    do { n = read(execP[0], &childErr, sizeof childErr); } while (n < 0 && errno == EINTR);
    quirk_gil_acquire();
    close(execP[0]);
    if (n == (ssize_t)sizeof childErr) {
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        if (inP[1] >= 0) close(inP[1]);
        if (outP[0] >= 0) close(outP[0]);
        if (errP[0] >= 0) close(errP[0]);
        proc__push_int(res, -1); proc__push_int(res, childErr);
        return res;
    }
    proc__push_int(res, pid);
    proc__push_int(res, inP[1]);
    proc__push_int(res, outP[0]);
    proc__push_int(res, errP[0]);
    return res;
#endif
}

// strerror for a spawn failure's errno.
String* Process_strerror(int64_t err) {
    return make_String(strerror((int)err));
}

// Wait for `pid` to finish, up to `timeout_ms` (-1 = no limit). Returns
// the exit status (see the encoding above) or PROC_RUNNING on timeout.
int64_t Process_wait_raw(int64_t pid, int64_t timeout_ms) {
#ifdef _WIN32
    (void)pid; (void)timeout_ms;
    return -1;
#else
    if (pid <= 0) return -1;
    int st;
    int wake = proc__wake_fd();
    if (timeout_ms < 0 && wake < 0) {
        pid_t r;
        quirk_gil_release();
        do { r = waitpid((pid_t)pid, &st, 0); } while (r < 0 && errno == EINTR);
        quirk_gil_acquire();
        return r == (pid_t)pid ? proc__status(st) : -1;
    }
    // Poll waitpid, backing off 1ms → 20ms, watching the wake pipe.
    int64_t deadline = timeout_ms >= 0 ? proc__now_ms() + timeout_ms : -1;
    int nap = 1;
    for (;;) {
        pid_t r = waitpid((pid_t)pid, &st, WNOHANG);
        if (r == (pid_t)pid) return proc__status(st);
        if (r < 0 && errno != EINTR) return -1;
        if (proc__cancel_pending()) {
            proc__kill_and_reap(pid);
            quirk_task_check_cancel();
        }
        int64_t left = deadline < 0 ? nap : deadline - proc__now_ms();
        if (deadline >= 0 && left <= 0) return PROC_RUNNING;
        int ms = (int)(left < nap ? left : nap);
        struct pollfd p = { wake, POLLIN, 0 };
        quirk_gil_release();
        poll(wake >= 0 ? &p : NULL, wake >= 0 ? 1 : 0, ms);
        quirk_gil_acquire();
        if (wake >= 0 && (p.revents & POLLIN)) proc__drain_wake(wake);
        if (nap < 20) nap *= 2;
    }
#endif
}

int64_t Process_kill_raw(int64_t pid, int64_t sig) {
#ifdef _WIN32
    (void)pid; (void)sig;
    return -1;
#else
    if (pid <= 0) return -1;
    return kill((pid_t)pid, (int)sig) == 0 ? 0 : -1;
#endif
}

void Process_close_raw(int64_t fd) {
#ifndef _WIN32
    if (fd >= 0) close((int)fd);
#endif
}

// Growable byte buffer for captured output.
typedef struct { char* p; size_t len, cap; } ProcBuf;

static void proc__buf_add(ProcBuf* b, const char* s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char* np = (char*)GC_malloc_atomic(cap);
        if (b->len) memcpy(np, b->p, b->len);
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static String* proc__buf_string(ProcBuf* b) {
    if (!b->p) return make_String("");
    String* s = make_String_taking_ownership(b->p);
    return s;
}

// Read up to `max` bytes from `fd` (blocking until some arrive). ""
// means end of output.
String* Process_read_raw(int64_t fd, int64_t max) {
#ifdef _WIN32
    (void)fd; (void)max;
    return make_String("");
#else
    if (fd < 0 || max <= 0) return make_String("");
    quirk_wait_io((int)fd, 0);
    char* buf = (char*)GC_malloc_atomic((size_t)max + 1);
    ssize_t n;
    quirk_gil_release();
    do { n = read((int)fd, buf, (size_t)max); } while (n < 0 && errno == EINTR);
    quirk_gil_acquire();
    if (n <= 0) return make_String("");
    buf[n] = '\0';
    return make_String_taking_ownership(buf);
#endif
}

// One line from `fd`, without its "\n"; [line, 1] or ["", 0] at the end.
// Reads a byte at a time so nothing past the line is consumed.
List* Process_read_line_raw(int64_t fd) {
    List* res = proc__new_list();
#ifdef _WIN32
    (void)fd;
    Core_Collections_List_List_append(res, make_String(""));
    proc__push_int(res, 0);
    return res;
#else
    ProcBuf b = {0};
    int got = 0;
    for (;;) {
        quirk_wait_io((int)fd, 0);
        char c;
        ssize_t n;
        quirk_gil_release();
        do { n = read((int)fd, &c, 1); } while (n < 0 && errno == EINTR);
        quirk_gil_acquire();
        if (n <= 0) break;
        got = 1;
        if (c == '\n') break;
        proc__buf_add(&b, &c, 1);
    }
    if (b.len && b.p[b.len - 1] == '\r') b.p[--b.len] = '\0';
    Core_Collections_List_List_append(res, proc__buf_string(&b));
    proc__push_int(res, got);
    return res;
#endif
}

// Write all of `data` to `fd`. Returns the bytes written, -1 on error
// (e.g. the child closed its stdin).
int64_t Process_write_raw(int64_t fd, String* data) {
#ifdef _WIN32
    (void)fd; (void)data;
    return -1;
#else
    if (fd < 0 || !data || !data->buffer) return -1;
    const char* p = data->buffer;
    size_t left = (size_t)data->length, done = 0;
    while (left > 0) {
        quirk_wait_io((int)fd, 1);
        ssize_t n;
        quirk_gil_release();
        do { n = write((int)fd, p + done, left); } while (n < 0 && errno == EINTR);
        quirk_gil_acquire();
        if (n < 0) return -1;
        done += (size_t)n;
        left -= (size_t)n;
    }
    return (int64_t)done;
#endif
}

// Feed `input` to `in_fd` (then close it), collect `out_fd` / `err_fd`
// to the end, and wait for `pid` — all at once, so a child filling one
// pipe while we wait on another can't deadlock. `timeout_ms` -1 = no
// limit; on timeout the child is killed. Any fd may be -1.
// Returns [status, stdout, stderr, timed_out].
List* Process_communicate_raw(int64_t pid, int64_t in_fd, int64_t out_fd, int64_t err_fd,
                              String* input, int64_t timeout_ms) {
    List* res = proc__new_list();
#ifdef _WIN32
    (void)pid; (void)in_fd; (void)out_fd; (void)err_fd; (void)input; (void)timeout_ms;
    proc__push_int(res, -1);
    Core_Collections_List_List_append(res, make_String(""));
    Core_Collections_List_List_append(res, make_String(""));
    proc__push_int(res, 0);
    return res;
#else
    ProcBuf out = {0}, err = {0};
    const char* inp = (input && input->buffer) ? input->buffer : "";
    size_t inLeft = (input && input->buffer) ? (size_t)input->length : 0, inDone = 0;
    int fin = (int)in_fd, fout = (int)out_fd, ferr = (int)err_fd;
    if (fin >= 0 && inLeft == 0) { close(fin); fin = -1; }
    if (fin >= 0) fcntl(fin, F_SETFL, fcntl(fin, F_GETFL) | O_NONBLOCK);
    int wake = proc__wake_fd();
    int64_t deadline = timeout_ms >= 0 ? proc__now_ms() + timeout_ms : -1;
    int timedOut = 0;
    char chunk[65536];

    while (fin >= 0 || fout >= 0 || ferr >= 0) {
        struct pollfd fds[4];
        int nf = 0, iIn = -1, iOut = -1, iErr = -1, iWake = -1;
        if (fin >= 0)  { iIn = nf;   fds[nf++] = (struct pollfd){ fin,  POLLOUT, 0 }; }
        if (fout >= 0) { iOut = nf;  fds[nf++] = (struct pollfd){ fout, POLLIN,  0 }; }
        if (ferr >= 0) { iErr = nf;  fds[nf++] = (struct pollfd){ ferr, POLLIN,  0 }; }
        if (wake >= 0) { iWake = nf; fds[nf++] = (struct pollfd){ wake, POLLIN,  0 }; }
        int ms = -1;
        if (deadline >= 0) {
            int64_t left = deadline - proc__now_ms();
            if (left <= 0) { timedOut = 1; break; }
            ms = (int)left;
        }
        int rc;
        quirk_gil_release();
        do { rc = poll(fds, (nfds_t)nf, ms); } while (rc < 0 && errno == EINTR);
        quirk_gil_acquire();
        if (rc < 0) break;
        if (iWake >= 0 && (fds[iWake].revents & POLLIN)) proc__drain_wake(wake);
        if (proc__cancel_pending()) {
            if (fin >= 0) close(fin);
            if (fout >= 0) close(fout);
            if (ferr >= 0) close(ferr);
            proc__kill_and_reap(pid);
            quirk_task_check_cancel();
        }
        if (iIn >= 0 && fds[iIn].revents) {
            ssize_t n = write(fin, inp + inDone, inLeft);
            if (n > 0) { inDone += (size_t)n; inLeft -= (size_t)n; }
            if (inLeft == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { close(fin); fin = -1; }
        }
        if (iOut >= 0 && fds[iOut].revents) {
            ssize_t n = read(fout, chunk, sizeof chunk);
            if (n > 0) proc__buf_add(&out, chunk, (size_t)n);
            else if (n == 0 || (errno != EAGAIN && errno != EINTR)) { close(fout); fout = -1; }
        }
        if (iErr >= 0 && fds[iErr].revents) {
            ssize_t n = read(ferr, chunk, sizeof chunk);
            if (n > 0) proc__buf_add(&err, chunk, (size_t)n);
            else if (n == 0 || (errno != EAGAIN && errno != EINTR)) { close(ferr); ferr = -1; }
        }
    }
    int64_t status;
    if (timedOut) {
        if (fin >= 0) close(fin);
        if (fout >= 0) close(fout);
        if (ferr >= 0) close(ferr);
        proc__kill_and_reap(pid);
        status = -SIGKILL;
    } else {
        int64_t left = deadline < 0 ? -1 : deadline - proc__now_ms();
        status = Process_wait_raw(pid, left < 0 && deadline >= 0 ? 0 : left);
        if (status == PROC_RUNNING) {
            proc__kill_and_reap(pid);
            status = -SIGKILL;
            timedOut = 1;
        }
    }
    proc__push_int(res, status);
    Core_Collections_List_List_append(res, proc__buf_string(&out));
    Core_Collections_List_List_append(res, proc__buf_string(&err));
    proc__push_int(res, timedOut);
    return res;
#endif
}

// This process's id.
int64_t Process_pid() {
#ifdef _WIN32
    return 0;
#else
    return (int64_t)getpid();
#endif
}
#endif  // !_WIN32
