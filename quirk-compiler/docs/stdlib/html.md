# `html` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `html/.venv/lib/quirk/stdlib/argparse/index.quirk`


## `html/.venv/lib/quirk/stdlib/console/index.quirk`


### Module-level functions

#### `define put(msg: Any) -> void`

Write `msg` to stdout WITHOUT a trailing newline. Available as both
`console.put` and `console.write` (the latter shadows the top-level
`write` builtin only in the `console.` namespace).
@example
console.put("Loading")
for i in 0..3 { console.put(".") }
console.log("done")

#### `define write(msg: Any) -> void`

Alias for `console.put`.

#### `define dir(value: Any) -> void`

Pretty-dump a value with newlines and indentation. Mirrors JS console.dir —
useful when `log` (single-line) hides structure.

#### `define password(prompt: String = "") -> String`

Read a line from stdin with echo disabled. Useful for password prompts.
A newline is echoed automatically after the user presses Enter so the next
output starts on a fresh line.

#### `define confirm(prompt: String, default: Bool = true) -> Bool`

Yes/no prompt with a default answer. Pressing Enter uses the default.
@example
if console.confirm("Continue?", default: false) { ... }

#### `define input_int(prompt: String = "") -> Int`

Prompt for an Int, retrying until the user enters a valid number.
@example
age := console.input_int("Your age: ")

#### `define input_float(prompt: String = "") -> Double`

Prompt for a Double, retrying until the user enters a valid number.

#### `define clear() -> void`

Clear the terminal screen.

#### `define clear_line() -> void`

Erase the current line and move the cursor to column 0.
Pairs with `put` for in-place updates (progress bars, spinners).

#### `define size() -> Tuple`

Terminal dimensions as `(cols, rows)`. Falls back to (80, 24) when stdout
is not a TTY or the syscall fails. Use `tuple[0]` and `tuple[1]` to access.

#### `define group(label: String) -> void`

Start an indented log group. Subsequent log/info/warn/etc. calls are indented
2 spaces per group level. Pair with `group_end()` to close.
@example
console.group("Build")
console.log("step 1")
console.log("step 2")
console.group_end()

#### `define time(label: String) -> void`

Start a named timer.

#### `define time_end(label: String) -> void`

Stop a named timer and log the elapsed milliseconds.
@example
console.time("fetch")
data := http.get("http://example.com")
console.time_end("fetch")  // logs: fetch: 123 ms

#### `define progress(current: Int, total: Int, label: String = "") -> void`

Render a one-line progress bar using `\r` to overwrite. Width auto-fits the
terminal (or 40 chars when not a TTY). Caller is responsible for printing a
newline after the final update (or call `console.log("")` to advance).

@example
for i in 0..101 {
    console.progress(i, 100, "Building")
    sys.sleep(20)
}
console.log("")  // newline after final bar

#### `define select(prompt: String, options: List) -> String`

Render an arrow-key menu and return the selected option string. Up/Down or
j/k navigate; Enter accepts; q/Esc cancels (returns the empty string).
Falls back to a numeric prompt when stdin is not a TTY.

@example
choice := console.select("Pick a fruit", ["apple", "banana", "cherry"])
console.log("You chose:", choice)


## `html/.venv/lib/quirk/stdlib/crypto/index.quirk`


### Module-level functions

#### `extern define md5(s: String) -> String`

MD5. Fast but cryptographically broken — use only for non-security checksums.

#### `extern define sha1(s: String) -> String`

SHA-1. Deprecated for new security work; ok for legacy compatibility.

#### `extern define sha256(s: String) -> String`

SHA-256. Current default for integrity, file checksums, content hashes.

#### `extern define sha512(s: String) -> String`

SHA-512. Larger output (128 hex chars); use when 256 bits aren't enough.

#### `extern define hmac_sha256(key: String, msg: String) -> String`

HMAC-SHA256 — message authentication code. Use for signed cookies, JWT
HS256, webhook signature verification, etc. Returns 64 hex chars.

#### `extern define random_hex(n: Int) -> String`

Cryptographically-strong random bytes, returned as a 2n-character lowercase
hex String. Suitable for tokens, salts, session IDs.
@example  token := crypto.random_hex(32)   // 64 hex chars = 256 random bits

#### `extern define uuid() -> String`

RFC 4122 v4 UUID. 36 chars: `xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx` where
the version is fixed at 4 and `y` is one of {8, 9, a, b}.


## `html/.venv/lib/quirk/stdlib/csv/index.quirk`


### Module-level functions

#### `define parse(text: String, delim: String = ",") -> List`

Parse `text` into a `List<List<String>>` — each outer item is a record,
each inner item is a field. Empty input returns an empty list.

Field rules (RFC 4180):
  - Fields are delimited by `delim` (default `,`).
  - Records end at LF or CRLF.
  - A field can be wrapped in `"..."`. Inside such a field:
      - the delimiter and newlines are taken literally
      - a literal quote is written as `""`
  - Trailing newline at the very end is ignored (no empty extra record).

#### `define parse_dicts(text: String, delim: String = ",") -> List`

Parse `text` taking the first row as a header, returning a
`List<Map<String,String>>`. Each record's keys come from the header row;
missing trailing fields map to the empty String.

#### `define _quote_if_needed(field: String, delim: String) -> String`

Wraps `field` in quotes and doubles internal quotes IF it contains
the delimiter, a quote, or a newline. Otherwise returns it unchanged.

#### `define write_dicts(records: List, delim: String = ",") -> String`

Serialize a `List<Map<String,String>>` with a header row drawn from the
keys of the first record. Records with missing keys emit empty fields.
Returns the empty String for an empty input.

#### `define read_file(path: String, delim: String = ",") -> List`

Read `path` as CSV and parse. Returns `List<List<String>>`.

#### `define write_file(path: String, records: List, delim: String = ",") -> void`

Write `records` to `path` as CSV.


## `html/.venv/lib/quirk/stdlib/datetime/index.quirk`

### `struct DateTime`

Snapshot of a moment in time. Construct via `datetime.now()`,
`datetime.from_unix(s)`, or `datetime.from_iso(s)` rather than calling
`__init` directly.

#### `define iso(self) -> String`

ISO-8601 representation.

#### `define format(self, fmt: String) -> String`

Format using a strftime-style spec. Common codes:
  `%Y` 4-digit year   `%m` month 01-12   `%d` day 01-31
  `%H` hour 00-23     `%M` minute 00-59  `%S` second 00-59
  `%A` weekday name   `%a` short day      `%B` month name
  `%j` day-of-year    `%w` weekday 0-6

#### `define add_seconds(self, n: Int) -> DateTime`

Returns a new DateTime offset by `n` seconds (can be negative).

#### `define diff_seconds(self, other: DateTime) -> Int`

`self - other` in seconds. Negative if `self` is earlier.

#### `define __str(self) -> String`

Default str: ISO-8601.

#### `define now() -> DateTime`

Current local DateTime.

#### `define utc_now() -> DateTime`

Current UTC DateTime.

#### `define from_unix(epoch: Int) -> DateTime`

Build a DateTime from an explicit Unix epoch (seconds), interpreted in the
local time zone.

#### `define utc_from_unix(epoch: Int) -> DateTime`

Build a DateTime from an explicit Unix epoch, interpreted as UTC.

#### `define from_iso(s: String) -> DateTime`

Parse an ISO-8601 string (`"YYYY-MM-DD[Thh:mm:ss[Z]]"`).
Trailing `Z` → UTC; otherwise local. Throws `ValueError` on bad input.

#### `define make(year: Int, month: Int, day: Int, hour: Int = 0, minute: Int = 0, second: Int = 0) -> DateTime`

Build a DateTime from explicit calendar components (local time).
@throws ValueError if the components are out of range.

#### `define utc_make(year: Int, month: Int, day: Int, hour: Int = 0, minute: Int = 0, second: Int = 0) -> DateTime`

Build a DateTime from explicit calendar components (UTC).
@throws ValueError on out-of-range components.

#### `define today() -> DateTime`

The current local date+time. Synonym for `now()`.

#### `define tomorrow() -> DateTime`

One day in the future, same time-of-day.

#### `define yesterday() -> DateTime`

One day in the past, same time-of-day.

#### `define is_leap(year: Int) -> Bool`

Standard Gregorian leap-year rule: divisible by 4, except centuries that
aren't divisible by 400. 2000 was leap; 1900 wasn't.

