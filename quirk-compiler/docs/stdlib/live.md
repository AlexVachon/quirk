# `live` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `live/index.quirk`

### `struct Component`

A component: a struct with its state in fields and its HTML in a template
— returned from `template()` (a raw string, `r"""..."""`), or in the file
named after the struct (`counter_page.html`) next to the `.qk` file:

    <h1>Count: {{ count }}</h1>
    <button @click="add(1)">+1</button>         calls self.add(1)
    <input model="draft" @enter="save">         self.draft follows the field; Enter calls self.save(text)
    <div @after="close" delay="3000">           calls self.close() 3 s after it appears
    <PlusButton :page="self" />                 another component, by its struct's name
    {% if count >= 10 %}<p>a lot</p>{% end %}
    {% for i, item in items %}<li>{{ item }}</li>{% else %}<li>none</li>{% end %}
    <style scoped> .title { color: red } </style>   CSS for this component's elements only

`on_load()` sets it up (once). Events call its methods by name; the page
is rendered again after each one. Props: `text="hi {{ name }}"` sets the
child's `text` field (a String), `:count="n + 1"` sets it to the value;
`@done="refresh"` runs when the child calls `self.emit("done")`. A
`<slot />` in a component's template shows what its tag enclosed.

A page is a Component served by a `live.Router` (usually a `live.Page`).
`title()` sets the page title; `self.go(path)` sends the browser to
another page. (The 1.0 way, overriding `render()` and `handle(event,
data)` with `q-click="name"` attributes, still works.)

#### `define refresh(self) -> void`

