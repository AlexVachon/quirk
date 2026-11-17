// libs/live.c — run-time reflection for the `live` package: a struct's
// fields and methods by name (templates read `{{ count }}` and run
// `@click="reset"`). Backed by the per-struct tables Codegen generates
// (runtime.c quirk_reflect).

void* quirk_reflect(void* obj, int32_t op, const char* name, void** args, int64_t n);
extern void quirk_throw_exception(const char* type_name, const char* message);
const char* quirk_reflect_names(void* obj, int which);
void* quirk_reflect_new(const char* name);
const char* quirk_reflect_type_name(void* obj);
const char* quirk_reflect_file(void* obj);

static const char* live_cname(String* s) { return s && s->buffer ? s->buffer : ""; }

// Bool externs return int32_t (widened for the C ABI).
int32_t Live_has_field(void* obj, String* name) {
    return quirk_reflect(obj, 3, live_cname(name), NULL, 0) != QUIRK_NOARG;
}

int32_t Live_has_method(void* obj, String* name) {
    return quirk_reflect(obj, 4, live_cname(name), NULL, 0) != QUIRK_NOARG;
}

// The field's value (null when there's no such field — check has_field).
void* Live_get_field(void* obj, String* name) {
    void* v = quirk_reflect(obj, 0, live_cname(name), NULL, 0);
    return v == QUIRK_NOARG ? NULL : v;
}

int32_t Live_set_field(void* obj, String* name, void* value) {
    void* args[1] = { value };
    return quirk_reflect(obj, 1, live_cname(name), args, 1) != QUIRK_NOARG;
}

// Call method `name` with `args` (missing ones take their defaults).
void* Live_call_method(void* obj, String* name, List* args) {
    int64_t n = args ? args->size : 0;
    void* v = quirk_reflect(obj, 2, live_cname(name), args ? args->data : NULL, n);
    if (v == QUIRK_NOARG) {
        char buf[256];
        snprintf(buf, sizeof(buf), "no method '%s'", live_cname(name));
        quirk_throw_exception("ValueError", buf);
    }
    return v;
}

static List* live_split_names_typed(const char* s, int keep_types);
static List* live_split_names(const char* s) { return live_split_names_typed(s, 1); }

static List* live_split_names_typed(const char* s, int keep_types) {
    extern void Core_Collections_List_List___init(List*);
    extern void Core_Collections_List_List_append(List*, void*);
    List* out = (List*)quirk_alloc(QT_LIST, sizeof(List));
    Core_Collections_List_List___init(out);
    const char* p = s;
    while (p && *p) {
        const char* e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (!keep_types) {
            const char* colon = memchr(p, ':', len);
            if (colon) len = (size_t)(colon - p);
        }
        char* part = (char*)GC_malloc(len + 1);
        memcpy(part, p, len);
        part[len] = 0;
        Core_Collections_List_List_append(out, make_String(part));
        p = e ? e + 1 : NULL;
    }
    return out;
}

List* Live_field_names(void* obj) { return live_split_names_typed(quirk_reflect_names(obj, 0), 0); }

// The declared type of field `name` ("Int", "String", "List[Int]", ...; "" if unknown).
String* Live_field_type(void* obj, String* name) {
    const char* s = quirk_reflect_names(obj, 0);
    const char* want = live_cname(name);
    size_t wl = strlen(want);
    const char* p = s;
    while (p && *p) {
        const char* e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > wl && strncmp(p, want, wl) == 0 && p[wl] == ':') {
            char buf[256];
            size_t tl = len - wl - 1;
            if (tl >= sizeof(buf)) tl = sizeof(buf) - 1;
            memcpy(buf, p + wl + 1, tl);
            buf[tl] = 0;
            return make_String(buf);
        }
        p = e ? e + 1 : NULL;
    }
    return make_String("");
}
List* Live_method_names(void* obj) { return live_split_names(quirk_reflect_names(obj, 1)); }

// A new struct of type `name` (no constructor arguments); null if none.
void* Live_new_struct(String* name) { return quirk_reflect_new(live_cname(name)); }
String* Live_type_name(void* obj) { return make_String(quirk_reflect_type_name(obj)); }
String* Live_source_file(void* obj) { return make_String(quirk_reflect_file(obj)); }
