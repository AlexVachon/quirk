# `process` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `process/index.quirk`

### `struct ProcessError : RuntimeError`

Thrown by `run(check=true)`, `shell(check=true)` and `output` when the
program exits with a non-zero code. The message names the command, the
exit code and the first line of its stderr.

#### `define check(self) -> void`

Throw `ProcessError` unless the program exited with code 0.

#### `define write(self, data: String) -> void`

Write `data` to the program's stdin. Throws `IOError` if stdin isn't a pipe or the program closed it.

#### `define close_stdin(self) -> void`

Close the program's stdin, so it sees end of input.

#### `define read(self, size: Int = 65536) -> String`

Up to `size` bytes of stdout, waiting until some arrive. "" once it's all been read.

#### `define read_line(self) -> String`

The next line of stdout, without its newline; null at the end of output.

#### `define lines(self) -> List`

Every remaining line of stdout.

#### `define read_err(self, size: Int = 65536) -> String`

Up to `size` bytes of stderr; "" once it's all been read.

#### `define communicate(self, input: String = "", timeout_ms: Int = -1) -> RunResult`

Send `input` to stdin (then close it), read stdout and stderr to the
end and wait for the program — all at once, so it can't stall on a
full pipe. With `timeout_ms`, the program is killed when it runs out.

#### `define wait(self) -> Int`

Wait for the program to finish; returns its exit code (`-N`: killed by signal N).

#### `define wait_for(self, timeout_ms: Int) -> Bool`

Wait up to `timeout_ms`; true if it finished (its code is then in `code`).

#### `define done(self) -> Bool`

True once the program has finished. Doesn't wait.

#### `define signal(self, sig: Int) -> void`

Send signal `sig` (15 = SIGTERM, 9 = SIGKILL, 2 = SIGINT, ...). No-op once it has finished.

#### `define terminate(self) -> void`

Ask it to stop (SIGTERM).

#### `define kill(self) -> void`

Stop it now (SIGKILL) and wait for it.

#### `define spawn(args: List, cwd: String = "", env: Map = Map(), inherit_env: Bool = true, stdin: String = "pipe", stdout: String = "pipe", stderr: String = "pipe") -> Process`

Start a program and return without waiting for it.

`stdin` / `stdout` / `stderr` each say where that stream goes:
`"pipe"` (default — use `write` / `read` / `read_line` / `communicate`),
`"inherit"` (this process's terminal), `"null"` (discard / empty input);
`stderr="stdout"` sends stderr into the stdout pipe.

Read its output to the end (or `communicate`) before `wait`ing: a
program that fills an unread pipe stops until someone reads it.
@throws FileNotFoundError when the program can't be found.
@example
p := process.spawn(["tail", "-f", "app.log"], stderr="null")
for i in 0..5 { print(p.read_line()) }
p.kill()

#### `define run(args: List, input: String = "", cwd: String = "", env: Map = Map(), inherit_env: Bool = true, timeout_ms: Int = -1, check: Bool = false, capture: Bool = true, merge_stderr: Bool = false) -> RunResult`

Run a program to completion and return its `RunResult`.

`input` is sent to its stdin (otherwise stdin is this process's).
stdout and stderr are captured unless `capture=false` (then they go to
this process's terminal); `merge_stderr=true` captures both, interleaved,
in `stdout`. `timeout_ms` kills it when time runs out; `check=true`
throws `ProcessError` on a non-zero exit.
@throws FileNotFoundError when the program can't be found.
@example
r := process.run(["ls", "-1"], cwd="/tmp")
for name in r.stdout.split("\n") { print(name) }
@example
process.run(["make", "test"], capture=false, check=true)

#### `define shell(command: String, input: String = "", cwd: String = "", env: Map = Map(), inherit_env: Bool = true, timeout_ms: Int = -1, check: Bool = false, capture: Bool = true, merge_stderr: Bool = false) -> RunResult`

`run` through the shell: `/bin/sh -c command`, so pipes, globs,
redirects and `$VARS` work. Only for commands you wrote — never paste
untrusted input into one (use `run` with an args List instead).
@example
r := process.shell("ls *.qk | wc -l")
print(r.stdout.trim())

#### `define output(args: List, cwd: String = "", env: Map = Map(), timeout_ms: Int = -1) -> String`

Run a program and return its stdout, with one trailing newline removed.
Throws `ProcessError` when it exits with a non-zero code.
@example
sha := process.output(["git", "rev-parse", "HEAD"])

### `struct RunResult`

A finished program.
  `args`       — what was run
  `code`       — exit code; `-N` when it was killed by signal N
  `stdout`     — everything it wrote to stdout ("" when not captured)
  `stderr`     — everything it wrote to stderr ("" when not captured or
                 merged into stdout)
  `timed_out`  — killed because `timeout_ms` passed
  `ok`         — `code == 0`

### `struct Process`

A running program, from `process.spawn`.
  `pid`   — its process id
  `args`  — what was run
  `code`  — exit code once it has finished (after `wait`, `done` or
            `communicate`); -1 before


### Module-level functions

#### `extern define pid() -> Int`

This process's id.
