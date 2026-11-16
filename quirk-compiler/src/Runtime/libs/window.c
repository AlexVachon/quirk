#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <gc.h>
#include "../types.h"

#ifndef _WIN32
    #include <dlfcn.h>
    #include <unistd.h>
    #include <time.h>
#else
    // windows.h's CreateWindow macro would rename the SDL table's member.
    #undef CreateWindow
    // SDL2.dll through the Win32 loader, with the same calls as dlopen.
    #define dlopen(name, flags) ((void*)LoadLibraryA(name))
    #define dlsym(lib, sym)     ((void*)GetProcAddress((HMODULE)(lib), (sym)))
#endif

#include "window_font.h"
#include "window_bidi.h"

// Image decoding (PNG / JPEG / BMP / GIF / TGA): stb_image, vendored in
// src/third_party/stb. Allocations go through the scanned GC heap — the
// JPEG decoder keeps pointers to other buffers inside its own structs.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MALLOC(sz)        GC_malloc(sz)
#define STBI_REALLOC(p, newsz) GC_realloc(p, newsz)
#define STBI_FREE(p)           ((void)(p))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "../../third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

// TrueType fonts (window.load_font): stb_truetype, vendored next to
// stb_image. Its allocations go to the GC (glyph bitmaps are copied into
// textures and dropped).
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_malloc(x, u)  ((void)(u), GC_malloc(x))
#define STBTT_free(x, u)    ((void)(x), (void)(u))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "../../third_party/stb/stb_truetype.h"
#pragma GCC diagnostic pop

// ---------------------------------------------------------------------------
//  Windows with 2D drawing, for packages/window/index.quirk.
//  Naming: Window_<name>.
//
//  Backed by SDL2, loaded with dlopen the first time a window opens — so
//  the runtime has no build-time or load-time dependency on it, and
//  programs that never open a window don't need it installed. Only the
//  SDL functions and struct layouts used here are declared (SDL2's ABI is
//  stable across 2.x).
//
//  Windows are small integer handles. SDL has one event queue for the
//  whole process; every pump sorts its events into per-window queues by
//  window id, so each Quirk window sees only its own.
//
//  Text uses a built-in 8x16 bitmap font (window_font.h: ASCII + Latin-1),
//  uploaded once per window as a white glyph atlas and tinted per draw.
// ---------------------------------------------------------------------------

void* quirk_int_to_opaque(int64_t v);   // runtime.c

typedef struct { int x, y, w, h; } QSdlRect;
typedef struct { float x, y, w, h; } QSdlFRect;

// SDL_Event is a 56-byte union; only these offsets are read.
typedef struct { uint8_t bytes[56]; } QSdlEvent;

#define QSDL_INIT_VIDEO           0x00000020u
#define QSDL_WINDOWPOS_CENTERED   0x2FFF0000
#define QSDL_WINDOW_SHOWN         0x00000004u
#define QSDL_WINDOW_HIDDEN        0x00000008u
#define QSDL_WINDOW_RESIZABLE     0x00000020u
#define QSDL_WINDOW_ALLOW_HIGHDPI 0x00002000u
#define QSDL_RENDERER_SOFTWARE    0x00000001u
#define QSDL_RENDERER_ACCELERATED 0x00000002u
#define QSDL_RENDERER_VSYNC       0x00000004u
#define QSDL_PIXELFORMAT_ARGB8888 0x16362004u
#define QSDL_PIXELFORMAT_ABGR8888 0x16762004u   // bytes R,G,B,A on little-endian
#define QSDL_BLENDMODE_BLEND      1

#define QSDL_QUIT            0x100
#define QSDL_WINDOWEVENT     0x200
#define QSDL_KEYDOWN         0x300
#define QSDL_KEYUP           0x301
#define QSDL_TEXTEDITING     0x302   // IME composition in progress
#define QSDL_TEXTINPUT       0x303
#define QSDL_TEXTEDITING_EXT 0x305   // same, text longer than 32 bytes (SDL 2.0.22+)
#define QSDL_MOUSEMOTION     0x400
#define QSDL_MOUSEBUTTONDOWN 0x401
#define QSDL_MOUSEBUTTONUP   0x402
#define QSDL_MOUSEWHEEL      0x403

#define QSDL_WE_SIZE_CHANGED  6
#define QSDL_WE_DISPLAY_CHANGED 18   // SDL 2.0.18+: moved to another monitor
#define QSDL_WE_FOCUS_GAINED 12
#define QSDL_WE_FOCUS_LOST   13
#define QSDL_WE_CLOSE        14

static struct {
    int loaded;            // 1 ok, -1 failed
    int inited;
    void* lib;
    int (*Init)(uint32_t);
    void (*Quit)(void);
    const char* (*GetError)(void);
    int (*SetHint)(const char*, const char*);
    void* (*CreateWindow)(const char*, int, int, int, int, uint32_t);
    void (*DestroyWindow)(void*);
    uint32_t (*GetWindowID)(void*);
    void (*SetWindowTitle)(void*, const char*);
    void (*GetWindowSize)(void*, int*, int*);
    void* (*CreateRenderer)(void*, int, uint32_t);
    void (*DestroyRenderer)(void*);
    int (*GetRendererOutputSize)(void*, int*, int*);
    int (*SetRenderDrawColor)(void*, uint8_t, uint8_t, uint8_t, uint8_t);
    int (*SetRenderDrawBlendMode)(void*, int);
    int (*RenderClear)(void*);
    int (*RenderFillRect)(void*, const QSdlRect*);
    int (*RenderDrawRect)(void*, const QSdlRect*);
    int (*RenderDrawLine)(void*, int, int, int, int);
    int (*RenderDrawPoint)(void*, int, int);
    void (*RenderPresent)(void*);
    int (*RenderReadPixels)(void*, const QSdlRect*, uint32_t, void*, int);
    int (*RenderCopy)(void*, void*, const QSdlRect*, const QSdlRect*);
    void* (*CreateTexture)(void*, uint32_t, int, int, int);
    void* (*CreateTextureFromSurface)(void*, void*);
    void (*DestroyTexture)(void*);
    int (*UpdateTexture)(void*, const QSdlRect*, const void*, int);
    int (*SetTextureBlendMode)(void*, int);
    int (*SetTextureColorMod)(void*, uint8_t, uint8_t, uint8_t);
    int (*SetTextureAlphaMod)(void*, uint8_t);
    int (*QueryTexture)(void*, uint32_t*, int*, int*, int*);
    int (*PollEvent)(QSdlEvent*);
    const char* (*GetKeyName)(int32_t);
    void (*StartTextInput)(void);
    uint32_t (*GetTicks)(void);
    void* (*RWFromFile)(const char*, const char*);
    void* (*LoadBMP_RW)(void*, int);
    int (*SaveBMP_RW)(void*, void*, int);
    void* (*CreateRGBSurfaceWithFormat)(uint32_t, int, int, int, uint32_t);
    void (*FreeSurface)(void*);
    int (*RenderSetClipRect)(void*, const QSdlRect*);
    int (*SetClipboardText)(const char*);
    char* (*GetClipboardText)(void);
    void (*Free)(void*);
    // Optional (NULL when this SDL lacks them): high-DPI drawing.
    int (*RenderSetScale)(void*, float, float);
    int (*RenderCopyF)(void*, void*, const QSdlRect*, const QSdlFRect*);
    // Optional: IME (input methods for Chinese / Japanese / Korean, dead keys).
    void (*SetTextInputRect)(const QSdlRect*);
    int (*PushEvent)(QSdlEvent*);
} qsdl;

static char qwin_err[512] = "";

static void qwin_set_err(const char* what) {
    const char* e = (qsdl.loaded == 1 && qsdl.GetError) ? qsdl.GetError() : "";
    snprintf(qwin_err, sizeof qwin_err, "%s%s%s", what, (e && *e) ? ": " : "", e ? e : "");
}