@example
is_leap(2024)   // true
is_leap(2023)   // false
is_leap(2000)   // true
is_leap(1900)   // false

#### `define days_in_month(year: Int, month: Int) -> Int`

Number of days in `month` (1-12) of `year`. Handles February's leap-year
adjustment via `is_leap`.

#### `define is_weekend(dt: DateTime) -> Bool`

True for Saturday and Sunday (where `weekday` is 0=Sunday … 6=Saturday).

#### `define day_name(dt: DateTime) -> String`

Full English weekday name for `dt`. `weekday` is 0=Sunday … 6=Saturday.

#### `define month_name(month: Int) -> String`

Full English month name for `month` (1-12).

#### `define start_of_day(dt: DateTime) -> DateTime`

Truncate `dt` to midnight. Returns a fresh DateTime with the same date
but `hour = minute = second = 0`.

#### `define start_of_week(dt: DateTime) -> DateTime`

Monday of the week containing `dt`, at midnight. Uses ISO weeks
(Monday = first day). If `dt` already falls on Monday, returns midnight
of the same day.

#### `define start_of_month(dt: DateTime) -> DateTime`

First day of `dt`'s month, at midnight.

#### `define start_of_year(dt: DateTime) -> DateTime`

January 1 of `dt`'s year, at midnight.

#### `define diff_minutes(a: DateTime, b: DateTime) -> Int`

Whole minutes between `a` and `b` (signed; `b - a`).

#### `define diff_hours(a: DateTime, b: DateTime) -> Int`

Whole hours between `a` and `b` (signed).

#### `define diff_days(a: DateTime, b: DateTime) -> Int`

Whole days between `a` and `b` (signed). Calendar-day boundaries
aren't honored — this is simply `seconds / 86400`.

#### `define humanize(dt: DateTime, relative_to: DateTime = now()) -> String`

Approximate relative time vs `relative_to`, e.g. "2 hours ago" or
"in 3 days". `relative_to` defaults to "now" via `now()`.
Granularity grows with the gap: seconds → minutes → hours → days →
weeks → months → years.

@example
humanize(yesterday())          // "1 day ago"
humanize(tomorrow())           // "in 1 day"


## `html/.venv/lib/quirk/stdlib/debug/index.quirk`


### Module-level functions

#### `extern define breakpoint(label: String = "") -> void`

Pause execution at this line and prompt for a debugger command. `label`
shows up in the banner so multiple breakpoints in the same file stay
distinguishable. Defaults to "" — fine when you only have one.

@example
debug.breakpoint()                      // unlabeled, simplest form
debug.breakpoint("before db write")     // labeled


## `html/.venv/lib/quirk/stdlib/encoding/base64.quirk`


### Module-level functions

#### `extern define encode(data: String) -> String`

Encode `data` to a Base64 String. Output length is roughly 4/3 of input,
rounded up to a multiple of 4 with `=` padding.

#### `extern define decode(data: String) -> String`

Decode a Base64 String back to the original. Tolerant of whitespace and
missing padding; returns "" on malformed input.


## `html/.venv/lib/quirk/stdlib/encoding/hex.quirk`


### Module-level functions

#### `extern define encode(data: String) -> String`

Encode each byte of `data` as two lowercase hex digits.
Output length is exactly `2 * data.length()`.

#### `extern define decode(data: String) -> String`

Decode a hex String back to its original bytes. Accepts mixed case.
Returns "" if the input length is odd or contains non-hex characters.


## `html/.venv/lib/quirk/stdlib/encoding/json.quirk`

### `struct JsonMap : ISerializable`

Wraps a Map as an ISerializable for net.http's `json:` parameter.

@example
http.post(url, json: JsonMap(m))

### `struct JsonList : ISerializable`

Wraps a List as an ISerializable for net.http's `json:` parameter.

@example
http.post(url, json: JsonList(l))


### Module-level functions

#### `define dumps_pretty(val: Any, indent: Int = 2) -> String`

Pretty-print `val` with `indent` spaces per nesting level. Indented JSON
is much easier to read for config files and debug output; pass `indent=4`
for a more spacious layout.

@example
m := Map()
m.put("name", "Alice")
m.put("scores", [98, 87, 92])
print(json.dumps_pretty(m))
// {
//   "name": "Alice",
//   "scores": [
//     98,
//     87,
//     92
//   ]
// }

#### `define loads(s: String) -> Any`

Parse a JSON string and return the corresponding Quirk value with
real types — no manual coercion required.

  JSON object  → `Map`     (String keys, mixed-type values)
  JSON array   → `List`    (mixed-type elements)
  JSON string  → `String`
  JSON number  → `Int` (no decimal/exponent) or `Double`
  JSON boolean → `Bool`
  JSON null    → null

@param s  A valid JSON string.
@returns  A Map, List, String, or boxed primitive.

@example
data: Map = json.loads('{"name":"Alice","age":30,"active":true}')
print(data.get("name"))     // Alice  (String)
print(data.get("age"))      // 30     (Int — usable in arithmetic)
print(data.get("active"))   // true   (Bool)

#### `define dump(val: Any, file: File) -> void`

Serialize `val` and write the JSON string to an open File.

@example
with File("config.json", "w") as f {
    json.dump(config_map, f)
}

#### `define load(file: File) -> Any`

Read the entire contents of `file` and parse as JSON.

@example
with File("config.json", "r") as f {
    config: Map = json.load(f)
    print(config.get("host"))
}


## `html/.venv/lib/quirk/stdlib/fs/index.quirk`


### Module-level functions

#### `extern define exists(path: String) -> Bool`

True if anything (file, directory, symlink) exists at `path`.

#### `extern define is_file(path: String) -> Bool`

True only if `path` is a regular file.

#### `extern define is_dir(path: String) -> Bool`

True only if `path` is a directory.

#### `extern define size(path: String) -> Int`

File size in bytes. Returns -1 if `path` doesn't exist or stat fails.

#### `extern define mtime(path: String) -> Int`

Last-modified time as Unix epoch seconds. -1 on failure.

#### `extern define mkdir_raw(path: String, parents: Int) -> Int`

Create the directory at `path`. Returns 0 on success, -1 on failure.
@param parents If true, also creates any missing intermediate directories
              (like `mkdir -p`) and treats "already exists" as success.

#### `define mkdir(path: String, parents: Bool = false) -> void`

Create a directory.
@param parents If true, intermediate directories are also created and an
              existing directory is not an error (`mkdir -p` semantics).
@throws IOError on failure.

#### `extern define rmdir_raw(path: String) -> Int`

Remove an empty directory. @throws IOError on failure (incl. non-empty dir).

#### `extern define remove_raw(path: String) -> Int`

Delete a file. @throws IOError on failure.

#### `extern define rename_raw(src: String, dst: String) -> Int`

Rename / move from one path to another. Atomic on the same filesystem.
@throws IOError on failure.

#### `extern define list_dir(path: String) -> List`

List the immediate entries of directory `path` as a List<String>.
Excludes `.` and `..`. Returns an empty List if the path doesn't exist
or isn't a directory.
@example
for name in fs.list_dir(".") { print(name) }

#### `extern define cwd() -> String`

Current working directory as a String.

#### `extern define chdir_raw(path: String) -> Int`

Change current working directory. @throws IOError on failure.

#### `define join(a: String, b: String) -> String`

Join two path components with `/`. Avoids double-slashes when `a` already
ends with one. (Forward slashes work on Windows in modern paths.)
@example fs.join("/etc", "hosts")  // "/etc/hosts"

#### `define basename(path: String) -> String`

Final component of `path` — strips any trailing slash and returns the
last segment. `"a/b/c.txt"` → `"c.txt"`; `"/etc/"` → `"etc"`.

#### `define dirname(path: String) -> String`

Everything before the final path component. `"a/b/c.txt"` → `"a/b"`.
Returns "" if the path has no separators.

#### `define extname(path: String) -> String`

File extension including the leading dot (`".txt"`), or "" when there
is none. A leading dot in the basename (e.g. `.gitignore`) is NOT an
extension — it's the filename.

#### `define split_ext(path: String) -> Tuple`

Splits `path` into `(stem, ext)`.
`"foo.tar.gz"` → `("foo.tar", ".gz")`. `"README"` → `("README", "")`.


## `html/.venv/lib/quirk/stdlib/io/file.quirk`

