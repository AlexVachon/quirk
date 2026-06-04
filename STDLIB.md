# Quirk Standard Library

Every module shipped with the compiler. Already importable — no install
needed. To see methods on a builtin type (`String`, `List`, `Map`, etc.),
peek at [`packages/typing/`](quirk-compiler/packages/typing/).

## Quick reference

| Module | What it does |
|--------|--------------|
| [`typing`](#typing) | Built-in types (auto-imported as the prelude) |
| [`console`](#console) | Colored logging, prompts, progress |
| [`math`](#math) | Trig, exponentials, random |
| [`fs`](#fs) | File and directory I/O |
| [`io`](#io) | `File` struct and reader/writer abstractions |
| [`time`](#time) | Now, formatting, durations, calendar arithmetic |
| [`regex`](#regex) | POSIX-style pattern matching |
| [`csv`](#csv) | Parse / write delimited records |
| [`encoding`](#encoding) | `json`, `hex`, `base64` |
| [`net`](#net) | Sockets + `net.http` client |
| [`crypto`](#crypto) | Hashes, HMAC, UUID, random bytes |
| [`url`](#url) | Parse URLs, percent-encode/decode, query strings |
| [`argparse`](#argparse) | CLI argument parser |
| [`sys`](#sys) | Process: argv, env, exit, sleep, system() |
| [`test`](#test) | Unit-test runner + assertions |

---

## `typing`

The prelude — every Quirk source file gets these for free. You don't write `use typing`.

**Primitives**: `Int`, `Double`, `Bool`, `String`
**Collections**: `List`, `Map`, `Set`, `Queue`, `Tuple`
**Exceptions**: `Exception`, `TypeError`, `ValueError`, `IndexError`, `KeyError`, `IOError`, `RuntimeError`, …
**Interfaces**: `Printable`, `Comparable`, `Hashable`, `Iterable`, `Iterator`, `Primitive`, `Representable`
**Other**: `Callable` (function values), `File` (file handles)

```quirk
nums := [1, 2, 3]
print(nums.length())            // 3
print(nums.map(fn(x) => x * 2)) // [2, 4, 6]

m := {"a": 1, "b": 2}
for (k, v) in m { print(k, "=", v) }
```

See [`packages/typing/`](quirk-compiler/packages/typing/) for every method.

---

## `console`

```quirk
use console
```

| Function | Purpose |
|----------|---------|
| `log(...args)` | Print to stdout |
| `info(...args)` / `warn(...args)` / `error(...args)` | Same, with cyan / yellow / red tint |
| `debug(...args)` | Dim print to stderr (toggleable via `console.set_debug`) |
| `success(...args)` / `green` / `red` / `yellow` / `blue` | Pre-tinted variants |
| `put(s)` | Write `s` to stdout without a newline |
| `confirm(prompt) -> Bool` | Y/N prompt |
| `input(prompt) -> String` | Read a line |
| `password(prompt) -> String` | Read a line without echoing |
| `read_key() -> Int` | One keypress (raw mode) |
| `select(prompt, options)` | Interactive menu |
| `assert(cond, msg)` | Print + exit if `cond` is false |
| `beep()` | Terminal bell |
| `group(label) / group_end()` | Indent nested logs |
| `time(label) / time_end(label)` | Measure elapsed time |
| `progress(current, total)` | Single-line progress bar |
| `clear_line()` / `size() -> (rows, cols)` | Terminal helpers |

```quirk
use console
define main() -> void {
    console.info("Starting…")
    name := console.input("Your name: ")
    console.success("Hi, " + name + "!")
    if console.confirm("Continue?") { console.green("ok") }
}
```

---

## `math`

```quirk
use math
```

| Function | |
|----------|--|
| `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2` | Trig (radians) |
| `sinh`, `cosh`, `tanh` | Hyperbolic |
| `sqrt`, `cbrt`, `pow(base, exp)`, `exp(x)`, `log`, `log10`, `log2` | Powers / logs |
| `floor`, `ceil`, `round`, `trunc`, `abs` | Rounding |
| `random() -> Double` | 0.0 .. 1.0 |
| `random_int(min, max) -> Int` | inclusive |
| `random_seed(n)` | Seed the xorshift PRNG |
| `pi`, `e`, `tau`, `inf`, `nan` | Constants |

```quirk
use math
define main() -> void {
    print(math.sqrt(2.0))               // 1.41421356
    print(math.sin(math.pi / 2.0))      // 1.0
    print(math.random_int(1, 6))        // dice roll
}
```

---

## `fs`

```quirk
use fs
```

| Function | Purpose |
|----------|---------|
| `exists(path) -> Bool` | |
| `is_file(path) -> Bool` / `is_dir(path) -> Bool` | |
| `size(path) -> Int` / `mtime(path) -> Int` | Bytes / epoch seconds |
| `mkdir(path, parents=false)` | `parents=true` ≈ `mkdir -p` |
| `rmdir(path)` | Empty dir only |
| `remove(path)` / `remove_all(path)` | File / recursive |
| `list_dir(path) -> List` | Entry names |
| `read(path) -> String` / `write(path, content)` | Bulk file I/O |
| `read_lines(path) -> List` | Lines without trailing `\n` |
| `copy(src, dst)` / `move(src, dst)` | |
| `cwd() -> String` / `chdir(path)` | |
| `join(parts)` / `dirname(p)` / `basename(p)` / `extension(p)` | Path manipulation |

```quirk
use fs
define main() -> void {
    if not fs.exists("data") { fs.mkdir("data") }
    fs.write("data/hello.txt", "world\n")
    for line in fs.read_lines("data/hello.txt") { print(line) }
}
```

---

## `io`

```quirk
from io use { File }
```

`File` is the underlying reader/writer used by `fs`, but exposes a streaming API for cases where you don't want to slurp the whole file:

```quirk
from io use { File }
define main() -> void {
    f := File.open("big.log", "r")
    for line in f { print(line) }
    f.close()
}
```

| Method | Purpose |
|--------|---------|
| `File.open(path, mode) -> File` | `mode`: `"r"`, `"w"`, `"a"`, `"r+"`, … |
| `f.read() -> String` | Whole content |
| `f.read_line() -> String` | One line; empty at EOF |
| `f.write(s)` / `f.writeln(s)` | |
| `f.close()` | (deferred via `with` block too) |
| `for line in f { … }` | Line iteration |

---

## `time`

```quirk
use time
```

| Function | |
|----------|--|
| `now() -> DateTime` | UTC now |
| `unix_now() -> Int` | Seconds since epoch |
| `monotonic_ms() -> Int` | Monotonic, for timing |
| `DateTime(epoch, utc=true)` | Construct from epoch |
| `dt.year() / .month() / .day() / .hour() / .minute() / .second() / .weekday()` | Components |
| `dt.format(fmt) -> String` | strftime-style |
| `dt.add_days(n)`, `.add_hours(n)`, … | Calendar arithmetic |
| `time.sleep_ms(ms)` | Block |

```quirk
use time
define main() -> void {
    t := time.now()
    print(t.format("%Y-%m-%d %H:%M"))
}
```

---

## `regex`

```quirk
use regex
```

POSIX extended regex (`grep -E`).

| Method | |
|--------|--|
| `regex.compile(pat, flags="") -> Regex` | `flags`: `i` (icase), `m` (multiline) |
| `r.test(s) -> Bool` | Any match? |
| `r.match(s) -> Match` | First match (or null) |
| `r.find_all(s) -> List` | Every match |
| `r.replace_all(s, repl) -> String` | |
| `r.split(s) -> List` | |
| `match.group(idx)` / `.start()` / `.end()` | Capture access |

```quirk
use regex
define main() -> void {
    r := regex.compile("(\\w+)@(\\w+\\.\\w+)")
    if r.test("contact alex@example.com") { print("found") }
    for m in r.find_all("a@b.c d@e.f") { print(m.group(0)) }
}
```

---

## `csv`

```quirk
use csv
```

| Function | |
|----------|--|
| `parse(text, delim=",") -> List` | Each row a `List` of fields |
| `parse_dicts(text, delim=",") -> List` | Each row a `Map` (first row = headers) |
| `write(records, delim=",") -> String` / `write_dicts(records, delim=",")` | |
| `read_file(path, delim=",") -> List` | Convenience |
| `write_file(path, records, delim=",")` | |

```quirk
use csv
define main() -> void {
    rows := csv.parse_dicts("name,age\nAlice,30\nBob,25")
    for r in rows { print(r.get("name"), r.get("age")) }
}
```

---

## `encoding`

```quirk
use encoding.json
use encoding.hex
use encoding.base64
```

**`encoding.json`** — JSON parser + serializer:

| Function | |
|----------|--|
| `json.parse(s) -> Any` | Returns `JsonMap` / `JsonList` / primitives |
| `json.dumps(val) -> String` | Stringify |
| `json.load(file) -> Any` / `json.dump(val, file)` | Stream variants |

```quirk
use encoding.json
define main() -> void {
    data := json.parse('{"name": "Alex", "age": 30}')
    print(data.get("name"))
    print(json.dumps({"x": 1, "y": [1, 2, 3]}))
}
```

**`encoding.hex`**: `hex.encode(s) -> String`, `hex.decode(s) -> String`
**`encoding.base64`**: `base64.encode(s)`, `base64.decode(s)`

---

## `net`

```quirk
use net.http
```

HTTP client (via libcurl underneath):

| Function | |
|----------|--|
| `http.get(url, headers={}) -> Response` | |
| `http.post(url, body, headers={}) -> Response` | |
| `http.put / .patch / .delete / .head` | Same shape |
| `http.request(method, url, body, headers)` | Generic |
| `resp.status -> Int` / `resp.body -> String` / `resp.headers -> Map` | |

```quirk
use net.http
define main() -> void {
    r := http.get("https://httpbin.org/get")
    print(r.status, r.body)
}
```

**Raw sockets** (lower level): `net.socket()`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`, `close`. Build your own server here.

---

## `crypto`

```quirk
use crypto
```

| Function | |
|----------|--|
| `crypto.md5(s)` / `.sha1` / `.sha256` / `.sha512` | Hex digest |
| `crypto.hmac_sha256(key, msg)` | |
| `crypto.random_hex(n)` | `n` random bytes, hex-encoded |
| `crypto.uuid()` | RFC 4122 v4 |

```quirk
use crypto
define main() -> void {
    print(crypto.sha256("hello"))     // 2cf24dba…
    print(crypto.uuid())              // 7b8a4c…
}
```

---

## `url`

```quirk
use url
```

| Function | |
|----------|--|
| `url.quote(s) -> String` / `url.unquote(s) -> String` | Percent encode/decode |
| `url.unquote_form(s)` | `+` → space variant (HTML forms) |
| `url.parse_query(q) -> Map` | `?a=1&b=2` → `{a:"1", b:"2"}` |
| `url.build_query(m) -> String` | inverse |
| `url.parse(u) -> URL` | scheme, host, port, path, query, fragment |

```quirk
use url
define main() -> void {
    print(url.quote("Hello, world!"))    // Hello%2C%20world%21
    q := url.parse_query("name=alex&age=30")
    print(q.get("name"))
}
```

---

## `argparse`

```quirk
use argparse
```

| Function | |
|----------|--|
| `argparse.parser(name, description) -> Parser` | |
| `p.positional(name)` / `p.option(name, short, default)` / `p.flag(name, short)` | Declare |
| `p.parse(argv) -> Map` | Returns name → value |
| `argparse.flag(args, name) -> Bool` / `.get(args, name)` | Reader helpers |

```quirk
use argparse
define main() -> void {
    p := argparse.parser("greet", "Say hello")
    p.positional("name")
    p.flag("loud", "l")
    args := p.parse(sys.argv())
    msg := "Hello, " + args.get("name")
    if argparse.flag(args, "loud") { msg = msg.upper() }
    print(msg)
}
```

---

## `sys`

```quirk
use sys
```

| Function | |
|----------|--|
| `sys.argv() -> List` | Script arguments |
| `sys.getenv(name) -> String` | "" if unset |
| `sys.exit(code)` | |
| `sys.sleep(ms)` | |
| `sys.system(cmd) -> Int` | Shell out (exit code) |
| `sys.run(cmd) -> String` | Shell out, return stdout |
| `sys.platform() -> String` | `"linux"`, `"darwin"`, … |

```quirk
use sys
define main() -> void {
    for arg in sys.argv() { print(arg) }
    home := sys.getenv("HOME")
    if home == "" { sys.exit(1) }
}
```

---

## `test`

```quirk
use test
from test use { TestCase }
```

| Function | |
|----------|--|
| `TestCase(name, fn)` | Constructor for a test case |
| `test.run_all(cases) -> Int` | Returns failure count |
| `test.assert_eq(actual, expected)` | |
| `test.assert_ne(actual, expected)` | |
| `test.assert_true(cond)` / `assert_false(cond)` | |
| `test.assert_approx(actual, expected, eps)` | Doubles |
| `test.assert_throws(fn)` | Function should throw |
| `test.assert_contains(haystack, needle)` | Substring or list element |

```quirk
use test
from test use { TestCase }

define main() -> void {
    t1 := TestCase("addition", fn() { test.assert_eq(2 + 2, 4) })
    t2 := TestCase("string concat", fn() {
        test.assert_eq("a" + "b", "ab")
    })
    test.run_all([t1, t2])
}
```

---

## What's NOT in stdlib (yet)

Things you might reasonably expect that *aren't* shipped — these are third-party
territory:

- **Full TOML parser** (only the `quirk.toml` subset is parsed internally)
- **YAML parser**
- **SQLite / database drivers**
- **Markdown → HTML**
- **Template engines** (Jinja-style, etc.)
- **Web framework** (Express-/Flask-style routing)
- **WebSocket client**

Browse the [registry](https://quirk-pkg.github.io/registry/) when you need
something like these, or write one and PR an entry.
