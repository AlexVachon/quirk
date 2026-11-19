// The `sqlite` package's runtime side: a thin layer over SQLite's C API
// (src/third_party/sqlite, linked into the runtime as its own object).
//
// Connections and statements cross into Quirk as Ints (the pointers). The
// Quirk side (packages/sqlite) checks types, binds parameters one by one
// with the typed Sqlite_bind_* calls and turns error codes into
// sqlite.Error; this file never throws.
//
// Calls that can wait on the disk or on another connection's lock
// (exec, step, close) release the scheduler lock meanwhile, so other async
// tasks run. SQLite is built thread-safe (SQLITE_THREADSAFE=1).

#include "../../third_party/sqlite/sqlite3.h"

extern String* make_String(const char* raw);
extern String* make_String_len(char* raw, int64_t len);   // takes `raw` over
extern Any* Core_Primitives_Any_box_int(int64_t v);
extern Any* Core_Primitives_Any_box_double(double v);
extern Any* Core_Primitives_Any_box_null(void);

static const char* Sqlite__cstr(String* s) {
    return (s && s->buffer) ? s->buffer : "";
}

// Why the last open() failed (per thread: tasks are threads).
static __thread char sqlite__open_error[512];

// The open flags, as Quirk passes them: 0 read-write, create if missing;
// 1 read-only; 2 read-write, must exist.
int64_t Sqlite_open_raw(String* path, int64_t mode, int64_t busy_ms) {
    int flags = SQLITE_OPEN_URI;
    if (mode == 1)      flags |= SQLITE_OPEN_READONLY;
    else if (mode == 2) flags |= SQLITE_OPEN_READWRITE;
    else                flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    sqlite3* db = NULL;
    quirk_gil_release();
    int rc = sqlite3_open_v2(Sqlite__cstr(path), &db, flags, NULL);
    quirk_gil_acquire();
    if (rc != SQLITE_OK) {
        snprintf(sqlite__open_error, sizeof(sqlite__open_error), "%s",
                 db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close_v2(db);
        return 0;
    }
    sqlite3_extended_result_codes(db, 1);
    if (busy_ms > 0) sqlite3_busy_timeout(db, (int)busy_ms);
    sqlite__open_error[0] = 0;
    return (int64_t)(intptr_t)db;
}

String* Sqlite_open_error(void) {
    return make_String(sqlite__open_error);
}

int64_t Sqlite_close_raw(int64_t db) {
    if (!db) return SQLITE_OK;
    quirk_gil_release();
    int rc = sqlite3_close_v2((sqlite3*)(intptr_t)db);      // waits for nothing: frees once statements are done
    quirk_gil_acquire();
    return rc;
}

String* Sqlite_errmsg(int64_t db) {
    if (!db) return make_String("not open");
    return make_String(sqlite3_errmsg((sqlite3*)(intptr_t)db));
}

int64_t Sqlite_errcode(int64_t db) {
    return db ? sqlite3_extended_errcode((sqlite3*)(intptr_t)db) : SQLITE_MISUSE;
}

String* Sqlite_errstr(int64_t rc) {
    return make_String(sqlite3_errstr((int)rc));
}

// Several statements, no parameters (a schema, a migration).
int64_t Sqlite_exec_raw(int64_t db, String* sql) {
    quirk_gil_release();
    int rc = sqlite3_exec((sqlite3*)(intptr_t)db, Sqlite__cstr(sql), NULL, NULL, NULL);
    quirk_gil_acquire();
    return rc;
}

// Is the rest of an SQL text only whitespace, `;` and comments?
static int Sqlite__only_blank(const char* p) {
    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ';') { p++; continue; }
        if (p[0] == '-' && p[1] == '-') { while (*p && *p != '\n') p++; continue; }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }
        return 0;
    }
    return 1;
}

// One statement: its handle; 0 when SQLite refused it (Sqlite_errmsg says
// why); -1 when the text holds more than one statement.
int64_t Sqlite_prepare_raw(int64_t db, String* sql) {
    sqlite3_stmt* st = NULL;
    const char* tail = NULL;
    int rc = sqlite3_prepare_v2((sqlite3*)(intptr_t)db, Sqlite__cstr(sql), -1, &st, &tail);
    if (rc != SQLITE_OK) return 0;
    if (tail && !Sqlite__only_blank(tail)) {
        sqlite3_finalize(st);
        return -1;
    }
    if (!st) return -2;                                      // only blanks / comments
    return (int64_t)(intptr_t)st;
}

int64_t Sqlite_finalize_raw(int64_t st) {
    return st > 0 ? sqlite3_finalize((sqlite3_stmt*)(intptr_t)st) : SQLITE_OK;
}

int64_t Sqlite_reset_raw(int64_t st) {
    sqlite3_stmt* s = (sqlite3_stmt*)(intptr_t)st;
    int rc = sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    return rc;
}

int64_t Sqlite_param_count(int64_t st) {
    return sqlite3_bind_parameter_count((sqlite3_stmt*)(intptr_t)st);
}

