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
//  Filesystem runtime functions exported to libs/fs/index.qk.
//  Naming: Fs_<name>  (matches `linkageName = "Fs_<name>"` from
//  Parser::computeModulePrefix for libs/fs/index.qk).
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
    List* l = (List*)GC_malloc(sizeof(List));
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
int Fs_size(String* path) {
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
int Fs_mtime(String* path) {
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
int Fs_mkdir_raw(String* path, int parents) {
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

int Fs_rmdir_raw(String* path) {
    char* p = make_safe_cstr(path);
    if (!p) return -1;
#ifdef _WIN32
    return _rmdir(p) == 0 ? 0 : -1;
#else
    return rmdir(p) == 0 ? 0 : -1;
#endif
}

int Fs_remove_raw(String* path) {
    char* p = make_safe_cstr(path);
    if (!p) return -1;
    return remove(p) == 0 ? 0 : -1;
}

int Fs_rename_raw(String* from, String* to) {
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

int Fs_chdir_raw(String* path) {
    char* p = make_safe_cstr(path);
    if (!p) return -1;
#ifdef _WIN32
    return _chdir(p) == 0 ? 0 : -1;
#else
    return chdir(p) == 0 ? 0 : -1;
#endif
}