### `struct File`

A handle for reading and writing to the OS file system.
Supports the `with` statement for automatic close on scope exit.

Use `sys.stdin()`, `sys.stdout()`, `sys.stderr()` to wrap the standard
streams as Files (those wrappers ignore `close()`).

#### `extern define __init(self, path: String, mode: String) -> void`

Open `path` in the requested `mode`. Sets `is_open` based on whether the
underlying `fopen` succeeded — check it before reading.
@param path File path (absolute or relative to cwd).
@param mode Standard fopen-style mode string.

#### `extern define read(self) -> String`

Read the entire file into a String. Returns "" on error or empty file.
@returns The file contents as a String.

#### `extern define read_line(self) -> String`

Read one line from the file. The trailing `\n` is stripped.
Returns the empty string at EOF; check both for empty content and EOF
by comparing against `""` plus a separate `is_open` check if needed.
Maximum line length is 2048 characters.

#### `extern define write(self, data: String) -> void`

Write `data` to the file at the current position and flush immediately.
No newline is appended — include `\n` in `data` if you want one.

#### `extern define close(self) -> void`

Close the file handle. Standard streams (`stdin`/`stdout`/`stderr`)
are protected from being closed.

#### `define __enter(self) -> File`

`with` enter — returns the File itself.

#### `define __exit(self) -> void`

`with` exit — closes the file.

#### `extern define __has_next(self) -> Bool`

True if more lines remain.

#### `extern define __next(self) -> String`

Returns the next line; trailing newline stripped.

### `struct FileIterator`

Iterator over a file's lines. Produced by `for line in file { ... }`.
Each `__next` call reads one line via `File.read_line`.


## `html/.venv/lib/quirk/stdlib/itertools/index.quirk`


### Module-level functions

#### `define range_list(start: Int, count: Int = -1, step: Int = 1) -> List`

List of `n` arithmetic-progression values starting at `start`, stepping
by `step`. `range_list(5)` → [0,1,2,3,4]; `range_list(2, 5)` →
[2,3,4,5,6]; `range_list(0, 10, 2)` → [0,2,4,6,8,10,12,14,16,18].

#### `define repeat(value: Any, n: Int) -> List`

List of `n` copies of `value`.
@example repeat("x", 3)   // ["x", "x", "x"]

#### `define cycle(items: List, total: Int) -> List`

Cycle `items` to produce a List of length `total`. Empty input yields an
empty list regardless of `total`.

@example
cycle([1, 2, 3], 7)   // [1, 2, 3, 1, 2, 3, 1]

#### `define enumerate(items: List, start: Int = 0) -> List`

Pair each value with a running index, returning `(idx, value)` tuples.
`start` defaults to 0 — pass `start=1` for 1-based numbering. Access the
returned pairs with `t.0` / `t.1` or `t[0]` / `t[1]`.

@example
for pair in enumerate(["a", "b", "c"]) {
    print("${pair.0}: ${pair.1}")
}

#### `define zip(a: List, b: List) -> List`

Pair up two Lists element-wise as tuples. Stops at the shorter length —
matches Python's `zip`, not `zip_longest`.

@example
zip([1, 2, 3], ["a", "b"])   // [(1, "a"), (2, "b")]

#### `define partition(pred: Callable, items: List) -> Tuple`

Split into `(matching, non_matching)` based on `pred`. Useful for "yes/no"
splits where you want both halves at once.

@example
p := partition(fn(x) => x > 0, [-2, 3, -1, 7])
print(p.0)   // [3, 7]
print(p.1)   // [-2, -1]

#### `define chain(a: List, b: List) -> List`

Concatenate two Lists in order.
@example chain([1, 2], [3, 4])   // [1, 2, 3, 4]

#### `define take_while(pred: Callable, items: List) -> List`

Take items from the front while `pred` is true; stop at the first false.

@example
take_while(fn(x) => x < 5, [1, 3, 5, 4, 2])   // [1, 3]

#### `define drop_while(pred: Callable, items: List) -> List`

Drop items from the front while `pred` is true, return the remainder.
@example drop_while(fn(x) => x < 5, [1, 3, 5, 4, 2])   // [5, 4, 2]

#### `define groupby(key_fn: Callable, items: List) -> Map`

Group items by the value produced by `key_fn`. Returns a Map from
stringified key to a List of items with that key. Order of insertion in
each group is preserved.

@example
words := ["pear", "plum", "apple", "peach", "apricot"]
groupby(fn(w) => w.substring(0, 1), words)
// {"p": ["pear", "plum", "peach"], "a": ["apple", "apricot"]}

#### `define unique(items: List) -> List`

Distinct elements, preserving the first-seen order. Items are deduped by
their string representation, which matches the rest of Quirk's value-
equality conventions.

@example
unique([3, 1, 4, 1, 5, 9, 2, 6, 5, 3])   // [3, 1, 4, 5, 9, 2, 6]


## `html/.venv/lib/quirk/stdlib/math/index.quirk`


### Module-level functions

#### `extern define sin(x: Double) -> Double`

Sine of `x` (in radians).

#### `extern define cos(x: Double) -> Double`

Cosine of `x` (in radians).

#### `extern define tan(x: Double) -> Double`

Tangent of `x` (in radians). Undefined at `pi/2 + k*pi`.

#### `extern define asin(x: Double) -> Double`

Arc sine — inverse of `sin`. Returns radians in `[-pi/2, pi/2]`. NaN if `|x| > 1`.

#### `extern define acos(x: Double) -> Double`

Arc cosine — inverse of `cos`. Returns radians in `[0, pi]`. NaN if `|x| > 1`.

#### `extern define atan(x: Double) -> Double`

Arc tangent — inverse of `tan`. Returns radians in `(-pi/2, pi/2)`.

#### `extern define atan2(y: Double, x: Double) -> Double`

Two-argument arctangent — angle of the point `(x, y)` from the origin.
Returns radians in `(-pi, pi]`. Handles all four quadrants correctly.
@example math.atan2(1.0, 0.0)  // pi/2

#### `extern define sinh(x: Double) -> Double`

Hyperbolic sine: `(e^x - e^-x) / 2`.

#### `extern define cosh(x: Double) -> Double`

Hyperbolic cosine: `(e^x + e^-x) / 2`.

#### `extern define tanh(x: Double) -> Double`

Hyperbolic tangent: `sinh(x) / cosh(x)`. Range `(-1, 1)`.

#### `extern define exp(x: Double) -> Double`

`e ** x`.

#### `extern define log(x: Double) -> Double`

Natural logarithm (base e). NaN if `x < 0`, `-inf` at 0.

#### `extern define log2(x: Double) -> Double`

Base-2 logarithm.

#### `extern define log10(x: Double) -> Double`

Base-10 logarithm.

#### `extern define pow(base: Double, exponent: Double) -> Double`

`base ** exponent`. Handles fractional and negative exponents.
For Int powers prefer `Int.pow`, which avoids floating-point rounding.

#### `extern define sqrt(x: Double) -> Double`

Square root. NaN if `x < 0`.

#### `extern define cbrt(x: Double) -> Double`

Cube root. Defined for all reals (negative input returns negative root).

#### `extern define floor(x: Double) -> Double`

Largest integer ≤ x, returned as Double. `floor(2.7) == 2.0`.

#### `extern define ceil(x: Double) -> Double`

Smallest integer ≥ x, returned as Double. `ceil(2.1) == 3.0`.

#### `extern define round(x: Double) -> Double`

Round to nearest integer (half-away-from-zero). `round(2.5) == 3.0`.

#### `extern define trunc(x: Double) -> Double`

Truncate toward zero. `trunc(-2.7) == -2.0`.

#### `extern define fmod(a: Double, b: Double) -> Double`

Floating-point remainder of `a / b` with the sign of `a`.
@example math.fmod(10.0, 3.0)  // 1.0

#### `extern define hypot(x: Double, y: Double) -> Double`

Euclidean distance from origin: `sqrt(x*x + y*y)` without intermediate overflow.
@example math.hypot(3.0, 4.0)  // 5.0

#### `extern define abs(x: Int) -> Int`

Absolute value of an Int. Use `abs_f` for Doubles.

#### `extern define fabs(x: Double) -> Double`

Absolute value of a Double. (Wrapped by `abs_f` for naming symmetry.)

#### `extern define sign_int(x: Int) -> Int`

