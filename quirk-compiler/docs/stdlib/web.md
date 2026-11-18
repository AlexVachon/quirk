# `web` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `web/index.quirk`

### `struct HttpError : Exception`

Throw from a handler to answer with `status` and `message` — e.g.
`throw web.HttpError(404, "no such user")`. The app's error handler gets
it (the default one sends the status and message).

### `struct Request`

An incoming request, as a handler sees it:
  `method`  — "GET", "POST", ... (upper case)
  `path`    — "/users/42" (decoded, without the query)
  `url`     — the request target as sent ("/users/42?tab=1")
  `query`   — Map of the query string ("?tab=1" → {"tab": "1"})
  `params`  — Map of the route's `:name` / `*name` parts
  `headers` — Map of headers, names in lower case
  `body`    — the raw body (bytes kept as they came)
  `locals`  — a Map for middleware to pass things to handlers
`json()`, `form()`, `cookies()`, `header(name)` read the rest.

#### `define session(self) -> Session`

The visitor's session (needs `app.use(web.sessions())`): values kept on
the server between their requests — `req.session().set("cart", items)`.

#### `define user(self) -> Any`

Who's logged in (what `web.login(req, user)` stored), or null.

#### `define header(self, name: String) -> String`

A header's value ("" if absent); the name is case-insensitive.

#### `define is_type(self, kind: String) -> Bool`

True if the request's Content-Type is `kind` ("json", "form", or a full type like "text/plain").

#### `define json(self) -> Any`

The body parsed as JSON (a Map, List, String, number...).
@throws HttpError 400 if it isn't valid JSON.

#### `define form(self) -> Map`

The body parsed as a form (`a=1&b=two`, as an HTML form posts it): a Map of Strings.

#### `define cookies(self) -> Map`

The request's cookies: a Map of name → value.

#### `define cookie(self, name: String) -> String`

One cookie's value, or "" when the request doesn't have it.

#### `define accepts(self, kind: String) -> Bool`

True if the client accepts `kind` ("json", "html", or a full type) — from the Accept header.

### `struct Response`

The answer a handler builds: `res.status(201).json(value)`,
`res.html(page)`, `res.redirect("/login")`, `res.file("report.pdf")`,
`res.set("X-Name", "v")`, `res.cookie("session", id)`. The first send
wins; it's written when the handler (and middleware) return.

#### `define event_stream(self) -> EventStream`

