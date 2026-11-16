/* Screen readers on Windows: a UI Automation provider for a window's
 * accessibility tree (included by window.c on _WIN32 only).
 *
 * `ui` sends the tree it builds (App.accessibility()) each frame while a
 * UIA client listens — Window_access_update_raw — as flat arrays: id,
 * parent index, role, name, value, description, states, box, range. This
 * file keeps a copy per window (behind a lock: UIA calls providers on its
 * own threads) and answers UIA from it:
 *   - WM_GETOBJECT (UiaRootObjectId) on SDL's window — reached by
 *     subclassing its HWND — returns the root fragment;
 *   - each node is an IRawElementProviderSimple / Fragment (the root also
 *     a FragmentRoot) with properties (control type, name, help text,
 *     focus, enabled, bounding box) and patterns: Invoke, Toggle, Value,
 *     RangeValue, SelectionItem, ExpandCollapse;
 *   - pattern calls (a screen reader pressing a button, setting a field)
 *     are queued as QEV_ACCESS events for the program's thread;
 *   - focus moves, value / state changes, structure changes and
 *     announcements are raised as UIA events.
 * Nothing here needs the Windows SDK's UIA headers or import libraries:
 * the COM interfaces are declared below in IDL order, and
 * UIAutomationCore.dll / OleAut32.dll are loaded on first use.
 */
#ifdef _WIN32
#include <windows.h>
#include <oleauto.h>
#include <stddef.h>

/* ---- UIA constants ---- */
#define Q_UiaRootObjectId (-25)
#define Q_UiaAppendRuntimeId 3
#define Q_UIA_E_ELEMENTNOTAVAILABLE ((HRESULT)0x80040201L)
#define Q_ProviderOptions_ServerSideProvider 0x1

enum { Q_NavParent = 0, Q_NavNext = 1, Q_NavPrev = 2, Q_NavFirst = 3, Q_NavLast = 4 };

enum {
    Q_PAT_Invoke = 10000, Q_PAT_Value = 10002, Q_PAT_RangeValue = 10003,
    Q_PAT_ExpandCollapse = 10005, Q_PAT_SelectionItem = 10010, Q_PAT_Toggle = 10015,
};
enum {
    Q_PROP_ControlType = 30003, Q_PROP_Name = 30005, Q_PROP_HasKeyboardFocus = 30008,
    Q_PROP_IsKeyboardFocusable = 30009, Q_PROP_IsEnabled = 30010, Q_PROP_AutomationId = 30011,
    Q_PROP_HelpText = 30013, Q_PROP_IsControlElement = 30016, Q_PROP_IsContentElement = 30017,
    Q_PROP_IsOffscreen = 30022, Q_PROP_FrameworkId = 30024, Q_PROP_ValueValue = 30045,
    Q_PROP_ExpandCollapseState = 30070, Q_PROP_SelectionItemIsSelected = 30079,
    Q_PROP_ToggleState = 30086, Q_PROP_ProviderDescription = 30107, Q_PROP_Level = 30154,
};
enum {
    Q_EVT_StructureChanged = 20002, Q_EVT_MenuOpened = 20003, Q_EVT_FocusChanged = 20005,
    Q_EVT_MenuClosed = 20007, Q_EVT_ElementSelected = 20012,
};

/* State bits (ui sends them; see AccessNode.states). */
enum {
    QS_FOCUSABLE = 1, QS_FOCUSED = 2, QS_DISABLED = 4, QS_CHECKED = 8, QS_SELECTED = 16,
    QS_EXPANDED = 32, QS_COLLAPSED = 64, QS_EDITABLE = 128, QS_MULTILINE = 256,
};

/* Actions queued for the program (QEV_ACCESS: a = node id, c = action). */
enum { QA_INVOKE = 1, QA_TOGGLE, QA_SET_VALUE, QA_SELECT, QA_EXPAND, QA_COLLAPSE, QA_FOCUS, QA_SET_RANGE };

/* ---- dynamically loaded functions ---- */
typedef struct { double left, top, width, height; } QUiaRect;
static struct {
    int loaded;
    LRESULT (WINAPI *ReturnRawElementProvider)(HWND, WPARAM, LPARAM, void*);
    HRESULT (WINAPI *HostProviderFromHwnd)(HWND, void**);
    HRESULT (WINAPI *RaiseAutomationEvent)(void*, int);
    HRESULT (WINAPI *RaisePropertyChanged)(void*, int, VARIANT, VARIANT);
    HRESULT (WINAPI *RaiseStructureChanged)(void*, int, int*, int);
    HRESULT (WINAPI *RaiseNotification)(void*, int, int, BSTR, BSTR);
    BOOL (WINAPI *ClientsAreListening)(void);
    BSTR (WINAPI *SysAllocString)(const OLECHAR*);
    void (WINAPI *SysFreeString)(BSTR);
    SAFEARRAY* (WINAPI *SafeArrayCreateVector)(VARTYPE, LONG, ULONG);
    HRESULT (WINAPI *SafeArrayPutElement)(SAFEARRAY*, LONG*, void*);
} quia;

