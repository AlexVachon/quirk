# `net` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `net/http.quirk`

### `struct Response`

HTTP response.
  `status_code`  — numeric status (200, 404, ...).
  `headers`      — Map of header name → value (header names are
                   case-preserved as the server returned them).
  `text`         — decoded response body (chunked encoding handled).
  `ok`           — true iff `200 <= status_code < 300`.

#### `define __init(self, status: Int, body: String, headers: Map) -> void`

Construct a Response. Sets `ok` based on the status code range.

#### `define __str(self) -> String`

Compact form: `<Response [200]>`.

#### `define request(method: String, target: String, data: String = "", headers: Map = Map(), params: Map = Map(), follow_redirects: Bool = true) -> Response`

Send a single HTTP request and return the response. Lower-level form
behind `get`/`post`/`delete`/`post_json`.

@param method HTTP verb (GET, POST, DELETE, PUT, ...) — case-insensitive.
@param target Full URL starting with `http://`. HTTPS is not supported.
@param data Request body — pass "" for GET/DELETE.
@param headers Extra headers to send (case-preserved). Caller-supplied
       Host / User-Agent / Connection / Content-Length override the defaults.
@param params Query-string parameters; merged into the URL's existing
       query and percent-encoded via `url.build_query`.
@param follow_redirects When true (the default), 3xx responses are
       followed automatically up to 5 hops. Set to false if you want to
       inspect or short-circuit the redirect yourself.
@returns The final `Response`.

#### `define get(target: String, headers: Map = Map(), params: Map = Map()) -> Response`

HTTP GET.
@param headers Extra request headers (optional).
@param params Query-string parameters merged into the URL (optional).
@example resp := http.get("http://example.com")
@example resp := http.get("http://api/search", params: { "q": "quirk" })

#### `define post(target: String, data: String, headers: Map = Map()) -> Response`

HTTP POST with a raw body. The body is sent as-is — set the matching
`Content-Type` in `headers` if it isn't `application/octet-stream`.
@example resp := http.post("http://api/echo", "name=Quirk")
@example resp := http.post(url, "raw", headers: { "Content-Type": "text/plain" })

#### `define post_json(target: String, body: Any, headers: Map = Map()) -> Response`

POST a value as JSON. Serializes `body` via `json.dumps` and sets
`Content-Type: application/json` unless the caller already supplied one.
Accepts anything `json.dumps` does: Map, List, ISerializable structs, or
a String.
@example resp := http.post_json("http://api/users", { "name": "Quirk" })

#### `define delete(target: String, headers: Map = Map()) -> Response`

HTTP DELETE.
@example resp := http.delete("http://api/users/42")


## `net/index.quirk`


### Module-level functions

#### `extern define socket() -> Int`

Create a new TCP/IPv4 socket. Returns the OS file descriptor as an Int,
or a negative value on error.

#### `extern define bind(fd: Int, host: String, port: Int) -> Int`

Bind socket `fd` to `host:port`. Returns 0 on success, negative on error.
@example bind(fd, "0.0.0.0", 8080)

#### `extern define listen(fd: Int, backlog: Int) -> Int`

Mark `fd` as a listening socket with the given pending-connection backlog.
Must be called after `bind`. Returns 0 on success.

#### `extern define accept(fd: Int) -> Int`

Block until a client connects to listening socket `fd`, returning the new
client socket's fd. Negative on error.

#### `extern define connect(fd: Int, host: String, port: Int) -> Int`

Connect socket `fd` to a remote `host:port`. Returns 0 on success.
@example connect(fd, "example.com", 80)

#### `extern define send(fd: Int, data: String) -> Int`

Send `data` over socket `fd`. Returns the number of bytes sent (may be
less than `data.length()` for partial sends).

#### `extern define recv(fd: Int, size: Int) -> String`

Read up to `size` bytes from socket `fd`. Returns the data received as
a String. An empty string indicates the peer closed the connection.

#### `extern define close(fd: Int) -> void`

Close socket `fd`, releasing the OS resource.


## `net/server.quirk`

### `struct Request`

A parsed incoming HTTP request — what `Server` hands to your handler.

  `method`   — uppercase verb (`GET`, `POST`, …).
  `path`     — request path with query stripped (e.g. `/users/42`).
  `query`    — Map of query-string params (parsed via `url.parse_query`).
  `headers`  — Map of header name → value (case-preserved as received).
  `body`     — request body for POST/PUT, "" otherwise.

#### `define __str(self) -> String`

Compact form: `<Request GET /path>`.

#### `define listen(self, host: String, port: Int, handler: Callable) -> void`

Block forever, accepting connections on `host:port` and dispatching
each parsed `Request` to `handler`. The handler returns a `Response`
which is sent back and the connection is closed.

Exceptions raised by the handler are caught and converted to a plain
500 — they don't take the server down.

@example
Server().listen("0.0.0.0", 8080, fn(req: Request) -> Response {
    return Response(200, "hi", Map())
})

#### `define stop(self) -> void`

Stop the accept-loop on the next iteration.

### `struct Server`

A simple HTTP/1.1 server. Holds the listener socket and an exit flag so
`stop()` can break the accept-loop from another thread once Quirk gains
that ability — for now `listen` runs forever.


## `net/socket.quirk`

### `struct Socket`

TCP socket. Wraps an OS file descriptor and provides high-level
`bind`/`listen`/`accept`/`connect`/`send`/`recv`/`close` operations.
All failure modes raise `SocketError`.

#### `define __init(self, initial_fd: Int = -1) -> void`

Create a new socket. Pass `initial_fd` only when wrapping a fd you
already own (e.g. inside `accept`). The default `-1` allocates a fresh
socket via the OS.
@throws SocketError if the OS could not provide a socket fd.

#### `define bind(self, host: String, port: Int) -> void`

Bind this socket to `host:port`. Use `"0.0.0.0"` to listen on all
interfaces, or `"127.0.0.1"` for loopback only.
@throws SocketError on bind failure (port in use, permission denied, ...).

#### `define listen(self, backlog: Int = 5) -> void`

Mark this socket as listening for incoming connections. Must follow `bind`.
@param backlog Pending-connection queue size (default 5).

#### `define accept(self) -> Socket`

Block until a client connects, then return a new Socket wrapping that
client's fd. The returned socket should be closed when done (or use
`with`).
@throws SocketError if the accept syscall fails.

#### `define connect(self, host: String, port: Int) -> void`

Connect this socket to a remote `host:port`.
@throws SocketError on connect failure (DNS, refused, timeout, ...).

#### `define send(self, data: String) -> Int`

Send `data` over the socket. Returns the number of bytes actually sent —
callers should retry on short writes for large payloads.

#### `define recv(self, size: Int = 1024) -> String`

Receive up to `size` bytes. Returns "" when the peer has closed the
connection. Default buffer is 1024 bytes.

#### `define close(self) -> void`

Close the socket. Idempotent — calling close twice is a no-op.

#### `define __enter(self) -> Socket { return self }`

`with` enter — returns the Socket itself.

#### `define __exit(self) -> void { self.close() }`

`with` exit — closes the socket.
