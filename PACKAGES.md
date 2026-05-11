# Quirk packages

Quirk's package manager lives inside the `quirk` binary — no separate tool to install. Packages are git repositories with a `quirk.toml` manifest, dropped into a `packages/` directory the compiler already searches.

```
quirk install <pkg>      install a dependency
quirk upgrade [pkg]      bump to latest
quirk remove <pkg>       uninstall
quirk list               (alias: packages, -p)
quirk show <pkg>         package details
quirk init               scaffold a quirk.toml in cwd
quirk venv <name>        create an isolated environment
```

Outside a venv, installs land in `./packages/` (project-local).
Inside a venv (`QUIRK_HOME` set), they land in `$QUIRK_HOME/lib/quirk/packages/`.

---

## Using a package

```bash
mkdir my-app && cd my-app
quirk init                          # writes quirk.toml
quirk install github.com/foo/pretty@v0.1.0
#   ✓ pretty

cat > main.qk <<'EOF'
use pretty
define main() {
    print(pretty.format({"name": "Alice", "tags": [1, 2, 3]}))
}
EOF
quirk main.qk
```

Layout after the install:

```
my-app/
├── main.qk
├── quirk.toml          ← lists `pretty = "github.com/foo/pretty@v0.1.0"`
└── packages/
    └── pretty/
        ├── src/index.qk
        └── quirk.toml
```

The compiler's resolver finds `use pretty` at `packages/pretty/src/index.qk` automatically.

### Package specs

```
github.com/owner/repo            # default branch's HEAD
github.com/owner/repo@v1.2.0     # tag (recommended for reproducibility)
github.com/owner/repo@main       # branch
git@host.com/owner/repo.git      # full git URL also accepted
```

Short `github.com/...` form gets auto-expanded to `https://...git`.

### Installing from a manifest

```bash
quirk install                       # reads ./quirk.toml [deps] and installs each
quirk install -r requirements.toml  # reads a different manifest file
```

This is what CI / fresh-checkout workflows want.

### Upgrading

```bash
quirk upgrade pretty                # re-clone pretty at the latest matching ref
quirk upgrade                       # upgrade everything in quirk.toml
```

`upgrade` strips any `@ref` so you get the default branch's HEAD. To pin again, edit `quirk.toml` and re-run `quirk install`.

### Removing

```bash
quirk remove pretty                 # deletes packages/pretty and the manifest entry
```

---

## Virtual environments

A venv isolates a project's dependencies — like Python's `venv` or Node's `node_modules` per-project. The stdlib is symlinked in (no duplication on disk).

```bash
quirk venv myenv                    # creates ./myenv/
source myenv/bin/activate

(quirk:myenv) $ quirk install github.com/foo/pretty
(quirk:myenv) $ quirk packages
1 package(s) installed:
  pretty   0.1.0

(quirk:myenv) $ quirk myscript.qk   # resolves pretty from the venv
(quirk:myenv) $ deactivate
```

Activating sets `QUIRK_HOME=<venv-abs-path>` and prepends `<venv>/bin/` to `PATH`. The `deactivate` function (defined by `activate`) restores the prior environment.

### Generated layout

```
myenv/
├── bin/
│   ├── activate           bash / zsh source script
│   ├── quirk              → symlink to system quirk
│   └── runtime.so         → symlink to system runtime.so
├── lib/quirk/
│   ├── typing/, console/, math/, …    (stdlib, symlinked)
│   └── packages/          installed dependencies live here
└── quirk.toml
```

### When to use a venv

- **Different projects need different versions of the same lib.** Without a venv, `./packages/` gets overwritten each time you switch.
- **You want a reproducible "snapshot" you can rebuild from scratch.** Delete `myenv/`, run `quirk venv myenv && source ... && quirk install`.
- **You're testing a library against a specific compiler version.** Symlink the test compiler into `<venv>/bin/quirk`.

---

## Creating a package

### Layout