static int quia_load(void) {
    if (quia.loaded) return quia.loaded == 1;
    quia.loaded = -1;
    HMODULE core = LoadLibraryA("UIAutomationCore.dll");
    HMODULE ole = LoadLibraryA("OleAut32.dll");
    if (!core || !ole) return 0;
    *(FARPROC*)&quia.ReturnRawElementProvider = GetProcAddress(core, "UiaReturnRawElementProvider");
    *(FARPROC*)&quia.HostProviderFromHwnd = GetProcAddress(core, "UiaHostProviderFromHwnd");
    *(FARPROC*)&quia.RaiseAutomationEvent = GetProcAddress(core, "UiaRaiseAutomationEvent");
    *(FARPROC*)&quia.RaisePropertyChanged = GetProcAddress(core, "UiaRaiseAutomationPropertyChangedEvent");
    *(FARPROC*)&quia.RaiseStructureChanged = GetProcAddress(core, "UiaRaiseStructureChangedEvent");
    *(FARPROC*)&quia.RaiseNotification = GetProcAddress(core, "UiaRaiseNotificationEvent");   /* Windows 10 1709+ */
    *(FARPROC*)&quia.ClientsAreListening = GetProcAddress(core, "UiaClientsAreListening");
    *(FARPROC*)&quia.SysAllocString = GetProcAddress(ole, "SysAllocString");
    *(FARPROC*)&quia.SysFreeString = GetProcAddress(ole, "SysFreeString");
    *(FARPROC*)&quia.SafeArrayCreateVector = GetProcAddress(ole, "SafeArrayCreateVector");
    *(FARPROC*)&quia.SafeArrayPutElement = GetProcAddress(ole, "SafeArrayPutElement");
    if (!quia.ReturnRawElementProvider || !quia.HostProviderFromHwnd || !quia.SysAllocString ||
        !quia.SafeArrayCreateVector || !quia.SafeArrayPutElement) return 0;
    quia.loaded = 1;
    return 1;
}

/* ---- the tree snapshot ---- */
typedef struct {
    int id;
    int parent, first, last, next, prev;     /* indices, -1 = none */
    int ctype;
    char role[24];
    WCHAR* name;
    WCHAR* value;
    WCHAR* desc;
    unsigned states;
    int x, y, w, h, level;
    double rmin, rmax, rval;
} QAccNode;

#define QACC_ACTIONS 64
typedef struct {
    CRITICAL_SECTION cs;
    int inited;
    HWND hwnd;
    WNDPROC oldproc;
    int slot;
    QAccNode* nodes;
    int n;
    int focus_id;
    float scale;
    int asked;                               /* a UIA client asked for the root */
    struct { int id, action; char text[128]; } act[QACC_ACTIONS];
    int nact;
} QAccWin;

static QAccWin qacc[QWIN_MAX];

static int qacc_index(QAccWin* a, int id) {
    for (int i = 0; i < a->n; i++) if (a->nodes[i].id == id) return i;
    return -1;
}

static WCHAR* qacc_wide(const char* s) {
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    WCHAR* w = (WCHAR*)malloc(sizeof(WCHAR) * (size_t)(n > 0 ? n : 1));
    if (!w) return NULL;
    if (n <= 0) { w[0] = 0; return w; }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void qacc_free_nodes(QAccNode* nodes, int n) {
    for (int i = 0; i < n; i++) { free(nodes[i].name); free(nodes[i].value); free(nodes[i].desc); }
    free(nodes);
}

/* ui role → UIA control type. */
static int qacc_ctype(const char* role) {
    static const struct { const char* r; int t; } map[] = {
        {"button", 50000}, {"checkbox", 50002}, {"combobox", 50003}, {"textbox", 50004},
        {"graphic", 50006}, {"listitem", 50007}, {"option", 50007}, {"list", 50008},
        {"menu", 50009}, {"menubar", 50010}, {"menuitem", 50011}, {"progressbar", 50012},
        {"radio", 50013}, {"slider", 50015}, {"status", 50017}, {"tablist", 50018},
        {"tab", 50019}, {"text", 50020}, {"alert", 50020}, {"toolbar", 50021}, {"tree", 50023},
        {"treeitem", 50024}, {"colorpicker", 50025}, {"group", 50026}, {"radiogroup", 50026},
        {"row", 50029}, {"cell", 50020}, {"dialog", 50032}, {"window", 50032},
        {"scrollpane", 50033}, {"tabpanel", 50033}, {"columnheader", 50035}, {"table", 50036},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strcmp(map[i].r, role) == 0) return map[i].t;
    return 50025;                            /* Custom */
}

/* ---- providers ---- */

/* One COM object per node, carrying every interface it might answer. */
typedef struct QProv {
    const void* vSimple;
    const void* vFrag;
    const void* vRoot;
    const void* vInvoke;
    const void* vToggle;
    const void* vValue;
    const void* vRange;
    const void* vSel;
    const void* vExpand;
    LONG ref;
    int slot;
    int id;
} QProv;

#define QP_FROM(This, field) ((QProv*)((char*)(This) - offsetof(QProv, field)))

static const GUID QIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID QIID_Simple = {0xd6dd68d1, 0x86fd, 0x4332, {0x86,0x66,0x9a,0xbe,0xde,0xa2,0xd2,0x4c}};
static const GUID QIID_Fragment = {0xf7063da8, 0x8359, 0x439c, {0x92,0x97,0xbb,0xc5,0x29,0x9a,0x7d,0x87}};
static const GUID QIID_FragmentRoot = {0x620ce2a5, 0xab8f, 0x40a9, {0x86,0xcb,0xde,0x3c,0x75,0x59,0x9b,0x58}};
static const GUID QIID_Invoke = {0x54fcb24b, 0xe18e, 0x47a2, {0xb4,0xd3,0xec,0xcb,0xe7,0x75,0x99,0xa2}};
static const GUID QIID_Toggle = {0x56d00bd0, 0xc4f4, 0x433c, {0xa8,0x36,0x1a,0x52,0xa5,0x7e,0x08,0x92}};
static const GUID QIID_Value = {0xc7935180, 0x6fb3, 0x4201, {0xb1,0x74,0x7d,0xf7,0x3a,0xdb,0xf6,0x4a}};
static const GUID QIID_RangeValue = {0x36dc7aef, 0x33e6, 0x4691, {0xaf,0xe1,0x2b,0xe7,0x27,0x4b,0x3d,0x33}};
static const GUID QIID_SelectionItem = {0x2acad808, 0xb2d4, 0x452d, {0xa4,0x07,0x91,0xff,0x1a,0xd1,0x67,0xb2}};
static const GUID QIID_ExpandCollapse = {0xd847d3a5, 0xcab0, 0x4a98, {0x8c,0x32,0xec,0xb4,0x5c,0x59,0xad,0x24}};

static QProv* qprov_new(int slot, int id);

/* Which patterns a node supports (by its role / states). */
static int qacc_has_pattern(const QAccNode* nd, int pat) {
    const char* r = nd->role;
    switch (pat) {
        case Q_PAT_Invoke:
            return strcmp(r, "button") == 0 || (strcmp(r, "menuitem") == 0 && !(nd->states & (QS_EXPANDED | QS_COLLAPSED)));
        case Q_PAT_Toggle: return strcmp(r, "checkbox") == 0;
        case Q_PAT_Value: return strcmp(r, "textbox") == 0 || strcmp(r, "combobox") == 0;
        case Q_PAT_RangeValue: return strcmp(r, "slider") == 0 || strcmp(r, "progressbar") == 0;
        case Q_PAT_SelectionItem:
            return strcmp(r, "listitem") == 0 || strcmp(r, "tab") == 0 || strcmp(r, "radio") == 0 ||
                   strcmp(r, "option") == 0 || strcmp(r, "treeitem") == 0 || strcmp(r, "row") == 0;
        case Q_PAT_ExpandCollapse: return (nd->states & (QS_EXPANDED | QS_COLLAPSED)) != 0;
    }
    return 0;
}

/* Look the provider's node up under the lock; NULL (and the lock not
 * held) when it's gone. */
static QAccNode* qprov_lock(QProv* p, QAccWin** out) {
    QAccWin* a = &qacc[p->slot];
    if (!a->inited) return NULL;
    EnterCriticalSection(&a->cs);
    int i = qacc_index(a, p->id);
    if (i < 0) { LeaveCriticalSection(&a->cs); return NULL; }
    *out = a;
    return &a->nodes[i];
}

static void qacc_queue(int slot, int id, int action, const char* text) {
    QAccWin* a = &qacc[slot];
    if (!a->inited) return;
    EnterCriticalSection(&a->cs);
    if (a->nact < QACC_ACTIONS) {
        a->act[a->nact].id = id;
        a->act[a->nact].action = action;
        snprintf(a->act[a->nact].text, sizeof a->act[a->nact].text, "%s", text ? text : "");
        a->nact++;
    }
    LeaveCriticalSection(&a->cs);
}

static ULONG STDMETHODCALLTYPE qp_AddRef(void* This);
static ULONG STDMETHODCALLTYPE qp_Release(void* This);
static HRESULT STDMETHODCALLTYPE qp_QueryInterface(void* This, REFIID riid, void** out);

/* Each interface gets its own IUnknown thunks so `This` → QProv is exact. */
#define QP_UNKNOWN(field)                                                                    \
    static HRESULT STDMETHODCALLTYPE field##_QI(void* This, REFIID riid, void** out) {       \
        return qp_QueryInterface(QP_FROM(This, field), riid, out);                           \
    }                                                                                        \
    static ULONG STDMETHODCALLTYPE field##_AddRef(void* This) { return qp_AddRef(QP_FROM(This, field)); } \
    static ULONG STDMETHODCALLTYPE field##_Release(void* This) { return qp_Release(QP_FROM(This, field)); }

