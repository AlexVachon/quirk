# `console` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `console/index.quirk`


### Module-level functions

#### `define put(msg: Any) -> void`

Write `msg` to stdout WITHOUT a trailing newline. Available as both
`console.put` and `console.write` (the latter shadows the top-level
`write` builtin only in the `console.` namespace).
@example
console.put("Loading")
for i in 0..3 { console.put(".") }
console.log("done")

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