Show the page again in the browser now — after a change that didn't come
from the visitor (a timer, another visitor's message, a background job).
Events don't need it: the page is shown again after each one anyway.

#### `define every(self, ms: Int, action: Callable) -> void`

Run `action()` every `ms` milliseconds while the page is open, then show
the page again: `self.every(1000, fn() { self.now = time.now() })`.

#### `define subscribe(self, topic: String) -> void`

Get the messages `live.publish(topic, data)` sends — in `on_message`
(override it). Usually in `on_load`: `self.subscribe("chat")`.

#### `define unsubscribe(self, topic: String) -> void`

Stop getting `topic`'s messages.

#### `define on_message(self, topic: String, data: Any) -> void`

A message from `live.publish(topic, data)` (override); the page is shown again after it.

#### `define on_load(self) -> void`

Set up the component's state (override). Runs once, before it's first shown.

#### `define template(self) -> String`

The component's template (override for one written in the .qk file; "" uses its .html file).

#### `define render(self) -> String`

Its HTML (the template, rendered). Override to write HTML yourself.

#### `define handle(self, event: String, data: Map) -> void`

React to `event` from the page. Override only for the 1.0 `q-click="name"` style.

#### `define title(self) -> String`

The page title while this component is shown (override; "" leaves it).

#### `define go(self, path: String) -> void`

After this event, send the browser to `path` (a live page, or any URL).

#### `define emit(self, event: String, value: Any = null) -> void`

Tell the parent: runs its `@event="method"` handler, with `value` (if given).

#### `define notify(self, message: String, title: String = "", type: String = "", position: String = "",`

Show a notification in a corner of the page, like Element Plus's
ElNotification — from any page or component, nothing to put in the
template. Returns its id (for `close_notification`).

- `type`: "success", "warning", "info", "error" or "" (none)
- `position`: "top-right", "top-left", "bottom-right", "bottom-left"
  ("": the default — top-right, or live.notification_defaults')
- `duration`: milliseconds before it closes (0: stays until closed;
  -1: the default — 4500); the timer pauses while the mouse is on it
- `show_close`: the × button; `icon`: instead of the type's
- `on_click()`: when it's clicked; `on_close()`: when it closes (×,
  timeout or close_notification)

#### `define close_notification(self, id: Int) -> void`

Close notification `id` (its `on_close` runs).

#### `define close_notifications(self) -> void`

Close every notification.

#### `define session(self) -> web.Session`

The visitor's session (needs `app.use(web.sessions())`): `self.session().set("cart", items)`.

#### `define login(self, user: Any) -> void`

Log `user` in (a name, a Map, a struct): a new session id, then `user` stored.

#### `define logout(self) -> void`

Log out: the session ends.

#### `define user(self) -> Any`

Who's logged in, or null (in templates: `{% if user %}…{{ user }}…{% end %}`).

#### `define param(self, name: String) -> String`

A `:param` of the page's path ("/users/:id" → `self.param("id")`); "" if none.

#### `define query(self, name: String) -> String`

A `?name=value` of the page's URL; "" if none.

### `struct Layout : Component`

What some pages are shown in — a sidebar, a header, an admin area — that
stays while you go from one of them to another: only its `<slot />`, the
page, changes. Its fields keep their values (an open menu stays open);
`on_load` runs once, `on_navigate(path)` each time the page changes.

    struct MainLayout : live.Layout {
        menu_open: Bool

        define on_load(self) -> void { self.menu_open = false }
        define toggle(self) -> void { self.menu_open = not self.menu_open }

        define template(self) -> String {
            return r"""
                <aside><button @click="toggle">Menu</button>
                  <a href="/" q-link :class="path == '/' ? 'active' : ''">Home</a></aside>
                <section><slot /></section>
            """
        }
    }

    main := router.group(layout=MainLayout)
    main.add("/", HomePage)
    main.add("/todos", TodoPage)

`self.path` is the current path, `self.page` the page shown.

#### `define on_navigate(self, path: String) -> void`

The page changed (also for the first one): `path` is its path (override).

#### `define template(self) -> String`

The layout's HTML, with `<slot />` where the page goes (override).

#### `define title(self) -> String`

The page's title, or the layout's own (override).

### `struct Widget`

Something on a page. The built-in widgets (Label, Button, TextInput,
Column, ...) render themselves; for one of your own, subclass Widget and
return what it's made of from `build()`:

    struct TodoItem : live.Widget {
        text: String
        define build(self) -> live.Widget {
            return live.Row([live.Label(self.text), live.Button("×")])
        }
    }

`with_class("name")` / `with_style("css")` add a CSS class / inline
style (they return the widget, so they chain).

#### `define build(self) -> Widget`

What this widget is made of (override for a widget of your own).

#### `define with_class(self, name: String) -> Widget`

Add a CSS class.

#### `define with_style(self, css: String) -> Widget`

Add inline CSS ("color: red").

### `struct Label : Widget`

A line of text.

### `struct Heading : Widget`

A title: `level` 1 (largest) to 6.

### `struct Html : Widget`

HTML of your own, as is (not escaped — don't put user input in it
unescaped; `live.html(template, data)` escapes for you).

### `struct Button : Widget`

A button. `on_click()` runs when it's clicked — pass `on_click=fn() {...}`,
or subclass Button and override `on_click(self)`.

#### `define on_click(self) -> void`

Runs on a click (override, or pass `on_click=`).

### `struct Link : Widget`

A link to another page. Between live pages it switches without a reload
(Back / Forward work); `external=true` for a normal link (another site).

### `struct Image : Widget`

A picture; `alt` describes it (for screen readers).

### `struct TextInput : Widget`

A one-line text field. `on_change(text)` runs as the user types (after a
short pause), `on_submit(text)` on Enter. Show the page's state in it
(`text=self.draft`); set that to "" in on_submit to clear the field.

#### `define on_change(self, text: String) -> void`

Runs as the text changes (override, or pass `on_change=`).

#### `define on_submit(self, text: String) -> void`

Runs on Enter (override, or pass `on_submit=`).

### `struct TextArea : Widget`

A text field of several lines; `on_change(text)` as the user types.

#### `define on_change(self, text: String) -> void`

Runs as the text changes (override, or pass `on_change=`).

### `struct Checkbox : Widget`

A box that's ticked or not, with a label; `on_change(checked)` when it's clicked.

#### `define on_change(self, checked: Bool) -> void`

Runs when it's ticked or unticked (override, or pass `on_change=`).

### `struct Dropdown : Widget`

A list to pick one of `options` from; `selected` is the index shown.
`on_change(index, option)` when another is picked.

#### `define on_change(self, index: Int, option: Any) -> void`

Runs when another option is picked (override, or pass `on_change=`).

### `struct Slider : Widget`

A number picked with a handle; `on_change(value)` when it's moved.

#### `define on_change(self, value: Int) -> void`

Runs when the value changes (override, or pass `on_change=`).

### `struct Column : Widget`

Widgets one above the other, `spacing` pixels apart.

### `struct Row : Widget`

Widgets side by side, `spacing` pixels apart (wrapping when the window is narrow).

### `struct Spacer : Widget`

Empty space that pushes the widgets after it in a Row to the end.

### `struct ListView : Widget`

A bulleted list of `items` — text, or widgets. With `on_select`, an item
can be clicked: `on_select(index, item)`.

#### `define on_select(self, index: Int, item: Any) -> void`

Runs when an item is clicked (override, or pass `on_select=`).

### `struct Table : Widget`

A table: `columns` the header texts, `rows` a List of rows (each a List of
cells — text or widgets). With `on_select`, a row can be clicked:
`on_select(index, row)`.

#### `define on_select(self, index: Int, row: List) -> void`

Runs when a row is clicked (override, or pass `on_select=`).

### `struct Page : Component`

A page: a Component the router serves (`router.add("/todos", TodoPage)`).
Write its HTML in `template()`, or build it from widgets by overriding
`build()`:

    define build(self) -> live.Widget {
        return live.Column([
            live.Label("Count: " + String(self.count)),
            live.Button("+1", on_click=fn() { self.count = self.count + 1 }),
        ])
    }

#### `define build(self) -> Widget`

What the page shows, built from widgets (override) — or leave it, and write a template.

### `struct Router : web.Router`

The app's live pages, in one place (a `router.qk`, say):

    from live use { Router }
    from src.counter use { CounterPage }
    from src.todos use { TodoPage }

    router := Router()
    router.add("/", CounterPage)
    router.add("/todos", TodoPage)
    router.add("/users/:id", UserPage)       // self.param("id") in the page

and in the app, `app.mount("/", router)`. It's a `web.Router`, so it can
hold ordinary routes too (`router.get("/api/items", handler)`). Mount it
at "/" (the browser script talks to /_live/).

#### `define add(self, path: String, page: Callable, layout: Callable = null, guard: Callable = null) -> Router`

Serve the page struct `page` (its name: `router.add("/", HomePage)`) at
`path` — in the live.Layout `layout`, if given (a group does that for
several pages).

#### `define group(self, layout: Callable = null, guard: Callable = null) -> RouteGroup`

Pages sharing a live.Layout and / or a guard:
`admin := router.group(layout=AdminLayout, guard=live.require_login())`,
then `admin.add("/admin", AdminPage)`.

### `struct RouteGroup`

Routes in one layout (Router.group).

#### `define add(self, path: String, page: Callable) -> RouteGroup`

Serve `page` at `path`, in the group's layouts, behind its guards.

#### `define group(self, layout: Callable = null, guard: Callable = null) -> RouteGroup`

A group inside this one: its pages are in `layout` *inside* this group's
layout, behind this group's guard and `guard`. Going between its pages
keeps both layouts; going to one of the outer group's keeps the outer one.

### `struct TestSession`

Drive live pages in tests, without a browser:

    s := live.TestSession(app)
    s.open("/")
    s.send("inc")
    test.assert_true(s.html.contains("<p>1</p>"))

`html` is the component's current HTML; `title` its title.

#### `define open(self, path: String) -> TestSession`

Load the page at `path`.

#### `define send(self, event: String, data: Map = {}) -> TestSession`

Send `event` (with `data`) to the page's component, as the browser would.

#### `define click(self, text: String) -> TestSession`

Click the button (or other element) showing `text`.

#### `define type(self, placeholder: String, value: String) -> TestSession`

Type `value` into the text field whose placeholder is `placeholder`.

#### `define submit(self, placeholder: String, value: String) -> TestSession`

Type `value` into the text field with `placeholder` and press Enter.

#### `define check(self, text: String, on: Bool = true) -> TestSession`

Tick (`on` true) or untick the checkbox labelled `text`.

#### `define choose(self, option: String, index: Int = 0) -> TestSession`

Pick an option: in a template's <select>, the option showing `option`
(`index` unused); for a live.Dropdown, the one whose first option is
`option`, at `index`.

#### `define run_timers(self) -> TestSession`

Run every `@after` timer on the page now, once each (as if its delay had passed; `delay="0"`: never).

#### `define submit_form(self, field: String, fields: Map) -> TestSession`

Submit the form holding the field with `placeholder` (or name), with `fields`.

#### `define receive(self) -> Bool`

Take what the server pushed (refresh / every / publish), as the browser would; false if nothing.

#### `define navigate(self, path: String) -> TestSession`

Follow a `q-link` to `path`.


### Module-level functions

#### `extern define has_field(obj: Any, name: String) -> Bool`

`obj` has a field `name`.

#### `extern define get_field(obj: Any, name: String) -> Any`

The value of field `name` (null if there's none).

#### `extern define set_field(obj: Any, name: String, value: Any) -> Bool`

Set field `name` (false if there's none).

#### `extern define has_method(obj: Any, name: String) -> Bool`

`obj` has a public method `name`.

#### `extern define call_method(obj: Any, name: String, args: List) -> Any`

Call method `name` with `args` (missing ones take their defaults); ValueError if there's none.

#### `extern define new_struct(name: String) -> Any`

A new struct of type `name`, made without constructor arguments (null if there's none).

#### `extern define type_name(obj: Any) -> String`

The struct's type name ("" for other values).

#### `extern define source_file(obj: Any) -> String`

The source file declaring the struct's type.

#### `extern define field_type(obj: Any, name: String) -> String`

The declared type of field `name` ("Int", "List", ...; "" if unknown).

#### `extern define field_names(obj: Any) -> List`

The struct's field names.

#### `extern define method_names(obj: Any) -> List`

The struct's public method names.

#### `define notification_defaults(position: String = "", duration: Int = -1, max: Int = -1) -> void`

Defaults for every `notify()` that doesn't say otherwise — set once, at
startup: `position` ("top-right", "top-left", "bottom-right",
"bottom-left"), `duration` (milliseconds; 0: until closed) and `max` (the
most shown at once; over it, the oldest close; 0: no limit). Arguments left
out keep their current value.

    live.notification_defaults(position="bottom-right", duration=3000, max=5)

#### `define html(template: String, data: Map = {}) -> String`

`template` (web's template syntax: `{{ value }}`, `{% if %}`, `{% for %}`
...) rendered with `data` — escaped, so user input can't inject HTML.
Templates are parsed once and cached.

#### `define publish(topic: String, data: Any = null) -> void`

Send `data` to every page subscribed to `topic` (`self.subscribe(topic)`):
each gets `on_message(topic, data)` and is shown again. From anywhere — an
event of another visitor, a route, a timer:

    live.publish("chat", {"from": self.user(), "text": self.draft})

#### `define subscribers(topic: String) -> Int`

How many pages subscribed to `topic` are open now (their tabs connected).

#### `define session_minutes(minutes: Int) -> void`

How long a page's component is kept after its last event (minutes; default 30).

#### `define session_count() -> Int`

The number of live components the server is holding (open pages).

#### `define configure(head: String = "", layout: String = "", layout_file: String = "") -> void`

Page settings: `layout_file` — the base page every page is shown in (an
HTML file with `<slot />` where the page goes, and `{{ title }}`); by
default `index.html` in the app's folder when it has a `<slot />`.
`head` — HTML added to every page's <head>. `layout` — a page template
string (web's template syntax) with `{{ title }}`, `{{ head | raw }}` and
`{{ body | raw }}`.

#### `define page(app: web.App, path: String, factory: Callable) -> void`

Serve a live page at `path` (":params" allowed, as in web routes):
`factory(req)` makes the page's component — one per visitor, kept while
they use it. (A `live.Router` does this for you: `router.add(path, Page)`.)

#### `define require_login(redirect: String = "/login") -> Callable`

A guard: only logged-in visitors (what `self.login(user)` / `web.login`
stored) see the page; others go to `redirect`, with `?next=` the page they
wanted. Needs `app.use(web.sessions())`. A guard of your own is
`fn(req) -> String`: "" to let the visitor in, else where to send them.
