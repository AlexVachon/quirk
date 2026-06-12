# `sys` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `sys/index.quirk`


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
