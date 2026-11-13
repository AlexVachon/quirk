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
  `font`  — a `window.Font` for all text (null: the built-in 8x16 font);
            `theme.font = window.load_font("DejaVuSans.ttf", 15)`

#### `define line(self) -> Int`

Height of one line of text.

#### `define char_w(self) -> Int`

Width of one (average) character — for sizes given in characters.

#### `define text_w(self, s: String) -> Int`

Width in pixels of `s` (its longest line) in the theme's font.

#### `define draw_text(self, win: window.Window, s: String, x: Int, y: Int, color: Int) -> void`

Draw `s` at (x, y) in the theme's font.

#### `define fit(self, s: String, px: Int) -> String`

`s` cut to fit `px` pixels.

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

#### `define tooltip(w: Widget, text: String) -> Widget`

Give `w` a tooltip and return it — for building trees inline:
`ui.tooltip(ui.Button("Save", on_click=save), "Write the file (Ctrl+S)")`.

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

#### `define undo(self) -> Bool`

Undo the last edit (Ctrl+Z); false when there's nothing to undo.

#### `define redo(self) -> Bool`

Redo what `undo` took back (Ctrl+Y / Ctrl+Shift+Z).

#### `define has_selection(self) -> Bool`

True when some text is selected.

#### `define selection_start(self) -> Int`

Where the selection starts (== `selection_end()` when nothing is selected).

#### `define selection_end(self) -> Int`

Where the selection ends.

#### `define selected_text(self) -> String`

The selected text ("" if none).

#### `define select(self, start: Int, end: Int) -> void`

Select from `start` to `end` (the cursor goes to `end`).

#### `define select_all(self) -> void`

Select everything.

#### `define insert(self, s: String) -> void`

Insert `s` at the cursor, as typing would (replaces the selection).

#### `define copy(self) -> void`

Copy the selection to the clipboard.

#### `define cut(self) -> void`

Copy the selection to the clipboard and remove it.

#### `define paste(self) -> void`

Insert what's on the clipboard.

#### `define index_at(self, px: Int, py: Int) -> Int`

The text index under the point (px, py) (where a click puts the cursor).

#### `define lines(self) -> List`

The text split into lines (at "\n"; wrapping doesn't count).

#### `define layout(self) -> List`

The rows on screen as [start, end] text ranges ("\n" not included).

#### `define set(self, v: Int) -> void`

Set the value (clamped to min..max); runs `on_change` if it changed.

#### `define text(self) -> String`

The chosen option as text ("" when none).

#### `define select(self, i: Int) -> void`

Choose option `i` (clamped); runs `on_change` if it changed.

#### `define percent(self) -> Int`

Progress from 0 to 100.

#### `define move_item(self, src: Int, dst: Int) -> void`

Move item `src` to position `dst` (both indices into `items`).

#### `define select(self, i: Int) -> void`

Select row `i` (clamped); runs `on_select` if it changed.

#### `define click(self) -> void`

Press it from code.

#### `define set(self, i: Int, text: String) -> void`

Change section `i`'s text (adds sections as needed).

#### `define get(self, i: Int) -> String`

Section `i`'s text ("" if there's none).

#### `define days_in_month(year: Int, month: Int) -> Int`

Days in `month` (1-12) of `year`.

#### `define weekday(year: Int, month: Int, day: Int) -> Int`

Day of the week of a date: 0 = Monday ... 6 = Sunday.

#### `define text(self) -> String`

The date as "YYYY-MM-DD".

#### `define set_date(self, year: Int, month: Int, day: Int) -> void`

Set the date (clamped to a real date); runs `on_change` if it changed.

#### `define open(self) -> void`

