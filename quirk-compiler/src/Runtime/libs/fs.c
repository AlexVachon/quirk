#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gc.h>
#include "../types.h"

#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #include <io.h>
    #include <sys/stat.h>
    #ifndef S_ISDIR
        #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
    #endif
    #ifndef S_ISREG
        #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
    #endif
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <dirent.h>
    #include <errno.h>
#endif

// ---------------------------------------------------------------------------
//  Filesystem runtime functions exported to libs/fs/index.quirk.
//  Naming: Fs_<name>  (matches `linkageName = "Fs_<name>"` from
//  Parser::computeModulePrefix for libs/fs/index.quirk).
//
//  Design notes:
//    - All char* paths from Quirk arrive as `String*`; we extract via
//      make_safe_cstr so embedded NULs / non-NUL-terminated buffers stay safe.
//    - Bool returns use int (Quirk's C ABI for Bool is i32).
//    - List<String> returns build a fresh List and append entries.
// ---------------------------------------------------------------------------

// list.c is included before fs.c in the unity build, so List___init and
// List_append are already in scope — no `extern` needed (and conflicting
// types if we tried).
static List* fs__new_list(void) {
    List* l = (List*)quirk_alloc(QT_LIST, sizeof(List));
    Core_Collections_List_List___init(l);
    return l;
}

// True if anything exists at `path` (file, directory, symlink, ...).
int Fs_exists(String* path) {
    char* p = make_safe_cstr(path);
    if (!p) return 0;
    struct stat st;
#ifdef _WIN32
    int ok = (_stat(p, (struct _stat*)&st) == 0);
#else
    int ok = (stat(p, &st) == 0);
#endif
    return ok ? 1 : 0;
}

int Fs_is_dir(String* path) {
    char* p = make_safe_cstr(path);
    if (!p) return 0;
    struct stat st;
#ifdef _WIN32
    if (_stat(p, (struct _stat*)&st) != 0) return 0;
#else
    if (stat(p, &st) != 0) return 0;
#endif
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int Fs_is_file(String* path) {
    char* p = make_safe_cstr(path);
    if (!p) return 0;
    struct stat st;
#ifdef _WIN32
    if (_stat(p, (struct _stat*)&st) != 0) return 0;
#else
    if (stat(p, &st) != 0) return 0;
#endif
    return S_ISREG(st.st_mode) ? 1 : 0;
}

// Size of the file at `path` in bytes. -1 if it can't be stat'd.
int64_t Fs_size(String* path) {
    char* p = make_safe_cstr(path);
    if (!p) return -1;
    struct stat st;
#ifdef _WIN32
    if (_stat(p, (struct _stat*)&st) != 0) return -1;
#else
    if (stat(p, &st) != 0) return -1;
#endif
    return (int)st.st_size;
}

// Last modification time as Unix epoch seconds. -1 on failure.
int64_t Fs_mtime(String* path) {
    char* p = make_safe_cstr(path);
    if (!p) return -1;
    struct stat st;
#ifdef _WIN32
    if (_stat(p, (struct _stat*)&st) != 0) return -1;
#else
    if (stat(p, &st) != 0) return -1;
#endif
    return (int)st.st_mtime;
}

// Create directory `path`. `parents == 1` makes intermediate dirs (mkdir -p).
// Returns 0 on success, -1 on error.
int64_t Fs_mkdir_raw(String* path, int64_t parents) {
    char* p = make_safe_cstr(path);
    if (!p || !*p) return -1;
    if (!parents) {
#ifdef _WIN32
        return _mkdir(p) == 0 ? 0 : -1;
#else
        return mkdir(p, 0755) == 0 ? 0 : -1;
#endif
    }
    // mkdir -p: walk path, creating each segment.
    char* buf = (char*)GC_malloc(strlen(p) + 1);
    strcpy(buf, p);
    for (char* slash = buf + 1; *slash; slash++) {
        if (*slash == '/' || *slash == '\\') {
            char saved = *slash;
            *slash = '\0';
#ifdef _WIN32
            _mkdir(buf);
#else
            mkdir(buf, 0755);
#endif
            *slash = saved;
        }
    }
#ifdef _WIN32
    int r = _mkdir(buf);
#else
    int r = mkdir(buf, 0755);
#endif
    // Treat "already exists" as success — matches `mkdir -p` semantics.
    if (r != 0) {
        struct stat st;
#ifdef _WIN32
        if (_stat(buf, (struct _stat*)&st) == 0 && S_ISDIR(st.st_mode)) return 0;
#else
        if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
#endif
        return -1;
    }
    return 0;
}

int64_t Fs_rmdir_raw(String* path) {
    char* p = make_safe_cstr(path);
    if (!p) return -1;
#ifdef _WIN32
    return _rmdir(p) == 0 ? 0 : -1;
#else
    return rmdir(p) == 0 ? 0 : -1;
#endif
}

int64_t Fs_remove_raw(String* path) {
    char* p = make_safe_cstr(path);
    if (!p) return -1;
    return remove(p) == 0 ? 0 : -1;
}

#ifndef _WIN32
// Delete `p` and, for a directory, everything under it. Symlinks are
// removed themselves, never followed (lstat), as `rm -rf` does.
static int fs__remove_tree(const char* p) {
    struct stat st;
    if (lstat(p, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p) == 0 ? 0 : -1;
    DIR* d = opendir(p);
    if (!d) return -1;
    int rc = 0;
    struct dirent* e;
    size_t plen = strlen(p);
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        size_t n = plen + 1 + strlen(e->d_name) + 1;
        char* child = (char*)malloc(n);
        snprintf(child, n, "%s/%s", p, e->d_name);
        if (fs__remove_tree(child) != 0) rc = -1;
        free(child);
    }
    closedir(d);
    if (rc == 0 && rmdir(p) != 0) rc = -1;
    return rc;
}
#endif

#ifdef _WIN32
// Delete `p` and everything under it. Read-only files are made writable
// first; a symlinked / junction directory is removed itself, never
// followed. 0 ok, -1 if anything couldn't be removed.
static int fs__win_remove_tree(const char* p) {
    DWORD attr = GetFileAttributesA(p);
    if (attr == INVALID_FILE_ATTRIBUTES) return -1;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(p, attr & ~FILE_ATTRIBUTE_READONLY);
        return DeleteFileA(p) ? 0 : -1;
    }
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryA(p) ? 0 : -1;
    size_t pl = strlen(p);
    char* pattern = (char*)GC_malloc_atomic(pl + 3);
    memcpy(pattern, p, pl);
    memcpy(pattern + pl, "\\*", 3);
    int rc = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const char* nm = fd.cFileName;
            if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
            size_t nl = strlen(nm);
            char* child = (char*)GC_malloc_atomic(pl + nl + 2);
            memcpy(child, p, pl);
            child[pl] = '\\';
            memcpy(child + pl + 1, nm, nl + 1);
            if (fs__win_remove_tree(child) != 0) rc = -1;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(p, attr & ~FILE_ATTRIBUTE_READONLY);
    if (!RemoveDirectoryA(p)) rc = -1;
    return rc;
}
#endif

