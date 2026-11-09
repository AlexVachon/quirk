// [task.c] — `async define` / `await` (compiler 7.12.0).
//
// Calling an `async define` function starts a Task: the call runs on its
// own OS thread, but only one task runs Quirk code at a time — they pass a
// single lock (`quirk_gil`), the way an event loop passes control. A task
// gives the lock up only while it waits: in `await`, `sys.sleep`, or a
// blocking socket call. So tasks interleave only at those points (no data
// races between them), and blocking I/O in one task lets the others run.
//
// `await task` waits for the task and returns its result, or re-throws the
// exception it ended with. `await [t1, t2, …]` awaits each and returns the
// list of results. Awaiting anything that isn't a Task returns it as is.

#include <pthread.h>

typedef struct QuirkTask {
    Callable* fn;
    void* result;
    void* exception;           // what the task threw, if it failed
    int done;
    int retrieved;             // somebody awaited it
    pthread_cond_t finished;   // waited on with quirk_gil held
    struct QuirkTask* next;    // quirk_tasks: unfinished / failed-unawaited
} QuirkTask;

static pthread_mutex_t quirk_gil = PTHREAD_MUTEX_INITIALIZER;
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

static void* quirk_task_main(void* arg) {
    QuirkTask* t = (QuirkTask*)arg;
    // This thread's call / try stacks; a GC root, since it can hold the
    // only reference to an exception being thrown.
    QuirkThreadState* ts = (QuirkThreadState*)GC_malloc_uncollectable(sizeof(QuirkThreadState));
    memset(ts, 0, sizeof(*ts));
    ts->try_depth = -1;
    quirk_ts = ts;
    pthread_mutex_lock(&quirk_gil);
    jmp_buf* jb = (jmp_buf*)quirk_get_jmp_buf();
    if (_setjmp(*jb) == 0) {
        typedef void* (*Thunk0)(void* env, void* end);
        t->result = ((Thunk0)t->fn->fn)(t->fn->env, QUIRK_NOARG);
        quirk_pop_try();
    } else {
        // A throw pops the try stack before jumping here.
        t->exception = quirk_get_exception();
    }
    t->done = 1;
    // Succeeded, or failed and already awaited: nothing left to report.
    if (!t->exception || t->retrieved) quirk_tasks_remove(t);
    pthread_cond_broadcast(&t->finished);
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
    pthread_cond_init(&t->finished, NULL);
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

static List* quirk_task_new_list(void) {
    List* l = (List*)quirk_alloc(QT_LIST, sizeof(List));
    Core_Collections_List_List___init(l);
    return l;
}

static void* quirk_await_one(void* v) {
    if (quirk_type_of(v) != QT_TASK) return v;
    QuirkTask* t = (QuirkTask*)v;
    while (!t->done) pthread_cond_wait(&t->finished, &quirk_gil);   // releases the lock meanwhile
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
                while (!t->done) pthread_cond_wait(&t->finished, &quirk_gil);
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
    return quirk_await_one(v);
}

// `task.done()` — finished (successfully or not), without waiting.
int32_t quirk_task_done(void* v) {
    return quirk_type_of(v) == QT_TASK ? ((QuirkTask*)v)->done : 1;
}