QP_UNKNOWN(vSimple) QP_UNKNOWN(vFrag) QP_UNKNOWN(vRoot) QP_UNKNOWN(vInvoke) QP_UNKNOWN(vToggle)
QP_UNKNOWN(vValue) QP_UNKNOWN(vRange) QP_UNKNOWN(vSel) QP_UNKNOWN(vExpand)

static ULONG STDMETHODCALLTYPE qp_AddRef(void* pv) {
    QProv* p = (QProv*)pv;
    return (ULONG)InterlockedIncrement(&p->ref);
}

static ULONG STDMETHODCALLTYPE qp_Release(void* pv) {
    QProv* p = (QProv*)pv;
    LONG r = InterlockedDecrement(&p->ref);
    if (r == 0) free(p);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE qp_QueryInterface(void* pv, REFIID riid, void** out) {
    QProv* p = (QProv*)pv;
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID(riid, &QIID_IUnknown) || IsEqualGUID(riid, &QIID_Simple)) *out = &p->vSimple;
    else if (IsEqualGUID(riid, &QIID_Fragment)) *out = &p->vFrag;
    else if (IsEqualGUID(riid, &QIID_FragmentRoot)) {
        QAccWin* a = &qacc[p->slot];
        int root = 0;
        if (a->inited) {
            EnterCriticalSection(&a->cs);
            root = a->n > 0 && p->id == a->nodes[0].id;
            LeaveCriticalSection(&a->cs);
        }
        if (root) *out = &p->vRoot;
    }
    if (!*out) return E_NOINTERFACE;
    qp_AddRef(p);
    return S_OK;
}

static BSTR qacc_bstr(const WCHAR* w) { return quia.SysAllocString(w ? w : L""); }

