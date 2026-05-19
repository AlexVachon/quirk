# Quirk CLI reference

The `quirk` binary is both the compiler/runner and the package manager. This page documents every command.

## Quick reference

| Command | Aliases | What it does |
|---------|---------|--------------|
| [`quirk run <file>`](#quirk-run) | `quirk <file>` | Run a Quirk script |
| [`quirk eval "<code>"`](#quirk-eval) | `-c` | Run an inline one-liner |
| [`quirk module <name>`](#quirk-module) | `-m` | Run an installed module's `main()` |
| [`quirk new <name>`](#quirk-new) | — | Scaffold a new package |
| [`quirk init`](#quirk-init) | — | Write a `quirk.toml` in the current dir |
| [`quirk venv <name>`](#quirk-venv) | — | Create an isolated environment |
| [`quirk env`](#quirk-env) | — | Show the active resolution context |
| [`quirk fmt [files]`](#quirk-fmt) | `--check`, `--stdout` | Reformat source files to canonical style |
| [`quirk i <spec>`](#quirk-install) | `quirk add`, `install`, `pkg install` | Install dependencies |
| [`quirk up [pkg ...]`](#quirk-upgrade) | `upgrade`, `pkg upgrade` | Bump installed versions |
| [`quirk rm <pkg>`](#quirk-remove) | `un`, `uninstall`, `remove`, `pkg remove` | Uninstall a package or one version |
| [`quirk ls`](#quirk-list) | `list`, `packages`, `-p`, `pkg -l`/`--list` | List installed packages |
| [`quirk show <pkg>`](#quirk-show) | `pkg show` | Detailed info on one package |
| [`quirk deps`](#quirk-deps) | `pkg deps` | Print installed packages in deps format |
| [`quirk cache`](#quirk-pkg-cache) | `pkg cache`, `cache -l`/`--list` | Manage the cross-project version cache |
| [`quirk registry`](#quirk-pkg-registry) | `pkg registry`, `registry -l`/`--list` | Manage name → URL mappings |
| [`quirk run --list`](#quirk-run) | `quirk run -l` | List named scripts from `[scripts]` |
| [`quirk help [cmd]`](#quirk-help) | `--help`, `-h` | Show overall or per-command help |
| [`quirk version`](#quirk-version) | `--version` | Print the compiler version |

## Common flags (for `run` / bare `quirk <file>`)

| Flag | Effect |
|------|--------|
| `-v` | Verbose: print debug info during compilation |
| `--check` | Type-check only, no codegen or run |
| `-o <out>` | Compile to a native binary at `<out>` (no run) |
| `--compile-only` | Compile + emit IR but don't run |
| `--emit-ir` | Write LLVM IR to `<basename>.ll` |
| `--emit-ast` | Write AST dump to `<basename>.ast.log` |
| `-O0` / `-O1` / `-O2` / `-O3` | LLVM optimization level (default `-O2`) |
| `--release` | Alias for `-O2` (kept for parity with future O3 modes) |

**Optimization defaults.** Quirk currently defaults to `-O2`. The IR the codegen emits is "loose" — it depends on the O2 pass pipeline (`mem2reg`, `instcombine`, `dce`) to be safe at the JIT level. Lowering below O2 segfaults some programs today; a future codegen cleanup will let `-O0` be a safe fast-iteration mode.

Anything after the script path is forwarded as script `argv`:

```bash
quirk run script.quirk --thing 1 -- foo bar
# script sees argv == ["script.quirk", "--thing", "1", "--", "foo", "bar"]
```

---

## Running code

### `quirk run`

Synopsis: `quirk run <file.quirk> [args...]`

Run a Quirk script. Equivalent to `quirk <file.quirk>` (the bare form is preserved for backwards-compat — `run` is the explicit verb). Use this when the filename could be confused with a subcommand (e.g. you have a file literally named `install.quirk`).

```bash
quirk run examples/hello.quirk
quirk run -v tests/parser_test.quirk
quirk run --check src/index.quirk
```

### `quirk eval`

Synopsis: `quirk eval "<code>"` &nbsp;·&nbsp; alias: `quirk -c "<code>"`

Wrap a one-liner in `define main() { ... }` and run it. The string is forwarded verbatim, so you can use semicolons and newlines.

```bash
quirk eval 'print("hello")'
quirk eval 'a := 2 + 2; print(a)'
quirk -c 'use math; print(math.sqrt(2.0))'
```

Notes:

- Imports work (`use math`, `use slug`, etc.).
- Multi-line code is supported but bash quoting can get tricky — for anything substantial, write a temp `.quirk` file.

### `quirk module`

Synopsis: `quirk module <name> [args...]` &nbsp;·&nbsp; alias: `quirk -m <name> [args...]`

Resolve `<name>` to its `src/index.quirk` (or the appropriate entry point — see [resolution order](#resolution-order)) and run it directly. The module is expected to define `main()`; if it doesn't, the compiler errors.

```bash
quirk module my-lib
quirk -m my-lib --debug some-arg
```

Mirrors `python -m <module>`. Arguments after the module name are forwarded to its `main()` via `sys.argv()`.

---

## Project & environment

### `quirk new`

Synopsis: `quirk new <name>`

Scaffold a new Quirk package:

```
<name>/
├── quirk.toml
├── src/
│   └── index.quirk        # define main() { print(hello("world")) }
├── tests/
│   └── <name>_test.quirk
└── .gitignore
```

```bash
quirk new my-lib
cd my-lib
quirk run src/index.quirk
# Hello, world!
```

Like `cargo new` / `npm init -y` — fastest way to start a publishable package.

### `quirk init`

Synopsis: `quirk init`

Write a `quirk.toml` in the current directory using the dir name as the package name. Use this when you want to retroactively make an existing directory a Quirk package (e.g. to start using `quirk install`).

```bash
mkdir my-app && cd my-app
quirk init
# Created quirk.toml for project 'my-app'.
```

### `quirk venv`

Synopsis: `quirk venv <dir>`

Create an isolated environment at `./<dir>/`. Layout:

```
<dir>/
├── bin/
│   ├── activate          # source from bash/zsh
│   ├── quirk             # → symlink to system quirk
│   └── runtime.so        # → symlink to runtime
├── lib/quirk/
│   ├── stdlib/           # symlinks to system stdlib modules
│   └── packages/         # `quirk install` lands here when activated
└── quirk.toml
```

```bash
quirk venv .venv
source .venv/bin/activate
(quirk:.venv) $ quirk install github.com/foo/bar@v0.1.0
```

The `(quirk:.venv)` prompt prefix is set by the `activate` script. Run `deactivate` to leave.

### `quirk env`

Synopsis: `quirk env`

Print the current resolution context. Use this to debug "why isn't this package found?" issues.

```bash
$ quirk env
quirk:           /usr/local/bin/quirk
version:         0.2.0
QUIRK_HOME:      /home/alex/proj/.venv
in venv:         yes
project root:    /home/alex/proj
install dir:     /home/alex/proj/.venv/lib/quirk/packages
stdlib:          /usr/local/lib/quirk
user-global:     /home/alex/.quirk/packages
```

---

### `quirk fmt`

Synopsis: `quirk fmt [--check|--stdout] [<file> ...]`

Reformat Quirk source files to a canonical style. Operates in place by default; with no files, walks the current directory and formats every `.quirk` (skipping `packages/`, `.venv/`, `.git/`).

The formatter applies these rules:

- **Indentation**: 4 spaces per level, derived from `{` / `[` brace depth (strings and comments are ignored when counting).
- **Operator spacing**: single space around `:=`, `->`, `==`, `!=`, `<=`, `>=`, `+`, `-`, `*`, `/`. One space after `:` (but not before, and not for `:=`).
- **Separator spacing**: one space after `,`, none before.
- **Brace spacing**: one space before `{` (and around, when on the same line).
- **Trailing whitespace** is stripped; the file ends with exactly one newline.
- **Docstrings** (`---` blocks) are re-indented but their contents are preserved verbatim.
- **String literals** and `//` comments are never touched.

Safe by design: the formatter only changes whitespace. It never reorders code, wraps long lines, or rebrackets expressions. Same rules as the VS Code formatter, so the CLI and editor produce identical output.

Flags:
- `--check`: don't modify files; exit `1` if any file would change, list them. Useful in CI.
- `--stdout`: print formatted output instead of writing back.

```bash
quirk fmt                            # format every .quirk under cwd
quirk fmt src/index.quirk               # format one file (in place)
quirk fmt --check tests/             # CI gate — fails on un-formatted code
quirk fmt --stdout messy.quirk | less   # preview without writing
```

`quirk fmt` is idempotent: running it on already-formatted code is a no-op.

---

## Packages

### `quirk install`

Synopsis: `quirk install [-e] [-r <file>] [<spec> ...]`

Install one or more dependencies. Specs accept several forms:

| Spec form | Effect |
|-----------|--------|
| `github.com/owner/repo[@ref]` | Clone the repo at `ref` (tag/branch/SHA). |
| `<path>` | Snapshot-install from a local directory (recursive copy). |
| `<path>@<version>` | Snapshot install, but verify the source's manifest declares that version. |
| `<path>@<range>` | Same with a version range (`>=`, `<`, `,` for AND — see [version specs](#version-specs)). |
| `<name>@<version>` | "Switch" an already-installed package to a specific cached version. No source needed. |
| `<name>@<range>` | Switch to the highest installed version matching the range. |

Flags:

| Flag | Effect |
|------|--------|
| `-e <path>` | Editable install — symlinks the source. Edits propagate live. |
| `-r <file>` | Read deps from a manifest file. Empty `quirk install` defaults to `./quirk.toml`. |

```bash
# Snapshot install from a path
quirk install ~/code/slug

# Editable install (live edits propagate)
quirk install -e ~/code/slug

# Pin to a version
quirk install ~/code/slug@0.2.0

# Range
quirk install ~/code/slug@'>=0.1,<1.0'

# Switch already-cached version (no source needed)
quirk install slug@0.1.0

# Git
quirk install github.com/foo/bar@v0.1.0

# From a manifest
quirk install -r requirements.toml
quirk install                   # reads ./quirk.toml
```

#### Where installs go

| State | Install location |
|-------|------------------|
| Active venv (`QUIRK_HOME` points at a dir with `bin/activate`) | `$QUIRK_HOME/lib/quirk/packages/` |
| Project (`quirk.toml` in cwd or ancestor) | `<project>/packages/` |
| Otherwise | `~/.quirk/packages/` (user-global) |

#### Layout per package

Each install lives under `packages/<name>/<version>/`, with a `current` symlink picking the active version:

```
packages/slug/
├── 0.1.0/
│   ├── quirk.toml
│   └── src/index.quirk
├── 0.2.0/
│   └── ...
└── current → 0.2.0
```

The compiler resolves `use slug` via `packages/slug/current/src/index.quirk`.

### `quirk upgrade`

Synopsis: `quirk upgrade [<pkg> ...]`

For each named package (or all `[deps]` in `./quirk.toml`):

- **Local installs** → `current` symlink moves to the highest installed version. No re-fetch.
- **Git installs** → re-clone at the default branch's HEAD.

```bash
quirk upgrade               # all deps in quirk.toml
quirk upgrade slug          # one package
```

### `quirk remove`

Synopsis: `quirk remove <pkg>[@<ver>] ...` &nbsp;·&nbsp; alias: `quirk uninstall ...`

Remove a package. If `@<ver>` is given, only that one version goes; otherwise the whole `packages/<pkg>/` tree is dropped.

```bash
quirk remove slug              # all versions
quirk remove slug@0.1.0        # just one
quirk remove slug other-lib    # multiple at once
```

If you remove the *active* version, `current` rolls back to the next-highest remaining version (or the package is fully removed if nothing's left).

### `quirk list`

Synopsis: `quirk list` &nbsp;·&nbsp; aliases: `quirk packages`, `quirk -p`

Print installed packages. Shows the active version; other installed versions are in parens.

```
$ quirk list
2 package(s) installed:
  slug      0.2.0   (also: 0.1.0)
  pretty    1.0.0
```

### `quirk show`

Synopsis: `quirk show <pkg>`

Detailed info: manifest fields, deps, and all installed versions.

```
$ quirk show slug
name:        slug
version:     0.2.0
description: Convert strings to URL-safe slugs
author:      Alex Vachon <alex.vachon@outlook.com>
license:     MIT
installed versions: 0.1.0, 0.2.0 (active)
```

### `quirk deps`

Synopsis: `quirk deps`

Print installed packages in `name = "version"` form — suitable for redirecting into a `quirk.toml [deps]` block to capture a reproducible set.

```bash
$ quirk deps
slug = "0.2.0"
pretty = "1.0.0"

$ quirk deps >> quirk.toml      # append to current manifest
```

Equivalent of `pip freeze`.

---

## Misc

### `quirk help`

Synopsis: `quirk help [<command>]` &nbsp;·&nbsp; aliases: `quirk --help`, `quirk -h`

With no argument, print the top-level command summary. With a command name, print that command's detailed help.

```bash
quirk help               # everything
quirk help install       # just install
quirk help eval
```

### `quirk version`

Synopsis: `quirk version` &nbsp;·&nbsp; alias: `quirk --version`

Print the compiler version:

```
$ quirk version
quirk 0.2.0
```

---

## Version specs

Used by `quirk install <pkg>@<spec>`.

| Form | Example | Meaning |
|------|---------|---------|
| Exact | `1.0.0` | Must equal |
| Bare (no operator) | `1.0.0` | Treated as `==` |
| Min | `>=1.0` | Greater or equal |
| Max | `<2.0` | Strictly less |
| Range | `>=1.0,<2.0` | AND of clauses |
| Combo | `>=1.0,<=1.9,!=1.5.3` | Multiple ANDs |

Available operators: `==`, `!=`, `>`, `>=`, `<`, `<=`. Commas mean AND.

```bash
quirk install slug@1.0.0           # exact
quirk install slug@'>=1.0,<2.0'    # range (quote to escape shell <>)
quirk install slug@'!=1.5.3'       # exclude one version
```

Versions parse as `MAJOR.MINOR.PATCH`. Pre-release / build metadata (`-rc1`, `+build42`) is dropped before comparison.

---

## Resolution order

When you write `use foo` in a script, the compiler searches in order:

1. `<project>/packages/`
2. `$QUIRK_HOME/lib/quirk/packages/` (venv-installed)
3. `$QUIRK_HOME/lib/quirk/stdlib/` (stdlib in venv layout)
4. `$QUIRK_HOME/lib/quirk/` (legacy / dev install)
5. `$QUIRK_HOME/libs/` (dev tree libs)
6. `~/.quirk/packages/` (user-global — **skipped when in a venv**)
7. `./libs/` (dev fallback)
8. `/usr/local/lib/quirk/packages/` (when `QUIRK_HOME` is unset)
9. `/usr/local/lib/quirk/` (system stdlib, when `QUIRK_HOME` unset)

Within a search root, the compiler tries these layouts:

```
<root>/foo.quirk
<root>/foo/index.quirk
<root>/foo/__init.quirk
<root>/foo/src/index.quirk
<root>/foo/src/foo.quirk
<root>/foo/current/src/index.quirk     ← versioned install
<root>/foo/current/src/foo.quirk
```

Project-local installs always win, which lets you shadow a global install for a single project.

---

## Environment variables

| Variable | Purpose |
|----------|---------|
| `QUIRK_HOME` | Root of an active venv or installation. Set by `source <venv>/bin/activate`. |
| `HOME` | User home — drives `~/.quirk/packages/`. |

---

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Generic error (compile error, missing arg, bad spec, ...) |
| 124 | (When wrapped in `timeout`) command timed out |
| 139 | Segfault — please file an issue with a reproducer |

---

## See also

- [INSTALL.md](INSTALL.md) — installing the compiler itself
- [PACKAGES.md](PACKAGES.md) — packaging guide for library authors