Signum of an Int: -1 (negative), 0, or +1 (positive). Wrapped by `sign_i`.

#### `extern define sign(x: Double) -> Double`

Signum of a Double: -1.0, 0.0, or +1.0. Returns 0 for NaN.

#### `define abs_f(x: Double) -> Double { return fabs(x) }`

Absolute value of a Double. (`abs` is the Int version.)

#### `define sign_i(x: Int) -> Int { return sign_int(x) }`

Sign of an Int: -1, 0, or 1.

#### `extern define is_nan(x: Double) -> Bool`

True if `x` is NaN (Not-a-Number).

#### `extern define is_finite(x: Double) -> Bool`

True if `x` is finite (not `inf`, `-inf`, or NaN).

#### `extern define is_inf(x: Double) -> Bool`

True if `x` is `+inf` or `-inf`.

#### `extern define pi() -> Double`

π — circle's circumference / diameter. ≈ 3.14159265358979

#### `extern define e() -> Double`

e — Euler's number. ≈ 2.71828182845905

#### `extern define tau() -> Double`

τ — full turn in radians. `tau == 2 * pi`. ≈ 6.28318530717959

#### `extern define infinity() -> Double`

Positive infinity (`1.0 / 0.0`).

#### `extern define nan() -> Double`

Not-a-Number (`0.0 / 0.0`). Distinguishable via `is_nan`; `nan != nan`.

#### `define min(a: Int, b: Int) -> Int`

Smaller of two Ints.

#### `define max(a: Int, b: Int) -> Int`

Larger of two Ints.

#### `define clamp(value: Int, lo: Int, hi: Int) -> Int`

Restrict `value` to the closed range [lo, hi].
@example math.clamp(150, 0, 100) // 100

#### `define min_f(a: Double, b: Double) -> Double`

Smaller of two Doubles.

#### `define max_f(a: Double, b: Double) -> Double`

Larger of two Doubles.

#### `define clamp_f(value: Double, lo: Double, hi: Double) -> Double`

Restrict a Double to the closed range `[lo, hi]`.

#### `extern define seed(s: Int) -> void`

Seed the pseudo-random generator. The same seed always produces the same
sequence of `random()` / `random_int()` results — useful for reproducible
tests and procedural generation.

#### `extern define random() -> Double`

Uniform Double in `[0.0, 1.0)`. 53 bits of precision.

#### `extern define random_int(lo: Int, hi: Int) -> Int`

Uniform Int in `[lo, hi]` (inclusive on both ends).
Returns `lo` if `hi <= lo`.
@example math.random_int(1, 6)  // dice roll

#### `define choice(items: List) -> Any`

Pick one element of `list` uniformly at random.
@throws ValueError if the list is empty.

#### `define shuffle(items: List) -> List`

Shuffle `items` in place using Fisher-Yates. Returns the same list for chaining.


## `html/.venv/lib/quirk/stdlib/math/vectors.quirk`

### `struct Vector2`

A 2D vector with `Double` components and operator overloading for
`+`, `-`, and scalar `*`.

#### `define __init(self, x: Double, y: Double) -> void`

Construct a Vector2 from two Doubles.

#### `define __str(self) -> String`

Compact human-readable form: `v(x, y)`.

#### `define __repr(self) -> String`

Developer-readable form: `Vector2 { x: ..., y: ... }`.

#### `define __add(self, other: Vector2) -> Vector2`

Componentwise addition: `a + b`.

#### `define __sub(self, other: Vector2) -> Vector2`

Componentwise subtraction: `a - b`.

#### `define __mul(self, scalar: Double) -> Vector2`

Scalar multiplication: `v * s` scales each component.

#### `define dot(self, other: Vector2) -> Double`

Dot product: `a · b = a.x*b.x + a.y*b.y`. Useful for projections,
angle calculations (`cos θ = dot(a, b) / (|a| * |b|)`), and detecting
orthogonality (dot == 0).

#### `define __init(self, x: Double, y: Double, z: Double) -> void`

Construct a Vector3 from three Doubles.

#### `define __str(self) -> String`

Compact human-readable form: `Vec3(x, y, z)`.

#### `define __add(self, other: Vector3) -> Vector3`

Componentwise addition: `a + b`.

### `struct Vector3`

A 3D vector with `Double` components. Currently supports `+` only —
extend with `__sub`, `__mul`, `dot`, `cross` as needed.


## `html/.venv/lib/quirk/stdlib/net/http.quirk`

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


## `html/.venv/lib/quirk/stdlib/net/index.quirk`


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


## `html/.venv/lib/quirk/stdlib/net/server.quirk`

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


## `html/.venv/lib/quirk/stdlib/net/socket.quirk`

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


## `html/.venv/lib/quirk/stdlib/prompt/index.quirk`


### Module-level functions

#### `define input(message: String, default: String = "") -> String`

Free-text input. Appends ` [default]: ` when a default is provided so
the user knows what an empty reply gets them. An empty reply with
a default returns the default; an empty reply with no default
re-prompts (mirrors `read -p` behaviour in shells).

#### `define input_optional(message: String) -> String?`

Like `input` but returns `null` on an empty reply instead of re-prompting.
Use when the caller wants to handle the "user didn't say anything" branch
itself — e.g. asking a custom follow-up before re-prompting, or skipping
the field entirely.

    name := prompt.input_optional("Your name")
    match name {
        case null => print("(skipped)")
        case _    => print("hi, " + name)
    }

#### `define password(message: String) -> String`

Hidden input. Shells out to console.password, which already disables
terminal echo via termios in the C runtime. Single-call wrapper so
client code doesn't need to know which stdlib module owns the helper.

#### `define confirm(message: String, default: Bool = true) -> Bool`

Yes/no question. `default=true` means an empty reply (pressing Enter)
accepts; `default=false` means it rejects. The prompt suffix is
`[Y/n]` / `[y/N]` matching most CLI conventions.

#### `define select(message: String, options: List, default_idx: Int = 0) -> String`

Pick one option from a numbered list. Renders:

    Mode?
      1) fast        [default]
      2) thorough
      3) debug
    Choice (1-3, default 1): _

Returns the selected option's text. `default_idx` is zero-based; out-
of-range values fall back to the first option.


## `html/.venv/lib/quirk/stdlib/random/index.quirk`


### Module-level functions

#### `extern define seed(n: Int) -> void`

Re-seed the global RNG. Same `n` => same stream of values.

#### `extern define _next_double() -> Double`

Internal: returns a Double in [0.0, 1.0). Prefer `random()` below.

#### `extern define _next_int(lo: Int, hi: Int) -> Int`

Internal: inclusive `[lo, hi]`. Used by `randint` and `choice`.

#### `extern define choice(items: List) -> Any`

Pick a random element from a non-empty list. Returns null if empty.

#### `extern define shuffle(items: List) -> List`

In-place Fisher-Yates shuffle. Returns the same list.

#### `define random() -> Double`

Uniform double in `[0.0, 1.0)`. The fundamental float-valued primitive
— all higher-level Double helpers build on this.

@example
if random.random() < 0.5 { print("heads") }

#### `define randint(lo: Int, hi: Int) -> Int`

Inclusive integer in `[lo, hi]`. Matches Python's `random.randint`.
Order of arguments doesn't matter — the runtime swaps if `hi < lo`.

@example
die := random.randint(1, 6)

#### `define uniform(lo: Double, hi: Double) -> Double`

Uniform Double in `[lo, hi)`. Useful for sampling continuous ranges
(positions, weights, etc.).

@example
angle := random.uniform(0.0, 6.28318)

#### `define bool() -> Bool`

True or False with 50/50 odds. One-liner around `_next_int(0, 1)`; the
sugar pays off because it reads better at call sites (`if random.bool()`).

#### `define sample(items: List, k: Int) -> List`

Return `k` distinct elements drawn from `items`. Throws if `k > items.length()`.
Implemented as a partial Fisher-Yates over a copy so the caller's list
isn't disturbed. O(k) work in the average case.

@example
deck := [1,2,3,4,5,6,7,8,9,10]
hand := random.sample(deck, 5)


## `html/.venv/lib/quirk/stdlib/regex/index.quirk`

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


## `html/.venv/lib/quirk/stdlib/statistics/index.quirk`


### Module-level functions

#### `define mean(items: List) -> Double`

Arithmetic mean. Throws `ValueError` on empty input.

@example
mean([1, 2, 3, 4])   // 2.5

