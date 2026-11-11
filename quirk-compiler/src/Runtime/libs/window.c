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

// SDL_Event is a 56-byte union; only these offsets are read.
typedef struct { uint8_t bytes[56]; } QSdlEvent;

#define QSDL_INIT_VIDEO           0x00000020u
#define QSDL_WINDOWPOS_CENTERED   0x2FFF0000
#define QSDL_WINDOW_SHOWN         0x00000004u
#define QSDL_WINDOW_HIDDEN        0x00000008u
#define QSDL_WINDOW_RESIZABLE     0x00000020u
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
#define QSDL_TEXTINPUT       0x303
#define QSDL_MOUSEMOTION     0x400
#define QSDL_MOUSEBUTTONDOWN 0x401
#define QSDL_MOUSEBUTTONUP   0x402
#define QSDL_MOUSEWHEEL      0x403

#define QSDL_WE_SIZE_CHANGED  6
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
    char text[32];
} QWinEvent;

enum { QEV_CLOSE = 1, QEV_KEY_DOWN, QEV_KEY_UP, QEV_TEXT, QEV_MOUSE_MOVE, QEV_MOUSE_DOWN,
       QEV_MOUSE_UP, QEV_WHEEL, QEV_RESIZE, QEV_FOCUS, QEV_BLUR, QEV_EXPOSE };

typedef struct {
    int used;
    void* win;
    void* ren;
    void* font;            // glyph atlas texture
    uint32_t id;
    uint8_t r, g, b, a;    // current draw colour
    QWinEvent q[QWIN_QUEUE];
    int qhead, qlen;
} QWin;

static QWin qwins[QWIN_MAX];
static int qwin_headless = 0;

static QWin* qwin_get(int64_t h) {
    if (h < 0 || h >= QWIN_MAX || !qwins[h].used) return NULL;
    return &qwins[h];
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
                else if (we == QSDL_WE_SIZE_CHANGED) { ev.kind = QEV_RESIZE; ev.a = qev_i32(&se, 16); ev.b = qev_i32(&se, 20); }
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
            case QSDL_MOUSEMOTION:
                ev.kind = QEV_MOUSE_MOVE;
                ev.a = qev_i32(&se, 20); ev.b = qev_i32(&se, 24);
                ev.c = (int)qev_u32(&se, 16);      // buttons held (bit 0 = left)
                break;
            case QSDL_MOUSEBUTTONDOWN:
            case QSDL_MOUSEBUTTONUP:
                ev.kind = type == QSDL_MOUSEBUTTONDOWN ? QEV_MOUSE_DOWN : QEV_MOUSE_UP;
                ev.a = qev_i32(&se, 20); ev.b = qev_i32(&se, 24);
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
    void* win = qsdl.CreateWindow(t ? t : "", QSDL_WINDOWPOS_CENTERED, QSDL_WINDOWPOS_CENTERED,
                                  (int)width, (int)height, wf);
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
    return slot;
}

static void qwin_free_images_of(int64_t h);

void Window_close_raw(int64_t h) {
    QWin* w = qwin_get(h);
    if (!w) return;
    qwin_free_images_of(h);
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
    if (w) qsdl.RenderClear(w->ren);
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
void Window_circle_raw(int64_t h, int64_t cx, int64_t cy, int64_t radius, int64_t color, int64_t fill) {
    QWin* w = qwin_draw(h, color);
    if (!w || radius < 0) return;
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

// [width, height] of the drawable area in pixels.
List* Window_size_raw(int64_t h) {
    List* res = (List*)quirk_alloc(QT_LIST, sizeof(List));
    Core_Collections_List_List___init(res);
    QWin* w = qwin_get(h);
    int ww = 0, hh = 0;
    if (w) qsdl.GetRendererOutputSize(w->ren, &ww, &hh);
    Core_Collections_List_List_append(res, quirk_int_to_opaque(ww));
    Core_Collections_List_List_append(res, quirk_int_to_opaque(hh));
    return res;
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
    QSdlRect r = { (int)x, (int)y, 1, 1 };
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
void Window_sleep(int64_t ms) {
    if (ms <= 0) return;
    quirk_gil_release();
#ifndef _WIN32
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
#else
    Sleep((DWORD)ms);
#endif
    quirk_gil_acquire();
}

// Milliseconds since an arbitrary start (monotonic).
int64_t Window_ticks() {
#ifndef _WIN32
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
    return (int64_t)GetTickCount64();
#endif
}
