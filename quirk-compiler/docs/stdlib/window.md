# `window` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `window/index.quirk`

### `struct WindowError : RuntimeError`

Thrown when a window can't be opened (no SDL2, no display, ...).

### `struct Event`

Something that happened to a window. `kind` is one of:
  "close"       — the close button (or Ctrl+C / quit); the window closes
                  unless `keep_open` was set
  "key_down" / "key_up" — `key` is the key's name in lower case: "a",
                  "escape", "return", "space", "left", "f1", "left_shift"...;
                  `repeat` is true for auto-repeat; `shift` / `ctrl` /
                  `alt` / `meta` say which modifiers were held, `caps`
                  whether Caps Lock was on
  "text"        — typed text (`text`), with the keyboard layout and
                  accents applied — use this for text entry, not key_down
  "compose"     — an input method (Chinese, Japanese, Korean, dead keys...)
                  is composing `text`: show it at the cursor (underlined)
                  until it arrives as a "text" event; `cursor` is the
                  position inside it and `length` how much of it the
                  input method has selected (characters). An empty
                  `text` ends the composition. See `text_input_area`.
  "access"      — a screen reader asks for `action` ("invoke", "toggle",
                  "set_value", "set_range", "select", "expand",
                  "collapse", "focus") on node `target` of the tree sent
                  with `access_update`; `text` is the new value. `ui`
                  handles these itself.
  "mouse_move"  — `x`, `y`; `left` true while the left button is held
  "mouse_down" / "mouse_up" — `x`, `y`, `button` (1 left, 2 middle,
                  3 right), `clicks` (2 for a double click)
  "wheel"       — `dx`, `dy` (positive = away from you / right)
  "resize"      — new `width`, `height`
  "focus" / "blur" — the window gained / lost keyboard focus

### `struct Font`

A TrueType / OpenType font at one pixel size (`window.load_font`). Pass
it to `Window.text(..., font=f)`. Text is UTF-8 — any character the font
has is drawn. `size` is the requested height; `ascent` / `descent` are
how far glyphs reach above / below the baseline, `line_height` the
distance between lines. One Font works in every window.

#### `define width(self, text: String) -> Int`

Width in pixels of `text` (its longest line) in this font.

#### `define free(self) -> void`

Release the font. Drawing with it afterwards does nothing.

#### `define add_fallback(self, path: String) -> void`

Draw characters this font doesn't have (other scripts, symbols) from
the font file at `path` — tried before the system fallbacks, in the
order added. Text keeps this font's size and line height.
@throws WindowError if the file can't be read or isn't a font.
@example
f.add_fallback("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc")

#### `define covers(self, text: String) -> Bool`

True if every character of `text` can be drawn — by this font or one of its fallbacks.

#### `define caret_x(self, text: String, index: Int) -> Int`

Where the cursor goes before character `index` of `text` (one line),
in pixels from the left of the drawn text. Right-to-left text
(Hebrew, Arabic) is drawn in visual order, so this isn't the width of
the first `index` characters: in "שלום", index 0 is at the right.

#### `define index_at(self, text: String, x: Int) -> Int`