#### `define median(items: List) -> Double`

Median value (50th percentile). For even-length inputs returns the
average of the two middle values.

@example
median([1, 2, 3, 4, 5])   // 3.0
median([1, 2, 3, 4])      // 2.5

#### `define median_low(items: List) -> Double`

Lower median (no averaging — returns one of the input values). For odd
n this matches `median`; for even n it returns the lower of the two
midpoints.

#### `define median_high(items: List) -> Double`

Upper median. Mirror of `median_low`: for even n returns the upper
midpoint.

#### `define mode(items: List) -> String`

Most common element. Ties resolve to the first-seen value. Returns the
stringified form (since elements can be heterogeneous types).

@example
mode([1, 2, 2, 3, 3, 3])   // "3"

#### `define variance(items: List) -> Double`

Sample variance (divides by `n - 1`, the unbiased estimator). Throws if
`n < 2`. Use `pvariance` when treating the input as the entire population.

#### `define pvariance(items: List) -> Double`

Population variance (divides by `n`). Use when the input *is* the
population, not a sample of it.

#### `define stdev(items: List) -> Double`

Sample standard deviation: `sqrt(variance(items))`.

#### `define pstdev(items: List) -> Double`

Population standard deviation: `sqrt(pvariance(items))`.

#### `define min_val(items: List) -> Double`

Smallest value as Double. Throws on empty input.

#### `define max_val(items: List) -> Double`

Largest value as Double. Throws on empty input.

#### `define quantile(items: List, q: Double) -> Double`

The `q`-th quantile of `items` for `q` in `[0.0, 1.0]`. Uses linear
interpolation between the two surrounding sample points. `quantile(xs,
0.5)` is the median; `0.25` is the lower quartile; `0.75` the upper.

@example
quantile([1, 2, 3, 4, 5], 0.25)   // 2.0
quantile([1, 2, 3, 4, 5], 0.75)   // 4.0


## `html/.venv/lib/quirk/stdlib/sys/index.quirk`


### Module-level functions

#### `extern define arg_count() -> Int`

Number of command-line arguments. Prefer `argv()` for the parsed List.

#### `extern define arg_get(index: Int) -> String`

Argument at `index` (0-based; 0 is the program name).
@throws IndexError if index is out of range.

#### `extern define getenv(name: String) -> String`

Reads an environment variable. Returns the empty string when unset.
Prefer `env(key)` for the same behavior with a friendlier name.

#### `extern define system(cmd: String) -> Int`

Runs `cmd` via the OS shell and returns the exit status.
Prefer `run(cmd)` for the same behavior with a friendlier name.

#### `extern define exit(code: Int) -> void`

Immediately terminates the program with `code`. Skips finalizers.
Prefer `terminate(code)` for the friendlier name.

#### `extern define sleep(ms: Int) -> void`

Suspend the current thread for `ms` milliseconds. Negative values are no-ops.

#### `extern define byteorder() -> String`

The host machine's byte order: `"little"` or `"big"`.

#### `extern define copyright() -> String`

A short copyright string for the Quirk runtime.

#### `extern define executable() -> String`

Absolute path to the running quirk executable.

#### `extern define getdefaultencoding() -> String`

Default text encoding (e.g. `"utf-8"`).

#### `extern define getrecursionlimit() -> Int`

Maximum recursion depth before a stack-overflow is raised.

#### `extern define setrecursionlimit(limit: Int) -> void`

Sets the maximum recursion depth. Use cautiously — too high can crash the host.

#### `extern define getsizeof(obj: Any) -> Int`

Approximate size of `obj` in bytes (best-effort).

#### `extern define maxsize() -> Int`

Largest representable Int on this platform.

#### `extern define platform() -> String`

Platform identifier, e.g. `"linux"`, `"darwin"`, `"win32"`.

#### `extern define version() -> String`

Quirk version string, e.g. `"0.1.0"`.

#### `extern define prefix() -> String`

Installation prefix — directory containing `bin/quirk` and `libs/`.

#### `extern define exc_info() -> Any`

Information about the most recently caught exception, or null if none.
Used by debugging tools.

#### `extern define shadow_size() -> Int`

Number of frames currently on the Quirk shadow stack (per-thread call depth).

#### `extern define shadow_frame(index: Int) -> String`

Returns one shadow-stack frame as a formatted string. Index 0 is the bottom
of the stack (`main`); higher indices are deeper calls.

#### `extern define capture_backtrace() -> void`

Captures the C-level backtrace into an internal buffer. Subsequent calls
overwrite. Used by exception handlers to attach traceback info.

#### `extern define backtrace_size() -> Int`

Number of frames in the buffer captured by the most recent `capture_backtrace()`.

#### `extern define backtrace_frame(index: Int) -> String`

Returns one captured C backtrace frame as a formatted string.

#### `extern define stdin() -> File`

Standard input wrapped as a `File`. Multiple calls return fresh wrappers
around the same underlying handle; closing one does not close stdin.

#### `extern define stdout() -> File`

Standard output wrapped as a `File`. See `stdin()` for handle semantics.

#### `extern define stderr() -> File`

Standard error wrapped as a `File`. See `stdin()` for handle semantics.

#### `extern define isatty(f: File) -> Bool`

Returns true if the given stream is a terminal (interactive). Use to decide
whether to emit ANSI codes — pipes/files should stay plain.

#### `extern define ansi(name: String) -> String`

Returns the ANSI SGR sequence for a named effect, or "" if unknown.
Names: `reset`, `bold`, `dim`, `red`, `green`, `yellow`, `blue`, `cyan`,
`gray`, and `clear` (full screen clear).

#### `extern define terminal_cols() -> Int`

Width of the terminal in columns. Returns 80 when stdout is not a TTY.

#### `extern define terminal_rows() -> Int`

Height of the terminal in rows. Returns 24 when stdout is not a TTY.

#### `extern define read_password() -> String`

Reads one line from stdin with echo disabled. The trailing newline is stripped.
Used by `console.password`.

#### `extern define read_key() -> Int`

Reads ONE keypress from stdin without waiting for Enter. Returns the byte
value as an Int (e.g. 13 for Enter, 27 for ESC, 0x41 for 'A').
Arrow keys produce a 3-byte sequence: `27, 91, 65/66/67/68`.

#### `extern define now_ms() -> Int`

Monotonic clock in milliseconds. Use deltas only — the absolute value is
not tied to the Unix epoch. Suitable for measuring elapsed time.

#### `extern define group_depth_get() -> Int`

Current group-indent depth (0 at the top level). Used by `console.group`.

#### `extern define group_depth_inc() -> void`

Increment group-indent depth. Paired with `group_depth_dec()` by `console.group`.

#### `extern define group_depth_dec() -> void`

Decrement group-indent depth (clamped at 0).

#### `extern define timer_start(label: String) -> void`

Start a named timer. Subsequent `timer_end(label)` returns elapsed ms.
Up to 32 timers can be active at once.

#### `extern define timer_end(label: String) -> Int`

Stop a named timer and return elapsed ms since `timer_start(label)`.
Returns -1 if the label was never started or has already ended.

#### `extern define srcline(filename: Any, line: Int) -> Any`

Returns the source line at `filename:line` as a String, or null on failure.
Used by exception printers to render code snippets in tracebacks.

#### `define argv() -> List`

Returns a List of Strings representing the command-line arguments.
Index 0 is the name/path of the executing program.
@example
for arg in sys.argv() { print(arg) }

#### `define builtin_module_names() -> List`

Names of modules compiled into this Quirk distribution.

#### `define path() -> List`

Search path for `use` / `from ... use` resolution.
Order: `<prefix>/libs`, `<prefix>/packages`, current directory.

#### `define modules() -> Map`

Map of loaded modules. Currently a stub — returns empty until reflection lands.

#### `define implementation() -> Map`

Implementation info: `name`, `cache_tag`, `version`.
@example
sys.implementation().get("version")

#### `define env(key: String) -> String`

Retrieves the value of an environment variable. Returns the empty string
when the variable is unset.
@example
home := sys.env("HOME")

#### `define run(command: String) -> Int`

Executes a shell command and returns the OS exit status (0 = success).
@example
if sys.run("git status") != 0 { print("not a repo") }

#### `define terminate(code: Int) -> void`

Immediately terminates the program with the given status code.
Equivalent to `exit(code)` but with a more descriptive name.