Show the calendar (on the chosen date's month).

#### `define close(self) -> void`

Hide the calendar.

#### `define color_to_hsv(color: Int) -> List`

[hue 0-359, saturation 0-100, value 0-100] of a colour.

#### `define set_color(self, color: Int) -> void`

Set the colour from code (no `on_change`).

#### `define hex(self) -> String`

The colour as "#rrggbb".

#### `define add(self, child: TreeNode) -> TreeNode`

Add `child` under this node; returns it.

#### `define depth(self) -> Int`

How deep it is (0 for a root).

#### `define visible_nodes(self) -> List`

The nodes on screen in order (expanded parents' children included).

#### `define select(self, node: TreeNode) -> void`

Select `node` (null: none); runs `on_select` if it changed.

#### `define toggle(self, node: TreeNode) -> void`

Expand or collapse `node`.

#### `define editing(self) -> Bool`

True while a cell is being edited.

#### `define edit_cell(self, r: Int, c: Int) -> void`

Edit cell (row index `r`, column `c`) in place.

#### `define finish_edit(self, save: Bool = true) -> void`

Stop editing; `save` keeps the typed value.

#### `define sort_by(self, c: Int, descending: Bool = false) -> void`

Sort by column `c` (`descending` for largest / last first).

#### `define select(self, i: Int) -> void`

Select row `i` (an index into `rows`; -1: none); runs `on_select` if it changed.

#### `define scroll_to(self, pixels: Int) -> void`

Scroll so `pixels` from the top of the content is at the top (clamped).

#### `define text(self) -> String`

The chosen option as text ("" when there are none).

#### `define select(self, i: Int) -> void`

Choose option `i` (clamped); runs `on_change` if it changed.

#### `define open(self) -> void`

Show the list.

#### `define close(self) -> void`

Hide the list.

#### `define add(self, title: String, page: Widget) -> void`

Add a page with its tab title.

#### `define select(self, i: Int) -> void`

Show page `i` (clamped); runs `on_change` if it changed.

#### `define page(self) -> Widget`

The page being shown (null when there are none).

#### `define is_separator(self) -> Bool`

True for a separator line.

#### `define matches(self, e: window.Event) -> Bool`

True when key event `e` is this item's `shortcut` ("Ctrl+S", "Ctrl+Shift+N", "F5", "Delete"; Ctrl also matches Cmd).

#### `define close(self) -> void`

Hide it.

#### `define open_menu(self, i: Int) -> void`

Open menu `i` under its title (-1: close).

#### `define show(self) -> void`

Open the dialog (reading `dir`).

#### `define go(self, path: String) -> void`

Show folder `path`.

#### `define cancel(self) -> void`

Dismiss the dialog without a choice.

#### `define lerp(a: Int, b: Int, t: Double) -> Int`

`a` + (`b` - `a`) * `t` (t from 0.0 to 1.0), rounded.

#### `define ease(name: String, t: Double) -> Double`

Easing: progress `t` (0.0-1.0) reshaped — "linear", "in" (starts slow),
"out" (ends slow), "in_out" (both), "back" (overshoots a little, then
settles).

#### `define cancel(self) -> void`

Stop it where it is.

#### `define animate(self, duration: Int, on_step: Callable, ease: String = "in_out", on_done: Callable = null) -> Animation`

Animate over `duration` ms: `on_step(t)` runs every frame with the
eased progress `t` (0.0 → 1.0; the last call is exactly 1.0), then
`on_done()`. `ease` is "linear", "in", "out", "in_out" or "back".
@example
app.animate(300, fn(t) { panel.x_offset = ui.lerp(-200, 0, t) }, ease="out")

#### `define animating(self) -> Bool`

True while an animation is running.

#### `define dragging(self) -> Bool`

True while something is being dragged (see `Widget.on_drag`).

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

#### `define draw(self) -> void`

Draw the tree, open popups and the tooltip (what `step` shows).

#### `define run(self, fps: Int = 60) -> void`

Run until the window closes, about `fps` frames a second.

### `struct Widget`

What every widget has. Layout fills in `x`, `y`, `width`, `height` each
frame; `min_w` / `min_h` are the size the widget asks for.
  `expand`   — take a share of the container's free space
  `visible`  — false: not drawn and takes no space
  `enabled`  — false: drawn dimmed, ignores input
  `hovered`, `focused`, `pressed` — set by the App
  `tooltip`  — text shown when the mouse rests on it ("" = none)
  `context_menu` — a `ui.Menu` opened by a right-click on it (or null)
  `on_drag`  — `fn() => data`: the widget can be dragged, carrying `data`
  `on_drop`  — `fn(data)`: something dragged can be dropped on it
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

### `struct TextEdit : Widget`

What TextInput and TextArea share: the text, a cursor and a selection,
and the editing keys.
  `text`             — the contents; set it to replace them
  `cursor`           — position (0 = before the first character)
  `has_selection()`, `selection_start()`, `selection_end()`,
  `selected_text()`, `select(start, end)`, `select_all()`
  `insert(s)`        — type `s` at the cursor (replacing the selection)
  `copy()`, `cut()`, `paste()` — through the system clipboard
  `undo()`, `redo()` — Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z
Keys: Shift + arrows / Home / End select, Ctrl+A selects everything,
Ctrl+C / X / V copy, cut and paste (Cmd on macOS), Ctrl + Left / Right
move by word, Ctrl + Backspace / Delete erase a word. Mouse: click to
place the cursor, drag or Shift+click to select, double-click selects a
word, triple-click a line.

### `struct TextInput : TextEdit`

A one-line text field. Type when it has the focus (click it or Tab to
it); editing, selection and copy / paste as described for `TextEdit`.
`on_change(text)` runs on every edit, `on_submit(text)` on Enter.
`columns` sets its width in characters.

### `struct TextArea : TextEdit`

Multi-line text. Enter starts a new line; Up / Down / Page Up / Page Down
move between lines, Home / End to the start / end of the line (with Ctrl:
of the text). Editing, selection and copy / paste as for `TextEdit`.
`rows` / `columns` set the size it asks for; it scrolls (wheel, or by
following the cursor) when the text is bigger. With `wrap=true` long
lines break at spaces to fit the width (and the keys move along the
wrapped lines) instead of scrolling sideways. `on_change(text)` runs on
every edit.

### `struct Slider : Widget`

Pick a number between `min` and `max` by dragging (or with the arrow
keys, by `step`). `on_change(value)` runs when it moves.

### `struct RadioGroup : Widget`

One choice out of `options` (anything with `.str()`), shown as a column
of round buttons. Click one, or Up / Down (Left / Right) when it has the
focus. `selected` is the index (-1 = none); `on_change(index, item)` runs
when it changes.

### `struct ProgressBar : Widget`

How far along something is: `value` out of `max` (clamped), drawn as a
filled bar. `show_text` adds the percentage. Set `value` as work
progresses.

### `struct ListItem`

An entry for a ListView or a Table cell with an `icon` (an image file,
drawn at the text's height) before its `text`; `data` keeps anything you
want with it. Its `.str()` is `text`, so sorting and `on_select` work as
with plain strings.

### `struct ListView : Widget`

A scrollable list of items (anything with `.str()`), one selectable.
`on_select(index, item)` runs when the selection changes; `selected` is
the index (-1 = none). `rows` sets how many rows it asks room for.

### `struct SplitView : Widget`

Two widgets side by side (or stacked: `vertical=true`) with a divider
you drag to share the space. `ratio` is the first pane's share in
percent; `min_size` the smallest a pane gets (pixels). It takes its
container's free space (`expand`) unless told otherwise.

### `struct ToolButton : Widget`

A button for a `ui.Toolbar`: an `icon` (an image file — PNG, JPEG, BMP,
GIF, TGA — drawn 16x16 times the theme scale) and / or `text`, plus a
`tooltip`. `on_click` like a Button. `ui.ToolButton("-")` is a separator.

### `struct Toolbar : Row`

A row of `ui.ToolButton`s on a raised strip, usually under the menu bar.

### `struct StatusBar : Widget`

A strip of text sections along the bottom of a window:
`ui.StatusBar(["Ready", "Ln 1, Col 1"])`. The first section takes the
free space; the others are right-aligned. `set(i, text)` changes one.

### `struct DatePicker : Widget`

A date field that opens a month calendar. `year`, `month` (1-12), `day`;
`text()` is "YYYY-MM-DD". Click the field (or Space / Enter / Alt+Down) to
open it; in the calendar click a day, `<` / `>` change month, arrows move
by day / week, Page Up / Page Down by month, Enter picks, Escape closes.
`on_change(year, month, day)` runs when the date changes.

### `struct ColorPicker : Widget`

Pick a colour: a square for saturation (left → right) and brightness
(bottom → top), a strip for the hue, and a swatch with its hex code.
Drag in either, or with the focus: Left / Right saturation, Up / Down
brightness, Page Up / Page Down hue. `color` is the colour (a `window`
colour Int); `on_change(color)` runs when it changes.

### `struct TreeNode`

One node of a `ui.TreeView`: `text`, `children` (TreeNodes), whether it's
`expanded`, and `data` — anything you want to keep with it.
`add(child)` adds a child and returns it.

### `struct TreeView : Widget`

A tree of `ui.TreeNode`s, nested nodes indented. Click the arrow (or
double-click the row) to expand / collapse; click a row to select it.
Keys: Up / Down move, Right expands (or goes to the first child), Left
collapses (or goes to the parent), Enter toggles. `on_select(node)` runs
when the selection changes; `selected` is the node (or null).

### `struct Table : Widget`

Rows of values under column headers: `ui.Table(["Name", "Size"], rows)`,
each row a List (anything with `.str()`). Columns size to their content
(or set `widths`); click a row (or Up / Down) to select it, click a
header to sort by that column (again: reverse). `on_select(index, row)`
runs when the selection changes — `index` into `rows`, whatever the sort.
With `editable=true` a cell can be changed in place: Left / Right pick
the column, Enter / F2 or a double-click edit it, Enter saves, Escape
cancels. The new value goes into `rows` (an Int column stays an Int when
the text is a number) and `on_edit(index, column, value)` runs.

### `struct ScrollView : Widget`

Shows `content` (any widget, usually a Column) in an area `height`
pixels tall that scrolls when the content is taller: mouse wheel, drag
the scroll bar, or Tab to a widget that's out of view. `offset` is how
far it's scrolled (pixels). With `expand` it takes its container's free
height instead.

### `struct Dropdown : Widget`

Pick one of `options` (anything with `.str()`) from a list that opens
below the field. Click it, or Space / Enter / Alt+Down when focused;
Up / Down change the choice directly while it's closed. `selected` is
the index; `on_change(index, item)` runs when it changes.

### `struct Tabs : Widget`

Pages under a row of tabs: `ui.Tabs(["General", "Advanced"], [page1, page2])`.
Click a tab, or Left / Right when the tab row has the focus. `selected`
is the page shown; `on_change(index, title)` runs when it changes;
`add(title, page)` adds one.

### `struct MenuItem`

One entry of a `ui.Menu`: `text`, `on_click` (no arguments), an optional
`shortcut` label shown on the right (just text — bind the keys yourself
with `app.on_key`), and `enabled`. `ui.MenuItem("-")` is a separator.

### `struct Menu : Widget`

A list of `ui.MenuItem`s that pops up: under a `ui.MenuBar` title, or
where the user right-clicks a widget whose `context_menu` it is
(`menu.open_at(x, y)` from code). Click an item, or Up / Down and Enter;
Escape closes it.

### `struct MenuBar : Widget`

A row of menu titles, usually at the top of the window:
`ui.MenuBar(["File", "Edit"], [file_menu, edit_menu])`. Click a title to
open its menu; while one is open, moving over another title (or Left /
Right) switches to it.

### `struct FileDialog : Widget`

A window-sized dialog to pick a file (`mode="open"`), a name to save as
(`mode="save"`) or a folder (`mode="folder"`), starting in `dir`.
`filter` (e.g. "*.qk") limits the files shown. `show()` opens it;
`on_pick(path)` runs with the full path chosen, `on_cancel()` (optional)
when it's dismissed. Double-click or Enter opens a folder or picks a
file; Backspace goes up a folder; Tab moves between the list and the
name field; Escape cancels. While it's open it takes all input.

### `struct Canvas : Widget`

A widget you draw yourself: `on_draw(win, x, y, width, height)` runs every
frame (drawing is clipped to the canvas), `on_event(e)` gets its mouse
events (coordinates are the window's) and keys while it has the focus.
`width` / `height` are the size it asks for; with `expand` it takes free
space.

### `struct Animation`

A running animation (`app.animate`). `cancel()` stops it (no more
`on_step`, no `on_done`); `done` says whether it has finished.

### `struct App`

A window showing a widget tree. Set `root`, then `run()`.
  `theme`     — colours (`ui.dark()` by default)
  `on_frame`  — optional `fn()` called once per frame before drawing
  `focus`     — the widget with the keyboard focus (or null)
  `tooltip_delay` — ms the mouse rests on a widget before its tooltip shows
  `on_key`    — optional `fn(e)` called for every key press, before the
                focused widget sees it (shortcuts: `if e.ctrl and e.key == "s"`)
`step()` runs one frame yourself (custom loops, tests).


### Module-level functions

#### `define mix(a: Int, b: Int, percent: Int) -> Int`

Blend colour `a` toward `b` by `percent` (0 = a, 100 = b). For hover and
pressed shades.