int64_t Fs_remove_tree_raw(String* path) {
    char* p = make_safe_cstr(path);
    if (!p || !*p) return -1;
#ifdef _WIN32
    return fs__win_remove_tree(p);
#else
    return fs__remove_tree(p);
#endif
}

int64_t Fs_rename_raw(String* from, String* to) {
    char* a = make_safe_cstr(from);
    char* b = make_safe_cstr(to);
    if (!a || !b) return -1;
    return rename(a, b) == 0 ? 0 : -1;
}

// List directory entries (NOT recursive). Skips "." and "..".
// Returns a List<String> on success, an empty List on error.
List* Fs_list_dir(String* path) {
    char* p = make_safe_cstr(path);
    List* result = fs__new_list();
    if (!p) return result;

#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", p);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return result;
    do {
        const char* name = fd.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        Core_Collections_List_List_append(result, make_String((char*)name));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(p);
    if (!d) return result;
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        Core_Collections_List_List_append(result, make_String((char*)name));
    }
    closedir(d);
#endif
    return result;
}

// Returns the current working directory as a String, or "" on error.
String* Fs_cwd() {
    char buf[4096];
#ifdef _WIN32
    if (_getcwd(buf, sizeof(buf)) == NULL) return make_String("");
#else
    if (getcwd(buf, sizeof(buf)) == NULL) return make_String("");
#endif
    return make_String(buf);
}

int64_t Fs_chdir_raw(String* path) {
    char* p = make_safe_cstr(path);
    if (!p) return -1;
#ifdef _WIN32
    return _chdir(p) == 0 ? 0 : -1;
#else
    return chdir(p) == 0 ? 0 : -1;
#endif
}

// ---------------------------------------------------------------------------
//  v7.19.0: links, realpath, glob matching, recursive walk, temp files,
//  copy, whole-file read / write. POSIX only; the Windows build gets
//  failure stubs.
// ---------------------------------------------------------------------------
#ifndef _WIN32
#include <fnmatch.h>
#include <fcntl.h>
#include <limits.h>
#endif

