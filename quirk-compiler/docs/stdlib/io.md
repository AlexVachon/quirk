# `io` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `io/file.quirk`

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