The cursor index (0 to the text's length) nearest to `x` pixels from the left of `text` drawn in this font — where a click goes.

#### `define selection(self, text: String, start: Int, end: Int) -> List`

The pieces of `text` (one line) that characters [start, end) cover,
as [x, width] pairs from the left — one piece for plain text, more
when the range crosses between left-to-right and right-to-left runs.
For drawing a selection.

### `struct Image`

An image loaded for one window (`Window.load_image`); `width` / `height` in pixels.

#### `define free(self) -> void`

Release the image's memory. Drawing it afterwards does nothing.

### `struct Window`

An open window (from `window.open`). Draw into it, then `present()` to
show the frame; drawing starts from what the last frame left unless you
`clear` first.
  `keep_open`  — set true to handle "close" events yourself (e.g. "save
                 changes?") instead of closing at once

#### `define is_open(self) -> Bool`

True until the window is closed (by `close()` or its close button).

#### `define close(self) -> void`

Close the window. Further drawing is ignored.

#### `define events(self) -> List`

Every event since the last call (an empty List if none). Doesn't wait.

#### `define wait_event(self, timeout_ms: Int = -1) -> Event`

Wait for the next event, up to `timeout_ms` (-1: until one comes);
null on timeout or once the window is closed. Other tasks run while
it waits. For programs that only redraw when something happens.

#### `define key_down(self, key: String) -> Bool`

True while `key` (a key name, as in `Event.key`) is held down.

#### `define mouse(self) -> Tuple`

The mouse position (x, y) from the latest event.

#### `define clear(self, color: Int = black()) -> void`

Fill the whole window with `color`.

#### `define rect(self, x: Int, y: Int, width: Int, height: Int, color: Int, fill: Bool = true) -> void`

A rectangle with its top-left corner at (x, y); `fill=false` draws the outline.

#### `define line(self, x1: Int, y1: Int, x2: Int, y2: Int, color: Int) -> void`

A straight line from (x1, y1) to (x2, y2).

#### `define point(self, x: Int, y: Int, color: Int) -> void`

One pixel.

#### `define circle(self, cx: Int, cy: Int, radius: Int, color: Int, fill: Bool = true) -> void`

A circle centred on (cx, cy); `fill=false` draws the outline.

#### `define text(self, text: String, x: Int, y: Int, color: Int = white(), scale: Int = 1, font: Font = null) -> Int`

Draw `text` with its top-left corner at (x, y) in the built-in font
(8x16 pixels per character at `scale=1`; "\n" starts a new line).
Returns the width drawn, in pixels.

#### `define load_image(self, path: String) -> Image`

Load an image file to draw in this window: PNG (with transparency),
JPEG, BMP, GIF (its first frame) or TGA.
@throws WindowError if it doesn't exist or can't be decoded.
@example logo := w.load_image("assets/logo.png")

#### `define image(self, img: Image, x: Int, y: Int, width: Int = -1, height: Int = -1, src_x: Int = 0, src_y: Int = 0, src_w: Int = -1, src_h: Int = -1, alpha: Int = 255) -> void`

Draw `img` with its top-left corner at (x, y).
@param width, height  scale it to this size (-1 = natural size)
@param src_x, src_y, src_w, src_h  draw only this rectangle of the
       image (a sprite-sheet frame); -1 widths run to the edge
@param alpha  opacity, 0 (invisible) to 255 (opaque)
@example w.image(logo, 10, 10)
@example w.image(sheet, x, y, src_x=frame * 32, src_w=32, src_h=32)

#### `define present(self) -> void`

Show everything drawn since the last `present`.

#### `define width(self) -> Int`

Drawable width in (drawing) pixels — see `scale()`.

#### `define height(self) -> Int`

Drawable height in (drawing) pixels — see `scale()`.

#### `define text_input_area(self, x: Int, y: Int, width: Int, height: Int) -> void`

Tell the input method where text is being typed — the cursor's
rectangle — so its candidate list (Chinese, Japanese, Korean input)
opens next to it. Call it while a text field has focus (every frame is
fine; it only reaches the system when the rectangle moves).

#### `define access_active(self) -> Bool`

True while a screen reader is reading this window (Windows UI
Automation; always false elsewhere for now) — then send it the
accessibility tree each frame with `access_update`. `ui` does this.

#### `define access_update(self, ids: List, parents: List, roles: List, names: List, values: List, descs: List, states: List, boxes: List, nums: List) -> void`

Send the accessibility tree, flattened parents-first (node 0 is the
window): per node its `id` (> 0, stable across frames), the index of
its parent (-1 for node 0), role, name, value, description, a state
bitmask (1 focusable, 2 focused, 4 disabled, 8 checked, 16 selected,
32 expanded, 64 collapsed, 128 editable, 256 multiline), its box as
4 Ints (x, y, width, height in drawing pixels) in `boxes`, and 4 Ints
in `nums`: tree level, then range minimum, maximum and value (sliders).

#### `define access_focus(self, id: Int) -> void`

Tell the screen reader that keyboard focus moved to node `id`.

#### `define access_changed(self, id: Int, what: String) -> void`

Node `id` changed: `what` = "value", "checked", "selected", "expanded" or "name".

#### `define access_announce(self, text: String) -> void`

Have the screen reader say `text` (a status message).

#### `define simulate_text(self, text: String, compose: Bool = false, cursor: Int = 0, length: Int = 0) -> void`

For tests: queue a "text" event, as if `text` had been typed — or, with
`compose=true`, a "compose" event (an input method composing `text`,
its cursor at `cursor`, `length` characters selected). The event comes
back from `events()` like a real one.

#### `define scale(self) -> Double`

Physical pixels per drawing pixel: 1.0 on a standard screen, 2.0 on a
Retina / 200% one. Everything you draw is in drawing pixels — sizes
and text come out the same physical size on every screen, and text and
shapes use the full resolution. Use it to pick a sharper image
(`icon@2x.png` when it's 2 or more). Set QUIRK_UI_SCALE (e.g. 1.5) to
scale a program where the system doesn't (X11).

#### `define clip(self, x: Int, y: Int, width: Int, height: Int) -> void`

Only draw inside the rectangle (x, y, width, height) until `unclip()`
— for scrolling areas and panels. `clear` still clears everything.

#### `define unclip(self) -> void`

Draw anywhere again (undoes `clip`).

#### `define set_title(self, title: String) -> void`

Change the title bar text.

#### `define set_hit_areas(self, drag: List, holes: List = [], border: Int = 5) -> Bool`

For a title bar of your own (a window opened `borderless=true`): a press
in one of the `drag` rectangles moves the window (with the system's
snapping, and a double-click maximizing on Windows), except in the
`holes` (your buttons there); presses within `border` pixels of an edge
resize it (when it's resizable and not maximized). Each rectangle is
`[x, y, width, height]`; call it again when they move (after a resize).
False if this SDL can't (before 2.0.4).
@example w.set_hit_areas([[0, 0, w.width(), 32]], [[w.width() - 138, 0, 138, 32]])

#### `define hit_at(self, x: Int, y: Int) -> String`

What a press at (x, y) does with the areas of `set_hit_areas`: "drag",
"resize_top", "resize_bottom_right", ... or "" (the program gets it).

#### `define minimize(self) -> void`

Minimize the window (to the taskbar / dock).

#### `define maximize(self) -> void`

Maximize the window (fill the screen, but for the taskbar).

#### `define restore(self) -> void`

Back to its size before being maximized or minimized.

#### `define maximized(self) -> Bool`

True while maximized.

#### `define minimized(self) -> Bool`

True while minimized.

#### `define inset(self) -> List`

How far the window reaches past the screen's usable area, as [left,
top, right, bottom] pixels: on Windows a maximized window overhangs each
edge by its (hidden) frame's width, so a borderless one keeps what it
draws there out of sight. [0, 0, 0, 0] when not maximized.

#### `define borderless(self) -> Bool`

True if it was opened `borderless=true` (no system title bar).

#### `define set_opacity(self, opacity: Double) -> Bool`

How opaque the whole window is: 1.0 (normal) down to 0.0 (invisible) —
to fade it in or out. False where the system can't (headless, some
Linux desktops).

#### `define pixel(self, x: Int, y: Int) -> Int`

The colour (0xRRGGBB, opaque) of pixel (x, y) in what's been drawn — mostly for tests.

#### `define set_icon(self, path: String) -> Bool`

The window's icon (its title bar's, the taskbar's, Alt+Tab's) from an
image file — a PNG about 256x256 with transparency. False if it couldn't
(no such file, an SDL without icons).


### Module-level functions

#### `extern define text_width(text: String, scale: Int = 1) -> Int`

Width in pixels of `text` drawn with `Window.text` at `scale` (its
longest line). For centring and layout.
@example x := (w.width() - window.text_width("Paused", 2)) / 2

#### `extern define ticks() -> Int`

Milliseconds on a steady clock (for timing frames and animation).

#### `define clipboard() -> String`

The text on the system clipboard ("" if none). Before any window is open,
or with headless windows, a clipboard private to the program.

#### `define set_clipboard(text: String) -> void`

Put `text` on the clipboard (see `clipboard()`).

#### `define rgb(r: Int, g: Int, b: Int) -> Int`

An opaque colour from red, green, blue (0-255 each).

#### `define rgba(r: Int, g: Int, b: Int, a: Int) -> Int`

A colour with transparency: `a` 0 (invisible) to 255 (opaque).

#### `define hex(code: String) -> Int`

A colour from `"#rrggbb"`, `"#rrggbbaa"`, `"#rgb"` (with or without `#`).
@throws ValueError for anything else.
@example window.hex("#ff8800")

#### `define channels(color: Int) -> List`

The colour's red, green, blue and alpha parts (0-255).

#### `define load_font(path: String, size: Int, fallback: Bool = true) -> Font`

Load a font file (`.ttf`, `.otf`, or the first font of a `.ttc`) at
`size` pixels tall. Characters it doesn't have — Chinese, Japanese,
Korean, Arabic, Hebrew, Thai, symbols — come from the system's fonts
(Noto, Microsoft YaHei, PingFang, ...: whichever are installed, read the
first time one is needed); `fallback=false` turns that off. Add your own
with `Font.add_fallback`.
@throws WindowError if the file can't be read or isn't a font.
@example
f := window.load_font("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 18)
w.text("Grüße — 東京 — ✓", 20, 20, window.white(), font=f)

#### `define is_rtl(text: String) -> Bool`

True if `text` (its first line) reads right to left — its first letter
is Hebrew, Arabic, ... — so it should be aligned to the right.

#### `define has_rtl(text: String) -> Bool`

True if `text` has any right-to-left (Hebrew, Arabic...) characters — then carets and selections need `Font.caret_x` / `selection` rather than prefix widths.

#### `define open(title: String, width: Int, height: Int, resizable: Bool = false, hidden: Bool = false, vsync: Bool = true, headless: Bool = false, borderless: Bool = false) -> Window`

Open a window `width` x `height` pixels, centred on the screen.
@param resizable let the user resize it ("resize" events tell you)
@param hidden    don't show it (drawing and `screenshot` still work)
@param vsync     make `present` wait for the display's refresh
@param borderless no system title bar or frame: draw your own and say
                 where it is with `set_hit_areas`
@param headless  no display at all (CI, servers): an off-screen window
                 for drawing and screenshots. Applies from the first
                 window opened.
@throws WindowError when it can't be opened (no SDL2, no display...).
@example w := window.open("Paint", 800, 600, resizable=true)

#### `extern define sleep(ms: Int) -> void`

Pause for `ms` milliseconds (other tasks run meanwhile).

#### `define run(w: Window, frame: Callable, fps: Int = 60) -> void`

Run window `w` until it closes: each frame, call `frame(w, events)` with
that frame's events, then `present`, then wait so frames come about
`fps` times a second. `frame` draws the whole frame (start with
`w.clear(...)`).
@example
define frame(w: window.Window, events: List) -> void {
    w.clear(window.black())
    w.text("Frame at " + window.ticks().str() + "ms", 10, 10)
}
window.run(window.open("Demo", 400, 300), frame)

#### `define caps_lock() -> Bool`

True while Caps Lock is on, as the window system sees it (asked now, not
from the last key). Under WSLg this can differ from the keyboard's light
— letters typed then come out in capitals: show it (an editor's status
bar). False before a window is open.
