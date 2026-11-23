// ---------------------------------------------------------------------------
//  Files built into the program (`quirk build` with `embed = true`, or
//  `quirk bundle --embed`): the program registers its table at start-up
//  (a global constructor the bundler adds); reading a file — fs.read_text,
//  fs.is_file / exists / size, images, fonts, a window's icon — looks here
//  first, then on disk. A path matches relative to the program's folder or
//  the current one: "assets/logo.png", "./assets/logo.png" and
//  "<the program's folder>/assets/logo.png" are the same file.
// ---------------------------------------------------------------------------

typedef struct {
    const char* path;              // relative, with /
    const unsigned char* data;
    int64_t size;
} QuirkEmbedded;

static const QuirkEmbedded* q_embedded = NULL;
static int64_t q_embedded_n = 0;
static char q_embed_exe_dir[4096] = "";
static char q_embed_cwd[4096] = "";

String* Sys_executable(void);

void quirk_embed_register(const QuirkEmbedded* table, int64_t n) {
    q_embedded = table;
    q_embedded_n = n;
}

// `p` without `dir/` in front (case-folded on Windows), or NULL.
static const char* q_embed_strip(const char* p, const char* dir) {
    size_t n = strlen(dir);
    if (n == 0) return NULL;
#ifdef _WIN32
    if (_strnicmp(p, dir, n) != 0) return NULL;
#else
    if (strncmp(p, dir, n) != 0) return NULL;
#endif
    if (p[n] != '/') return NULL;
    return p + n + 1;
}

static void q_embed_slashes(char* s) {
    for (; *s; s++) if (*s == '\\') *s = '/';
}

// `s` (with /) without "." segments, "x/.." pairs or doubled slashes, in place.
static void q_embed_normalize(char* s) {
    char out[4096];
    size_t n = 0;
    int abs = s[0] == '/';
    size_t starts[512];
    int depth = 0;
    const char* p = s;
    while (*p) {
        while (*p == '/') p++;
        const char* e = p;
        while (*e && *e != '/') e++;
        size_t len = (size_t)(e - p);
        if (len == 0) break;
        if (len == 1 && p[0] == '.') { p = e; continue; }
        if (len == 2 && p[0] == '.' && p[1] == '.' && depth > 0) { n = starts[--depth]; p = e; continue; }
        if (depth < 512) starts[depth++] = n;
        if (n > 0 || abs) out[n++] = '/';
        if (n + len >= sizeof out) break;
        memcpy(out + n, p, len);
        n += len;
        p = e;
    }
    out[n] = '\0';
    if (n == 0 && abs) { out[0] = '/'; out[1] = '\0'; }
    strcpy(s, out);
}

// The embedded file at `path` (its bytes and size), or NULL.
const unsigned char* quirk_embedded(const char* path, int64_t* size) {
    if (!q_embedded || !path || !*path) return NULL;
    char key[4096];
    snprintf(key, sizeof key, "%s", path);
    q_embed_slashes(key);
    q_embed_normalize(key);
    const char* k = key;
    if (!q_embed_exe_dir[0]) {
        String* exe = Sys_executable();
        if (exe && exe->buffer) {
            snprintf(q_embed_exe_dir, sizeof q_embed_exe_dir, "%s", exe->buffer);
            q_embed_slashes(q_embed_exe_dir);
            char* slash = strrchr(q_embed_exe_dir, '/');
            if (slash) *slash = '\0';
            q_embed_normalize(q_embed_exe_dir);
        }
    }
    const char* rel = q_embed_strip(k, q_embed_exe_dir);
    if (!rel) {
        if (!q_embed_cwd[0]) {
#ifdef _WIN32
            if (_getcwd(q_embed_cwd, sizeof q_embed_cwd)) q_embed_slashes(q_embed_cwd);
#else
            if (!getcwd(q_embed_cwd, sizeof q_embed_cwd)) q_embed_cwd[0] = '\0';
#endif
            q_embed_normalize(q_embed_cwd);
        }
        rel = q_embed_strip(k, q_embed_cwd);
    }
    if (!rel) rel = k;
    for (int64_t i = 0; i < q_embedded_n; i++) {
#ifdef _WIN32
        if (_stricmp(q_embedded[i].path, rel) == 0) {
#else
        if (strcmp(q_embedded[i].path, rel) == 0) {
#endif
            if (size) *size = q_embedded[i].size;
            return q_embedded[i].data;
        }
    }
    return NULL;
}

// True if some embedded file is inside the folder `path`.
int quirk_embedded_dir(const char* path) {
    if (!q_embedded || !path) return 0;
    quirk_embedded(path, NULL);                     // (finds the program's folder, the current one)
    char key[4096];
    snprintf(key, sizeof key, "%s", path);
    q_embed_slashes(key);
    q_embed_normalize(key);
    const char* k = key;
    if (q_embed_exe_dir[0]) { const char* r = q_embed_strip(k, q_embed_exe_dir); if (r) k = r; }
    if (q_embed_cwd[0]) { const char* r = q_embed_strip(k, q_embed_cwd); if (r) k = r; }
    size_t kn = strlen(k);
    for (int64_t i = 0; i < q_embedded_n; i++)
        if (strncmp(q_embedded[i].path, k, kn) == 0 && q_embedded[i].path[kn] == '/') return 1;
    return 0;
}