// True if `path` itself is a symbolic link (not followed).
int Fs_is_link(String* path) {
    char* p = make_safe_cstr(path);
    if (!p) return 0;
#ifdef _WIN32
    return 0;
#else
    struct stat st;
    return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

// Absolute path with symlinks, `.` and `..` resolved; "" if it doesn't exist.
String* Fs_realpath(String* path) {
    char* p = make_safe_cstr(path);
    if (!p) return make_String("");
#ifdef _WIN32
    char buf[1024];
    // _fullpath doesn't check the path exists; realpath does.
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return make_String("");
    if (!_fullpath(buf, p, sizeof buf)) return make_String("");
    return make_String(buf);
#else
    char buf[PATH_MAX];
    if (!realpath(p, buf)) return make_String("");
    return make_String(buf);
#endif
}

// Shell-style match of one name against `pattern` (`*`, `?`, `[a-z]`,
// `[!x]`). Unless `hidden`, a leading `.` must be matched explicitly, as
// in the shell.
#ifdef _WIN32
// fnmatch for one name (Windows has none): `*`, `?`, `[abc]` / `[a-z]` /
// `[!x]` (or `[^x]`), `\x` for a literal x. With `period`, a leading `.`
// only matches a literal `.` in the pattern.
static int fs__wildmatch(const char* p, const char* s, int period) {
    if (period && *s == '.' && *p != '.') return 0;
    for (; *p; p++, s++) {
        switch (*p) {
            case '*': {
                while (*p == '*') p++;
                if (!*p) return 1;
                for (; *s; s++) if (fs__wildmatch(p, s, 0)) return 1;
                return 0;
            }
            case '?':
                if (!*s) return 0;
                break;
            case '[': {
                if (!*s) return 0;
                const char* q = p + 1;
                int negate = (*q == '!' || *q == '^');
                if (negate) q++;
                int matched = 0;
                unsigned char c = (unsigned char)*s;
                do {
                    unsigned char lo = (unsigned char)*q, hi = lo;
                    if (q[1] == '-' && q[2] && q[2] != ']') { hi = (unsigned char)q[2]; q += 2; }
                    if (c >= lo && c <= hi) matched = 1;
                    q++;
                } while (*q && *q != ']');
                if (*q != ']') { if ((unsigned char)*p != c) return 0; break; }   // no closing ]: literal '['
                if (matched == negate) return 0;
                p = q;
                break;
            }
            case '\\':
                if (p[1]) p++;
                /* fallthrough */
            default:
                if (*p != *s) return 0;
        }
    }
    return *s == '\0';
}
#endif

int Fs_match_raw(String* pattern, String* name, int64_t hidden) {
    char* pt = make_safe_cstr(pattern);
    char* nm = make_safe_cstr(name);
    if (!pt || !nm) return 0;
#ifdef _WIN32
    return fs__wildmatch(pt, nm, !hidden);
#else
    return fnmatch(pt, nm, hidden ? 0 : FNM_PERIOD) == 0;
#endif
}

static int fs__cmp_names(const void* a, const void* b) {
    return strcmp(*(char* const*)a, *(char* const*)b);
}

#ifdef _WIN32
// Sorted entry names of `dir` (no `.` / `..`); NULL if it can't be read.
static char** fs__sorted_names(const char* dir, size_t* count) {
    size_t dl = strlen(dir);
    char* pattern = (char*)GC_malloc_atomic(dl + 3);
    memcpy(pattern, dir, dl);
    memcpy(pattern + dl, "\\*", 3);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    size_t n = 0, cap = 32;
    char** names = (char**)GC_malloc(cap * sizeof(char*));
    do {
        const char* nm = fd.cFileName;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
        if (n == cap) {
            char** grown = (char**)GC_malloc(cap * 2 * sizeof(char*));
            memcpy(grown, names, n * sizeof(char*));
            names = grown;
            cap *= 2;
        }
        size_t len = strlen(nm);
        names[n] = (char*)GC_malloc_atomic(len + 1);
        memcpy(names[n], nm, len + 1);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, n, sizeof(char*), fs__cmp_names);
    *count = n;
    return names;
}
#define lstat stat     // no symlink distinction in the walk on Windows
#else
// Sorted entry names of `dir` (no `.` / `..`); NULL if it can't be read.
static char** fs__sorted_names(const char* dir, size_t* count) {
    DIR* d = opendir(dir);
    if (!d) return NULL;
    size_t n = 0, cap = 32;
    char** names = (char**)GC_malloc(cap * sizeof(char*));
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (n == cap) {
            char** grown = (char**)GC_malloc(cap * 2 * sizeof(char*));
            memcpy(grown, names, n * sizeof(char*));
            names = grown;
            cap *= 2;
        }
        size_t len = strlen(e->d_name);
        names[n] = (char*)GC_malloc_atomic(len + 1);
        memcpy(names[n], e->d_name, len + 1);
        n++;
    }
    closedir(d);
    qsort(names, n, sizeof(char*), fs__cmp_names);
    *count = n;
    return names;
}
#endif