## `html/.venv/lib/quirk/stdlib/test/index.quirk`

### `struct TestCase`

A single named test. Construct with `TestCase("name", fn() { ... })`.
The lambda body uses `test.assert_*` helpers; any AssertionError fails
the case. Other exceptions also fail it (their type is shown).

#### `define run_all(cases: List) -> Int`

Run every case in `cases`, printing pass/fail marks and a final summary.
Returns the number of failures (0 == all green).


### Module-level functions

#### `define assert_eq(actual: Any, expected: Any, msg: String = "") -> void`

Asserts `actual == expected` by value. For primitive types and anything
with a `__str` method this matches stringified equality; for arbitrary
struct refs it amounts to pointer identity.

#### `define assert_ne(actual: Any, expected: Any, msg: String = "") -> void`

Asserts `actual != expected` by value.

#### `define assert_true(cond: Bool, msg: String = "") -> void`

Asserts `cond` is true.

#### `define assert_false(cond: Bool, msg: String = "") -> void`

Asserts `cond` is false.

#### `define assert_approx(actual: Double, expected: Double, tolerance: Double = 0.0001, msg: String = "") -> void`

Asserts `actual` and `expected` differ by at most `tolerance`. Use for
Doubles where exact equality is fragile.

#### `define assert_throws(cb: Callable, expected_type: String = "", msg: String = "") -> void`

Asserts `cb` throws an exception. If `expected_type` is non-empty, the
thrown exception's type must match (e.g. `"ValueError"`).

#### `define assert_contains(haystack: Any, needle: Any, msg: String = "") -> void`

Asserts `haystack` contains `needle` (substring for Strings, element for
Lists, or anything else with a `.contains` method).


## `html/.venv/lib/quirk/stdlib/time/index.quirk`


### Module-level functions

#### `extern define unix_now() -> Int`

Current Unix epoch in seconds. -1 if the host clock is unavailable.

#### `extern define year(epoch: Int, utc: Int) -> Int`

Year (e.g. 2026) for `epoch`. `utc=1` for UTC, `0` for local.

#### `extern define month(epoch: Int, utc: Int) -> Int`

Month 1-12 (1=January).

#### `extern define day(epoch: Int, utc: Int) -> Int`

Day of month, 1-31.

#### `extern define hour(epoch: Int, utc: Int) -> Int`

Hour 0-23.

#### `extern define minute(epoch: Int, utc: Int) -> Int`

Minute 0-59.

#### `extern define second(epoch: Int, utc: Int) -> Int`

Second 0-60 (60 for leap seconds, normally 0-59).

#### `extern define weekday(epoch: Int, utc: Int) -> Int`

Day of week, 0-6 (0=Sunday, 6=Saturday).

#### `extern define yearday(epoch: Int, utc: Int) -> Int`

Day of year, 1-366.

#### `extern define to_unix(year: Int, month: Int, day: Int, hour: Int, minute: Int, second: Int, utc: Int) -> Int`

Convert calendar components to a Unix epoch. `utc=1` interprets the inputs
as UTC; `utc=0` as the host's local time zone (DST handled).
Returns -1 if the components are out of range.

#### `extern define format_at(epoch: Int, fmt: String, utc: Int) -> String`

Format `epoch` with a strftime-style spec. Empty spec falls back to
`"%Y-%m-%dT%H:%M:%S"`.

#### `extern define iso_at(epoch: Int, utc: Int) -> String`

ISO-8601 form. UTC: `2026-01-15T12:34:56Z`. Local: `...+05:30` style offset.

#### `extern define parse_iso(s: String) -> Int`

Parse an ISO-8601 string. Trailing `Z` means UTC; absent → local. Returns
-1 on malformed input.


## `html/.venv/lib/quirk/stdlib/toml/index.quirk`

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


## `html/.venv/lib/quirk/stdlib/typing/callable.quirk`

### `struct Callable`

A first-class function value produced by a lambda expression (`fn(...) => ...`).
Callables can be stored in variables, passed as arguments, and invoked
by higher-order methods such as `List.map`, `List.filter`, and `List.each`.

@example:
double := fn(x: Int) => x * 2
print(double(21))                      // 42
evens := [1, 2, 3, 4].filter(fn(x: Int) => x % 2 == 0)

#### `extern define __str(self) -> String`

String representation of this callable.
@returns:
    String — always `"<Callable>"`


## `html/.venv/lib/quirk/stdlib/typing/collections/list.quirk`

### `struct List : Printable, Representable, Sizeable, Iterable`

A dynamic, ordered sequence of values of any type.
Elements are zero-indexed. Lists grow automatically as items are appended.

@example:
nums := [1, 2, 3]
nums.append(4)
print(nums.length())    // 4
print(nums[0])          // 1
doubled := nums.map(fn(x: Int) => x * 2)


## `html/.venv/lib/quirk/stdlib/typing/collections/map.quirk`

### `struct Map : Printable, Sizeable, Iterable`

A hash map (dictionary) that associates `String` keys with values of any type.
Insertion order is not guaranteed. All keys must be `String`.

@example:
m := {}
m.put("name", "Alice")
m.put("age", 30)
print(m.get("name"))    // "Alice"
print(m.has("age"))     // true
m.remove("age")
print(m.length())       // 1


## `html/.venv/lib/quirk/stdlib/typing/collections/queue.quirk`

### `struct Queue : Printable, Sizeable, Iterable`

A double-ended queue (deque) supporting O(1) push/pop at both ends.
Useful for BFS, sliding windows, and task scheduling.

@example:
q := Queue()
q.push_back(1)
q.push_back(2)
q.push_front(0)
print(q.pop_front())  // 0
print(q.peek_front()) // 1
print(q.size())       // 2


## `html/.venv/lib/quirk/stdlib/typing/collections/set.quirk`

### `struct Set : Printable, Sizeable, Iterable`

An unordered collection of unique values. Insertion order is preserved for
iteration. Values are compared by their string representation.

@example:
s := {1, 2, 3}
s.add(4)
print(s.has(2))      // true
s.remove(2)
print(s.size())      // 3
for v in s { print(v) }


## `html/.venv/lib/quirk/stdlib/typing/collections/tuple.quirk`

### `struct Tuple : Printable, Sizeable, Iterable`

An immutable, fixed-size ordered sequence of heterogeneous values.
Tuples are created with parenthesis syntax and support index access,
`length()`, and destructuring assignment.

A trailing comma is required for single-element tuples: `(x,)`.
An empty tuple is written as `()`.

@example:
coords := (10, 20)
print(coords[0])        // 10
print(coords.length())  // 2
x, y := coords          // destructuring
nested := ((1, 2), "outer")
print(nested[1])        // "outer"


## `html/.venv/lib/quirk/stdlib/typing/exceptions/base.quirk`

### `struct Exception`

Base class for all exceptions in Quirk.
Catch this type to handle any exception regardless of its specific kind.

Fields: message, type, file, line, traceback, cause_trace

@note Prefer catching a specific subclass (ValueError, IOError, etc.) over
catching Exception directly, so unrelated errors are not silently swallowed.

@warning Catching Exception will also catch RuntimeError, NullError, and all
other built-in exceptions — use with care in broad catch blocks.

@example:
try {
    throw ValueError("bad input")
} catch (e: Exception) {
    print(e.type + ": " + e.message)
}


## `html/.venv/lib/quirk/stdlib/typing/exceptions/types.quirk`

### `struct TypeError : Exception`

Raised when an operation is applied to an object of the wrong type.
Use this when a function receives an argument whose type is incompatible
with the expected type.

@note Prefer ValueError when the type is correct but the value is invalid.

@example:
define require_int(n: Any) -> void {
    if n.type != "Int" {
        throw TypeError("Expected Int, got " + n.type)
    }
}

### `struct ValueError : Exception`

Raised when an argument has the right type but an unacceptable value.
Parent of IndexError and KeyError — catch ValueError to handle both.

@example:
define set_age(age: Int) -> void {
    if age < 0 {
        throw ValueError("Age cannot be negative: " + age.str())
    }
}

### `struct IndexError : ValueError`

Raised when a list or string index is out of range.
Inherits from ValueError.

@note
- The runtime throws this automatically on invalid list access.
- Negative indices are supported: -1 refers to the last element.

@example:
items := [1, 2, 3]
try {
    _ := items[10]
} catch (e: IndexError) {
    print("Out of bounds: " + e.message)
}

