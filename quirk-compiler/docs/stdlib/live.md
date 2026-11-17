# `live` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `live/index.quirk`

### `struct Component`

A live component — subclass it. Keep its state in fields; return its
HTML from `render()` (usually `live.html(template, data)`); react to the
events its template names in `handle(event, data)`:

    <button q-click="save">          a click      (data: q-value / data-* attributes)
    <form q-submit="add">            a submit     (data: the form's fields)
    <input q-input="search">         typing       (data["value"], sent after a pause)
    <select q-change="pick">         a change     (data["value"]; checkboxes "true"/"false")
    <input q-enter="send">           Enter key    (data["value"])
    <a href="/about" q-link>         navigate to another live page without a reload

Override `title()` for the page title. In `handle`, `self.go(path)`
navigates the browser to another page after the update.

#### `define render(self) -> String`

The component's HTML (override).

#### `define handle(self, event: String, data: Map) -> void`

React to `event` from the page (override); `data` holds the event's values (Strings).

#### `define title(self) -> String`

The page title while this component is shown (override; "" leaves it).

#### `define go(self, path: String) -> void`

After this event, send the browser to `path` (a live page, or any URL).

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

#### `define navigate(self, path: String) -> TestSession`

Follow a `q-link` to `path`.


### Module-level functions

#### `define html(template: String, data: Map = {}) -> String`

`template` (web's template syntax: `{{ value }}`, `{% if %}`, `{% for %}`
...) rendered with `data` — escaped, so user input can't inject HTML.
Templates are parsed once and cached.

#### `define session_minutes(minutes: Int) -> void`

How long a page's component is kept after its last event (minutes; default 30).

#### `define session_count() -> Int`

The number of live components the server is holding (open pages).

#### `define configure(head: String = "", layout: String = "") -> void`

Page settings: `head` — HTML added to every page's <head> (stylesheets,
meta tags); `layout` — a whole page template of your own, with
`{{ title }}`, `{{ head | raw }}`, `{{ body | raw }}` and a
`<script src="/_live/live.js" defer></script>`.

#### `define page(app: web.App, path: String, factory: Callable) -> void`

Serve a live page at `path` (":params" allowed, as in web routes):
`factory(req)` makes the page's component — one per visitor, kept while
they use it. Links with `q-link` between live pages switch without a
reload, and Back / Forward work.