```
my-lib/
├── quirk.toml           # required: name, version, [deps]
├── src/
│   └── index.qk         # module entry point — `use my-lib` resolves here
└── tests/
    └── my_lib_test.qk   # optional; run with `quirk tests/*.qk`
```

The compiler resolves `use my-lib` to either:

- `<root>/my-lib.qk` (single-file package), OR
- `<root>/my-lib/index.qk` (directory package — preferred for anything non-trivial)

Use the directory form when you want multiple files (`src/`, `tests/`, etc).

### Manifest (`quirk.toml`)

```toml
name        = "my-lib"
version     = "0.1.0"
description = "What this lib does in one line"
author      = "Your Name <you@example.com>"
license     = "MIT"

[deps]
# Anything in `[deps]` gets `quirk install`'d transitively (or will, once
# transitive resolution lands — for now, declare and run install yourself).
pretty = "github.com/foo/pretty@v0.2.0"
```

The format is a TOML subset:

- Bare `key = "value"` strings at the top.
- `[deps]` section: `name = "spec"` pairs.
- `#` line comments.

### Conventions

- **Name** matches the import users will write: `name = "pretty"` ↔ `use pretty`.
- **Version** is a semver-ish string. The package manager doesn't yet do semver resolution — pinning is by exact tag.
- **Entry point** is `src/index.qk`. Re-export the public API from there:
  ```quirk
  // src/index.qk
  from .formatters use { format, format_short }
  from .types     use { PrettyOptions }
  ```
- **Builtin-named functions** (`write`, `print`, `type`, ...) are safe to use as library function names — the compiler prefixes their linkage names automatically. Just don't shadow them in the top-level user script.

### Local development

Before publishing, develop the library in-place by symlinking it into a project's `packages/`:

```bash
mkdir -p test-project/packages
ln -s ~/code/my-lib test-project/packages/my-lib
cd test-project
quirk main.qk             # imports my-lib straight from the working copy
```

This skips the install/publish cycle while iterating.

### Publishing

1. Tag a release in your repo:
   ```bash
   git tag v0.1.0
   git push --tags
   ```
2. Tell users to install it:
   ```bash
   quirk install github.com/yourname/my-lib@v0.1.0
   ```

That's it — there's no central registry. The package spec IS the source location.

### Testing your library

```bash
quirk tests/my_lib_test.qk
```

Use the `test` stdlib module for a structured runner:

```quirk
use test
from test use { TestCase }

define main() -> void {
    t1 := TestCase("format roundtrip", fn() {
        test.assert_eq(my_lib.format(42), "42")
    })
    test.run_all([t1])
}
```

---

## Subcommand reference

| Command | Effect |
|---------|--------|
| `quirk install <spec ...>` | Clone each spec into the package dir; append to `quirk.toml` `[deps]` |
| `quirk install -r <file>` | Install everything in a given manifest |
| `quirk install` (no args) | Install everything in `./quirk.toml` |
| `quirk upgrade [pkg ...]` | Re-clone at HEAD (use to bump from a pinned tag to latest) |
| `quirk remove <pkg ...>` | Delete from package dir + manifest |
| `quirk list` / `packages` / `-p` | Print installed packages with versions |
| `quirk show <pkg>` | Print one package's manifest |
| `quirk init` | Scaffold a `quirk.toml` for the current directory |
| `quirk venv <name>` | Create an isolated environment at `./<name>/` |
| `quirk version` / `--version` | Print the Quirk compiler version |

---

## Resolution order

When the compiler sees `use foo`, it looks in:

1. `$QUIRK_HOME/lib/quirk/packages/foo/` — venv-installed
2. `$QUIRK_HOME/lib/quirk/foo/` — stdlib (symlinked into venv) or system install
3. `$QUIRK_HOME/libs/foo/` — dev layout
4. `./libs/foo/` — current working directory's stdlib (dev convenience)
5. `./packages/foo/` — project-local install
6. `/usr/local/lib/quirk/packages/foo/` — system, when `$QUIRK_HOME` unset
7. `/usr/local/lib/quirk/foo/` — system stdlib, when `$QUIRK_HOME` unset

First match wins. Set `QUIRK_HOME` to control this hierarchy explicitly.

---

## Limitations (today)

- **No transitive deps.** `quirk install` only fetches what you list. If a library's own `quirk.toml` has deps, install them yourself for now.
- **No lockfile yet.** Versions are pinned by `@tag` in `quirk.toml`; commit SHAs aren't auto-recorded.
- **No central registry.** All specs are git URLs. Search/discovery is via GitHub/web search, not `quirk search`.
- **No semver solver.** If `A` wants `pretty@v1` and `B` wants `pretty@v2`, you pick one.

These are all additive — when they land, existing `quirk.toml` files keep working.
