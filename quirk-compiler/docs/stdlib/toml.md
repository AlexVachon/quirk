# `toml` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `toml/index.quirk`

### `struct StringResult`

Parsed string result paired with the index of the closing quote so
the caller can detect trailing junk.

#### `define _parse_string(line: String, start_idx: Int) -> StringResult`

Parse a string literal starting at `start_idx` in `line`. Supports
basic (`"..."` with escapes) and literal (`'...'`, no escapes) forms.
Raises ValueError on an unterminated string.

#### `define _is_digit(c: String) -> Bool`

True if `c` (a length-1 String) is an ASCII digit '0'..'9'. String
comparisons (`<=`, `>=`) are defined on the stdlib String type, so
this is a single comparison-pair on the host runtime.

#### `define _parse_value(raw: String) -> Any`

Parse a single value (right-hand side of `key = ...`). Handles
strings, integers, booleans, and one-line arrays.

#### `define parse(text: String) -> Map`

Parse a complete TOML document into a Map. Sections become nested
Maps; `[[name]]` headers accumulate Lists of Maps under `name` in
the root.

Errors raise ValueError annotated with the offending line number.

### `struct KvResult`

Key + value extracted from a `key = value` line. Bare-key syntax only
for now (no quoted keys).


### Module-level functions

#### `define _char(s: String, i: Int) -> String`

One character of `s` at byte offset `i`, returned as a length-1
String. Quirk's String API doesn't expose `char_at` today, so we
simulate via substring — same semantics, slightly more allocation
that the parser absorbs in its inner loops.

#### `define _index_outside_strings(hay: String, needle: String, start_idx: Int) -> Int`

Index of `needle` in `hay` starting from `start_idx`, or -1. Skips
occurrences inside double/single-quoted strings so a `#` inside
`"foo # bar"` doesn't get mistaken for a comment marker.

#### `define _strip_comment(line: String) -> String`

Strip a trailing `# ...` comment from a logical line, respecting
quoted strings.