### `struct KeyError : ValueError`

Raised when a map key does not exist.
Inherits from ValueError.

@note
- The runtime throws this automatically on missing key access.
- Use map.has(key) to check before accessing if you want to avoid the exception.

@example:
m := {"a": 1}
try {
    _ := m["missing"]
} catch (e: KeyError) {
    print("No such key: " + e.message)
}

### `struct IOError : Exception`

Raised for file and I/O operation failures such as permission errors,
read/write failures, or other OS-level I/O problems.
Parent of FileNotFoundError.

@example:
try {
    f := File("output.log", "w")
} catch (e: IOError) {
    print("I/O failed: " + e.message)
}

### `struct FileNotFoundError : IOError`

Raised when a file or directory cannot be found at the given path.
Inherits from IOError.

@note Catch IOError instead if you want to handle all I/O failures together.

@example:
try {
    f := File("config.json", "r")
} catch (e: FileNotFoundError) {
    print("Missing file: " + e.message)
}

### `struct RuntimeError : Exception`

Raised for generic runtime errors that don't fit a more specific category.
Parent of NotImplementedError and NullError.

@warning Avoid throwing RuntimeError directly when a more specific subclass applies.

@example:
define run(mode: String) -> void {
    if mode != "fast" and mode != "slow" {
        throw RuntimeError("Unknown mode: " + mode)
    }
}

### `struct NotImplementedError : RuntimeError`

Raised when a method that must be overridden is called without a concrete implementation.
Inherits from RuntimeError.

@note Use this as a sentinel in base struct methods to enforce that substructs override them.

@example:
struct Animal {
    define speak(self) -> void {
        throw NotImplementedError("speak() must be overridden")
    }
}

### `struct SocketError : Exception`

Raised for socket and network operation failures such as connection
refused, timeout, or DNS resolution errors.

@example:
try {
    conn := Socket.connect("localhost", 8080)
} catch (e: SocketError) {
    print("Connection failed: " + e.message)
}

### `struct ZeroDivisionError : Exception`

Raised when division or modulo by zero is attempted.

@example:
define divide(a: Int, b: Int) -> Int {
    if b == 0 {
        throw ZeroDivisionError("Cannot divide " + a.str() + " by zero")
    }
    return a / b
}

### `struct AssertionError : Exception`

Raised when an assertion fails.
Use this to enforce invariants or preconditions in your code.

@note Prefer specific exception types (ValueError, NullError, etc.) in library code so callers can catch them precisely.

@example:
define assert_positive(n: Int) -> void {
    if n <= 0 {
        throw AssertionError("Expected positive, got " + n.str())
    }
}

### `struct NullError : RuntimeError`

Raised when a null value is accessed where a non-null value is required. Inherits from RuntimeError.

@note Check for null explicitly before accessing fields or calling methods on optional values.

@example:
define get_name(user: Any) -> String {
    if user == null {
        throw NullError("user must not be null")
    }
    return user.name
}

### `struct WhereConditionError : RuntimeError`

Raised when a function's `where` precondition is violated at runtime.
Inherits from RuntimeError.

@example:
define sqrt(x: Double) -> Double where x >= 0.0 {
    return x
}

try {
    sqrt(-1.0)
} catch (e: WhereConditionError) {
    print("Precondition failed: " + e.message)
}


## `html/.venv/lib/quirk/stdlib/typing/interfaces/comparable.quirk`

### `interface Comparable : Equatable`

Implemented by types that support ordering and equality comparisons.
Required for use in sorted collections, binary search, and generic
functions that need to compare values.

@example
struct Temperature : Comparable {
    degrees: Double

    define __lt(self, other: Temperature) -> Bool {
        return self.degrees < other.degrees
    }

    define __eq(self, other: Temperature) -> Bool {
        return self.degrees == other.degrees
    }
}


### Module-level functions

#### `define __lt(self, other: Self) -> Bool`

Returns true if this value is strictly less than `other`.


## `html/.venv/lib/quirk/stdlib/typing/interfaces/equatable.quirk`

### `interface Equatable`

Implemented by types that support equality comparison.
All types that implement `Comparable` or `Hashable` extend `Equatable`.

@example
struct Color : Equatable {
    r: Int
    g: Int
    b: Int

    define __eq(self, other: Color) -> Bool {
        return self.r == other.r and self.g == other.g and self.b == other.b
    }
}


### Module-level functions

#### `define __eq(self, other: Self) -> Bool`

Returns true if this value is equal to `other`.


## `html/.venv/lib/quirk/stdlib/typing/interfaces/hashable.quirk`

### `interface Hashable : Equatable`

Implemented by types that can produce a stable integer hash of their value.
Required for use as keys in `Map` or elements in `Set`.
Any `Hashable` type must also be `Equatable`: two equal values must hash identically.

@example
struct Point : Hashable {
    x: Int
    y: Int

    define __eq(self, other: Point) -> Bool {
        return self.x == other.x and self.y == other.y
    }

    define __hash(self) -> Int {
        return self.x * 31 + self.y
    }
}


### Module-level functions

#### `define __hash(self) -> Int`

Returns a stable integer hash of this value.


## `html/.venv/lib/quirk/stdlib/typing/interfaces/iterable.quirk`

### `interface Iterable`

Implemented by types that can be traversed with a `for` loop.
`String`, `List`, `Map`, `Set`, `Queue`, and `Tuple` all implement `Iterable`.

@example
define print_all[T](col: T) where T: Iterable {
    for item in col {
        print(item)
    }
}


### Module-level functions

#### `define __iter(self) -> Iterator`

Returns an iterator over the elements of this collection.


## `html/.venv/lib/quirk/stdlib/typing/interfaces/iterator.quirk`

### `interface Iterator`

Protocol for types that produce values one at a time.
All built-in collection iterators implement `Iterator`.
Enables the `for` loop and any code that calls `__has_next` / `__next` directly.

@example
define collect[T](it: Iterator) -> List {
    result := []
    while it.__has_next() {
        result.append(it.__next())
    }
    return result
}


### Module-level functions

#### `define __has_next(self) -> Bool`

Returns true if there are more elements to yield.

#### `define __next(self) -> Any`

Returns the next element and advances the iterator.


## `html/.venv/lib/quirk/stdlib/typing/interfaces/parseable.quirk`

### `interface Parseable`

Implemented by types that can be constructed from a string representation.
Pairs naturally with `Printable` — a round-trip of `parse(val.str())` should
return the original value.

@example
n := Int.parse("42")     // 42
d := Double.parse("3.14") // 3.14
b := Bool.parse("true")   // true


### Module-level functions

#### `define parse(s: String) -> Self`

Parses `s` and returns the corresponding value.
@param s: String — the string to parse
@throws ValueError — if `s` is not a valid representation


## `html/.venv/lib/quirk/stdlib/typing/interfaces/primitive.quirk`

### `interface Primitive : Printable, Comparable`

Base interface for primitive value types.

All built-in scalar types (`Int`, `Double`, `Bool`, `String`) implement
`Primitive`. Extends both `Printable` and `Comparable`, so any `where T: Primitive`
constraint guarantees string conversion (`__str`) and ordering (`__lt`, `__eq`).

For types that can be constructed from a string, see `Parseable`.
For types usable as collection keys, see `Hashable`.

@example
struct Stack[T] where T: Primitive {
    items: List[T]
    ...
}

define echo[T](val: T) -> String where T: Primitive {
    return val.__str()
}


## `html/.venv/lib/quirk/stdlib/typing/interfaces/printable.quirk`

### `interface Printable`

Implemented by types that have a human-readable string representation.
Any struct implementing `Printable` can be passed directly to `print()`.

@example
struct Point : Printable {
    x: Int
    y: Int

    define __str(self) -> String {
        return "(" + self.x + ", " + self.y + ")"
    }
}

p := Point(3, 4)
print(p)   // (3, 4)


### Module-level functions

#### `define __str(self) -> String`

Returns a human-readable string representation of this value.


## `html/.venv/lib/quirk/stdlib/typing/interfaces/representable.quirk`

### `interface Representable`

Implemented by types that have a developer-facing debug representation,
distinct from the human-readable `__str`. Used by the REPL and logging tools.

@example
struct Token : Representable {
    kind: String
    value: String

    define __repr(self) -> String {
        return "Token(" + self.kind + ", " + self.value + ")"
    }
}


