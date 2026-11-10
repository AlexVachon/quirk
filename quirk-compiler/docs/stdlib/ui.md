# `ui` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `ui/index.quirk`

### `struct Theme`

Colours and sizes every widget draws with. Make your own by changing a
copy of `ui.dark()` / `ui.light()`.
  `background` — the window; `surface` — panels; `text`, `muted` — text
  `accent` / `accent_text` — buttons, checks, sliders, selection
  `input` — text field / list background; `border`; `focus` — focus ring
  `scale` — text size multiplier (1 = 8x16 pixel characters)

#### `define line(self) -> Int`

Height of one line of text.

#### `define char_w(self) -> Int`

Width of one character.

#### `define dark() -> Theme`

Light text on dark grey, blue accent.

#### `define light() -> Theme`

Dark text on white, blue accent.

#### `define measure(self, t: Theme) -> void`

Work out `min_w` / `min_h` (containers measure their children first).

#### `define place(self, x: Int, y: Int, width: Int, height: Int) -> void`

Take the box (x, y, width, height); containers place their children inside.

#### `define draw(self, win: window.Window, t: Theme) -> void`

Draw into `win` inside the widget's box.

#### `define handle(self, e: window.Event) -> Bool`

React to an event; true if the widget used it.

#### `define children(self) -> List`

Child widgets (containers).

#### `define contains(self, px: Int, py: Int) -> Bool`

True if (px, py) is inside the widget's box.

#### `define add(self, w: Widget) -> void`

Add a widget at the end.

#### `define add(self, w: Widget) -> void`

Add a widget at the end.

#### `define Panel(children: List, padding: Int = 12, spacing: Int = 8, expand: Bool = false) -> Column`

A Column drawn on a raised surface with a border (12px padding by default).

#### `define click(self) -> void`

Press the button from code.

#### `define toggle(self) -> void`

Flip it, as a click would (runs `on_change`).

#### `define insert(self, s: String) -> void`

Insert `s` at the cursor, as typing would.

#### `define set(self, v: Int) -> void`

Set the value (clamped to min..max); runs `on_change` if it changed.

#### `define select(self, i: Int) -> void`

Select row `i` (clamped); runs `on_select` if it changed.

#### `define is_open(self) -> Bool`

True until the window is closed.

#### `define close(self) -> void`

Close the window (ends `run`).

#### `define focus_on(self, w: Widget) -> void`

Give `w` the keyboard focus (null: nobody).

#### `define layout(self) -> void`

Measure and place the tree to fill the window.

#### `define dispatch(self, e: window.Event) -> void`

Route one event to the widget it's for (also usable from tests).

#### `define step(self) -> Bool`

One frame: handle events, lay out, draw, present. False once the window is closed.

#### `define run(self, fps: Int = 60) -> void`

Run until the window closes, about `fps` frames a second.

### `struct Widget`

What every widget has. Layout fills in `x`, `y`, `width`, `height` each
frame; `min_w` / `min_h` are the size the widget asks for.
  `expand`   — take a share of the container's free space
  `visible`  — false: not drawn and takes no space
  `enabled`  — false: drawn dimmed, ignores input
  `hovered`, `focused`, `pressed` — set by the App
To make your own widget, subclass it and override `measure`, `draw` and
`handle` (and `children` for containers).

### `struct Column : Widget`

Children stacked top to bottom, each as wide as the column. Free height
goes to children with `expand = true`.

### `struct Row : Widget`

Children side by side, each as tall as the row. Free width goes to
children with `expand = true`.

### `struct Spacer : Widget`

Empty space that takes whatever its container has left over.

### `struct Label : Widget`

Text. "\n" starts a new line. `color` -1 uses the theme's text colour;
`align` is "left", "center" or "right".

### `struct Button : Widget`

A push button. `on_click` (no arguments) runs when it's clicked, or
when it has the focus and Enter / Space is pressed.

### `struct Checkbox : Widget`

A box that's ticked or not, with a label. `on_change(checked)` runs when
the user toggles it.

### `struct TextInput : Widget`

A one-line text field. Type when it has the focus (click it or Tab to
it); Left / Right / Home / End move the cursor, Backspace / Delete
erase. `on_change(text)` runs on every edit, `on_submit(text)` on Enter.
`columns` sets its width in characters.

### `struct Slider : Widget`

Pick a number between `min` and `max` by dragging (or with the arrow
keys, by `step`). `on_change(value)` runs when it moves.

### `struct ListView : Widget`

A scrollable list of items (anything with `.str()`), one selectable.
`on_select(index, item)` runs when the selection changes; `selected` is
the index (-1 = none). `rows` sets how many rows it asks room for.

### `struct App`

A window showing a widget tree. Set `root`, then `run()`.
  `theme`     — colours (`ui.dark()` by default)
  `on_frame`  — optional `fn()` called once per frame before drawing
  `focus`     — the widget with the keyboard focus (or null)
`step()` runs one frame yourself (custom loops, tests).


### Module-level functions

#### `define mix(a: Int, b: Int, percent: Int) -> Int`

Blend colour `a` toward `b` by `percent` (0 = a, 100 = b). For hover and
pressed shades.
