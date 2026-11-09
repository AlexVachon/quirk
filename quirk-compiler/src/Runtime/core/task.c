// [task.c] — `async define` / `await` (compiler 7.12.0; cancel / wait 7.13.0).
//
// Calling an `async define` function starts a Task: the call runs on its
// own OS thread, but only one task runs Quirk code at a time — they pass a
// single lock (`quirk_gil`), the way an event loop passes control. A task
// gives the lock up only while it waits: in `await`, `sys.sleep`, socket
// I/O, file / console reads. So tasks interleave only at those points (no
// data races between them), and blocking I/O in one lets the others run.
//
// `await task` waits for the task and returns its result, or re-throws the
// exception it ended with. `await [t1, t2, …]` awaits each and returns the
// list of results. Awaiting anything that isn't a Task returns it as is.
//
// `task.cancel()` asks a task to stop: it gets a CancelledError at its next
// `sys.sleep` / `await` / `wait` (a sleep in progress is cut short), which
// it may catch to clean up. Awaiting a cancelled task raises that error.
// `task.wait(ms)` waits up to ms milliseconds and says whether it finished.
//
// Every wait — for a task to finish, for a sleep to end — is on one
// condition variable (`quirk_sched`) with quirk_gil held, broadcast when a
// task finishes or is cancelled; waiters re-check their own condition.

#include <pthread.h>
#include <time.h>
#include <errno.h>

typedef struct QuirkTask {
    Callable* fn;
    void* result;
    void* exception;           // what the task threw, if it failed
    int done;
    int retrieved;             // somebody awaited it
    int cancel_requested;      // task.cancel() was called
    int cancel_raised;         // ...and the task has seen its CancelledError
    struct QuirkTask* next;    // quirk_tasks: unfinished / failed-unawaited
} QuirkTask;

static pthread_mutex_t quirk_gil = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  quirk_sched = PTHREAD_COND_INITIALIZER;
// Off until the first task starts: a program without tasks never takes
// the lock. Set by the thread that holds Quirk's only flow of control, so
// it needs no synchronisation.
static int quirk_gil_on = 0;

// GC root for every task that still matters (the threads' own references
// aren't enough while a task is between threads). Guarded by quirk_gil.
static QuirkTask* quirk_tasks = NULL;

void quirk_gil_release(void) { if (quirk_gil_on) pthread_mutex_unlock(&quirk_gil); }
void quirk_gil_acquire(void) { if (quirk_gil_on) pthread_mutex_lock(&quirk_gil); }

static void quirk_tasks_remove(QuirkTask* t) {
    for (QuirkTask** p = &quirk_tasks; *p; p = &(*p)->next)
        if (*p == t) { *p = t->next; t->next = NULL; return; }
}

// The running task asked to stop: raise CancelledError here (once).
static void quirk_task_check_cancel(void) {
    QuirkTask* me = (QuirkTask*)quirk_ts->task;
    if (me && me->cancel_requested && !me->cancel_raised) {
        me->cancel_raised = 1;
        quirk_throw_exception("CancelledError", "the task was cancelled");
    }
}

static void quirk_deadline(struct timespec* ts, int64_t ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static void* quirk_task_main(void* arg) {
    QuirkTask* t = (QuirkTask*)arg;
    // This thread's call / try stacks; a GC root, since it can hold the
    // only reference to an exception being thrown.
    QuirkThreadState* ts = (QuirkThreadState*)GC_malloc_uncollectable(sizeof(QuirkThreadState));
    memset(ts, 0, sizeof(*ts));
    ts->try_depth = -1;
    ts->task = t;
    quirk_ts = ts;
    pthread_mutex_lock(&quirk_gil);
    jmp_buf* jb = (jmp_buf*)quirk_get_jmp_buf();
    if (_setjmp(*jb) == 0) {
        quirk_task_check_cancel();   // cancelled before it got to run
        typedef void* (*Thunk0)(void* env, void* end);
        t->result = ((Thunk0)t->fn->fn)(t->fn->env, QUIRK_NOARG);
        quirk_pop_try();
    } else {
        // A throw pops the try stack before jumping here.
        t->exception = quirk_get_exception();
    }
    t->done = 1;
    // Succeeded, or failed and already awaited: nothing left to report.
    // A cancelled task isn't a failure to report either — somebody asked.
    if (!t->exception || t->retrieved || t->cancel_requested) quirk_tasks_remove(t);
    pthread_cond_broadcast(&quirk_sched);
    pthread_mutex_unlock(&quirk_gil);
    quirk_ts = &quirk_main_ts;   // nothing on this thread runs Quirk code now
    GC_free(ts);
    return NULL;
}

// When main() returns (compiled code calls this): a task that failed and
// was never awaited would otherwise fail silently (Python's "Task
// exception was never retrieved"). Not an atexit handler — under the JIT
// the program's code and constants are gone by then.
void quirk_tasks_main_done(void) {
    for (QuirkTask* t = quirk_tasks; t; t = t->next) {
        if (!t->done || !t->exception || t->retrieved) continue;
        fflush(stdout);
        fprintf(stderr, "A task failed and was never awaited:\n");
        quirk_print_uncaught(t->exception);
    }
}

void* quirk_task_spawn(Callable* fn) {
    if (!quirk_gil_on) {
        // This thread is the only one running Quirk code; it takes the
        // lock and keeps it until it waits.
        pthread_mutex_lock(&quirk_gil);
        quirk_gil_on = 1;
    }
    QuirkTask* t = (QuirkTask*)quirk_alloc(QT_TASK, sizeof(QuirkTask));
    t->fn = fn;
    t->next = quirk_tasks;
    quirk_tasks = t;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    // A deep call stack in a task needs room like the main thread's.
    pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&th, &attr, quirk_task_main, t) != 0) {
        pthread_attr_destroy(&attr);
        quirk_tasks_remove(t);
        quirk_throw_exception("RuntimeError", "could not start a task (pthread_create failed)");
    }
    pthread_attr_destroy(&attr);
    return t;
}