/* IRawElementProviderSimple */
static HRESULT STDMETHODCALLTYPE simple_get_ProviderOptions(void* This, int* ret) {
    (void)This;
    *ret = Q_ProviderOptions_ServerSideProvider;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE simple_GetPatternProvider(void* This, int pat, IUnknown** ret) {
    QProv* p = QP_FROM(This, vSimple);
    *ret = NULL;
    QAccWin* a;
    QAccNode* nd = qprov_lock(p, &a);
    if (!nd) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    int has = qacc_has_pattern(nd, pat);
    LeaveCriticalSection(&a->cs);
    if (!has) return S_OK;
    void* iface = NULL;
    switch (pat) {
        case Q_PAT_Invoke: iface = &p->vInvoke; break;
        case Q_PAT_Toggle: iface = &p->vToggle; break;
        case Q_PAT_Value: iface = &p->vValue; break;
        case Q_PAT_RangeValue: iface = &p->vRange; break;
        case Q_PAT_SelectionItem: iface = &p->vSel; break;
        case Q_PAT_ExpandCollapse: iface = &p->vExpand; break;
    }
    if (iface) { qp_AddRef(p); *ret = (IUnknown*)iface; }
    return S_OK;
}

static void qv_bool(VARIANT* v, int b) { v->vt = VT_BOOL; v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE; }
static void qv_int(VARIANT* v, int i) { v->vt = VT_I4; v->lVal = i; }
static void qv_str(VARIANT* v, const WCHAR* w) { v->vt = VT_BSTR; v->bstrVal = qacc_bstr(w); }

static HRESULT STDMETHODCALLTYPE simple_GetPropertyValue(void* This, int prop, VARIANT* ret) {
    QProv* p = QP_FROM(This, vSimple);
    memset(ret, 0, sizeof(VARIANT));   /* VT_EMPTY (no OleAut32 import) */
    QAccWin* a;
    QAccNode* nd = qprov_lock(p, &a);
    if (!nd) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    int root = nd == &a->nodes[0];
    switch (prop) {
        case Q_PROP_ControlType: if (!root) qv_int(ret, nd->ctype); break;
        case Q_PROP_Name: if (nd->name && nd->name[0]) qv_str(ret, nd->name); break;
        case Q_PROP_HelpText: if (nd->desc && nd->desc[0]) qv_str(ret, nd->desc); break;
        case Q_PROP_HasKeyboardFocus: qv_bool(ret, (nd->states & QS_FOCUSED) != 0); break;
        case Q_PROP_IsKeyboardFocusable: qv_bool(ret, (nd->states & QS_FOCUSABLE) != 0); break;
        case Q_PROP_IsEnabled: qv_bool(ret, (nd->states & QS_DISABLED) == 0); break;
        case Q_PROP_IsControlElement:
        case Q_PROP_IsContentElement: qv_bool(ret, 1); break;
        case Q_PROP_IsOffscreen: qv_bool(ret, nd->w <= 0 || nd->h <= 0); break;
        case Q_PROP_AutomationId: {
            WCHAR buf[24];
            swprintf(buf, 24, L"%d", nd->id);
            qv_str(ret, buf);
            break;
        }
        case Q_PROP_FrameworkId: qv_str(ret, L"Quirk"); break;
        case Q_PROP_ProviderDescription: qv_str(ret, L"Quirk ui"); break;
        case Q_PROP_Level: if (nd->level > 0) qv_int(ret, nd->level); break;
    }
    LeaveCriticalSection(&a->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE simple_get_HostRawElementProvider(void* This, void** ret) {
    QProv* p = QP_FROM(This, vSimple);
    *ret = NULL;
    QAccWin* a = &qacc[p->slot];
    if (!a->inited) return S_OK;
    EnterCriticalSection(&a->cs);
    int root = a->n > 0 && p->id == a->nodes[0].id;
    HWND h = a->hwnd;
    LeaveCriticalSection(&a->cs);
    if (root && h) return quia.HostProviderFromHwnd(h, ret);
    return S_OK;
}

/* IRawElementProviderFragment */
static HRESULT STDMETHODCALLTYPE frag_Navigate(void* This, int dir, void** ret) {
    QProv* p = QP_FROM(This, vFrag);
    *ret = NULL;
    QAccWin* a;
    QAccNode* nd = qprov_lock(p, &a);
    if (!nd) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    int to = -1;
    switch (dir) {
        case Q_NavParent: to = nd->parent; break;
        case Q_NavNext: to = nd->next; break;
        case Q_NavPrev: to = nd->prev; break;
        case Q_NavFirst: to = nd->first; break;
        case Q_NavLast: to = nd->last; break;
    }
    int id = to >= 0 ? a->nodes[to].id : 0;
    int slot = a->slot;
    LeaveCriticalSection(&a->cs);
    if (to < 0) return S_OK;
    QProv* q = qprov_new(slot, id);
    if (!q) return E_OUTOFMEMORY;
    *ret = &q->vFrag;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE frag_GetRuntimeId(void* This, SAFEARRAY** ret) {
    QProv* p = QP_FROM(This, vFrag);
    *ret = NULL;
    QAccWin* a = &qacc[p->slot];
    if (a->inited) {
        EnterCriticalSection(&a->cs);
        int root = a->n > 0 && p->id == a->nodes[0].id;
        LeaveCriticalSection(&a->cs);
        if (root) return S_OK;                                         /* the host gives the root's */
    }
    SAFEARRAY* sa = quia.SafeArrayCreateVector(VT_I4, 0, 2);
    if (!sa) return E_OUTOFMEMORY;
    LONG i0 = 0, i1 = 1, v0 = Q_UiaAppendRuntimeId, v1 = p->id;
    quia.SafeArrayPutElement(sa, &i0, &v0);
    quia.SafeArrayPutElement(sa, &i1, &v1);
    *ret = sa;
    return S_OK;
}

/* A node's box in screen coordinates (physical pixels). */
static void qacc_screen_rect(QAccWin* a, const QAccNode* nd, QUiaRect* r) {
    POINT origin = {0, 0};
    if (a->hwnd) ClientToScreen(a->hwnd, &origin);
    float s = a->scale > 0 ? a->scale : 1.0f;
    r->left = origin.x + nd->x * s;
    r->top = origin.y + nd->y * s;
    r->width = nd->w * s;
    r->height = nd->h * s;
}

static HRESULT STDMETHODCALLTYPE frag_get_BoundingRectangle(void* This, QUiaRect* ret) {
    QProv* p = QP_FROM(This, vFrag);
    memset(ret, 0, sizeof *ret);
    QAccWin* a;
    QAccNode* nd = qprov_lock(p, &a);
    if (!nd) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    if (nd != &a->nodes[0]) qacc_screen_rect(a, nd, ret);   /* the host gives the window's */
    LeaveCriticalSection(&a->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE frag_GetEmbeddedFragmentRoots(void* This, SAFEARRAY** ret) {
    (void)This;
    *ret = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE frag_SetFocus(void* This) {
    QProv* p = QP_FROM(This, vFrag);
    qacc_queue(p->slot, p->id, QA_FOCUS, NULL);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE frag_get_FragmentRoot(void* This, void** ret) {
    QProv* p = QP_FROM(This, vFrag);
    *ret = NULL;
    QAccWin* a = &qacc[p->slot];
    if (!a->inited) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    EnterCriticalSection(&a->cs);
    int rid = a->n > 0 ? a->nodes[0].id : -1;
    LeaveCriticalSection(&a->cs);
    if (rid < 0) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    QProv* q = qprov_new(p->slot, rid);
    if (!q) return E_OUTOFMEMORY;
    *ret = &q->vRoot;
    return S_OK;
}

/* IRawElementProviderFragmentRoot (node 0) */
static HRESULT STDMETHODCALLTYPE root_ElementProviderFromPoint(void* This, double x, double y, void** ret) {
    QProv* p = QP_FROM(This, vRoot);
    *ret = NULL;
    QAccWin* a = &qacc[p->slot];
    if (!a->inited) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    EnterCriticalSection(&a->cs);
    POINT origin = {0, 0};
    if (a->hwnd) ClientToScreen(a->hwnd, &origin);
    float s = a->scale > 0 ? a->scale : 1.0f;
    double lx = (x - origin.x) / s, ly = (y - origin.y) / s;
    int best = -1;
    long bestArea = 0;
    for (int i = 1; i < a->n; i++) {
        QAccNode* nd = &a->nodes[i];
        if (nd->w <= 0 || nd->h <= 0) continue;
        if (lx < nd->x || ly < nd->y || lx >= nd->x + nd->w || ly >= nd->y + nd->h) continue;
        long area = (long)nd->w * nd->h;
        if (best < 0 || area <= bestArea) { best = i; bestArea = area; }
    }
    int id = best >= 0 ? a->nodes[best].id : 0;
    int slot = a->slot;
    LeaveCriticalSection(&a->cs);
    if (best < 0) return S_OK;
    QProv* q = qprov_new(slot, id);
    if (!q) return E_OUTOFMEMORY;
    *ret = &q->vFrag;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE root_GetFocus(void* This, void** ret) {
    QProv* p = QP_FROM(This, vRoot);
    *ret = NULL;
    QAccWin* a = &qacc[p->slot];
    if (!a->inited) return S_OK;
    EnterCriticalSection(&a->cs);
    int id = a->focus_id;
    int ok = id > 0 && qacc_index(a, id) > 0;
    LeaveCriticalSection(&a->cs);
    if (!ok) return S_OK;
    QProv* q = qprov_new(p->slot, id);
    if (!q) return E_OUTOFMEMORY;
    *ret = &q->vFrag;
    return S_OK;
}

/* Patterns */
static HRESULT STDMETHODCALLTYPE invoke_Invoke(void* This) {
    QProv* p = QP_FROM(This, vInvoke);
    qacc_queue(p->slot, p->id, QA_INVOKE, NULL);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE toggle_Toggle(void* This) {
    QProv* p = QP_FROM(This, vToggle);
    qacc_queue(p->slot, p->id, QA_TOGGLE, NULL);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE toggle_get_ToggleState(void* This, int* ret) {
    QProv* p = QP_FROM(This, vToggle);
    *ret = 0;
    QAccWin* a;
    QAccNode* nd = qprov_lock(p, &a);
    if (!nd) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    *ret = (nd->states & QS_CHECKED) ? 1 : 0;
    LeaveCriticalSection(&a->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE value_SetValue(void* This, LPCWSTR val) {
    QProv* p = QP_FROM(This, vValue);
    char buf[128];
    int n = WideCharToMultiByte(CP_UTF8, 0, val ? val : L"", -1, buf, (int)sizeof buf, NULL, NULL);
    if (n <= 0) buf[0] = 0;
    qacc_queue(p->slot, p->id, QA_SET_VALUE, buf);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE value_get_Value(void* This, BSTR* ret) {
    QProv* p = QP_FROM(This, vValue);
    *ret = NULL;
    QAccWin* a;
    QAccNode* nd = qprov_lock(p, &a);
    if (!nd) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    *ret = qacc_bstr(nd->value);
    LeaveCriticalSection(&a->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE value_get_IsReadOnly(void* This, BOOL* ret) {
    QProv* p = QP_FROM(This, vValue);
    *ret = TRUE;
    QAccWin* a;
    QAccNode* nd = qprov_lock(p, &a);
    if (!nd) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    *ret = !((nd->states & QS_EDITABLE) && !(nd->states & QS_DISABLED));
    LeaveCriticalSection(&a->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE range_SetValue(void* This, double v) {
    QProv* p = QP_FROM(This, vRange);
    char buf[64];
    snprintf(buf, sizeof buf, "%.0f", v);
    qacc_queue(p->slot, p->id, QA_SET_RANGE, buf);
    return S_OK;
}

static HRESULT qrange_get(void* This, double* ret, int which) {
    QProv* p = QP_FROM(This, vRange);
    *ret = 0;
    QAccWin* a;
    QAccNode* nd = qprov_lock(p, &a);
    if (!nd) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    double span = nd->rmax - nd->rmin;
    *ret = which == 0 ? nd->rval : which == 1 ? nd->rmax : which == 2 ? nd->rmin
         : which == 3 ? (span > 10 ? span / 10 : 1) : 1;
    LeaveCriticalSection(&a->cs);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE range_get_Value(void* This, double* r) { return qrange_get(This, r, 0); }
static HRESULT STDMETHODCALLTYPE range_get_Maximum(void* This, double* r) { return qrange_get(This, r, 1); }
static HRESULT STDMETHODCALLTYPE range_get_Minimum(void* This, double* r) { return qrange_get(This, r, 2); }
static HRESULT STDMETHODCALLTYPE range_get_LargeChange(void* This, double* r) { return qrange_get(This, r, 3); }
static HRESULT STDMETHODCALLTYPE range_get_SmallChange(void* This, double* r) { return qrange_get(This, r, 4); }

static HRESULT STDMETHODCALLTYPE range_get_IsReadOnly(void* This, BOOL* ret) {
    QProv* p = QP_FROM(This, vRange);
    *ret = TRUE;
    QAccWin* a;
    QAccNode* nd = qprov_lock(p, &a);
    if (!nd) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    *ret = strcmp(nd->role, "slider") != 0 || (nd->states & QS_DISABLED);
    LeaveCriticalSection(&a->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE sel_Select(void* This) {
    QProv* p = QP_FROM(This, vSel);
    qacc_queue(p->slot, p->id, QA_SELECT, NULL);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sel_AddToSelection(void* This) { return sel_Select(This); }
static HRESULT STDMETHODCALLTYPE sel_RemoveFromSelection(void* This) { (void)This; return S_OK; }

static HRESULT STDMETHODCALLTYPE sel_get_IsSelected(void* This, BOOL* ret) {
    QProv* p = QP_FROM(This, vSel);
    *ret = FALSE;
    QAccWin* a;
    QAccNode* nd = qprov_lock(p, &a);
    if (!nd) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    *ret = (nd->states & (QS_SELECTED | QS_CHECKED)) != 0;
    LeaveCriticalSection(&a->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE sel_get_SelectionContainer(void* This, void** ret) {
    QProv* p = QP_FROM(This, vSel);
    *ret = NULL;
    QAccWin* a;
    QAccNode* nd = qprov_lock(p, &a);
    if (!nd) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    int par = nd->parent;
    int id = par >= 0 ? a->nodes[par].id : 0;
    LeaveCriticalSection(&a->cs);
    if (par < 0) return S_OK;
    QProv* q = qprov_new(p->slot, id);
    if (!q) return E_OUTOFMEMORY;
    *ret = &q->vSimple;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE expand_Expand(void* This) {
    QProv* p = QP_FROM(This, vExpand);
    qacc_queue(p->slot, p->id, QA_EXPAND, NULL);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE expand_Collapse(void* This) {
    QProv* p = QP_FROM(This, vExpand);
    qacc_queue(p->slot, p->id, QA_COLLAPSE, NULL);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE expand_get_State(void* This, int* ret) {
    QProv* p = QP_FROM(This, vExpand);
    *ret = 3;                                /* LeafNode */
    QAccWin* a;
    QAccNode* nd = qprov_lock(p, &a);
    if (!nd) return Q_UIA_E_ELEMENTNOTAVAILABLE;
    *ret = (nd->states & QS_EXPANDED) ? 1 : (nd->states & QS_COLLAPSED) ? 0 : 3;
    LeaveCriticalSection(&a->cs);
    return S_OK;
}

/* Vtables, in IDL order. */
static void* const qvt_simple[] = { (void*)vSimple_QI, (void*)vSimple_AddRef, (void*)vSimple_Release,
    (void*)simple_get_ProviderOptions, (void*)simple_GetPatternProvider, (void*)simple_GetPropertyValue,
    (void*)simple_get_HostRawElementProvider };
static void* const qvt_frag[] = { (void*)vFrag_QI, (void*)vFrag_AddRef, (void*)vFrag_Release,
    (void*)frag_Navigate, (void*)frag_GetRuntimeId, (void*)frag_get_BoundingRectangle,
    (void*)frag_GetEmbeddedFragmentRoots, (void*)frag_SetFocus, (void*)frag_get_FragmentRoot };
static void* const qvt_root[] = { (void*)vRoot_QI, (void*)vRoot_AddRef, (void*)vRoot_Release,
    (void*)root_ElementProviderFromPoint, (void*)root_GetFocus };
static void* const qvt_invoke[] = { (void*)vInvoke_QI, (void*)vInvoke_AddRef, (void*)vInvoke_Release,
    (void*)invoke_Invoke };
static void* const qvt_toggle[] = { (void*)vToggle_QI, (void*)vToggle_AddRef, (void*)vToggle_Release,
    (void*)toggle_Toggle, (void*)toggle_get_ToggleState };
static void* const qvt_value[] = { (void*)vValue_QI, (void*)vValue_AddRef, (void*)vValue_Release,
    (void*)value_SetValue, (void*)value_get_Value, (void*)value_get_IsReadOnly };
static void* const qvt_range[] = { (void*)vRange_QI, (void*)vRange_AddRef, (void*)vRange_Release,
    (void*)range_SetValue, (void*)range_get_Value, (void*)range_get_IsReadOnly, (void*)range_get_Maximum,
    (void*)range_get_Minimum, (void*)range_get_LargeChange, (void*)range_get_SmallChange };
static void* const qvt_sel[] = { (void*)vSel_QI, (void*)vSel_AddRef, (void*)vSel_Release,
    (void*)sel_Select, (void*)sel_AddToSelection, (void*)sel_RemoveFromSelection,
    (void*)sel_get_IsSelected, (void*)sel_get_SelectionContainer };
static void* const qvt_expand[] = { (void*)vExpand_QI, (void*)vExpand_AddRef, (void*)vExpand_Release,
    (void*)expand_Expand, (void*)expand_Collapse, (void*)expand_get_State };

static QProv* qprov_new(int slot, int id) {
    QProv* p = (QProv*)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->vSimple = qvt_simple; p->vFrag = qvt_frag; p->vRoot = qvt_root;
    p->vInvoke = qvt_invoke; p->vToggle = qvt_toggle; p->vValue = qvt_value;
    p->vRange = qvt_range; p->vSel = qvt_sel; p->vExpand = qvt_expand;
    p->ref = 1;
    p->slot = slot;
    p->id = id;
    return p;
}

/* ---- hooking SDL's window ---- */

static LRESULT CALLBACK qacc_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    for (int i = 0; i < QWIN_MAX; i++) {
        QAccWin* a = &qacc[i];
        if (!a->inited || a->hwnd != hwnd) continue;
        if (msg == WM_GETOBJECT && (long)lp == Q_UiaRootObjectId && quia.loaded == 1) {
            EnterCriticalSection(&a->cs);
            int have = a->n > 0;
            int id = have ? a->nodes[0].id : 0;
            a->asked = 1;
            LeaveCriticalSection(&a->cs);
            if (have) {
                QProv* root = qprov_new(i, id);
                if (root) {
                    LRESULT r = quia.ReturnRawElementProvider(hwnd, wp, lp, &root->vSimple);
                    qp_Release(root);
                    return r;
                }
            }
        }
        WNDPROC old = a->oldproc;
        if (msg == WM_NCDESTROY && quia.loaded == 1) quia.ReturnRawElementProvider(hwnd, 0, 0, NULL);
        return CallWindowProcW(old, hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* SDL_SysWMinfo: version (3 bytes), subsystem (int) at 4, HWND at 8. */
static HWND qacc_hwnd_of(void* sdl_window) {
    static int (*GetWMInfo)(void*, void*) = NULL;
    static void (*GetVersion)(void*) = NULL;
    if (!GetWMInfo) {
        *(void**)&GetWMInfo = dlsym(qsdl.lib, "SDL_GetWindowWMInfo");
        *(void**)&GetVersion = dlsym(qsdl.lib, "SDL_GetVersion");
    }
    if (!GetWMInfo || !GetVersion) return NULL;
    unsigned char info[256];
    memset(info, 0, sizeof info);
    GetVersion(info);
    if (!GetWMInfo(sdl_window, info)) return NULL;
    HWND h;
    memcpy(&h, info + 8, sizeof h);
    return h;
}

/* Called when a window opens (not headless). Experimental for now: only
 * with QUIRK_ACCESSIBILITY=1 in the environment, until it has been tried
 * with NVDA / Narrator (Windows sends UIA queries to every window, so a
 * mistake here would reach every program). */
static void qacc_attach(int slot, void* sdl_window) {
    QAccWin* a = &qacc[slot];
    const char* on = getenv("QUIRK_ACCESSIBILITY");
    if (!on || strcmp(on, "1") != 0) return;
    if (!quia_load()) return;
    HWND h = qacc_hwnd_of(sdl_window);
    if (!h) return;
    if (!a->inited) { InitializeCriticalSection(&a->cs); a->inited = 1; }
    EnterCriticalSection(&a->cs);
    a->hwnd = h;
    a->slot = slot;
    a->nodes = NULL; a->n = 0; a->focus_id = 0; a->nact = 0; a->asked = 0;
    a->oldproc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)qacc_wndproc);
    LeaveCriticalSection(&a->cs);
}

static void qacc_detach(int slot) {
    QAccWin* a = &qacc[slot];
    if (!a->inited) return;
    EnterCriticalSection(&a->cs);
    if (a->hwnd && a->oldproc) SetWindowLongPtrW(a->hwnd, GWLP_WNDPROC, (LONG_PTR)a->oldproc);
    if (a->hwnd && quia.loaded == 1) quia.ReturnRawElementProvider(a->hwnd, 0, 0, NULL);
    qacc_free_nodes(a->nodes, a->n);
    a->nodes = NULL; a->n = 0; a->hwnd = NULL; a->oldproc = NULL; a->nact = 0;
    LeaveCriticalSection(&a->cs);
}

/* Move queued screen-reader actions into the window's event queue (main thread). */
static void qacc_drain(int slot, QWin* w) {
    QAccWin* a = &qacc[slot];
    if (!a->inited) return;
    EnterCriticalSection(&a->cs);
    for (int i = 0; i < a->nact; i++) {
        QWinEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.kind = QEV_ACCESS;
        ev.a = a->act[i].id;
        ev.c = a->act[i].action;
        snprintf(ev.text, sizeof ev.text, "%s", a->act[i].text);
        qwin_push(w, &ev);
    }
    a->nact = 0;
    LeaveCriticalSection(&a->cs);
}

static int qacc_active(int slot) {
    QAccWin* a = &qacc[slot];
    if (!a->inited || quia.loaded != 1) return 0;
    if (a->asked) return 1;
    return quia.ClientsAreListening && quia.ClientsAreListening();
}

/* Raise `event` for node `id` (no lock held while UIA calls back). */
static void qacc_raise(int slot, int id, int event) {
    if (!qacc_active(slot) || !quia.RaiseAutomationEvent) return;
    QProv* p = qprov_new(slot, id);
    if (!p) return;
    quia.RaiseAutomationEvent(&p->vSimple, event);
    qp_Release(p);
}

static void qacc_raise_prop(int slot, int id, int prop, VARIANT oldv, VARIANT newv) {
    if (!qacc_active(slot) || !quia.RaisePropertyChanged) return;
    QProv* p = qprov_new(slot, id);
    if (!p) return;
    quia.RaisePropertyChanged(&p->vSimple, prop, oldv, newv);
    qp_Release(p);
}


/* ---- what ui calls ---- */

static int qlist_int(List* l, int64_t i) {
    return (l && i < l->size) ? (int)quirk_opaque_to_int(l->data[i]) : 0;
}

static const char* qlist_str(List* l, int64_t i) {
    if (!l || i >= l->size) return "";
    String* s = quirk_opaque_to_string(l->data[i]);
    return s ? make_safe_cstr(s) : "";
}

/* Replace the window's tree: node i has id ids[i], parent index
 * parents[i] (-1 for the root, which is node 0), role / name / value /
 * description strings, a state bitmask, box[4i..4i+3] = x, y, w, h and
 * nums[4i..4i+3] = level, range min, range max, range value. */
static void qacc_update(int slot, float scale, List* ids, List* parents, List* roles, List* names,
                        List* values, List* descs, List* states, List* boxes, List* nums) {
    QAccWin* a = &qacc[slot];
    if (!a->inited || !ids) return;
    int n = (int)ids->size;
    QAccNode* nodes = (QAccNode*)calloc((size_t)(n > 0 ? n : 1), sizeof *nodes);
    if (!nodes) return;
    for (int i = 0; i < n; i++) {
        QAccNode* nd = &nodes[i];
        nd->id = qlist_int(ids, i);
        nd->parent = qlist_int(parents, i);
        if (nd->parent >= i) nd->parent = -1;              /* parents come first */
        nd->first = nd->last = nd->next = nd->prev = -1;
        snprintf(nd->role, sizeof nd->role, "%s", qlist_str(roles, i));
        nd->ctype = qacc_ctype(nd->role);
        nd->name = qacc_wide(qlist_str(names, i));
        nd->value = qacc_wide(qlist_str(values, i));
        nd->desc = qacc_wide(qlist_str(descs, i));
        nd->states = (unsigned)qlist_int(states, i);
        nd->x = qlist_int(boxes, 4 * (int64_t)i); nd->y = qlist_int(boxes, 4 * (int64_t)i + 1);
        nd->w = qlist_int(boxes, 4 * (int64_t)i + 2); nd->h = qlist_int(boxes, 4 * (int64_t)i + 3);
        nd->level = qlist_int(nums, 4 * (int64_t)i);
        nd->rmin = qlist_int(nums, 4 * (int64_t)i + 1);
        nd->rmax = qlist_int(nums, 4 * (int64_t)i + 2);
        nd->rval = qlist_int(nums, 4 * (int64_t)i + 3);
        int par = nd->parent;
        if (par >= 0) {
            QAccNode* pn = &nodes[par];
            if (pn->first < 0) pn->first = i;
            else { nodes[pn->last].next = i; nd->prev = pn->last; }
            pn->last = i;
        }
    }
    int changed = 0, focus = 0;
    for (int i = 0; i < n; i++) if (nodes[i].states & QS_FOCUSED) focus = nodes[i].id;
    EnterCriticalSection(&a->cs);
    if (a->n != n) changed = 1;
    else
        for (int i = 0; i < n && !changed; i++)
            if (a->nodes[i].id != nodes[i].id || a->nodes[i].parent != nodes[i].parent) changed = 1;
    QAccNode* old = a->nodes;
    int oldn = a->n;
    a->nodes = nodes;
    a->n = n;
    a->focus_id = focus;
    a->scale = scale;
    int root = n > 0 ? nodes[0].id : 0;
    LeaveCriticalSection(&a->cs);
    qacc_free_nodes(old, oldn);
    if (changed && oldn > 0 && qacc_active(slot) && quia.RaiseStructureChanged) {
        QProv* p = qprov_new(slot, root);
        if (p) {
            int rid[2] = { Q_UiaAppendRuntimeId, root };
            quia.RaiseStructureChanged(&p->vSimple, 2 /* ChildrenInvalidated */, rid, 2);
            qp_Release(p);
        }
    }
}

static void qacc_changed(int slot, int id, int what) {
    QAccWin* a = &qacc[slot];
    if (!a->inited) return;
    VARIANT oldv, newv;
    memset(&oldv, 0, sizeof(VARIANT));   /* VT_EMPTY (no OleAut32 import) */
    memset(&newv, 0, sizeof(VARIANT));   /* VT_EMPTY (no OleAut32 import) */
    EnterCriticalSection(&a->cs);
    int i = qacc_index(a, id);
    if (i < 0) { LeaveCriticalSection(&a->cs); return; }
    QAccNode* nd = &a->nodes[i];
    int prop = 0, event = 0;
    switch (what) {
        case 1: prop = Q_PROP_ValueValue; qv_str(&newv, nd->value); break;
        case 2: prop = Q_PROP_ToggleState; qv_int(&newv, (nd->states & QS_CHECKED) ? 1 : 0); break;
        case 3: event = Q_EVT_ElementSelected; break;
        case 4: prop = Q_PROP_ExpandCollapseState; qv_int(&newv, (nd->states & QS_EXPANDED) ? 1 : 0); break;
        case 5: prop = Q_PROP_Name; qv_str(&newv, nd->name); break;
    }
    LeaveCriticalSection(&a->cs);
    if (prop) qacc_raise_prop(slot, id, prop, oldv, newv);
    if (event) qacc_raise(slot, id, event);
    if (newv.vt == VT_BSTR && newv.bstrVal) quia.SysFreeString(newv.bstrVal);
}

static void qacc_announce(int slot, const char* text) {
    QAccWin* a = &qacc[slot];
    if (!a->inited || !qacc_active(slot) || !quia.RaiseNotification) return;
    EnterCriticalSection(&a->cs);
    int root = a->n > 0 ? a->nodes[0].id : -1;
    LeaveCriticalSection(&a->cs);
    if (root < 0) return;
    QProv* p = qprov_new(slot, root);
    if (!p) return;
    WCHAR* w = qacc_wide(text);
    BSTR msg = qacc_bstr(w), act = quia.SysAllocString(L"quirk-announce");
    quia.RaiseNotification(&p->vSimple, 4 /* Other */, 1 /* ImportantMostRecent */, msg, act);
    quia.SysFreeString(msg);
    quia.SysFreeString(act);
    free(w);
    qp_Release(p);
}

#endif /* _WIN32 */