Keep the connection open and send events as they happen (Server-Sent
Events — the browser's `EventSource`). The handler keeps running:

    app.get("/clock", fn(req, res) {
        stream := res.event_stream()
        while stream.open {
            stream.send("time", String(sys.now_ms()))
            sys.sleep(1000)
        }
    })

`stream.open` turns false once the visitor is gone (a send failed).

#### `define status(self, code: Int) -> Response`

Set the status code (chainable: `res.status(404).text("nope")`).

#### `define set(self, name: String, value: String) -> Response`

Set a header, replacing one of the same name (chainable).

#### `define append_header(self, name: String, value: String) -> Response`

Add a header, keeping others of the same name (Set-Cookie).

#### `define unset(self, name: String) -> void`

Remove a header.

#### `define get(self, name: String) -> String`

A header's value as set so far ("" if none).

#### `define content_type(self, kind: String) -> Response`

Set the Content-Type (chainable): "json", "html", "text", or a full type.

#### `define send(self, body: String) -> Response`

Send `body` as the response — text/html unless a Content-Type is
set. Later sends are ignored (the first one wins).

#### `define text(self, body: String) -> Response`

Send plain text.

#### `define html(self, page: Any) -> Response`

Send HTML: a String, or an `html` Element (rendered).

#### `define json(self, value: Any) -> Response`

Send `value` (Map, List, String, number...) as JSON.

#### `define send_status(self, code: Int) -> Response`

Answer with just a status code and its text ("Not Found").

#### `define redirect(self, location: String, code: Int = 302) -> Response`

Redirect to `location` (302 Found, or `code`: 301, 303, 307, 308).

#### `define cookie(self, name: String, value: String, max_age: Int = -1, path: String = "/", http_only: Bool = true, secure: Bool = false, same_site: String = "Lax", domain: String = "") -> Response`

Set a cookie. `max_age` in seconds (-1: until the browser closes);
`http_only` hides it from JavaScript; `same_site` is "Lax", "Strict"
or "None" (then `secure` too).

#### `define clear_cookie(self, name: String, path: String = "/") -> Response`

Delete a cookie in the browser.

#### `define file(self, path: String, download: Bool = false, name: String = "") -> Response`

Send a file, with its Content-Type and Last-Modified — or 304 Not
Modified when the browser's copy is current, 404 when there's no such
file. `download=true` makes the browser save it (as `name`, default
the file's own name).

#### `define render(self, name: String, data: Map = {}) -> Response`

Render the template `name` (a file in the app's views folder, see
`App.views`) with `data`, and send it as HTML.

#### `define end(self) -> Response`

End the response with no body (204 if nothing else was set).

### `struct EventStream`

An open Server-Sent Events stream (Response.event_stream()). In a
TestClient, what's sent is kept in `sent`: a List of [event, data].

#### `define send(self, event: String, data: String, id: String = "") -> Bool`

Send `data` as event `event` ("message" is EventSource's default); false once the visitor is gone.

#### `define ping(self) -> Bool`

A comment line, to keep the connection (and proxies) awake — and to notice the visitor left.

#### `define close(self) -> void`

Stop: the connection closes when the handler returns.

### `struct Router`

A group of routes and middleware to mount in an app under a prefix:

    api := web.Router()
    api.get("/users", list_users)
    app.mount("/api", api)          // GET /api/users

#### `define route(self, method: String, pattern: String, handler: Callable) -> Router`

Handle `method` requests to `pattern` ("/users/:id", "/files/*path").

#### `define all(self, pattern: String, handler: Callable) -> Router { return self.route("", pattern, handler) }`

Any method.

#### `define use(self, mw: Callable, path: String = "/") -> Router`

Run middleware `mw(req, res, next)` before the routes — for every
request, or only under `path` ("/admin"). Call `next()` to go on; don't
to answer here (e.g. a login check).

### `struct Session`

A visitor's session: values kept on the server between their requests.
The browser only holds a random id, signed, in an HttpOnly cookie. Get it
with `req.session()` (after `app.use(web.sessions())`).

#### `define get(self, key: String, default: Any = null) -> Any`

The value of `key`, or `default`.

#### `define clear(self) -> void`

Forget every value (the session itself stays).

#### `define regenerate(self) -> void`

A new id for the same values — call it when someone logs in (web.login
does), so an id someone else saw before can't be used to act as them.

#### `define destroy(self) -> void`

End the session: its values go, and the browser's cookie is cleared.

### `struct App`

A web app: routes, middleware, error handling, and a server.

    app := web.App()
    app.get("/", fn(req, res) { res.text("hi") })
    app.listen(3000)

Routes run in the order added; the first whose method and path match
answers. `views` is the templates folder for `res.render`.

#### `define route(self, method: String, pattern: String, handler: Callable) -> App`

Handle `method` requests to `pattern` ("/users/:id", "/files/*path"); `handler(req, res)`.

#### `define all(self, pattern: String, handler: Callable) -> App { return self.route("", pattern, handler) }`

Any method.

#### `define use(self, mw: Callable, path: String = "/") -> App`

Run middleware `mw(req, res, next)` before the routes — for every
request, or only under `path`. Call `next()` to continue.

#### `define mount(self, prefix: String, router: Router) -> App`

Add a Router's routes and middleware under `prefix` ("/api").

#### `define on_error(self, handler: Callable) -> App`

Handle errors thrown by handlers: `handler(err, req, res)`. By default
an HttpError answers with its status and message, anything else with
500 (and the error printed).

#### `define on_not_found(self, handler: Callable) -> App`

Answer requests no route matched: `handler(req, res)` (default: 404 Not Found).

#### `define handle(self, req: Request, res: Response) -> void`

Answer `req` into `res` — middleware, the matching route, error and
not-found handling — without a network. `listen` uses it for every
request; tests can call it directly (see `TestClient`).

#### `define listen(self, port: Int = 3000, host: String = "127.0.0.1", on_ready: Callable = null, quiet: Bool = false) -> void`

Serve the app on `port` (and `host`: "127.0.0.1" only this machine,
"0.0.0.0" every interface) until `stop()`. Each connection is handled
in its own task, with keep-alive. `on_ready()` runs once listening.

#### `define stop(self) -> void`

Stop `listen` (it returns after the current accept).

### `struct Template`

A page template — HTML with places for data:

    {{ user.name }}           a value, HTML-escaped (dotted: Map keys, List indexes)
    {{ bio | raw }}           not escaped; also `| upper`, `| lower`, `| length`, `| json`
    {% if user.admin %} … {% elif guest %} … {% else %} … {% end %}
    {% if not items %} … {% end %}       also `==` / `!=` with "text" or numbers
    {% for item in items %} {{ loop.index }}. {{ item }} {% else %} none {% end %}
    {% include "header.html" %}          another file (from the same folder)
    {# a comment #}

`Template(text).render(data)` gives the page; `render_file(path, data)`
reads (and caches) a file.

#### `define render(self, data: Map = {}) -> String`

The page for `data` (a Map of names to values).

### `struct TestClient`

Call an app's routes without a network, for tests:

    client := web.TestClient(app)
    res := client.get("/users/1")
    test.assert_eq(res.status_code, 200)
    test.assert_eq(res.body, "...")

Cookies the app sets are sent back on later requests, like a browser.

#### `define post_json(self, path: String, value: Any) -> Response`

POST `value` as JSON.

#### `define post_form(self, path: String, fields: Map) -> Response`

POST a form (a Map of Strings, urlencoded).


### Module-level functions

#### `define mime_type(path: String) -> String`

The Content-Type for a file name's extension ("style.css" → "text/css;
charset=utf-8"); "application/octet-stream" when unknown.

#### `define sessions(secret: String = "", minutes: Int = 10080, cookie: String = "quirk_session", secure: Bool = false) -> Callable`

Middleware for sessions — `app.use(web.sessions())`, then `req.session()`
in handlers. Sessions live in the server's memory (they end when it
restarts) and expire `minutes` after the visitor's last request (default:
a week). `secret` signs the cookie; by default a random one is made when
the server starts. `secure=true` sends the cookie over HTTPS only.

#### `define session_count() -> Int`

How many sessions the server holds.

#### `define login(req: Request, user: Any) -> void`

Log `user` in (any value: a name, a Map, a struct): a new session id, then `user` stored.

#### `define logout(req: Request) -> void`

Log out: the session ends (its values go, the cookie is cleared).

#### `define require_login(redirect: String = "/login") -> Callable`

Middleware that lets only logged-in visitors through — `app.use(web.require_login(), path="/admin")`.
A page request is sent to `redirect` (with `?next=` the page it wanted);
anything else (an API call, a form post) gets 401.

#### `define static(dir: String, prefix: String = "/") -> Callable`

Middleware that serves files under the folder `dir` — GET / HEAD of
"/style.css" sends "dir/style.css", "/" and folders their index.html.
Requests for other paths (or methods) go on to the routes. `prefix`
serves them under a URL path instead ("/assets").

#### `define logger() -> Callable`

Middleware that prints one line per request: method, path, status and
time taken ("GET /users 200 3ms").

#### `define cors(origin: String = "*", methods: String = "GET, POST, PUT, PATCH, DELETE, OPTIONS", headers: String = "Content-Type, Authorization") -> Callable`

Middleware that lets pages on other sites call this API from browsers
(CORS): `origin` "*" (any) or one site ("https://example.com").
Answers preflight OPTIONS requests itself.

#### `define render_file(path: String, data: Map = {}) -> String`

Render the template file at `path` with `data` (see `Template`).

#### `define escape(s: String) -> String`

`s` with HTML's special characters escaped (& < > " ').