static int qsdl_load(void) {
    if (qsdl.loaded) return qsdl.loaded == 1;
    qsdl.loaded = -1;
    {
    const char* names[] = {
        getenv("QUIRK_SDL2"),
#if defined(_WIN32)
        "SDL2.dll",
#elif defined(__APPLE__)
        "libSDL2-2.0.0.dylib", "/opt/homebrew/lib/libSDL2-2.0.0.dylib",
        "/usr/local/lib/libSDL2-2.0.0.dylib", "libSDL2.dylib",
#else
        "libSDL2-2.0.so.0", "libSDL2.so",
#endif
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !qsdl.lib; i++)
        if (names[i] && *names[i]) qsdl.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!qsdl.lib) {
        snprintf(qwin_err, sizeof qwin_err,
#if defined(_WIN32)
                 "SDL2 isn't installed (put SDL2.dll next to quirk.exe or on PATH)"
#elif defined(__APPLE__)
                 "SDL2 isn't installed (brew install sdl2)"
#else
                 "SDL2 isn't installed (e.g. sudo apt install libsdl2-2.0-0)"
#endif
                 );
        return 0;
    }
    #define QLOAD(field, sym) \
        if (!(*(void**)&qsdl.field = dlsym(qsdl.lib, sym))) { \
            snprintf(qwin_err, sizeof qwin_err, "SDL2 is too old (no %s)", sym); return 0; }
    QLOAD(Init, "SDL_Init")                       QLOAD(Quit, "SDL_Quit")
    QLOAD(GetError, "SDL_GetError")               QLOAD(SetHint, "SDL_SetHint")
    QLOAD(CreateWindow, "SDL_CreateWindow")       QLOAD(DestroyWindow, "SDL_DestroyWindow")
    QLOAD(GetWindowID, "SDL_GetWindowID")         QLOAD(SetWindowTitle, "SDL_SetWindowTitle")
    QLOAD(GetWindowSize, "SDL_GetWindowSize")
    QLOAD(CreateRenderer, "SDL_CreateRenderer")   QLOAD(DestroyRenderer, "SDL_DestroyRenderer")
    QLOAD(GetRendererOutputSize, "SDL_GetRendererOutputSize")
    QLOAD(SetRenderDrawColor, "SDL_SetRenderDrawColor")
    QLOAD(SetRenderDrawBlendMode, "SDL_SetRenderDrawBlendMode")
    QLOAD(RenderClear, "SDL_RenderClear")         QLOAD(RenderFillRect, "SDL_RenderFillRect")
    QLOAD(RenderDrawRect, "SDL_RenderDrawRect")   QLOAD(RenderDrawLine, "SDL_RenderDrawLine")
    QLOAD(RenderDrawPoint, "SDL_RenderDrawPoint") QLOAD(RenderPresent, "SDL_RenderPresent")
    QLOAD(RenderReadPixels, "SDL_RenderReadPixels") QLOAD(RenderCopy, "SDL_RenderCopy")
    QLOAD(CreateTexture, "SDL_CreateTexture")
    QLOAD(CreateTextureFromSurface, "SDL_CreateTextureFromSurface")
    QLOAD(DestroyTexture, "SDL_DestroyTexture")   QLOAD(UpdateTexture, "SDL_UpdateTexture")
    QLOAD(SetTextureBlendMode, "SDL_SetTextureBlendMode")
    QLOAD(SetTextureColorMod, "SDL_SetTextureColorMod")
    QLOAD(SetTextureAlphaMod, "SDL_SetTextureAlphaMod")
    QLOAD(QueryTexture, "SDL_QueryTexture")
    QLOAD(PollEvent, "SDL_PollEvent")             QLOAD(GetKeyName, "SDL_GetKeyName")
    QLOAD(StartTextInput, "SDL_StartTextInput")   QLOAD(GetTicks, "SDL_GetTicks")
    QLOAD(RWFromFile, "SDL_RWFromFile")           QLOAD(LoadBMP_RW, "SDL_LoadBMP_RW")
    QLOAD(SaveBMP_RW, "SDL_SaveBMP_RW")
    QLOAD(CreateRGBSurfaceWithFormat, "SDL_CreateRGBSurfaceWithFormat")
    QLOAD(FreeSurface, "SDL_FreeSurface")
    QLOAD(RenderSetClipRect, "SDL_RenderSetClipRect")
    QLOAD(SetClipboardText, "SDL_SetClipboardText")
    QLOAD(GetClipboardText, "SDL_GetClipboardText")
    QLOAD(Free, "SDL_free")
    #undef QLOAD
    *(void**)&qsdl.RenderSetScale = dlsym(qsdl.lib, "SDL_RenderSetScale");
    *(void**)&qsdl.RenderCopyF = dlsym(qsdl.lib, "SDL_RenderCopyF");
    *(void**)&qsdl.SetTextInputRect = dlsym(qsdl.lib, "SDL_SetTextInputRect");
    *(void**)&qsdl.PushEvent = dlsym(qsdl.lib, "SDL_PushEvent");
    qsdl.loaded = 1;
    return 1;
    }
}

// ----- windows -----

#define QWIN_MAX   64
#define QWIN_QUEUE 512

typedef struct {
    int kind;              // QEV_*
    int a, b, c, d;
    char text[128];        // key name, typed text, or an IME composition
} QWinEvent;

enum { QEV_CLOSE = 1, QEV_KEY_DOWN, QEV_KEY_UP, QEV_TEXT, QEV_MOUSE_MOVE, QEV_MOUSE_DOWN,
       QEV_MOUSE_UP, QEV_WHEEL, QEV_RESIZE, QEV_FOCUS, QEV_BLUR, QEV_EXPOSE, QEV_COMPOSE };

typedef struct {
    int used;
    void* win;
    void* ren;
    void* font;            // glyph atlas texture
    uint32_t id;
    uint8_t r, g, b, a;    // current draw colour
    QWinEvent q[QWIN_QUEUE];
    int qhead, qlen;
    // High DPI. Programs draw in logical pixels; `scale` physical pixels
    // per logical one = the OS's backing ratio (macOS Retina, Wayland,
    // Windows with DPI scaling: drawable / window size) x `ui` (an extra
    // factor from QUIRK_UI_SCALE / GDK_SCALE, for X11 and tests). Window
    // and mouse coordinates are in window units = logical x ui.
    float scale, ui;
} QWin;

static QWin qwins[QWIN_MAX];
static int qwin_headless = 0;

static QWin* qwin_get(int64_t h) {
    if (h < 0 || h >= QWIN_MAX || !qwins[h].used) return NULL;
    return &qwins[h];
}

static void qfont_free_glyphs_of(int64_t h);

// Extra scale factor asked for in the environment: QUIRK_UI_SCALE (any
// platform), else GDK_SCALE (the GNOME / X11 convention). 1 when unset.
static float qwin_env_scale(void) {
    const char* names[] = { "QUIRK_UI_SCALE", "GDK_SCALE" };
    for (int i = 0; i < 2; i++) {
        const char* v = getenv(names[i]);
        if (v && *v) {
            float f = (float)atof(v);
            if (f >= 0.5f && f <= 8.0f) return f;
        }
    }
    return 1.0f;
}

// Re-read the window's pixel ratio (it changes when the window moves to a
// screen with another density) and apply it to the renderer. Glyphs are
// rendered at the physical size, so a new scale drops the window's glyphs.
static void qwin_rescale(QWin* w) {
    int ww = 0, wh = 0, ow = 0, oh = 0;
    qsdl.GetWindowSize(w->win, &ww, &wh);
    qsdl.GetRendererOutputSize(w->ren, &ow, &oh);
    float ratio = (ww > 0 && ow > 0) ? (float)ow / (float)ww : 1.0f;
    float s = ratio * w->ui;
    if (!qsdl.RenderSetScale) s = 1.0f;
    if (s < 0.25f) s = 0.25f;
    if (w->scale > 0 && fabsf(s - w->scale) < 0.001f) return;
    w->scale = s;
    qsdl.RenderSetScale(w->ren, s, s);
    qfont_free_glyphs_of(w - qwins);
}

// Window units (events) → logical pixels.
static int qwin_to_logical(const QWin* w, int v) {
    return w->ui == 1.0f ? v : (int)floorf((float)v / w->ui);
}

static void qwin_push(QWin* w, const QWinEvent* e) {
    if (w->qlen == QWIN_QUEUE) {                   // full: drop the oldest
        w->qhead = (w->qhead + 1) % QWIN_QUEUE;
        w->qlen--;
    }
    w->q[(w->qhead + w->qlen) % QWIN_QUEUE] = *e;
    w->qlen++;
}

static QWin* qwin_by_id(uint32_t id) {
    for (int i = 0; i < QWIN_MAX; i++)
        if (qwins[i].used && qwins[i].id == id) return &qwins[i];
    return NULL;
}

static int32_t qev_i32(const QSdlEvent* e, int off) { int32_t v; memcpy(&v, e->bytes + off, 4); return v; }
static uint32_t qev_u32(const QSdlEvent* e, int off) { uint32_t v; memcpy(&v, e->bytes + off, 4); return v; }
static uint16_t qev_u16(const QSdlEvent* e, int off) { uint16_t v; memcpy(&v, e->bytes + off, 2); return v; }

