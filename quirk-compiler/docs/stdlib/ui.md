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

#### `define measure(self, t: Theme) -> void`

Work out `min_w` / `min_h` (containers measure their children first).

#### `define place(self, x: Int, y: Int, width: Int, height: Int) -> void`

Take the box (x, y, width, height); containers place their children inside.

#### `define draw(self, win: window.Window, t: Theme) -> void`

Draw into `win` inside the widget's box.

#### `define handle(self, e: window.Event) -> Bool`

React to an event; true if the widget used it.

#### `define next_redraw(self) -> Int`

In how many ms it must be drawn again though nothing happened (a
blinking caret), -1 for never. The App redraws only when something
happened — an event, an animation — or when this says so.

#### `define children(self) -> List`

Child widgets (containers).

#### `define access_role(self) -> String`

What the widget is to assistive technology: "button", "checkbox",
"textbox", "slider", "list", "tab", "menu"... — "group" for a
container, "none" for something purely visual (a Spacer).

#### `define access_name(self) -> String`

Its accessible name — what a screen reader says: `accessible_name` if
set, else what the widget shows (a button's text), else the text of a
Label just before it, else its tooltip.

#### `define access_value(self) -> String`

Its value as text — a field's text, a slider's number — or "".

#### `define access_states(self) -> List`

States: "focusable", "focused", "disabled", "checked", "selected", "expanded"...

#### `define access_node(self) -> AccessNode`

This widget as an `AccessNode` (children included); null when hidden or purely visual.

#### `define access_id(self) -> Int`

A number identifying the widget to assistive technology, the same for its whole life.

#### `define access_action(self, action: String, value: String, node: AccessNode) -> Bool`

Do what a screen reader asks: `action` is "invoke", "toggle",
"set_value" / "set_range" (`value`), "select", "expand" or "collapse",
on this widget or one of its items (`node.index`, `node.data`). True if
it did something.

#### `define contains(self, px: Int, py: Int) -> Bool`

True if (px, py) is inside the widget's box.

### `struct Column : Widget`

Children stacked top to bottom, each as wide as the column. Free height
goes to children with `expand = true`.

#### `define add(self, w: Widget) -> void`

Add a widget at the end.

### `struct Row : Widget`

Children side by side, each as tall as the row. Free width goes to
children with `expand = true`.

#### `define add(self, w: Widget) -> void`

Add a widget at the end.

### `struct Spacer : Widget`

Empty space that takes whatever its container has left over.

### `struct Label : Widget`

Text. "\n" starts a new line. `color` -1 uses the theme's text colour;
`align` is "left", "center" or "right".

### `struct Button : Widget`

A push button. `on_click` (no arguments) runs when it's clicked, or
when it has the focus and Enter / Space is pressed.

#### `define click(self) -> void`

Press the button from code.

### `struct Checkbox : Widget`

A box that's ticked or not, with a label. `on_change(checked)` runs when
the user toggles it.

#### `define toggle(self) -> void`

Flip it, as a click would (runs `on_change`).

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

#### `define lines(self) -> List`

The text split into lines (at "\n"; wrapping doesn't count).

#### `define layout(self) -> List`

The rows on screen as [start, end] text ranges ("\n" not included).

### `struct Slider : Widget`

Pick a number between `min` and `max` by dragging (or with the arrow
keys, by `step`). `on_change(value)` runs when it moves.

#### `define set(self, v: Int) -> void`

Set the value (clamped to min..max); runs `on_change` if it changed.

### `struct RadioGroup : Widget`

One choice out of `options` (anything with `.str()`), shown as a column
of round buttons. Click one, or Up / Down (Left / Right) when it has the
focus. `selected` is the index (-1 = none); `on_change(index, item)` runs
when it changes.

#### `define text(self) -> String`

The chosen option as text ("" when none).

#### `define select(self, i: Int) -> void`

Choose option `i` (clamped); runs `on_change` if it changed.

### `struct ProgressBar : Widget`

How far along something is: `value` out of `max` (clamped), drawn as a
filled bar. `show_text` adds the percentage. Set `value` as work
progresses.

#### `define percent(self) -> Int`

Progress from 0 to 100.

### `struct ListItem`

An entry for a ListView or a Table cell with an `icon` (an image file,
drawn at the text's height) before its `text`; `data` keeps anything you
want with it. Its `.str()` is `text`, so sorting and `on_select` work as
with plain strings.

### `struct ListView : Widget`

A scrollable list of items (anything with `.str()`), one selectable.
`on_select(index, item)` runs when the selection changes; `selected` is
the index (-1 = none). `rows` sets how many rows it asks room for.

#### `define move_item(self, src: Int, dst: Int) -> void`

Move item `src` to position `dst` (both indices into `items`).

#### `define select(self, i: Int) -> void`

Select row `i` (clamped); runs `on_select` if it changed.

### `struct SplitView : Widget`

Two widgets side by side (or stacked: `vertical=true`) with a divider
you drag to share the space. `ratio` is the first pane's share in
percent; `min_size` the smallest a pane gets (pixels). It takes its
container's free space (`expand`) unless told otherwise.

### `struct ToolButton : Widget`

A button for a `ui.Toolbar`: an `icon` (an image file — PNG, JPEG, BMP,
GIF, TGA — drawn 16x16 times the theme scale) and / or `text`, plus a
`tooltip`. `on_click` like a Button. `ui.ToolButton("-")` is a separator.

#### `define click(self) -> void`

Press it from code.

### `struct Toolbar : Row`

A row of `ui.ToolButton`s on a raised strip, usually under the menu bar.

### `struct StatusBar : Widget`

A strip of text sections along the bottom of a window:
`ui.StatusBar(["Ready", "Ln 1, Col 1"])`. The first section takes the
free space; the others are right-aligned. `set(i, text)` changes one.

#### `define set(self, i: Int, text: String) -> void`

Change section `i`'s text (adds sections as needed).

#### `define get(self, i: Int) -> String`

Section `i`'s text ("" if there's none).

### `struct DatePicker : Widget`

A date field that opens a month calendar. `year`, `month` (1-12), `day`;
`text()` is "YYYY-MM-DD". Click the field (or Space / Enter / Alt+Down) to
open it; in the calendar click a day, `<` / `>` change month, arrows move
by day / week, Page Up / Page Down by month, Enter picks, Escape closes.
`on_change(year, month, day)` runs when the date changes.

#### `define text(self) -> String`

The date as "YYYY-MM-DD".

#### `define set_date(self, year: Int, month: Int, day: Int) -> void`

Set the date (clamped to a real date); runs `on_change` if it changed.

#### `define open(self) -> void`

Show the calendar (on the chosen date's month).

#### `define close(self) -> void`

Hide the calendar.

### `struct ColorPicker : Widget`

Pick a colour: a square for saturation (left → right) and brightness
(bottom → top), a strip for the hue, and a swatch with its hex code.
Drag in either, or with the focus: Left / Right saturation, Up / Down
brightness, Page Up / Page Down hue. `color` is the colour (a `window`
colour Int); `on_change(color)` runs when it changes.

#### `define set_color(self, color: Int) -> void`

Set the colour from code (no `on_change`).

#### `define hex(self) -> String`

The colour as "#rrggbb".

### `struct TreeNode`

One node of a `ui.TreeView`: `text`, `children` (TreeNodes), whether it's
`expanded`, and `data` — anything you want to keep with it.
`add(child)` adds a child and returns it.

#### `define add(self, child: TreeNode) -> TreeNode`

Add `child` under this node; returns it.

#### `define depth(self) -> Int`

How deep it is (0 for a root).

### `struct TreeView : Widget`

A tree of `ui.TreeNode`s, nested nodes indented. Click the arrow (or
double-click the row) to expand / collapse; click a row to select it.
Keys: Up / Down move, Right expands (or goes to the first child), Left
collapses (or goes to the parent), Enter toggles. `on_select(node)` runs
when the selection changes; `selected` is the node (or null);
`on_expand(node)` runs just before a node opens (fill in its children
then: a folder read lazily).

`flat=true` is the look of a side bar (a file explorer): no box, rows lit
under the mouse, the selected row tinted (outlined while focused), thin
chevrons, and a click on a parent opens / closes it. `row_height`,
`indent`, `guides` (lines down each level) and `icon` — `fn(node)` giving
`[text, colour]` (a badge before the name: "Q", "{}") or null — fine-tune
either look; `background` fills it (-1: the theme's input colour, or
nothing when flat).

#### `define indent(self) -> Int`

How far each level is indented (pixels).

#### `define visible_nodes(self) -> List`

The nodes on screen in order (expanded parents' children included).

#### `define select(self, node: TreeNode) -> void`

Select `node` (null: none); runs `on_select` if it changed.

#### `define toggle(self, node: TreeNode) -> void`

Expand or collapse `node`.

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

### `struct ScrollView : Widget`

Shows `content` (any widget, usually a Column) in an area `height`
pixels tall that scrolls when the content is taller: mouse wheel, drag
the scroll bar, or Tab to a widget that's out of view. `offset` is how
far it's scrolled (pixels). With `expand` it takes its container's free
height instead.

#### `define scroll_to(self, pixels: Int) -> void`

Scroll so `pixels` from the top of the content is at the top (clamped).

### `struct Dropdown : Widget`

Pick one of `options` (anything with `.str()`) from a list that opens
below the field. Click it, or Space / Enter / Alt+Down when focused;
Up / Down change the choice directly while it's closed. `selected` is
the index; `on_change(index, item)` runs when it changes.

#### `define text(self) -> String`

The chosen option as text ("" when there are none).

#### `define select(self, i: Int) -> void`

Choose option `i` (clamped); runs `on_change` if it changed.

#### `define open(self) -> void`

Show the list.

#### `define close(self) -> void`

Hide the list.

### `struct Tabs : Widget`

Pages under a row of tabs: `ui.Tabs(["General", "Advanced"], [page1, page2])`.
Click a tab, or Left / Right when the tab row has the focus. `selected`
is the page shown; `on_change(index, title)` runs when it changes;
`add(title, page)` adds one, `close(i)` removes one.
`closable=true` puts a × on each tab: clicking it (or middle-clicking the
tab) calls `on_close(index)` — say, to ask about unsaved changes, then
`close(index)` — or, without an `on_close`, closes the tab.

#### `define close(self, i: Int) -> void`

Remove tab `i` (its title and page). The selection stays on the same
page when it can (the one after it when it's the one removed); runs
`on_change` when the page shown changes.

#### `define add(self, title: String, page: Widget) -> void`

Add a page with its tab title.

#### `define select(self, i: Int) -> void`

Show page `i` (clamped); runs `on_change` if it changed.

#### `define page(self) -> Widget`

The page being shown (null when there are none).

### `struct MenuItem`

One entry of a `ui.Menu`: `text`, `on_click` (no arguments), an optional
`shortcut` label shown on the right (just text — bind the keys yourself
with `app.on_key`), and `enabled`. `ui.MenuItem("-")` is a separator.

#### `define is_separator(self) -> Bool`

True for a separator line.

#### `define matches(self, e: window.Event) -> Bool`

True when key event `e` is this item's `shortcut` ("Ctrl+S", "Ctrl+Shift+N", "F5", "Delete"; Ctrl also matches Cmd).

### `struct Menu : Widget`

A list of `ui.MenuItem`s that pops up: under a `ui.MenuBar` title, or
where the user right-clicks a widget whose `context_menu` it is
(`menu.open_at(x, y)` from code). Click an item, or Up / Down and Enter;
Escape closes it.

#### `define close(self) -> void`

Hide it.

### `struct MenuBar : Widget`

A row of menu titles, usually at the top of the window:
`ui.MenuBar(["File", "Edit"], [file_menu, edit_menu])`. Click a title to
open its menu; while one is open, moving over another title (or Left /
Right) switches to it.

#### `define open_menu(self, i: Int) -> void`

Open menu `i` under its title (-1: close).

### `struct FileDialog : Widget`

A window-sized dialog to pick a file (`mode="open"`), a name to save as
(`mode="save"`) or a folder (`mode="folder"`), starting in `dir`.
`filter` (e.g. "*.qk") limits the files shown. `show()` opens it;
`on_pick(path)` runs with the full path chosen, `on_cancel()` (optional)
when it's dismissed. Double-click or Enter opens a folder or picks a
file; Backspace goes up a folder; Tab moves between the list and the
name field; Escape cancels. While it's open it takes all input.

#### `define show(self) -> void`

Open the dialog (reading `dir`).

#### `define go(self, path: String) -> void`

Show folder `path`.

#### `define cancel(self) -> void`

Dismiss the dialog without a choice.

### `struct Canvas : Widget`

A widget you draw yourself: `on_draw(win, x, y, width, height)` runs every
frame (drawing is clipped to the canvas), `on_event(e)` gets its mouse
events (coordinates are the window's) and keys while it has the focus.
`width` / `height` are the size it asks for; with `expand` it takes free
space.

### `struct Animation`

A running animation (`app.animate`). `cancel()` stops it (no more
`on_step`, no `on_done`); `done` says whether it has finished.

#### `define cancel(self) -> void`

Stop it where it is.

### `struct TitleBar : Widget`

The title bar of an `App(title_bar=true)`: the window's title, an optional
widget on its left (`content`: a `MenuBar`, say), and minimize / maximize
(restore) / close buttons on its right. Drawn by the program in its theme
instead of the system's; dragging it moves the window (with the system's
snapping), a double-click maximizes, and the window's edges still resize
it. `app.title_bar` is it.

#### `define buttons_box(self) -> List`

Where the buttons are: [x, y, width, height].

#### `define button_at(self, px: Int, py: Int) -> Int`

The button at (px, py): 0 minimize, 1 maximize / restore, 2 close, -1 none.

#### `define holes(self) -> List`

What a press here doesn't drag: the buttons and `content`.

#### `define press(self, i: Int) -> void`

Press button `i` from code (0 minimize, 1 maximize / restore, 2 close).

### `struct Timer`

A timer from `app.every(ms, fn)` / `app.after(ms, fn)`: `fn()` runs on
the App's loop, between frames. It doesn't draw a frame by itself: call
`app.redraw()` in it when it changed something. `cancel()` stops it.

#### `define cancel(self) -> void`

Don't run it again.

### `struct AccessNode`

One thing in the accessibility tree (`App.accessibility()`) — what a
screen reader is told about a widget or one of its items: `role`
("button", "checkbox", "textbox", "list", "listitem", "tab", "menuitem",
...), `name` (what it's called), `value` (a field's text, a slider's
number, the selected option), `description`, `states` ("focused",
"disabled", "checked", "selected", "expanded"...), its box, `children`,
and the `widget` it came from (null for items). `level` is the depth of
a tree item.

#### `define has(self, state: String) -> Bool`

True if `state` ("focused", "checked"...) is one of its states.

#### `define find(self, name: String, role: String = "") -> AccessNode`

The first node (this one or below) named `name` — and with `role`, when given — or null.

#### `define find_state(self, state: String) -> AccessNode`

The first node (this one or below) with `state` (e.g. "focused"), or null.

#### `define describe(self) -> String`

One line: `button "Save" [focusable, focused]`, with `= value` when it has one.

#### `define dump(self, indent: Int = 0) -> String`

The whole subtree, one `describe()` line per node, indented — for tests and debugging.

### `struct AccessEvent`

A change assistive technology should hear about (`App.on_access`):
`kind` is "focus" (keyboard focus moved to `node`), "value" (the focused
widget's name, value or states changed) or "announce" (`App.announce`:
`node.name` is the message).


### Module-level functions

#### `define mix(a: Int, b: Int, percent: Int) -> Int`

Blend colour `a` toward `b` by `percent` (0 = a, 100 = b). For hover and
pressed shades.

#### `define dark() -> Theme`

Light text on dark grey, blue accent.

#### `define light() -> Theme`

Dark text on white, blue accent.

#### `define tooltip(w: Widget, text: String) -> Widget`

Give `w` a tooltip and return it — for building trees inline:
`ui.tooltip(ui.Button("Save", on_click=save), "Write the file (Ctrl+S)")`.

#### `define Panel(children: List, padding: Int = 12, spacing: Int = 8, expand: Bool = false) -> Column`

A Column drawn on a raised surface with a border (12px padding by default).

#### `define days_in_month(year: Int, month: Int) -> Int`

Days in `month` (1-12) of `year`.

#### `define weekday(year: Int, month: Int, day: Int) -> Int`

Day of the week of a date: 0 = Monday ... 6 = Sunday.

#### `define color_to_hsv(color: Int) -> List`

[hue 0-359, saturation 0-100, value 0-100] of a colour.

#### `define lerp(a: Int, b: Int, t: Double) -> Int`

`a` + (`b` - `a`) * `t` (t from 0.0 to 1.0), rounded.

#### `define ease(name: String, t: Double) -> Double`

Easing: progress `t` (0.0-1.0) reshaped — "linear", "in" (starts slow),
"out" (ends slow), "in_out" (both), "back" (overshoots a little, then
settles).