// Re-throw an exception object at the current point (used by `await`).
static void quirk_rethrow(void* exc) {
    quirk_active_exception = exc;
    if (quirk_try_depth >= 0) {
        quirk_shadow_sp = quirk_saved_shadow_sp[quirk_try_depth];
        jmp_buf* buf = &quirk_try_stack[quirk_try_depth];
        quirk_try_depth--;
        longjmp(*buf, 1);
    }
    quirk_print_uncaught(exc);
    exit(1);
}

// Wait until `t` finishes; with ms >= 0, give up after ms milliseconds.
// Returns whether it finished. Raises CancelledError if the waiting task
// is cancelled meanwhile.
static int quirk_task_wait_done(QuirkTask* t, int64_t ms) {
    struct timespec until;
    if (ms >= 0) quirk_deadline(&until, ms);
    while (!t->done) {
        quirk_task_check_cancel();
        if (ms < 0) pthread_cond_wait(&quirk_sched, &quirk_gil);   // releases the lock meanwhile
        else if (pthread_cond_timedwait(&quirk_sched, &quirk_gil, &until) == ETIMEDOUT) break;
    }
    quirk_task_check_cancel();
    return t->done;
}

static List* quirk_task_new_list(void) {
    List* l = (List*)quirk_alloc(QT_LIST, sizeof(List));
    Core_Collections_List_List___init(l);
    return l;
}

static void* quirk_await_one(void* v) {
    if (quirk_type_of(v) != QT_TASK) return v;
    QuirkTask* t = (QuirkTask*)v;
    quirk_task_wait_done(t, -1);
    t->retrieved = 1;
    if (t->exception) {
        quirk_tasks_remove(t);
        quirk_rethrow(t->exception);
    }
    return t->result;
}

void* quirk_task_await(void* v) {
    if (quirk_type_of(v) == QT_LIST) {
        // `await [t1, t2]`: every result, in order. The first failure is
        // re-thrown — after the others have finished, so none is left
        // running unobserved.
        List* in = (List*)v;
        List* out = quirk_task_new_list();
        void* firstExc = NULL;
        for (int64_t i = 0; i < in->size; i++) {
            void* e = in->data[i];
            if (quirk_type_of(e) == QT_TASK) {
                QuirkTask* t = (QuirkTask*)e;
                quirk_task_wait_done(t, -1);
                t->retrieved = 1;
                if (t->exception) {
                    quirk_tasks_remove(t);
                    if (!firstExc) firstExc = t->exception;
                    Core_Collections_List_List_append(out, NULL);
                    continue;
                }
                e = t->result;
            }
            Core_Collections_List_List_append(out, e);
        }
        if (firstExc) quirk_rethrow(firstExc);
        return out;
    }
    quirk_task_check_cancel();
    return quirk_await_one(v);
}

// `task.done()` — finished (successfully or not), without waiting.
int32_t quirk_task_done(void* v) {
    return quirk_type_of(v) == QT_TASK ? ((QuirkTask*)v)->done : 1;
}

// `task.wait(ms)` — wait up to ms milliseconds (negative: no limit); true
// if the task has finished. Doesn't raise the task's exception (`await`
// does).
int32_t quirk_task_wait(void* v, int64_t ms) {
    if (quirk_type_of(v) != QT_TASK) return 1;
    return quirk_task_wait_done((QuirkTask*)v, ms);
}

// `task.cancel()` — ask the task to stop (see the top of the file).
void quirk_task_cancel(void* v) {
    if (quirk_type_of(v) != QT_TASK) return;
    QuirkTask* t = (QuirkTask*)v;
    if (t->done) return;
    t->cancel_requested = 1;
    pthread_cond_broadcast(&quirk_sched);   // cut short its sleep / await
}

// `sys.sleep(ms)` inside a task: a timed wait on the scheduler condition,
// so cancel() can interrupt it. Outside a task: a plain sleep that lets
// the tasks run.
void quirk_task_sleep(int64_t ms) {
    QuirkTask* me = (QuirkTask*)quirk_ts->task;
    if (!me || !quirk_gil_on) {
        struct timespec d = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
        quirk_gil_release();
        nanosleep(&d, NULL);
        quirk_gil_acquire();
        return;
    }
    struct timespec until;
    quirk_deadline(&until, ms);
    quirk_task_check_cancel();
    while (!(me->cancel_requested && !me->cancel_raised)) {
        if (pthread_cond_timedwait(&quirk_sched, &quirk_gil, &until) == ETIMEDOUT) break;
    }
    quirk_task_check_cancel();
}