// Move SDL's pending events into the windows' queues.
static void qwin_pump(void) {
    if (qsdl.loaded != 1 || !qsdl.inited) return;
    QSdlEvent se;
    while (qsdl.PollEvent(&se)) {
        uint32_t type = qev_u32(&se, 0);
        QWinEvent ev;
        memset(&ev, 0, sizeof ev);
        if (type == QSDL_QUIT) {                   // e.g. Ctrl+C / last window: close all
            ev.kind = QEV_CLOSE;
            for (int i = 0; i < QWIN_MAX; i++) if (qwins[i].used) qwin_push(&qwins[i], &ev);
            continue;
        }
        QWin* w = qwin_by_id(qev_u32(&se, 8));
        if (!w) continue;
        switch (type) {
            case QSDL_WINDOWEVENT: {
                uint8_t we = se.bytes[12];
                if (we == QSDL_WE_CLOSE) ev.kind = QEV_CLOSE;
                else if (we == QSDL_WE_SIZE_CHANGED) {
                    qwin_rescale(w);
                    ev.kind = QEV_RESIZE;
                    ev.a = qwin_to_logical(w, qev_i32(&se, 16)); ev.b = qwin_to_logical(w, qev_i32(&se, 20));
                }
                else if (we == QSDL_WE_DISPLAY_CHANGED) qwin_rescale(w);
                else if (we == QSDL_WE_FOCUS_GAINED) ev.kind = QEV_FOCUS;
                else if (we == QSDL_WE_FOCUS_LOST) ev.kind = QEV_BLUR;
                break;
            }
            case QSDL_KEYDOWN:
            case QSDL_KEYUP: {
                ev.kind = type == QSDL_KEYDOWN ? QEV_KEY_DOWN : QEV_KEY_UP;
                int32_t sym = qev_i32(&se, 20);
                uint16_t mod = qev_u16(&se, 24);
                ev.a = sym;
                ev.b = se.bytes[13];               // repeat
                ev.d = ((mod & 0x0003) ? 1 : 0) | ((mod & 0x00C0) ? 2 : 0) |
                       ((mod & 0x0300) ? 4 : 0) | ((mod & 0x0C00) ? 8 : 0);
                const char* name = qsdl.GetKeyName(sym);
                size_t k = 0;
                for (; name && name[k] && k < sizeof ev.text - 1; k++) {
                    char c = name[k];
                    ev.text[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : (c == ' ' ? '_' : c);
                }
                ev.text[k] = '\0';
                break;
            }
            case QSDL_TEXTINPUT:
                ev.kind = QEV_TEXT;
                memcpy(ev.text, se.bytes + 12, 32);
                ev.text[31] = '\0';
                break;
            // The text an input method is composing (shown at the cursor
            // until it's committed as QEV_TEXT; "" when composition ends).
            // a = cursor within it, b = length of its selected part, both
            // in characters.
            case QSDL_TEXTEDITING:
                ev.kind = QEV_COMPOSE;
                memcpy(ev.text, se.bytes + 12, 32);
                ev.text[31] = '\0';
                ev.a = qev_i32(&se, 44); ev.b = qev_i32(&se, 48);
                break;
            case QSDL_TEXTEDITING_EXT: {
                char* t;
                memcpy(&t, se.bytes + 16, sizeof t);
                ev.kind = QEV_COMPOSE;
                if (t) {
                    size_t n = strlen(t);
                    if (n > sizeof ev.text - 1) {          // cut on a character boundary
                        n = sizeof ev.text - 1;
                        while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--;
                    }
                    memcpy(ev.text, t, n);
                    ev.text[n] = '\0';
                    qsdl.Free(t);
                }
                ev.a = qev_i32(&se, 24); ev.b = qev_i32(&se, 28);
                break;
            }
            case QSDL_MOUSEMOTION:
                ev.kind = QEV_MOUSE_MOVE;
                ev.a = qwin_to_logical(w, qev_i32(&se, 20)); ev.b = qwin_to_logical(w, qev_i32(&se, 24));
                ev.c = (int)qev_u32(&se, 16);      // buttons held (bit 0 = left)
                break;
            case QSDL_MOUSEBUTTONDOWN:
            case QSDL_MOUSEBUTTONUP:
                ev.kind = type == QSDL_MOUSEBUTTONDOWN ? QEV_MOUSE_DOWN : QEV_MOUSE_UP;
                ev.a = qwin_to_logical(w, qev_i32(&se, 20)); ev.b = qwin_to_logical(w, qev_i32(&se, 24));
                ev.c = se.bytes[16];               // 1 left, 2 middle, 3 right
                ev.d = se.bytes[18];               // clicks
                break;
            case QSDL_MOUSEWHEEL: {
                int32_t dx = qev_i32(&se, 16), dy = qev_i32(&se, 20);
                if (qev_u32(&se, 24) == 1) { dx = -dx; dy = -dy; }   // flipped
                ev.kind = QEV_WHEEL; ev.a = dx; ev.b = dy;
                break;
            }
        }
        if (ev.kind) qwin_push(w, &ev);
    }
}

// Upload the font as a white-on-transparent atlas, one glyph per 8px column.
static void* qwin_make_font(void* ren) {
    int n = (int)(sizeof qfont_glyphs / sizeof qfont_glyphs[0]);
    int W = n * QFONT_W, H = QFONT_H;
    uint32_t* px = (uint32_t*)malloc((size_t)W * H * 4);
    if (!px) return NULL;
    for (int g = 0; g < n; g++)
        for (int y = 0; y < H; y++)
            for (int x = 0; x < QFONT_W; x++)
                px[y * W + g * QFONT_W + x] = (qfont_glyphs[g][y] & (0x80 >> x)) ? 0xFFFFFFFFu : 0x00FFFFFFu;
    void* tex = qsdl.CreateTexture(ren, QSDL_PIXELFORMAT_ARGB8888, 0, W, H);
    if (tex) {
        qsdl.UpdateTexture(tex, NULL, px, W * 4);
        qsdl.SetTextureBlendMode(tex, QSDL_BLENDMODE_BLEND);
    }
    free(px);
    return tex;
}

// True if windows can be opened here (SDL2 found and its video starts).
int Window_available() {
    return qsdl_load();
}

// Why the last window operation failed.
String* Window_error() {
    return make_String(qwin_err);
}

// Open a window; returns its handle, or -1 (see Window_error).
// flags: 1 resizable, 2 hidden, 4 vsync, 8 headless (no display: SDL's
// dummy video driver + software rendering; only honoured before the first
// window opens).
int64_t Window_open_raw(String* title, int64_t width, int64_t height, int64_t flags) {
    if (!qsdl_load()) return -1;
    if (!qsdl.inited) {
        if (flags & 8) {
            // The environment variable, not just the hint: SDL before 2.0.22
            // only reads SDL_VIDEODRIVER from the environment.
#ifndef _WIN32
            setenv("SDL_VIDEODRIVER", "dummy", 1);
#endif
            qsdl.SetHint("SDL_VIDEODRIVER", "dummy");
            qwin_headless = 1;
        }
        qsdl.SetHint("SDL_NO_SIGNAL_HANDLERS", "1");      // keep Ctrl+C for Quirk
        // Windows: per-monitor DPI aware, with window coordinates in
        // logical units and a full-resolution drawable (like macOS) —
        // instead of Windows stretching a 96-DPI bitmap (blurry).
        qsdl.SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
        qsdl.SetHint("SDL_WINDOWS_DPI_SCALING", "1");
        // IME: the input method's own candidate list (Windows), and
        // compositions longer than 32 bytes in one event (SDL 2.0.22+).
        qsdl.SetHint("SDL_IME_SHOW_UI", "1");
        qsdl.SetHint("SDL_IME_SUPPORT_EXTENDED_TEXT", "1");
        if (qsdl.Init(QSDL_INIT_VIDEO) != 0) { qwin_set_err("can't start SDL video"); return -1; }
        qsdl.inited = 1;
        qsdl.StartTextInput();
    }
    int slot = -1;
    for (int i = 0; i < QWIN_MAX && slot < 0; i++) if (!qwins[i].used) slot = i;
    if (slot < 0) { snprintf(qwin_err, sizeof qwin_err, "too many windows (max %d)", QWIN_MAX); return -1; }
    char* t = title ? make_safe_cstr(title) : "";
    uint32_t wf = (flags & 2) ? QSDL_WINDOW_HIDDEN : QSDL_WINDOW_SHOWN;
    if (flags & 1) wf |= QSDL_WINDOW_RESIZABLE;
    wf |= QSDL_WINDOW_ALLOW_HIGHDPI;
    float ui = qsdl.RenderSetScale ? qwin_env_scale() : 1.0f;
    void* win = qsdl.CreateWindow(t ? t : "", QSDL_WINDOWPOS_CENTERED, QSDL_WINDOWPOS_CENTERED,
                                  (int)(width * ui + 0.5f), (int)(height * ui + 0.5f), wf);
    if (!win) { qwin_set_err("can't create the window"); return -1; }
    uint32_t rf = qwin_headless ? QSDL_RENDERER_SOFTWARE : QSDL_RENDERER_ACCELERATED;
    if ((flags & 4) && !qwin_headless) rf |= QSDL_RENDERER_VSYNC;
    void* ren = qsdl.CreateRenderer(win, -1, rf);
    if (!ren) ren = qsdl.CreateRenderer(win, -1, QSDL_RENDERER_SOFTWARE);
    if (!ren) { qwin_set_err("can't create a renderer"); qsdl.DestroyWindow(win); return -1; }
    qsdl.SetRenderDrawBlendMode(ren, QSDL_BLENDMODE_BLEND);
    QWin* w = &qwins[slot];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->win = win;
    w->ren = ren;
    w->id = qsdl.GetWindowID(win);
    w->font = qwin_make_font(ren);
    w->r = w->g = w->b = 0; w->a = 255;
    w->ui = ui;
    w->scale = 0;
    qwin_rescale(w);
    return slot;
}

static void qwin_free_images_of(int64_t h);

void Window_close_raw(int64_t h) {
    QWin* w = qwin_get(h);
    if (!w) return;
    qwin_free_images_of(h);
    qfont_free_glyphs_of(h);
    if (w->font) qsdl.DestroyTexture(w->font);
    qsdl.DestroyRenderer(w->ren);
    qsdl.DestroyWindow(w->win);
    w->used = 0;
    qwin_pump();                                   // drop its leftover events
}

int Window_is_open_raw(int64_t h) {
    return qwin_get(h) != NULL;
}

// The window's next event as [kind, a, b, c, d, text], or [] if none is
// waiting. `timeout_ms` > 0 waits up to that long for one (letting other
// tasks run); -1 waits forever.
List* Window_next_event_raw(int64_t h, int64_t timeout_ms) {
    List* res = (List*)quirk_alloc(QT_LIST, sizeof(List));
    Core_Collections_List_List___init(res);
    QWin* w = qwin_get(h);
    if (!w) return res;
    uint32_t start = qsdl.GetTicks();
    for (;;) {
        qwin_pump();
        if (w->qlen > 0 || !qwin_get(h)) break;
        if (timeout_ms == 0) break;
        if (timeout_ms > 0 && (int64_t)(qsdl.GetTicks() - start) >= timeout_ms) break;
        quirk_gil_release();
#ifndef _WIN32
        usleep(2000);
#endif
        quirk_gil_acquire();
    }
    if (!qwin_get(h) || w->qlen == 0) return res;
    QWinEvent ev = w->q[w->qhead];
    w->qhead = (w->qhead + 1) % QWIN_QUEUE;
    w->qlen--;
    Core_Collections_List_List_append(res, quirk_int_to_opaque(ev.kind));
    Core_Collections_List_List_append(res, quirk_int_to_opaque(ev.a));
    Core_Collections_List_List_append(res, quirk_int_to_opaque(ev.b));
    Core_Collections_List_List_append(res, quirk_int_to_opaque(ev.c));
    Core_Collections_List_List_append(res, quirk_int_to_opaque(ev.d));
    Core_Collections_List_List_append(res, make_String(ev.text));
    return res;
}

// ----- drawing (colour is 0xAARRGGBB) -----

static QWin* qwin_draw(int64_t h, int64_t color) {
    QWin* w = qwin_get(h);
    if (!w) return NULL;
    uint32_t c = (uint32_t)color;
    w->a = (uint8_t)(c >> 24); w->r = (uint8_t)(c >> 16); w->g = (uint8_t)(c >> 8); w->b = (uint8_t)c;
    qsdl.SetRenderDrawColor(w->ren, w->r, w->g, w->b, w->a);
    return w;
}

void Window_clear_raw(int64_t h, int64_t color) {
    QWin* w = qwin_draw(h, color);
    if (!w) return;
    qwin_rescale(w);           // once a frame: catches a move to another screen
    qsdl.RenderClear(w->ren);
}

void Window_rect_raw(int64_t h, int64_t x, int64_t y, int64_t rw, int64_t rh, int64_t color, int64_t fill) {
    QWin* w = qwin_draw(h, color);
    if (!w) return;
    QSdlRect r = { (int)x, (int)y, (int)rw, (int)rh };
    if (fill) qsdl.RenderFillRect(w->ren, &r); else qsdl.RenderDrawRect(w->ren, &r);
}

void Window_line_raw(int64_t h, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t color) {
    QWin* w = qwin_draw(h, color);
    if (w) qsdl.RenderDrawLine(w->ren, (int)x1, (int)y1, (int)x2, (int)y2);
}

void Window_point_raw(int64_t h, int64_t x, int64_t y, int64_t color) {
    QWin* w = qwin_draw(h, color);
    if (w) qsdl.RenderDrawPoint(w->ren, (int)x, (int)y);
}

// Midpoint circle: an outline, or horizontal spans when filled.
// Midpoint circle in the renderer's current units.
static void qwin_circle(QWin* w, int64_t cx, int64_t cy, int64_t radius, int64_t fill) {
    int x = (int)radius, y = 0, err = 1 - x;
    int lastY = -1;
    while (x >= y) {
        if (fill) {
            if (y != lastY) {
                qsdl.RenderDrawLine(w->ren, (int)cx - x, (int)cy + y, (int)cx + x, (int)cy + y);
                if (y) qsdl.RenderDrawLine(w->ren, (int)cx - x, (int)cy - y, (int)cx + x, (int)cy - y);
            }
            if (x != y && err >= 0) {
                qsdl.RenderDrawLine(w->ren, (int)cx - y, (int)cy + x, (int)cx + y, (int)cy + x);
                qsdl.RenderDrawLine(w->ren, (int)cx - y, (int)cy - x, (int)cx + y, (int)cy - x);
            }
            lastY = y;
        } else {
            int pts[8][2] = { {x, y}, {y, x}, {-y, x}, {-x, y}, {-x, -y}, {-y, -x}, {y, -x}, {x, -y} };
            for (int i = 0; i < 8; i++) qsdl.RenderDrawPoint(w->ren, (int)cx + pts[i][0], (int)cy + pts[i][1]);
        }
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
    if (fill && radius == 0) qsdl.RenderDrawPoint(w->ren, (int)cx, (int)cy);
}

void Window_circle_raw(int64_t h, int64_t cx, int64_t cy, int64_t radius, int64_t color, int64_t fill) {
    QWin* w = qwin_draw(h, color);
    if (!w || radius < 0) return;
    float sc = w->scale > 0 ? w->scale : 1.0f;
    if (sc == 1.0f || !qsdl.RenderSetScale) { qwin_circle(w, cx, cy, radius, fill); return; }
    // High DPI: trace the circle in physical pixels (smooth at 2x, not a
    // circle of 2x2 blocks), over the same logical pixels.
    qsdl.RenderSetScale(w->ren, 1.0f, 1.0f);
    qwin_circle(w, (int64_t)floorf(((float)cx + 0.5f) * sc),
                (int64_t)floorf(((float)cy + 0.5f) * sc),
                (int64_t)floorf(((float)radius + 0.5f) * sc - 0.5f + 0.5f), fill);
    qsdl.RenderSetScale(w->ren, sc, sc);
}

// Next UTF-8 codepoint of `s` at *i (advances it); invalid bytes give '?'.
static unsigned qwin_utf8(const char* s, size_t len, size_t* i) {
    unsigned char c = (unsigned char)s[*i];
    if (c < 0x80) { (*i)++; return c; }
    int n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (n < 0 || *i + (size_t)n >= len + 0) { (*i)++; return '?'; }
    unsigned cp = c & (0x3F >> n);
    for (int k = 1; k <= n; k++) {
        unsigned char cc = (unsigned char)s[*i + k];
        if ((cc & 0xC0) != 0x80) { (*i)++; return '?'; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *i += (size_t)n + 1;
    return cp;
}

// Pixel width of `text` in the built-in font at `scale` (longest line).
int64_t Window_text_width(String* text, int64_t scale) {
    if (!text || !text->buffer) return 0;
    if (scale < 1) scale = 1;
    size_t i = 0, len = (size_t)text->length;
    int64_t col = 0, best = 0;
    while (i < len) {
        unsigned cp = qwin_utf8(text->buffer, len, &i);
        if (cp == '\n') { col = 0; continue; }
        col++;
        if (col > best) best = col;
    }
    return best * QFONT_W * scale;
}

// Draw `text` with its top-left at (x, y); "\n" starts a new line.
void Window_text_raw(int64_t h, String* text, int64_t x, int64_t y, int64_t color, int64_t scale) {
    QWin* w = qwin_draw(h, color);
    if (!w || !w->font || !text || !text->buffer) return;
    if (scale < 1) scale = 1;
    qsdl.SetTextureColorMod(w->font, w->r, w->g, w->b);
    qsdl.SetTextureAlphaMod(w->font, w->a);
    size_t i = 0, len = (size_t)text->length;
    int64_t cx = x, cy = y;
    while (i < len) {
        unsigned cp = qwin_utf8(text->buffer, len, &i);
        if (cp == '\n') { cx = x; cy += QFONT_H * scale; continue; }
        if (cp != ' ') {
            QSdlRect src = { qfont_index(cp) * QFONT_W, 0, QFONT_W, QFONT_H };
            QSdlRect dst = { (int)cx, (int)cy, (int)(QFONT_W * scale), (int)(QFONT_H * scale) };
            qsdl.RenderCopy(w->ren, w->font, &src, &dst);
        }
        cx += QFONT_W * scale;
    }
}

// Show what has been drawn since the last present. With vsync this
// waits for the display; other tasks run meanwhile.
void Window_present_raw(int64_t h) {
    QWin* w = qwin_get(h);
    if (!w) return;
    quirk_gil_release();
    qsdl.RenderPresent(w->ren);
    quirk_gil_acquire();
}

// ----- window properties -----

// [width, height] of the drawable area in logical pixels.
List* Window_size_raw(int64_t h) {
    List* res = (List*)quirk_alloc(QT_LIST, sizeof(List));
    Core_Collections_List_List___init(res);
    QWin* w = qwin_get(h);
    int ww = 0, hh = 0;
    if (w) {
        qsdl.GetRendererOutputSize(w->ren, &ww, &hh);
        if (w->scale > 0 && w->scale != 1.0f) {
            ww = (int)((float)ww / w->scale + 0.5f);
            hh = (int)((float)hh / w->scale + 0.5f);
        }
    }
    Core_Collections_List_List_append(res, quirk_int_to_opaque(ww));
    Core_Collections_List_List_append(res, quirk_int_to_opaque(hh));
    return res;
}

// Physical pixels per logical pixel (2.0 on a Retina / 200% screen).
double Window_scale_raw(int64_t h) {
    QWin* w = qwin_get(h);
    return (w && w->scale > 0) ? (double)w->scale : 1.0;
}

// Where text is being typed (the cursor's rectangle, logical pixels), so
// the input method opens its candidate list next to it.
void Window_text_input_area_raw(int64_t h, int64_t x, int64_t y, int64_t rw, int64_t rh) {
    QWin* w = qwin_get(h);
    if (!w || !qsdl.SetTextInputRect) return;
    float u = w->ui > 0 ? w->ui : 1.0f;
    QSdlRect r = { (int)((float)x * u), (int)((float)y * u), (int)((float)rw * u), (int)((float)rh * u) };
    qsdl.SetTextInputRect(&r);
}

// Tests: queue a text (kind 0) or IME composition (kind 1) event for the
// window through SDL's own event queue, as an input method would.
void Window_simulate_text_raw(int64_t h, int64_t kind, String* text, int64_t start, int64_t length) {
    QWin* w = qwin_get(h);
    if (!w || !qsdl.PushEvent) return;
    QSdlEvent se;
    memset(&se, 0, sizeof se);
    uint32_t type = kind == 1 ? QSDL_TEXTEDITING : QSDL_TEXTINPUT;
    memcpy(se.bytes, &type, 4);
    memcpy(se.bytes + 8, &w->id, 4);
    const char* t = text ? make_safe_cstr(text) : "";
    size_t n = strlen(t);
    if (n > 31) { n = 31; while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--; }
    memcpy(se.bytes + 12, t, n);
    int32_t st = (int32_t)start, ln = (int32_t)length;
    memcpy(se.bytes + 44, &st, 4);
    memcpy(se.bytes + 48, &ln, 4);
    qsdl.PushEvent(&se);
}

// Limit drawing to a rectangle (scrolling areas, popups); width < 0
// lifts the limit. Window_clear still clears the whole window.
void Window_clip_raw(int64_t h, int64_t x, int64_t y, int64_t cw, int64_t ch) {
    QWin* w = qwin_get(h);
    if (!w) return;
    if (cw < 0) { qsdl.RenderSetClipRect(w->ren, NULL); return; }
    QSdlRect r = { (int)x, (int)y, (int)(cw > 0 ? cw : 0), (int)(ch > 0 ? ch : 0) };
    qsdl.RenderSetClipRect(w->ren, &r);
}

// The clipboard. The system one once a (non-headless) window has started
// SDL's video; otherwise — headless tests, before any window — a copy kept
// here, so copy / paste still round-trips.
static char* qwin_clipboard = NULL;

void Window_set_clipboard_raw(String* text) {
    const char* t = text ? make_safe_cstr(text) : "";
    size_t n = strlen(t);
    char* copy = (char*)malloc(n + 1);
    if (copy) { memcpy(copy, t, n + 1); qwin_clipboard = copy; }
    if (qsdl.loaded == 1 && qsdl.inited && !qwin_headless) qsdl.SetClipboardText(t);
}

String* Window_clipboard_raw() {
    if (qsdl.loaded == 1 && qsdl.inited && !qwin_headless) {
        char* t = qsdl.GetClipboardText();
        if (t) {
            String* s = make_String(t);
            qsdl.Free(t);
            return s;
        }
    }
    return make_String(qwin_clipboard ? qwin_clipboard : "");
}

void Window_set_title_raw(int64_t h, String* title) {
    QWin* w = qwin_get(h);
    char* t = title ? make_safe_cstr(title) : NULL;
    if (w && t) qsdl.SetWindowTitle(w->win, t);
}

// Colour (0xRRGGBB) of a pixel in what has been drawn so far, or -1.
int64_t Window_pixel_raw(int64_t h, int64_t x, int64_t y) {
    QWin* w = qwin_get(h);
    if (!w) return -1;
    float sc = w->scale > 0 ? w->scale : 1.0f;
    // The physical pixel at the centre of logical pixel (x, y).
    QSdlRect r = { (int)(((float)x + 0.5f) * sc), (int)(((float)y + 0.5f) * sc), 1, 1 };
    uint32_t px = 0;
    if (qsdl.RenderReadPixels(w->ren, &r, QSDL_PIXELFORMAT_ARGB8888, &px, 4) != 0) return -1;
    return (int64_t)(px & 0xFFFFFFu);
}

// Save what has been drawn so far as a BMP file. 0 ok, -1 failed.
int64_t Window_save_bmp_raw(int64_t h, String* path) {
    QWin* w = qwin_get(h);
    char* p = path ? make_safe_cstr(path) : NULL;
    if (!w || !p) return -1;
    int ww = 0, hh = 0;
    qsdl.GetRendererOutputSize(w->ren, &ww, &hh);
    void* surf = qsdl.CreateRGBSurfaceWithFormat(0, ww, hh, 32, QSDL_PIXELFORMAT_ARGB8888);
    if (!surf) { qwin_set_err("can't make a surface"); return -1; }
    // SDL_Surface: flags u32 @0, format* @8, w @16, h @20, pitch @24, pixels @32.
    int pitch; void* pixels;
    memcpy(&pitch, (char*)surf + 24, sizeof pitch);
    memcpy(&pixels, (char*)surf + 32, sizeof pixels);
    int rc = qsdl.RenderReadPixels(w->ren, NULL, QSDL_PIXELFORMAT_ARGB8888, pixels, pitch);
    if (rc == 0) {
        void* rw = qsdl.RWFromFile(p, "wb");
        rc = rw ? qsdl.SaveBMP_RW(surf, rw, 1) : -1;
    }
    if (rc != 0) qwin_set_err("can't save the image");
    qsdl.FreeSurface(surf);
    return rc == 0 ? 0 : -1;
}

// ----- images (PNG / JPEG / BMP / GIF / TGA) -----

#define QIMG_MAX 1024
static struct { int used; int64_t win; void* tex; int w, h; } qimgs[QIMG_MAX];

// Load an image file for drawing in window `h`; returns an image handle
// or -1. A GIF gives its first frame. Decoding runs with the task lock
// released.
int64_t Window_image_load_raw(int64_t h, String* path) {
    QWin* w = qwin_get(h);
    char* p = path ? make_safe_cstr(path) : NULL;
    if (!w || !p) return -1;
    int iw = 0, ih = 0, chans = 0;
    quirk_gil_release();
    unsigned char* px = stbi_load(p, &iw, &ih, &chans, 4);
    quirk_gil_acquire();
    if (!px) {
        FILE* probe = fopen(p, "rb");
        if (!probe) snprintf(qwin_err, sizeof qwin_err, "no such file");
        else { fclose(probe); snprintf(qwin_err, sizeof qwin_err, "not an image it can read (%s)", stbi_failure_reason()); }
        return -1;
    }
    void* tex = qsdl.CreateTexture(w->ren, QSDL_PIXELFORMAT_ABGR8888, 0, iw, ih);
    if (tex) qsdl.UpdateTexture(tex, NULL, px, iw * 4);
    stbi_image_free(px);
    if (!tex) { qwin_set_err("can't make a texture"); return -1; }
    int slot = -1;
    for (int i = 0; i < QIMG_MAX && slot < 0; i++) if (!qimgs[i].used) slot = i;
    if (slot < 0) { qsdl.DestroyTexture(tex); snprintf(qwin_err, sizeof qwin_err, "too many images"); return -1; }
    qimgs[slot].used = 1;
    qimgs[slot].win = h;
    qimgs[slot].tex = tex;
    qsdl.QueryTexture(tex, NULL, NULL, &qimgs[slot].w, &qimgs[slot].h);
    qsdl.SetTextureBlendMode(tex, QSDL_BLENDMODE_BLEND);
    return slot;
}

// [width, height] of an image, or [0, 0].
List* Window_image_size_raw(int64_t img) {
    List* res = (List*)quirk_alloc(QT_LIST, sizeof(List));
    Core_Collections_List_List___init(res);
    int ok = img >= 0 && img < QIMG_MAX && qimgs[img].used;
    Core_Collections_List_List_append(res, quirk_int_to_opaque(ok ? qimgs[img].w : 0));
    Core_Collections_List_List_append(res, quirk_int_to_opaque(ok ? qimgs[img].h : 0));
    return res;
}

// Draw the part (sx, sy, sw, sh) of image `img` (sw / sh -1: to its
// right / bottom edge) at (x, y), scaled to dw x dh (-1: the part's own
// size), with opacity `alpha` 0-255.
void Window_image_draw_raw(int64_t h, int64_t img, int64_t x, int64_t y, int64_t dw, int64_t dh,
                           int64_t sx, int64_t sy, int64_t sw, int64_t sh, int64_t alpha) {
    QWin* w = qwin_get(h);
    if (!w || img < 0 || img >= QIMG_MAX || !qimgs[img].used || qimgs[img].win != h) return;
    int iw = qimgs[img].w, ih = qimgs[img].h;
    QSdlRect src = { (int)sx, (int)sy, sw < 0 ? iw - (int)sx : (int)sw, sh < 0 ? ih - (int)sy : (int)sh };
    if (src.w <= 0 || src.h <= 0) return;
    QSdlRect dst = { (int)x, (int)y, dw < 0 ? src.w : (int)dw, dh < 0 ? src.h : (int)dh };
    qsdl.SetTextureAlphaMod(qimgs[img].tex, (uint8_t)(alpha < 0 ? 0 : alpha > 255 ? 255 : alpha));
    qsdl.RenderCopy(w->ren, qimgs[img].tex, &src, &dst);
}

static void qwin_free_images_of(int64_t h) {
    for (int i = 0; i < QIMG_MAX; i++)
        if (qimgs[i].used && qimgs[i].win == h) { qsdl.DestroyTexture(qimgs[i].tex); qimgs[i].used = 0; }
}

void Window_image_free_raw(int64_t img) {
    if (img < 0 || img >= QIMG_MAX || !qimgs[img].used) return;
    if (qwin_get(qimgs[img].win)) qsdl.DestroyTexture(qimgs[img].tex);
    qimgs[img].used = 0;
}

// ----- time -----

// Pause `ms` milliseconds with the task lock released.
#ifdef _WIN32
// Milliseconds on the precise monotonic clock (QueryPerformanceCounter via
// win_compat.h). GetTickCount64 only moves in ~15.6 ms steps, so ticks()
// around a 30 ms sleep could read 15.
static int64_t qwin_precise_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

void Window_sleep(int64_t ms) {
    if (ms <= 0) return;
    quirk_gil_release();
#ifndef _WIN32
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
#else
    // Sleep() can wake up to one timer tick (~15.6 ms) early: top up until
    // the full time has passed on the precise clock.
    int64_t until = qwin_precise_ms() + ms;
    Sleep((DWORD)ms);
    for (int64_t now = qwin_precise_ms(); now < until; now = qwin_precise_ms())
        Sleep((DWORD)(until - now > 1 ? until - now - 1 : 0));
#endif
    quirk_gil_acquire();
}

// Milliseconds since an arbitrary start (monotonic).
int64_t Window_ticks() {
    struct timespec ts;   // precise on Windows too (QueryPerformanceCounter, win_compat.h)
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ----- TrueType fonts -----
//
// A font is loaded once at a pixel size. Glyphs are rendered on first use
// into a white-on-transparent texture per (window, font, code point) and
// tinted when drawn. Text is UTF-8: any code point the font has works.

#define QFONT_MAX    32
#define QGLYPH_SLOTS 8192

typedef struct {
    int64_t win;          // -1 = empty slot
    int cp;
    void* tex;
    int w, h, xoff, yoff;
} QGlyph;

// A fallback font: loaded the first time the main font lacks a character.
typedef struct {
    char path[512];
    int state;            // 0 not loaded yet, 1 ok, -1 failed
    unsigned char* data;
    stbtt_fontinfo info;
    float scale;
} QFace;

#define QFONT_FALLBACKS 24

typedef struct {
    int used;
    unsigned char* data;
    stbtt_fontinfo info;
    float scale;
    int size;
    int ascent, descent, line_gap;
    QGlyph* glyphs;       // QGLYPH_SLOTS, open addressing
    QFace fb[QFONT_FALLBACKS];   // tried in order for characters it lacks
    int nfb;
} QFont;

static QFont qfonts[QFONT_MAX];

static QFont* qfont_get(int64_t f) {
    if (f < 0 || f >= QFONT_MAX || !qfonts[f].used) return NULL;
    return &qfonts[f];
}

// Next code point of UTF-8 text at *i (invalid bytes read as Latin-1).
static int qutf8_next(const unsigned char* s, size_t n, size_t* i) {
    unsigned char c = s[*i];
    int cp, extra;
    if (c < 0x80) { (*i)++; return c; }
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { (*i)++; return c; }
    if (*i + (size_t)extra >= n) { (*i)++; return c; }    // cut off at the end
    size_t j = *i + 1;
    for (int k = 0; k < extra; k++, j++) {
        if (j >= n || (s[j] & 0xC0) != 0x80) { (*i)++; return c; }
        cp = (cp << 6) | (s[j] & 0x3F);
    }
    *i = j;
    return cp;
}

// Read a font file into GC memory and set up `info` (the first font of a
// .ttc collection). 0 ok, -1 failed (qwin_err says why).
static int qfont_read(const char* p, unsigned char** data_out, stbtt_fontinfo* info) {
    FILE* fp = fopen(p, "rb");
    if (!fp) { snprintf(qwin_err, sizeof qwin_err, "can't open '%s'", p); return -1; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0) { fclose(fp); snprintf(qwin_err, sizeof qwin_err, "'%s' is empty", p); return -1; }
    unsigned char* data = (unsigned char*)GC_malloc_atomic((size_t)n);
    size_t got = data ? fread(data, 1, (size_t)n, fp) : 0;
    fclose(fp);
    if (got != (size_t)n) { snprintf(qwin_err, sizeof qwin_err, "can't read '%s'", p); return -1; }
    int off = stbtt_GetFontOffsetForIndex(data, 0);
    if (off < 0 || !stbtt_InitFont(info, data, off)) {
        snprintf(qwin_err, sizeof qwin_err, "'%s' isn't a TrueType / OpenType font", p);
        return -1;
    }
    *data_out = data;
    return 0;
}

static int qfont_file_exists(const char* p) {
    FILE* fp = fopen(p, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

static void qfont_add_face(QFont* f, const char* p) {
    if (f->nfb >= QFONT_FALLBACKS || !p || !*p) return;
    for (int i = 0; i < f->nfb; i++) if (strcmp(f->fb[i].path, p) == 0) return;
    QFace* fc = &f->fb[f->nfb++];
    memset(fc, 0, sizeof *fc);
    snprintf(fc->path, sizeof fc->path, "%s", p);
}

// Common system fonts for scripts a Latin font lacks (CJK, Hangul,
// Arabic, Hebrew, Thai, Devanagari, symbols) — only the ones present
// are kept, and each is read the first time it's needed.
static void qfont_add_system_fallbacks(QFont* f) {
#if defined(_WIN32)
    const char* win = getenv("WINDIR");
    char base[256];
    snprintf(base, sizeof base, "%s\\Fonts\\", win && *win ? win : "C:\\Windows");
    const char* names[] = { "segoeui.ttf", "msyh.ttc", "msgothic.ttc", "malgun.ttf", "simsun.ttc",
                            "tahoma.ttf", "arial.ttf", "nirmala.ttf", "seguisym.ttf", "arialuni.ttf" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char p[512];
        snprintf(p, sizeof p, "%s%s", base, names[i]);
        if (qfont_file_exists(p)) qfont_add_face(f, p);
    }
#else
    const char* paths[] = {
#if defined(__APPLE__)
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
        "/System/Library/Fonts/AppleSDGothicNeo.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
        "/System/Library/Fonts/GeezaPro.ttc",
        "/System/Library/Fonts/SFHebrew.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Thonburi.ttc",
        "/System/Library/Fonts/Apple Symbols.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/google-droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc",
        "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
        "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansHebrew-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansDevanagari-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansSymbols-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        "/usr/share/fonts/gnu-free/FreeSans.ttf",
#endif
    };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++)
        if (qfont_file_exists(paths[i])) qfont_add_face(f, paths[i]);
#endif
}

// The font that draws `cp`: the main one when it has the character, else
// the first fallback that does (read on first use), else the main one
// (its "missing" box). *fi = -1 for the main font, or the fallback index.
static stbtt_fontinfo* qfont_pick(QFont* f, int cp, float* scale, int* fi) {
    *fi = -1;
    *scale = f->scale;
    if (cp < 32 || stbtt_FindGlyphIndex(&f->info, cp)) return &f->info;
    for (int i = 0; i < f->nfb; i++) {
        QFace* fc = &f->fb[i];
        if (fc->state == 0) {
            char saved[sizeof qwin_err];
            memcpy(saved, qwin_err, sizeof saved);          // a lazy load doesn't report
            fc->state = qfont_read(fc->path, &fc->data, &fc->info) == 0 ? 1 : -1;
            memcpy(qwin_err, saved, sizeof saved);
            if (fc->state == 1) fc->scale = stbtt_ScaleForPixelHeight(&fc->info, (float)f->size);
        }
        if (fc->state == 1 && stbtt_FindGlyphIndex(&fc->info, cp)) {
            *fi = i;
            *scale = fc->scale;
            return &fc->info;
        }
    }
    return &f->info;
}

static void qfont_free_font_glyphs(QFont* f);

// Load a .ttf / .otf / .ttc at `size` pixels; a handle, or -1 (see
// Window_error). `fallback` = 1: characters it lacks come from common
// system fonts (see qfont_add_system_fallbacks).
int64_t Window_font_load_raw(String* path, int64_t size, int64_t fallback) {
    const char* p = path ? make_safe_cstr(path) : "";
    if (size < 1 || size > 512) { snprintf(qwin_err, sizeof qwin_err, "font size %lld is out of range (1-512)", (long long)size); return -1; }
    int slot = -1;
    for (int i = 0; i < QFONT_MAX; i++) if (!qfonts[i].used) { slot = i; break; }
    if (slot < 0) { snprintf(qwin_err, sizeof qwin_err, "too many fonts loaded (max %d)", QFONT_MAX); return -1; }
    QFont* f = &qfonts[slot];
    memset(f, 0, sizeof *f);
    unsigned char* data = NULL;
    if (qfont_read(p, &data, &f->info) != 0) return -1;
    f->data = data;
    f->size = (int)size;
    f->scale = stbtt_ScaleForPixelHeight(&f->info, (float)size);
    int a, d, g;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
    f->ascent = (int)(a * f->scale + 0.5f);
    f->descent = (int)(-d * f->scale + 0.5f);
    f->line_gap = (int)(g * f->scale + 0.5f);
    f->glyphs = (QGlyph*)GC_malloc(sizeof(QGlyph) * QGLYPH_SLOTS);
    for (int i = 0; i < QGLYPH_SLOTS; i++) f->glyphs[i].win = -1;
    if (fallback) qfont_add_system_fallbacks(f);
    f->used = 1;
    return slot;
}

// Add a font file to try (before the system ones) for characters `fh`
// lacks. 0 ok, -1 can't read it (see Window_error).
int64_t Window_font_add_fallback_raw(int64_t fh, String* path) {
    QFont* f = qfont_get(fh);
    const char* p = path ? make_safe_cstr(path) : "";
    if (!f) { snprintf(qwin_err, sizeof qwin_err, "the font was freed"); return -1; }
    QFace probe;
    memset(&probe, 0, sizeof probe);
    if (qfont_read(p, &probe.data, &probe.info) != 0) return -1;
    if (f->nfb >= QFONT_FALLBACKS) f->nfb = QFONT_FALLBACKS - 1;   // drop the last system one
    // Explicit fallbacks go ahead of the system ones.
    memmove(&f->fb[1], &f->fb[0], sizeof(QFace) * (size_t)f->nfb);
    QFace* fc = &f->fb[0];
    *fc = probe;
    snprintf(fc->path, sizeof fc->path, "%s", p);
    fc->state = 1;
    fc->scale = stbtt_ScaleForPixelHeight(&fc->info, (float)f->size);
    f->nfb++;
    qfont_free_font_glyphs(f);                      // characters may now come from it
    return 0;
}

// 1 if every character of `text` can be drawn (by the font or a fallback).
int64_t Window_font_covers_raw(int64_t fh, String* text) {
    QFont* f = qfont_get(fh);
    if (!f || !text) return 0;
    const unsigned char* s = (const unsigned char*)make_safe_cstr(text);
    size_t n = strlen((const char*)s), i = 0;
    while (i < n) {
        int cp = qutf8_next(s, n, &i);
        if (cp < 32) continue;
        float sc; int fi;
        stbtt_fontinfo* info = qfont_pick(f, cp, &sc, &fi);
        if (!stbtt_FindGlyphIndex(info, cp)) return 0;
    }
    return 1;
}

void Window_font_free_raw(int64_t fh) {
    QFont* f = qfont_get(fh);
    if (!f) return;
    for (int i = 0; i < QGLYPH_SLOTS; i++)
        if (f->glyphs[i].win >= 0 && f->glyphs[i].tex) qsdl.DestroyTexture(f->glyphs[i].tex);
    f->used = 0;
    f->data = NULL;
    f->glyphs = NULL;
    for (int i = 0; i < f->nfb; i++) f->fb[i].data = NULL;
    f->nfb = 0;
}

// Drop every rendered glyph of font `f` (in all windows): they're drawn
// again on next use — after a fallback is added, a character may come
// from a different font.
static void qfont_free_font_glyphs(QFont* f) {
    if (!f->glyphs) return;
    for (int i = 0; i < QGLYPH_SLOTS; i++) {
        QGlyph* g = &f->glyphs[i];
        if (g->win >= 0 && g->tex) qsdl.DestroyTexture(g->tex);
        g->win = -1;
        g->tex = NULL;
    }
}

// [ascent, descent, line height] in pixels.
List* Window_font_metrics_raw(int64_t fh) {
    List* res = (List*)quirk_alloc(QT_LIST, sizeof(List));
    Core_Collections_List_List___init(res);
    QFont* f = qfont_get(fh);
    int a = f ? f->ascent : 0, d = f ? f->descent : 0;
    int lh = f ? f->ascent + f->descent + f->line_gap : 0;
    Core_Collections_List_List_append(res, quirk_int_to_opaque(a));
    Core_Collections_List_List_append(res, quirk_int_to_opaque(d));
    Core_Collections_List_List_append(res, quirk_int_to_opaque(lh));
    return res;
}


// ----- line layout (bidi + Arabic joining, see window_bidi.h) -----

static int qfont_has_cb(int c, void* ctx) {
    QFont* f = (QFont*)ctx;
    float sc; int fi;
    return stbtt_FindGlyphIndex(qfont_pick(f, c, &sc, &fi), c) != 0;
}

// One line laid out left to right: glyph k (visual order) is code point
// gcp[k] covering logical characters [cstart, cstart + clen), at level
// lvl (odd = right to left), from x0 to x1.
typedef struct {
    int n, nchars, base;
    int *gcp, *cstart, *clen;
    unsigned char* lvl;
    float *x0, *x1;
    float width;
} QLine;

// Code points of `s` (UTF-8, no newline handling) into a GC array.
static int* qutf8_decode(const unsigned char* s, size_t len, int* count) {
    int* cp = (int*)GC_malloc_atomic(sizeof(int) * (len + 1));
    size_t i = 0;
    int n = 0;
    while (i < len) cp[n++] = qutf8_next(s, len, &i);
    *count = n;
    return cp;
}

static void qline_layout(QFont* f, const int* cps, int n, QLine* L) {
    L->nchars = n;
    L->gcp = (int*)GC_malloc_atomic(sizeof(int) * (n + 1));
    L->cstart = (int*)GC_malloc_atomic(sizeof(int) * (n + 1));
    L->clen = (int*)GC_malloc_atomic(sizeof(int) * (n + 1));
    L->lvl = (unsigned char*)GC_malloc_atomic((size_t)n + 1);
    L->x0 = (float*)GC_malloc_atomic(sizeof(float) * (n + 1));
    L->x1 = (float*)GC_malloc_atomic(sizeof(float) * (n + 1));
    int k;
    int complex = qbidi_needed(cps, n);
    if (!complex) {
        for (k = 0; k < n; k++) { L->gcp[k] = cps[k]; L->cstart[k] = k; L->clen[k] = 1; L->lvl[k] = 0; }
        L->base = 0;
    } else {
        int* g = (int*)GC_malloc_atomic(sizeof(int) * (n + 1));
        int* cs = (int*)GC_malloc_atomic(sizeof(int) * (n + 1));
        int* cl = (int*)GC_malloc_atomic(sizeof(int) * (n + 1));
        k = qshape_arabic(cps, n, g, cs, cl, qfont_has_cb, f);
        int* cls = (int*)GC_malloc_atomic(sizeof(int) * (k + 1));
        for (int i = 0; i < k; i++) cls[i] = qbidi_class(cps[cs[i]]);
        unsigned char* lv = (unsigned char*)GC_malloc_atomic((size_t)k + 1);
        L->base = qbidi_levels(cls, k, -1, lv);
        // L1: trailing white space goes to the paragraph level.
        for (int i = k - 1; i >= 0; i--) {
            int t = qbidi_class(cps[cs[i]]);
            if (t != QB_WS && t != QB_S) break;
            lv[i] = (unsigned char)L->base;
        }
        int* order = (int*)GC_malloc_atomic(sizeof(int) * (k + 1));
        qbidi_reorder(lv, k, order);
        for (int v = 0; v < k; v++) {
            int i = order[v];
            int c = g[i];
            if (lv[i] & 1) c = qbidi_mirror(c);
            L->gcp[v] = c; L->cstart[v] = cs[i]; L->clen[v] = cl[i]; L->lvl[v] = lv[i];
        }
        n = k;
    }
    L->n = n;
    float pen = 0;
    int prev = 0, prevf = -2;
    for (int v = 0; v < n; v++) {
        int c = L->gcp[v], adv, lsb, fi;
        float sc;
        stbtt_fontinfo* info = qfont_pick(f, c, &sc, &fi);
        if (!complex && prev && fi == prevf) pen += stbtt_GetCodepointKernAdvance(info, prev, c) * sc;
        stbtt_GetCodepointHMetrics(info, c, &adv, &lsb);
        L->x0[v] = pen;
        pen += adv * sc;
        L->x1[v] = pen;
        prev = c; prevf = fi;
    }
    L->width = pen;
}

// The first line of `text` (up to "\n"), laid out.
static void qline_of(QFont* f, String* text, QLine* L) {
    const unsigned char* s = (const unsigned char*)make_safe_cstr(text);
    size_t len = strlen((const char*)s);
    const unsigned char* nl = (const unsigned char*)memchr(s, '\n', len);
    if (nl) len = (size_t)(nl - s);
    int n;
    int* cps = qutf8_decode(s, len, &n);
    qline_layout(f, cps, n, L);
}

// Visual x of the caret before logical character `i` (0..nchars).
static float qline_caret(const QLine* L, int i) {
    if (L->n == 0) return 0;
    for (int v = 0; v < L->n; v++) {
        int a = L->cstart[v], len = L->clen[v];
        if (i >= a && i < a + len) {
            float frac = (float)(i - a) / (float)len;
            float w = L->x1[v] - L->x0[v];
            return (L->lvl[v] & 1) ? L->x1[v] - frac * w : L->x0[v] + frac * w;
        }
    }
    // After the last character: the trailing edge of the logically last glyph.
    int best = -1, bestEnd = -1;
    for (int v = 0; v < L->n; v++)
        if (L->cstart[v] + L->clen[v] > bestEnd) { bestEnd = L->cstart[v] + L->clen[v]; best = v; }
    return (L->lvl[best] & 1) ? L->x0[best] : L->x1[best];
}

// The logical caret index nearest to visual x.
static int qline_index_at(const QLine* L, float x) {
    if (L->n == 0) return 0;
    if (x < L->x0[0]) {
        int v = 0;
        return (L->lvl[v] & 1) ? L->cstart[v] + L->clen[v] : L->cstart[v];
    }
    for (int v = 0; v < L->n; v++) {
        if (x >= L->x1[v] && v + 1 < L->n) continue;
        int a = L->cstart[v], len = L->clen[v];
        float w = L->x1[v] - L->x0[v];
        float rel = w > 0 ? (x - L->x0[v]) / w : 0;            // 0..1 across the glyph
        if (rel > 1) rel = 1;
        if (rel < 0) rel = 0;
        int part = (int)(rel * len + 0.5f);                     // 0..len from the left
        return (L->lvl[v] & 1) ? a + len - part : a + part;
    }
    return L->nchars;
}

// [x, width] of the visual pieces covering logical [a, b), appended to `res`.
static void qline_selection(const QLine* L, int a, int b, List* res) {
    float runA = 0, runB = -1;
    for (int v = 0; v < L->n; v++) {
        int s0 = L->cstart[v], s1 = s0 + L->clen[v];
        int lo = a > s0 ? a : s0, hi = b < s1 ? b : s1;
        if (lo >= hi) continue;
        float w = L->x1[v] - L->x0[v];
        float f0 = (float)(lo - s0) / (float)(s1 - s0), f1 = (float)(hi - s0) / (float)(s1 - s0);
        float p0, p1;
        if (L->lvl[v] & 1) { p0 = L->x1[v] - f1 * w; p1 = L->x1[v] - f0 * w; }
        else { p0 = L->x0[v] + f0 * w; p1 = L->x0[v] + f1 * w; }
        if (runB >= 0 && p0 <= runB + 0.5f) { if (p1 > runB) runB = p1; continue; }
        if (runB >= 0) {
            Core_Collections_List_List_append(res, quirk_int_to_opaque((int)(runA + 0.5f)));
            Core_Collections_List_List_append(res, quirk_int_to_opaque((int)(runB - runA + 0.5f)));
        }
        runA = p0; runB = p1;
    }
    if (runB >= 0) {
        Core_Collections_List_List_append(res, quirk_int_to_opaque((int)(runA + 0.5f)));
        Core_Collections_List_List_append(res, quirk_int_to_opaque((int)(runB - runA + 0.5f)));
    }
}

int64_t Window_font_caret_x_raw(int64_t fh, String* text, int64_t index) {
    QFont* f = qfont_get(fh);
    if (!f || !text) return 0;
    QLine L;
    qline_of(f, text, &L);
    if (index < 0) index = 0;
    if (index > L.nchars) index = L.nchars;
    return (int64_t)(qline_caret(&L, (int)index) + 0.5f);
}

int64_t Window_font_index_at_raw(int64_t fh, String* text, int64_t x) {
    QFont* f = qfont_get(fh);
    if (!f || !text) return 0;
    QLine L;
    qline_of(f, text, &L);
    return qline_index_at(&L, (float)x);
}

List* Window_font_selection_raw(int64_t fh, String* text, int64_t a, int64_t b) {
    List* res = (List*)quirk_alloc(QT_LIST, sizeof(List));
    Core_Collections_List_List___init(res);
    QFont* f = qfont_get(fh);
    if (!f || !text || b <= a) return res;
    QLine L;
    qline_of(f, text, &L);
    qline_selection(&L, (int)a, (int)b, res);
    return res;
}

// 1 if `text` has right-to-left or Arabic characters anywhere (it needs
// the visual layout to measure carets and selections).
int64_t Window_text_has_rtl_raw(String* text) {
    if (!text) return 0;
    const unsigned char* s = (const unsigned char*)make_safe_cstr(text);
    int n;
    int* cps = qutf8_decode(s, strlen((const char*)s), &n);
    return qbidi_needed(cps, n);
}

// 1 if the first line of `text` reads right to left (its first strong
// character is Hebrew / Arabic / ...).
int64_t Window_text_is_rtl_raw(String* text) {
    if (!text) return 0;
    const unsigned char* s = (const unsigned char*)make_safe_cstr(text);
    size_t n = strlen((const char*)s), i = 0;
    while (i < n) {
        int c = qutf8_next(s, n, &i);
        if (c == '\n') break;
        int t = qbidi_class(c);
        if (t == QB_L) return 0;
        if (t == QB_R || t == QB_AL) return 1;
    }
    return 0;
}

// Width in pixels of the longest line of `text` in font `fh`.
int64_t Window_font_text_width_raw(int64_t fh, String* text) {
    QFont* f = qfont_get(fh);
    if (!f || !text) return 0;
    const unsigned char* s = (const unsigned char*)make_safe_cstr(text);
    size_t n = strlen((const char*)s), i = 0;
    {
        // Right-to-left / Arabic text: measure the laid-out lines.
        int cnt;
        int* all = qutf8_decode(s, n, &cnt);
        if (qbidi_needed(all, cnt)) {
            float wmax = 0;
            int start = 0;
            for (int j = 0; j <= cnt; j++) {
                if (j < cnt && all[j] != '\n') continue;
                QLine L;
                qline_layout(f, all + start, j - start, &L);
                if (L.width > wmax) wmax = L.width;
                start = j + 1;
            }
            return (int64_t)(wmax + 0.999f);
        }
    }
    float pen = 0, widest = 0;
    int prev = 0, prevf = -2;
    while (i < n) {
        int cp = qutf8_next(s, n, &i);
        if (cp == '\n') { if (pen > widest) widest = pen; pen = 0; prev = 0; continue; }
        int adv, lsb, fi;
        float sc;
        stbtt_fontinfo* info = qfont_pick(f, cp, &sc, &fi);
        stbtt_GetCodepointHMetrics(info, cp, &adv, &lsb);
        if (prev && fi == prevf) pen += stbtt_GetCodepointKernAdvance(info, prev, cp) * sc;
        pen += adv * sc;
        prev = cp;
        prevf = fi;
    }
    if (pen > widest) widest = pen;
    return (int64_t)(widest + 0.999f);
}

static QGlyph* qfont_glyph(QFont* f, int64_t h, QWin* w, int cp) {
    uint32_t hash = (uint32_t)cp * 2654435761u ^ (uint32_t)h * 40503u;
    for (int probe = 0; probe < QGLYPH_SLOTS; probe++) {
        QGlyph* g = &f->glyphs[(hash + (uint32_t)probe) % QGLYPH_SLOTS];
        if (g->win == h && g->cp == cp) return g;
        if (g->win >= 0) continue;
        // Empty: render it.
        int gw = 0, gh = 0, xo = 0, yo = 0;
        float fsc; int fi;
        stbtt_fontinfo* info = qfont_pick(f, cp, &fsc, &fi);       // the main font or a fallback
        float gs = fsc * (w->scale > 0 ? w->scale : 1.0f);          // physical pixels
        unsigned char* bmp = stbtt_GetCodepointBitmap(info, gs, gs, cp, &gw, &gh, &xo, &yo);
        g->win = h; g->cp = cp; g->w = gw; g->h = gh; g->xoff = xo; g->yoff = yo; g->tex = NULL;
        if (bmp && gw > 0 && gh > 0) {
            uint32_t* px = (uint32_t*)GC_malloc_atomic((size_t)gw * gh * 4);
            if (px) {
                for (int k = 0; k < gw * gh; k++) px[k] = ((uint32_t)bmp[k] << 24) | 0x00FFFFFFu;
                void* tex = qsdl.CreateTexture(w->ren, QSDL_PIXELFORMAT_ARGB8888, 0, gw, gh);
                if (tex) {
                    qsdl.UpdateTexture(tex, NULL, px, gw * 4);
                    qsdl.SetTextureBlendMode(tex, QSDL_BLENDMODE_BLEND);
                    g->tex = tex;
                }
            }
        }
        return g;
    }
    return NULL;                       // cache full: the glyph isn't drawn
}

// Draw `text` with its top-left at (x, y) (the first baseline is at
// y + ascent); "\n" starts a line.
// Draw code point `cp` with its origin at (pen, baseline).
static void qfont_blit(QWin* w, QFont* f, int64_t h, int cp, float pen, int base,
                       uint8_t r, uint8_t gc, uint8_t b, uint8_t a) {
        QGlyph* g = qfont_glyph(f, h, w, cp);
        if (g && g->tex) {
            qsdl.SetTextureColorMod(g->tex, r, gc, b);
            qsdl.SetTextureAlphaMod(g->tex, a);
            float sc = w->scale > 0 ? w->scale : 1.0f;
            if (sc != 1.0f && qsdl.RenderCopyF) {
                // The glyph is at physical size: place it on whole physical
                // pixels and let the renderer's scale map it back 1:1.
                float px = floorf(pen * sc + 0.5f) + (float)g->xoff;
                float py = floorf((float)base * sc + 0.5f) + (float)g->yoff;
                QSdlFRect dst = { px / sc, py / sc, (float)g->w / sc, (float)g->h / sc };
                qsdl.RenderCopyF(w->ren, g->tex, NULL, &dst);
            } else {
                QSdlRect dst = { (int)(pen + 0.5f) + g->xoff, base + g->yoff, g->w, g->h };
                qsdl.RenderCopy(w->ren, g->tex, NULL, &dst);
            }
        }
}

void Window_font_text_raw(int64_t h, int64_t fh, String* text, int64_t x, int64_t y, int64_t color) {
    QWin* w = qwin_get(h);
    QFont* f = qfont_get(fh);
    if (!w || !f || !text) return;
    uint8_t a = (uint8_t)((color >> 24) & 0xFF), r = (uint8_t)((color >> 16) & 0xFF);
    uint8_t gc = (uint8_t)((color >> 8) & 0xFF), b = (uint8_t)(color & 0xFF);
    const unsigned char* s = (const unsigned char*)make_safe_cstr(text);
    size_t n = strlen((const char*)s), i = 0;
    float pen = (float)x;
    int base = (int)y + f->ascent;
    int lh = f->ascent + f->descent + f->line_gap;
    {
        // Right-to-left / Arabic text: draw each laid-out line.
        int cnt;
        int* all = qutf8_decode(s, n, &cnt);
        if (qbidi_needed(all, cnt)) {
            int start = 0;
            for (int j = 0; j <= cnt; j++) {
                if (j < cnt && all[j] != '\n') continue;
                QLine L;
                qline_layout(f, all + start, j - start, &L);
                for (int v = 0; v < L.n; v++)
                    qfont_blit(w, f, h, L.gcp[v], (float)x + L.x0[v], base, r, gc, b, a);
                base += lh;
                start = j + 1;
            }
            return;
        }
    }
    int prev = 0, prevf = -2;
    while (i < n) {
        int cp = qutf8_next(s, n, &i);
        if (cp == '\n') { pen = (float)x; base += lh; prev = 0; continue; }
        float fsc; int fi;
        stbtt_fontinfo* info = qfont_pick(f, cp, &fsc, &fi);
        if (prev && fi == prevf) pen += stbtt_GetCodepointKernAdvance(info, prev, cp) * fsc;
        qfont_blit(w, f, h, cp, pen, base, r, gc, b, a);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(info, cp, &adv, &lsb);
        pen += adv * fsc;
        prev = cp;
        prevf = fi;
    }
}

static void qfont_free_glyphs_of(int64_t h) {
    for (int fi = 0; fi < QFONT_MAX; fi++) {
        QFont* f = &qfonts[fi];
        if (!f->used || !f->glyphs) continue;
        for (int i = 0; i < QGLYPH_SLOTS; i++) {
            QGlyph* g = &f->glyphs[i];
            if (g->win == h) {
                if (g->tex) qsdl.DestroyTexture(g->tex);
                // Keep the slot occupied (a tombstone) so probing still finds
                // the glyphs after it; it can't match a live window again
                // until this handle is reused, and then it re-renders.
                g->tex = NULL;
                g->w = g->h = 0;
                g->win = -2;
            }
        }
    }
}