static char* fs__join(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    int slash = la > 0 && a[la - 1] != '/';
    char* out = (char*)GC_malloc_atomic(la + slash + lb + 1);
    memcpy(out, a, la);
    if (slash) out[la] = '/';
    memcpy(out + la + slash, b, lb + 1);
    return out;
}

// Depth-first, names sorted in each directory, directories listed before
// their contents. `depth` -1 = unlimited.
static void fs__walk(const char* dir, List* out, int dirs, int files, int follow,
                     int64_t depth, int hidden) {
    size_t n = 0;
    char** names = fs__sorted_names(dir, &n);
    if (!names) return;
    for (size_t i = 0; i < n; i++) {
        if (!hidden && names[i][0] == '.') continue;
        char* full = fs__join(dir, names[i]);
        struct stat st;
        if ((follow ? stat(full, &st) : lstat(full, &st)) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (dirs) Core_Collections_List_List_append(out, make_String(full));
            if (depth != 0) fs__walk(full, out, dirs, files, follow, depth < 0 ? -1 : depth - 1, hidden);
        } else if (files) {
            Core_Collections_List_List_append(out, make_String(full));
        }
    }
}

// Every path under `root` (not `root` itself), as `root/…` strings.
// flags: 1 files, 2 directories, 4 follow symlinked directories,
// 8 include hidden (dot) entries. `max_depth` -1 = unlimited, 0 = only
// `root`'s own entries.
List* Fs_walk_raw(String* root, int64_t flags, int64_t max_depth) {
    List* out = fs__new_list();
    char* r = make_safe_cstr(root);
    if (!r) return out;
    fs__walk(r, out, (flags & 2) != 0, (flags & 1) != 0, (flags & 4) != 0, max_depth, (flags & 8) != 0);
    return out;
}

static const char* fs__tmp_root(void) {
#ifdef _WIN32
    static char buf[MAX_PATH + 1];
    DWORD n = GetTempPathA(sizeof buf, buf);
    if (n == 0 || n > MAX_PATH) return ".";
    while (n > 3 && (buf[n - 1] == '\\' || buf[n - 1] == '/')) buf[--n] = '\0';
    return buf;
#else
    const char* t = getenv("TMPDIR");
    return (t && *t) ? t : "/tmp";
#endif
}

