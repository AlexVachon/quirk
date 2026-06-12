# `url` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `url/index.quirk`

### `struct URL`

Parsed URL components. All fields are present (empty strings / 0 when the
URL omits them) so callers don't need null-checks.

  scheme:   `"https"`, `"http"`, `"file"`, `"ftp"`, ...
  host:     hostname or IP literal (no port)
  port:     port number, or 0 if absent
  path:     `/path` portion (may be empty)
  query:    raw query without the leading `?`
  fragment: raw fragment without the leading `#`

#### `define parse(raw: String) -> URL`

Parse a URL string into a `URL` struct. Tolerates missing scheme and
missing host: e.g. `"/path?x=1"` parses with scheme="" and host="".


### Module-level functions

#### `define quote(s: String) -> String`

Percent-encode `s` for safe use in a URL component. Every byte that is not
in the unreserved set (A-Z a-z 0-9 - _ . ~) becomes `%XX` (uppercase hex).
Spaces are encoded as `%20`, NOT `+`.
@example  url.quote("Hello, world!")  // "Hello%2C%20world%21"

#### `define unquote(s: String) -> String`

Decode `%XX` byte escapes. `+` is NOT converted to space (use
`url.unquote_form` if you need application/x-www-form-urlencoded behavior).

#### `define unquote_form(s: String) -> String`

Decode a form-encoded value: handles `%XX` AND maps `+` to space, matching
the `application/x-www-form-urlencoded` spec used by HTML forms.

#### `define parse_query(q: String) -> Map`

Parse a query string `"k1=v1&k2=v2"` into a Map. Both keys and values are
percent-decoded. Keys with no `=` map to the empty String.

#### `define build_query(m: Map) -> String`

Build a `&`-joined query string from a Map. Each key and value is
percent-encoded.