// The 1-based index of `:name` / `@name` / `$name`, 0 if the statement has none.
int64_t Sqlite_param_index(int64_t st, String* name) {
    return sqlite3_bind_parameter_index((sqlite3_stmt*)(intptr_t)st, Sqlite__cstr(name));
}

String* Sqlite_param_name(int64_t st, int64_t i) {
    const char* n = sqlite3_bind_parameter_name((sqlite3_stmt*)(intptr_t)st, (int)i);
    return make_String(n ? n : "");
}

int64_t Sqlite_bind_int(int64_t st, int64_t i, int64_t v) {
    return sqlite3_bind_int64((sqlite3_stmt*)(intptr_t)st, (int)i, (sqlite3_int64)v);
}

int64_t Sqlite_bind_double(int64_t st, int64_t i, double v) {
    return sqlite3_bind_double((sqlite3_stmt*)(intptr_t)st, (int)i, v);
}

int64_t Sqlite_bind_text(int64_t st, int64_t i, String* v) {
    // SQLITE_TRANSIENT: SQLite copies it (the String may move or be freed).
    return sqlite3_bind_text64((sqlite3_stmt*)(intptr_t)st, (int)i,
                               v && v->buffer ? v->buffer : "", v ? (sqlite3_uint64)v->length : 0,
                               SQLITE_TRANSIENT, SQLITE_UTF8);
}

int64_t Sqlite_bind_null(int64_t st, int64_t i) {
    return sqlite3_bind_null((sqlite3_stmt*)(intptr_t)st, (int)i);
}

// SQLITE_ROW (100), SQLITE_DONE (101), or an error code.
int64_t Sqlite_step_raw(int64_t st) {
    quirk_gil_release();
    int rc = sqlite3_step((sqlite3_stmt*)(intptr_t)st);
    quirk_gil_acquire();
    return rc;
}

int64_t Sqlite_column_count(int64_t st) {
    return sqlite3_column_count((sqlite3_stmt*)(intptr_t)st);
}

String* Sqlite_column_name(int64_t st, int64_t i) {
    const char* n = sqlite3_column_name((sqlite3_stmt*)(intptr_t)st, (int)i);
    return make_String(n ? n : "");
}

// `n` bytes copied into a String of its own (make_String_len takes the
// buffer over, and SQLite reuses its own on the next step).
static String* Sqlite__copy(const void* p, int n) {
    char* buf = (char*)GC_malloc_atomic((size_t)n + 1);
    if (n > 0) memcpy(buf, p, (size_t)n);
    buf[n] = 0;
    return make_String_len(buf, n);
}

// A column of the current row, as Quirk reads untyped values: INTEGER → Int,
// REAL → Double, TEXT → String, BLOB → String (its bytes), NULL → null.
static void* Sqlite__value(sqlite3_stmt* s, int i) {
    switch (sqlite3_column_type(s, i)) {
        case SQLITE_INTEGER: return Core_Primitives_Any_box_int((int64_t)sqlite3_column_int64(s, i));
        case SQLITE_FLOAT:   return Core_Primitives_Any_box_double(sqlite3_column_double(s, i));
        case SQLITE_TEXT: {
            const unsigned char* t = sqlite3_column_text(s, i);
            int n = sqlite3_column_bytes(s, i);
            return Sqlite__copy(t ? (const void*)t : "", t ? n : 0);
        }
        case SQLITE_BLOB: {
            const void* b = sqlite3_column_blob(s, i);
            int n = sqlite3_column_bytes(s, i);
            return Sqlite__copy(b ? b : "", b ? n : 0);
        }
        default: return NULL;
    }
}

void* Sqlite_column_value(int64_t st, int64_t i) {
    return Sqlite__value((sqlite3_stmt*)(intptr_t)st, (int)i);
}

// The current row: column name → value, in the columns' order.
Map* Sqlite_row_raw(int64_t st) {
    extern void Core_Collections_Map_Map___init(Map*);
    extern void Core_Collections_Map_Map_put(Map*, String*, void*);
    sqlite3_stmt* s = (sqlite3_stmt*)(intptr_t)st;
    Map* m = (Map*)quirk_alloc(QT_MAP, sizeof(Map));
    Core_Collections_Map_Map___init(m);
    int n = sqlite3_column_count(s);
    for (int i = 0; i < n; i++) {
        const char* name = sqlite3_column_name(s, i);
        Core_Collections_Map_Map_put(m, make_String(name ? name : ""), Sqlite__value(s, i));
    }
    return m;
}

int64_t Sqlite_changes(int64_t db) {
    return sqlite3_changes64((sqlite3*)(intptr_t)db);
}

int64_t Sqlite_last_insert_rowid(int64_t db) {
    return sqlite3_last_insert_rowid((sqlite3*)(intptr_t)db);
}

int64_t Sqlite_in_transaction(int64_t db) {
    return !sqlite3_get_autocommit((sqlite3*)(intptr_t)db);
}

String* Sqlite_version(void) {
    return make_String(sqlite3_libversion());
}