#ifdef _WIN32
// `<dir>/<prefix>` + 6 random [a-z0-9] + `<suffix>` (a candidate name).
static char* fs__win_temp_name(const char* dir, const char* prefix, const char* suffix) {
    static unsigned long long counter = 0;
    unsigned long long seed = GetTickCount64() ^ ((unsigned long long)GetCurrentProcessId() << 32) ^ (++counter * 0x9E3779B97F4A7C15ULL);
    char rnd[7];
    for (int i = 0; i < 6; i++) { rnd[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[seed % 36]; seed = seed / 36 + (seed << 7) + 0x2545F4914F6CDD1DULL; }
    rnd[6] = '\0';
    char* base = fs__join(dir, prefix);
    size_t lb = strlen(base), ls = strlen(suffix);
    char* out = (char*)GC_malloc_atomic(lb + 6 + ls + 1);
    memcpy(out, base, lb);
    memcpy(out + lb, rnd, 6);
    memcpy(out + lb + 6, suffix, ls + 1);
    return out;
}
#endif

// Directory temp files go in ($TMPDIR, else /tmp).
String* Fs_temp_root() {
    return make_String((char*)fs__tmp_root());
}

// Create a fresh, private (0700) directory `<dir>/<prefix>XXXXXX`; "" on
// failure. `dir` "" = the temp root.
String* Fs_temp_dir_raw(String* dir, String* prefix) {
#ifdef _WIN32
    char* d = make_safe_cstr(dir);
    char* pre = make_safe_cstr(prefix);
    if (!d || !*d) d = (char*)fs__tmp_root();
    for (int attempt = 0; attempt < 100; attempt++) {
        char* path = fs__win_temp_name(d, pre ? pre : "", "");
        if (CreateDirectoryA(path, NULL)) return make_String(path);
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    return make_String("");
#else
    char* d = make_safe_cstr(dir);
    char* pre = make_safe_cstr(prefix);
    if (!d || !*d) d = (char*)fs__tmp_root();
    if (!pre) pre = "";
    char* tmpl = fs__join(d, pre);
    size_t len = strlen(tmpl);
    char* buf = (char*)GC_malloc_atomic(len + 7);
    memcpy(buf, tmpl, len);
    memcpy(buf + len, "XXXXXX", 7);
    if (!mkdtemp(buf)) return make_String("");
    return make_String(buf);
#endif
}

// Create a fresh, empty (0600) file `<dir>/<prefix>XXXXXX<suffix>`; "" on
// failure.
String* Fs_temp_file_raw(String* dir, String* prefix, String* suffix) {
#ifdef _WIN32
    char* d = make_safe_cstr(dir);
    char* pre = make_safe_cstr(prefix);
    char* suf = make_safe_cstr(suffix);
    if (!d || !*d) d = (char*)fs__tmp_root();
    for (int attempt = 0; attempt < 100; attempt++) {
        char* path = fs__win_temp_name(d, pre ? pre : "", suf ? suf : "");
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return make_String(path); }
        if (GetLastError() != ERROR_FILE_EXISTS) break;
    }
    return make_String("");
#else
    char* d = make_safe_cstr(dir);
    char* pre = make_safe_cstr(prefix);
    char* suf = make_safe_cstr(suffix);
    if (!d || !*d) d = (char*)fs__tmp_root();
    if (!pre) pre = "";
    if (!suf) suf = "";
    char* tmpl = fs__join(d, pre);
    size_t lt = strlen(tmpl), ls = strlen(suf);
    char* buf = (char*)GC_malloc_atomic(lt + 6 + ls + 1);
    memcpy(buf, tmpl, lt);
    memcpy(buf + lt, "XXXXXX", 6);
    memcpy(buf + lt + 6, suf, ls + 1);
    int fd = mkstemps(buf, (int)ls);
    if (fd < 0) return make_String("");
    close(fd);
    return make_String(buf);
#endif
}

// Copy file `src` to `dst` (replacing it), keeping the permission bits.
// 0 on success, -1 on failure.
int64_t Fs_copy_raw(String* src, String* dst) {
#ifdef _WIN32
    char* s = make_safe_cstr(src);
    char* d = make_safe_cstr(dst);
    if (!s || !d) return -1;
    return CopyFileA(s, d, FALSE) ? 0 : -1;
#else
    char* s = make_safe_cstr(src);
    char* d = make_safe_cstr(dst);
    if (!s || !d) return -1;
    int in = open(s, O_RDONLY);
    if (in < 0) return -1;
    struct stat st;
    if (fstat(in, &st) != 0 || S_ISDIR(st.st_mode)) { close(in); return -1; }
    int out = open(d, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        char* p = buf;
        while (n > 0) {
            ssize_t w = write(out, p, (size_t)n);
            if (w < 0) { if (errno == EINTR) continue; rc = -1; break; }
            p += w;
            n -= w;
        }
        if (rc) break;
    }
    if (n < 0) rc = -1;
    fchmod(out, st.st_mode & 07777);
    if (close(out) != 0) rc = -1;
    close(in);
    return rc;
#endif
}

// The whole file as a String, or null if it can't be read.
String* Fs_read_text_raw(String* path) {
    char* p = make_safe_cstr(path);
    if (!p) return NULL;
    FILE* f = fopen(p, "rb");
    if (!f) return NULL;
    size_t cap = 65536, len = 0;
    char* buf = (char*)GC_malloc_atomic(cap);
    for (;;) {
        if (len + 4096 + 1 > cap) {
            char* grown = (char*)GC_malloc_atomic(cap * 2);
            memcpy(grown, buf, len);
            buf = grown;
            cap *= 2;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0) break;
    }
    int err = ferror(f);
    fclose(f);
    if (err) return NULL;
    buf[len] = '\0';
    return make_String_taking_ownership(buf);
}

// Write `data` to `path` (replacing it, or appending with `append`).
// 0 on success, -1 on failure.
int64_t Fs_write_text_raw(String* path, String* data, int64_t append) {
    char* p = make_safe_cstr(path);
    if (!p) return -1;
    FILE* f = fopen(p, append ? "ab" : "wb");
    if (!f) return -1;
    size_t len = (data && data->buffer) ? (size_t)data->length : 0;
    int rc = (len == 0 || fwrite(data->buffer, 1, len, f) == len) ? 0 : -1;
    if (fclose(f) != 0) rc = -1;
    return rc;
}