### Module-level functions

#### `define __repr(self) -> String`

Returns a developer-readable debug representation of this value.


## `html/.venv/lib/quirk/stdlib/typing/interfaces/serializable.quirk`

### `struct ISerializable`

Serialization interface for Quirk structs.

Inherit `ISerializable` and override `to_json()` to make any struct
serializable — including as the `json:` argument to `net.http` requests.

@example
struct User : ISerializable {
    name: String
    age: Int

    define __init(self, name: String, age: Int) -> void {
        self.name = name
        self.age = age
    }

    define to_json(self) -> String {
        return "{\"name\":\"" + self.name + "\",\"age\":" + self.age.str() + "}"
    }
}

user := User("Alice", 30)
res := http.post("http://api.example.com/users", json: user)

#### `define to_json(self) -> String`

Serialize this object to a JSON string.
Override this in every subclass.
@returns A valid JSON string representing this object.


## `html/.venv/lib/quirk/stdlib/typing/interfaces/sizeable.quirk`

### `interface Sizeable`

Implemented by types that have a finite, countable number of elements.
`String`, `List`, `Map`, `Set`, and `Queue` all implement `Sizeable`.

@example
define is_empty[T](col: T) -> Bool where T: Sizeable {
    return col.length() == 0
}


### Module-level functions

#### `define length(self) -> Int`

Returns the number of elements in this collection.


## `html/.venv/lib/quirk/stdlib/typing/option.quirk`

### `type Option[T] = Some(value: T) | None()`

#### `define is_some(self) -> Bool`

Returns true when the value is present.

#### `define is_none(self) -> Bool { return not self.is_some() }`

Returns true when the value is absent.

#### `define unwrap_or(self, default_value: T) -> T`

Returns the wrapped value if present, else `default_value`.

#### `define unwrap(self) -> T`

Returns the wrapped value if present; throws `NullError` if `None`.

#### `define unwrap_or_else(self, f: Callable) -> T`

Returns the wrapped value if present, else calls `f()` for a lazy default.

#### `define map(self, f: Callable) -> Option`

Maps `Some(v)` to `Some(f(v))`; `None` stays `None`.

#### `define and_then(self, f: Callable) -> Option`

Monadic bind: `f` must return an `Option`. Chains lookups: `cache.get(k).and_then(parse)`.

#### `define or_else(self, f: Callable) -> Option`

Returns `self` if `Some`, else calls `f()` for an alternative `Option`.

#### `define filter(self, pred: Callable) -> Option`

Keeps `Some(v)` only if `pred(v)` is true; otherwise `None`.

#### `define ok_or(self, err: Any) -> Result`

Converts to `Result`: `Some(v)` → `Ok(v)`; `None` → `Err(err)`.

### `variant Some of Option`

### `variant None of Option`


## `html/.venv/lib/quirk/stdlib/typing/primitives/bool.quirk`

### `struct Bool : Primitive, Comparable, Parseable`

Boolean type. Only two values: `true` and `false`.

#### `extern define parse(s: String) -> Bool`

Parses `"true"` or `"false"`.
@param s: String
@returns: Bool
@throws ValueError — if `s` is not `"true"` or `"false"`


## `html/.venv/lib/quirk/stdlib/typing/primitives/double.quirk`

### `struct Double : Primitive, Comparable, Parseable`

64-bit IEEE 754 floating-point number.

@example:
x := 3.14
print(x.ceil())    // 4.0
print(x.str())     // "3.14"

#### `extern define sqrt(self) -> Double`

Returns the square root.
@returns: Double
@throws ValueError — if the value is negative

#### `extern define parse(s: String) -> Double`

Parses a string as a floating-point number.
@param s: String
@returns: Double
@throws ValueError — if `s` is not a valid number


## `html/.venv/lib/quirk/stdlib/typing/primitives/int.quirk`

### `struct Int : Primitive, Comparable, Parseable`

32-bit signed integer.
Supports arithmetic operators (`+`, `-`, `*`, `/`, `%`, `**`) and comparisons.

@example:
n := 42
print(n.str())       // "42"
print(n.is_even())   // true
print(n.pow(2))      // 1764

#### `extern define str(self) -> String`

Returns the string representation of this integer.
@returns: String

#### `extern define abs(self) -> Int`

Returns the absolute value.
@returns: Int

#### `extern define pow(self, exp: Int) -> Int`

Returns `self` raised to the power `exp`.
@param exp: Int — exponent (must be ≥ 0)
@returns: Int

#### `extern define parse(s: String) -> Int`

Parses a string as an integer.
@param s: String
@returns: Int
@throws ValueError — if `s` is not a valid integer


## `html/.venv/lib/quirk/stdlib/typing/primitives/string.quirk`

### `struct String : Primitive, Comparable, Sizeable, Iterable, Representable`

An immutable UTF-8 string. String literals use double or single quotes.
Supports interpolation with `"Hello ${name}"`, iteration over characters,
and a rich set of transformation and query methods.

@example:
s := "Hello, world!"
print(s.upper())            // "HELLO, WORLD!"
print(s.contains("world")) // true
parts := s.split(", ")
print(parts.length())       // 2


## `html/.venv/lib/quirk/stdlib/typing/result.quirk`

### `type Result[T, E] = Ok(value: T) | Err(error: E)`

#### `define is_ok(self) -> Bool`

Returns true when the result is `Ok`.

#### `define is_err(self) -> Bool { return not self.is_ok() }`

Returns true when the result is `Err`.

#### `define unwrap_or(self, default_value: T) -> T`

Returns the success value if `Ok`, else `default_value`.

#### `define unwrap(self) -> T`

Returns the success value if `Ok`; throws `ValueError` if `Err`.

#### `define unwrap_err(self) -> E`

Returns the error payload if `Err`; throws `ValueError` if `Ok`.

#### `define unwrap_or_else(self, f: Callable) -> T`

Returns the success value if `Ok`, else calls `f(error)` for a lazy default.

#### `define map(self, f: Callable) -> Result`

Maps `Ok(v)` to `Ok(f(v))`; `Err(e)` stays `Err(e)`.

#### `define map_err(self, f: Callable) -> Result`

Maps `Err(e)` to `Err(f(e))`; `Ok(v)` stays `Ok(v)`.

#### `define and_then(self, f: Callable) -> Result`

Monadic bind: `f` must return a `Result`. Chains fallible steps.

#### `define or_else(self, f: Callable) -> Result`

Returns `self` if `Ok`, else calls `f(error)` for recovery.

#### `define ok(self) -> Option`

Converts to `Option`: `Ok(v)` → `Some(v)`; `Err(_)` → `None`.

#### `define err(self) -> Option`

Converts to `Option` of the error: `Err(e)` → `Some(e)`; `Ok(_)` → `None`.

### `variant Ok of Result`

### `variant Err of Result`


## `html/.venv/lib/quirk/stdlib/url/index.quirk`

### `struct URL`

Parsed URL components. All fields are present (empty strings / 0 when the
URL omits them) so callers don't need null-checks.

  scheme:   `"https"`, `"http"`, `"file"`, `"ftp"`, ...
  host:     hostname or IP literal (no port)
  port:     port number, or 0 if absent
  path:     `/path` portion (may be empty)
  query:    raw query without the leading `?`
  fragment: raw fragment without the leading `#`

#### `define to_string(self) -> String`

Recompose into a canonical URL string.

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


## `html/.venv/lib/quirk/stdlib/uuid/index.quirk`


### Module-level functions

#### `define v4() -> String`

Generate a random RFC 4122 v4 UUID. Two calls back-to-back return
different values (with overwhelming probability — 122 random bits of
entropy each).

@example
id := uuid.v4()
print(id.length())           // 36

#### `define nil() -> String`

The all-zero "nil" UUID — useful as a sentinel for "no UUID yet" in
schemas that disallow nulls.

#### `define is_valid(s: String) -> Bool`

Lightweight format check. Returns true iff `s` matches the canonical
36-char shape with dashes at positions 8 / 13 / 18 / 23 and hex
characters everywhere else. The version / variant nibbles aren't
enforced — anything you parse cleanly via `v4()` will round-trip.

@example
uuid.is_valid("550e8400-e29b-41d4-a716-446655440000")   // true
uuid.is_valid("not-a-uuid")                             // false
uuid.is_valid("550E8400-E29B-41D4-A716-446655440000")   // true (case-insensitive)
