# `pty` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `pty/index.quirk`

### `struct PtyError : RuntimeError`

Thrown when a program can't be started in a pseudo-terminal.

### `struct Pty`

A program running in a pseudo-terminal (from `pty.spawn`). `exit_code`
is set once it has ended (-N: killed by signal N).

#### `define read(self, timeout_ms: Int = 0) -> String`

What it wrote since the last read, waiting up to `timeout_ms` for
something (0: don't wait; -1: until there is). "" when nothing came —
or when its output has ended (`ended()`).

#### `define ended(self) -> Bool`

True once everything it will ever write has been read.

#### `define write(self, text: String) -> void`

Type `text` at it: "\r" is Enter, "\x03" Ctrl+C, "\x1b[A" the Up key.

#### `define resize(self, cols: Int, rows: Int) -> void`

Its window is now `cols` x `rows` characters (it redraws to fit).

#### `define alive(self) -> Bool`

True while the program runs.

#### `define wait(self, timeout_ms: Int = -1) -> Int`

Wait for it to end (up to `timeout_ms`, -1: no limit); its exit code, or -1000 if still running.

#### `define interrupt(self) -> void`

Interrupt it, as Ctrl+C would.

#### `define kill(self) -> void`

End it now (SIGKILL; TerminateProcess on Windows).

#### `define close(self) -> void`

Close the terminal: the program is told (SIGHUP) or ended.


### Module-level functions

#### `define available() -> Bool`

True where pseudo-terminals work (everywhere but Windows before 10 1809).

#### `define shell() -> List`

The user's shell, as `spawn` args: $SHELL (else /bin/sh) on Linux and
macOS; on Windows PowerShell 7 (`pwsh`) if it's installed, else Windows
PowerShell.

#### `define spawn(args: List, cols: Int = 80, rows: Int = 24, cwd: String = "", env: Map = {}) -> Pty`

Start `args` (program and arguments, looked up on PATH) in a new
pseudo-terminal `cols` x `rows` characters.
@param cwd  directory to run in ("" = this one)
@param env  variables added to its environment; TERM is xterm-256color
            unless given
@throws PtyError when it can't be started (no such program, no ConPTY).
@example p := pty.spawn(["bash", "-l"], cols=120, rows=30, cwd="/tmp")
