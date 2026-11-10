# `window` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `window/index.quirk`

### `struct WindowError : RuntimeError`

Thrown when a window can't be opened (no SDL2, no display, ...).

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

#### `define free(self) -> void`

Release the image's memory. Drawing it afterwards does nothing.

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

#### `define text(self, text: String, x: Int, y: Int, color: Int = white(), scale: Int = 1) -> Int`

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

Drawable width in pixels.

#### `define height(self) -> Int`

Drawable height in pixels.

#### `define set_title(self, title: String) -> void`

Change the title bar text.

#### `define pixel(self, x: Int, y: Int) -> Int`

The colour (0xRRGGBB, opaque) of pixel (x, y) in what's been drawn — mostly for tests.

#### `define screenshot(self, path: String) -> void`

Save what's been drawn as a BMP image.
@throws WindowError on failure.

#### `define open(title: String, width: Int, height: Int, resizable: Bool = false, hidden: Bool = false, vsync: Bool = true, headless: Bool = false) -> Window`

Open a window `width` x `height` pixels, centred on the screen.
@param resizable let the user resize it ("resize" events tell you)
@param hidden    don't show it (drawing and `screenshot` still work)
@param vsync     make `present` wait for the display's refresh
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

### `struct Event`

Something that happened to a window. `kind` is one of:
  "close"       — the close button (or Ctrl+C / quit); the window closes
                  unless `keep_open` was set
  "key_down" / "key_up" — `key` is the key's name in lower case: "a",
                  "escape", "return", "space", "left", "f1", "left_shift"...;
                  `repeat` is true for auto-repeat; `shift` / `ctrl` /
                  `alt` / `meta` say which modifiers were held
  "text"        — typed text (`text`), with the keyboard layout and
                  accents applied — use this for text entry, not key_down
  "mouse_move"  — `x`, `y`; `left` true while the left button is held
  "mouse_down" / "mouse_up" — `x`, `y`, `button` (1 left, 2 middle,
                  3 right), `clicks` (2 for a double click)
  "wheel"       — `dx`, `dy` (positive = away from you / right)
  "resize"      — new `width`, `height`
  "focus" / "blur" — the window gained / lost keyboard focus

### `struct Image`

An image loaded for one window (`Window.load_image`); `width` / `height` in pixels.

### `struct Window`

An open window (from `window.open`). Draw into it, then `present()` to
show the frame; drawing starts from what the last frame left unless you
`clear` first.
  `keep_open`  — set true to handle "close" events yourself (e.g. "save
                 changes?") instead of closing at once


### Module-level functions

#### `extern define text_width(text: String, scale: Int = 1) -> Int`

Width in pixels of `text` drawn with `Window.text` at `scale` (its
longest line). For centring and layout.
@example x := (w.width() - window.text_width("Paused", 2)) / 2

#### `extern define ticks() -> Int`

Milliseconds on a steady clock (for timing frames and animation).
