# `regex` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `regex/index.quirk`

### `struct Match`

Result of `Regex.find`. `start` and `end` are byte offsets into the input
String; `text` is the substring `[start, end)`; `groups` is a `List<String>`
of the captured groups (group 0 = full match is NOT included — `text` is
the whole match).

#### `define test(self, s: String) -> Bool`

True if the pattern matches anywhere in `s`.

#### `define find(self, s: String) -> Match`

Find the first match starting at or after `from_offset`. Returns a Match
with `text` = full match string and `groups` = captured-group strings.
Returns null when no match is found.

#### `define find_all(self, s: String) -> List`

Find every non-overlapping match. Returns a List<Match>.
Empty matches advance one byte to avoid infinite loops.

#### `define replace_all(self, s: String, replacement: String) -> String`

Replace every match with `replacement`. Replacement supports `\1`, `\2`, ...
backrefs to captured groups. `\0` is the entire match.

#### `define split(self, s: String) -> List`

Split `s` on every match of the pattern. Empty input yields a single
empty-string element.

#### `define compile(pattern: String, flags: String = "") -> Regex`

Compile a pattern. Equivalent to `Regex(pattern, flags)`.

#### `define test(pattern: String, s: String) -> Bool`

One-shot test: compile + test. Cheaper to keep a Regex around if reusing.

#### `define find(pattern: String, s: String) -> Match`

One-shot find: compile + find.

#### `define replace_all(pattern: String, s: String, replacement: String) -> String`

One-shot replace_all.

### `struct Regex`

A compiled regular expression. Construct via `regex.compile(pattern, flags)`.


### Module-level functions

#### `extern define compile_raw(pattern: String, flags: String) -> Any`

Compile a pattern. Returns an opaque handle stored in `Regex._handle`.
@throws ValueError if the pattern fails to compile.

#### `extern define test_raw(handle: Any, s: String) -> Bool`

True iff the compiled pattern matches anywhere in `s`.

#### `extern define find_at(handle: Any, s: String, from_offset: Int) -> Int`

Searches `s` starting at `from_offset` and caches the match groups in the
handle. Returns the start offset of the full match, or -1 if no match.
Quirk wrappers then read group_count/group_start/group_end to assemble Match.

#### `extern define replace_all_raw(handle: Any, s: String, replacement: String) -> String`

Returns a fresh String with every match of `handle`'s pattern replaced
by `replacement`. The replacement may reference captured groups via
`\1`, `\2`, ... (`\0` is the entire match).

#### `extern define split_raw(handle: Any, s: String) -> List`

Splits `s` on every match of `handle`'s pattern. Empty matches advance one
byte to prevent infinite loops.
