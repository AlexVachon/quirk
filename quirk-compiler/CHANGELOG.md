# Changelog

All notable changes to Quirk land here. The format is loosely
[Keep a Changelog](https://keepachangelog.com/) and the project follows
SemVer — minor bumps for new features, patches for fixes, major bumps
only for breaking changes.

## [1.6.4] — 2026-06-03

### `quirk-lsp` 0.5.0 — find references (`textDocument/references`)

Word-boundary text search for every occurrence of the identifier
under the cursor, across every `.quirk` file in the workspace
folders. Skips `packages/`, `.venv/`, `.git/`, `node_modules/`,
`build/`, `out/`, `obj/`, `target/`, `.cache/`. Caps at 5000 files
and 1 MiB per file to keep accidental walkthroughs bounded.

- Open documents are scanned via their in-memory text first, so
  unsaved edits show up.
- Files not currently open are read from disk.
- `context.includeDeclaration: false` strips the canonical
  declaration site from the result.

Coarse on purpose — finds textual matches regardless of scope, so a
parameter named `foo` in one function and a top-level `foo()` both
appear. A scope-aware version needs the compiler to expose its
symbol table; until then, the find-references panel is "good enough"
for navigation and a bad fit for fully-automatic rename.

Compiler binary is byte-identical to 1.6.3 modulo the version
constant — this release is `quirk-lsp` only.

## [1.6.3] — 2026-06-03

### `quirk resolve <name>` + cross-file LSP go-to-def

- **New `quirk resolve <name>` subcommand.** Prints the absolute path
  of the `.quirk` file that `use <name>` would load, or exits 1 on a
  miss. Accepts both bare names (`quirk resolve argparse`) and dotted
  paths (`quirk resolve typing.primitives.int`) — the dotted form
  converts `.` → `/` before the lookup so the file layout under
  `packages/typing/primitives/int.quirk` is reachable. Reuses
  `locate_module_file` so the resolution matches what `use` does at
  compile time.
- **`quirk-lsp` 0.4.0** uses this to extend go-to-definition across
  files. The LSP scans the current document's import block
  (`use X`, `from X use { Y, Z }`, including the multi-line brace
  form heavily used by `typing/`) into a `name → module` map. On
  ctrl-click, same-file declarations still win first; if the cursor
  identifier matches an imported name, the LSP runs `quirk resolve`
  and jumps to that module's matching decl. Resolved paths are
  cached per session so repeat lookups don't re-spawn the compiler.

Tested round-trip: ctrl-click on `argparse` jumps to the user-global
install at `~/.quirk/packages/argparse/index.quirk`; ctrl-click on
`Int` (imported via `from typing.primitives.int use { Int }`) jumps
to `packages/typing/primitives/int.quirk` at the `struct Int` line.

## [1.6.2] — 2026-06-03

### `quirk-lsp` 0.3.0 — go-to-definition (current file)

- **`textDocument/definition`** — ctrl-click a name to jump to its
  `define` / `struct` / `enum` / `interface` declaration. Scope is
  intentionally tight: same-file top-level + struct methods only.
  Returns multiple `Location`s when the editor would render that as
  a chooser (e.g. interface + concrete method with the same name).
- Local variables, struct fields, and parameters don't resolve yet
  — they need scope tracking that's bigger than what regex can do.
- Cross-file resolution (`use argparse` → `packages/argparse/...`)
  is also deferred. The cleanest path is a `quirk resolve <name>
  --from <file>` query on the compiler so the LSP doesn't have to
  duplicate the C++ resolver in TypeScript; that lands in a later
  1.6.x release.

Compiler binary is byte-identical to 1.6.1 modulo the version constant
— this release is `quirk-lsp` only.

## [1.6.1] — 2026-06-03

### `quirk-lsp` 0.2.0 — outline + formatting

Two more LSP features land in the standalone server; the compiler
itself is unchanged this release. Same install instructions as 1.6.0
still work — pull a fresh `quirk-lsp` from the repo + `npm run build`.

- **`textDocument/documentSymbol`** populates the editor's outline
  panel, breadcrumbs, and `@`-prefix symbol picker. Top-level
  `define` / `struct` / `enum` / `interface` show as Function /
  Struct / Enum / Interface; methods inside a struct nest under it.
  Regex-driven (no compiler invocation) so it's cheap on every keystroke.
- **`textDocument/formatting`** shells out to `quirk fmt --stdout`
  on the buffer and returns a single edit replacing the whole
  document. Same canonical output as `quirk fmt` from the CLI, so
  format-on-save in any editor stays in sync with the project's
  pre-commit hook.

The version bump in `quirk-compiler/src/PackageManager.hpp` is only
for the bundled CHANGELOG entry; the binary itself is byte-identical
to 1.6.0 modulo the version constant.

## [1.6.0] — 2026-06-03

### LSP foundation: `quirk-lsp` server

Quirk now ships with a standalone Language Server Protocol implementation
under `quirk-lsp/`. Same diagnostics in any LSP-aware editor — Neovim,
Helix, Zed, JetBrains. The VSCode extension keeps its in-process
providers for now; switching it to use the LSP is on the v1.6.x roadmap.

This release covers the foundation:

- **`quirk --check --diagnostics-json`** — new flag on the compiler.
  Emits one NDJSON record per diagnostic to stdout instead of the
  human-readable ANSI output to stderr. Both Sema and Parser route
  through it. `--check` also stays silent on success in JSON mode
  (no `: OK` banner) so the LSP can infer success from an empty
  stream + exit 0.
- **`quirk-lsp` TypeScript server** — single Node binary. Speaks
  stdio. Spawns the compiler on `didOpen` + `didSave`, translates
  NDJSON records into LSP `Diagnostic` objects. Compiler binary
  resolved via `initializationOptions.quirk.executablePath` →
  `QUIRK_BIN` env → `quirk` on `PATH`.
- **Editor configs** — `quirk-lsp/README.md` has copy-pasteable
  snippets for Neovim's built-in LSP, Helix's `languages.toml`, and
  Zed's settings. The wire format is documented for one-off tools
  (CI gates, pre-commit hooks).

What ships in later 1.6.x:
- Hover, completion, definition, references, rename, signature help,
  semantic tokens, formatter, outline (each is a port of an existing
  VSCode provider).
- VSCode extension switches to the LSP for at least diagnostics.

## [1.5.1] — 2026-06-03

### `pkg install --frozen` lockfile-name vs URL-basename mismatch

A stdlib package spec like `crypto = "github.com/AlexVachon/quirk-crypto@v1.0.0"`
produced a lockfile entry keyed by the package's manifest name (`crypto`),
but `--frozen` lookup used the URL basename (`quirk-crypto`). Result:
every `pkg install --frozen` against a clean working tree failed with
`'quirk-crypto' not in quirk.lock` even though the lockfile was correct.

Fixed: the resolver now also builds a URL → lockfile-name map before
the install loop and falls back to it whenever `preview_name(spec)`
disagrees with what's actually in the lockfile. Same code path
handles the dedup/conflict check, so two specs pointing at the same
repo with different prefixes (e.g. one `crypto` alias and one full
`github.com/.../quirk-crypto`) are treated as duplicates.

Surfaced during a full audit of the 21 stdlib packages — round-trip
install + use works for every package; the `--frozen` path was the
only crack.

## [1.5.0] — 2026-06-03

### Full stdlib split — every package now has a canonical repo

v1.4.0 shipped the *mechanism* (compiler-baked `stdlib_registry()`)
and one pilot package (`argparse`). v1.5.0 completes the rollout:
all 21 stdlib packages now resolve via the registry to their own
GitHub repo at `github.com/AlexVachon/quirk-<name>@v1.0.0`. The
bundled copies under `<QUIRK_HOME>/packages/` stay in place as the
offline fallback.

### Resolver fixes (uncovered while validating the split)

Two related issues surfaced when round-tripping `typing` end-to-end:

- **`src/` layout install flatten.** Repos for stdlib packages use
  the `src/index.quirk` convention (separate package source from
  README/LICENSE). On install, `materialize_from_cache` now copies
  the contents of `src/` (not `src/` itself) into
  `<pkgRoot>/<name>/`, so the installed layout matches what the
  bundled packages look like. Detection is presence-of-
  `src/index.quirk`; legacy packages without `src/` are unchanged.
- **Relative-import fall-through.** `from ...sys` inside the typing
  package used to rely on `sys` being a sibling directory in the
  bundled flat layout. In a project-local install of only `typing`,
  that sibling doesn't exist. The resolver now falls through to the
  absolute search when a relative walk misses, so the import lands
  on the bundled `sys` (or a separately-installed one) instead of
  hard-failing.

Together these unblock running scripts that depend on packages
installed via the new registry.

Newly registered: `console`, `crypto`, `csv`, `datetime`, `debug`,
`encoding`, `fs`, `io`, `itertools`, `math`, `net`, `random`,
`regex`, `statistics`, `sys`, `test`, `time`, `typing`, `url`, `uuid`.

Each repo carries the same source the compiler shipped, a generated
`quirk.toml` (`quirk-version = ">=1.4.0"`), a README with the
top-of-file docstring, MIT LICENSE, and a `v1.0.0` tag. Future
maintenance: bump tags in the individual repos for fixes;
compiler-baked URLs stay stable.

`quirk pkg install typing` (or any other name) now fetches the
latest tag and shadows the bundled copy at `use typing` time.
`quirk pkg upgrade typing` re-fetches if a newer tag has been
published.

### Roadmap update

The LSP rollout (was v1.5) slides to **v1.6**. Doing the full
stdlib split first was higher leverage — every stdlib fix from
this point can ship via a package tag rather than a compiler
release.

## [1.4.0] — 2026-06-03

### Stdlib packages can now live in independent repos

First step of the stdlib-decoupling roadmap. The bundled stdlib at
`<QUIRK_HOME>/packages/` still ships and stays the offline fallback,
but `quirk pkg install <stdlib-name>` now resolves to a canonical
GitHub repo and can fetch a newer version than the one the compiler
shipped with. Users no longer need a compiler bump to pick up an
argparse fix.

- **`stdlib_registry()` in `PackageManager.hpp`** is a baked-in map
  of stdlib names → canonical repo URLs. Falls in after user aliases
  (`~/.quirk/aliases.toml`) and the cached external registry, so user
  overrides win.
- **`quirk pkg registry list`** now displays the built-in entries
  alongside aliases and the cached registry. Run it to see what bare
  names resolve to.
- **First entry: `argparse → github.com/AlexVachon/quirk-argparse`.**
  A staging repo for the package is prepared locally at
  `/tmp/quirk-argparse-staging/` (with `quirk.toml`, README, LICENSE,
  `v1.0.0` tag); compiler-maintainer creates the GitHub repo and
  pushes from there. The bundled `packages/argparse/` in the compiler
  tree stays in place as the offline fallback.

Adding a new stdlib package to the registry is one entry in
`stdlib_registry()` + a compiler ship. See
[PACKAGES.md](../PACKAGES.md#stdlib-in-independent-repos-since-140)
for the full maintainer workflow.

## [1.3.0] — 2026-06-03

### First macOS support — source-buildable + CI-validated

Quirk now compiles cleanly on macOS (Apple Silicon). All Linux-specific
code paths have been replaced with portable wrappers:

- **`self_binary()`** dispatches on `__APPLE__`: uses
  `_NSGetExecutablePath` + `realpath()` on macOS, `/proc/self/exe`
  + `readlink()` on Linux. The half-dozen `readlink("/proc/self/exe", ...)`
  call sites scattered across `PackageManager.hpp` + `Compiler.cpp`
  now go through this single helper.
- **`runtime.so`** filename is kept on macOS too (Apple's `dlopen`
  doesn't care about the extension, and renaming would ripple into
  7+ install-script and resolver sites for no practical benefit).
- **Makefile** uses `?=` on `CXX`/`CC`/`LLVM_CONFIG` so macOS users
  can build with `LLVM_CONFIG=$(brew --prefix llvm@14)/bin/llvm-config make`
  without touching any source.

### CI

A new `macos-arm64` job in the release workflow builds + smoke-tests
the compiler on every tag push and uploads `quirk-X.Y.Z-darwin-arm64.tar.gz`
alongside the Linux tarball. Marked `continue-on-error: true` for now —
won't block a release if the macOS build flakes during this
first-shipping window.

### Installer

`install.sh` recognises macOS arm64 and attempts to fetch the matching
tarball. On 404 (e.g. installing a pre-1.3 tag), it prints the
build-from-source path:

```
brew install llvm@14 bdw-gc openssl@3
LLVM_CONFIG=$(brew --prefix llvm@14)/bin/llvm-config make
```

Also: `sha256sum` (Linux) vs `shasum -a 256` (macOS) is dispatched by
availability rather than hardcoded — fixes a checksum-verification
crash that would otherwise hit Mac users.

### Known caveats

- **No local validation.** This release is a blind port — built and
  tested on Linux only. The first macOS binary will materialise from
  CI; expect a v1.3.1 with the first round of "wait, that doesn't
  compile on macOS after all" fixes.
- **Apple Silicon only.** Intel-Mac (`darwin-x86_64`) and Windows are
  not yet built; both are on the roadmap.

## [1.2.0] — 2026-06-03

### REPL line editing + persistent history

`quirk repl` now does the things you'd expect from a real interactive
shell:

- **Arrow-key recall** (↑/↓) walks through previous inputs.
- **Line editing** — ←/→, ctrl-A/E, ctrl-W, backspace etc. all work.
- **Persistent history** at `~/.quirk/repl_history` (capped at 1000
  entries). Sessions merge with the file on save, so quitting and
  reopening keeps your context.
- **Non-TTY input still works** — when stdin is piped (`printf ... |
  quirk repl`), linenoise transparently falls back to plain fgets.

The REPL itself (preamble/state model, multi-line via brace balance,
`:help` / `:quit` / `:reset` / `:state` meta-commands) was already in
the codebase; this release just makes it pleasant to use.

Implementation: vendored linenoise (BSD-2-clause, antirez/linenoise)
into `src/third_party/linenoise/`. No new build dep — linenoise is a
single-file C library, compiled in alongside the existing objects.
~1500 LOC added, scoped to `quirk repl` only.

## [1.1.0] — 2026-06-03

### `quirk test` is now usable

The runner has been in the codebase for a while — walks `tests/`,
spawns `quirk run` per `*_test.quirk`, parses framework summary
lines — but in practice it deadlocked on any test that opened a
network port or waited on stdin (e.g. `server_test.quirk` would
block forever binding a socket). v1.1.0 ships the fixes that make
it actually usable as a CI / pre-tag check.

- **`--timeout <secs>`** (default 30s, `0` to disable). Per-file
  wall-clock cap, enforced by shelling out to `timeout(1)`. Tests
  over the cap fail with exit 124 and render as `(timeout)` in the
  status line instead of `(exit 124)`.
- **`--filter <substr>`** runs only files whose path contains
  `<substr>`. Useful for iterating on one suite without paying for
  the whole batch.
- **`-v` / `--verbose`** still dumps each file's full output (was
  already there; just documented now).

The runner discovers files matching `<name>_test.quirk` recursively
under `tests/` by default, or the path you pass. It already skipped
`packages/`, `.venv`, `.git`, `node_modules` — that's unchanged.

The framework contract: tests must end with a summary line matching
`N passed` (success) or `M failed, N passed (of T)` (failure). The
bundled `test` package emits both shapes; user frameworks can match
either format. Files that exit non-zero without a summary line still
count as failures with `(exit N)` in the status line.

## [1.0.12] — 2026-06-03

### `pkg remove` now keeps the lockfile consistent

`quirk pkg remove <name>` deleted `packages/<name>/` and stripped the
entry from `quirk.toml`, but left a stale `[[package]]` block in
`quirk.lock`. A subsequent `pkg install --frozen` would then either
resurrect the package or fail with a misleading "lockfile/manifest
mismatch". Fixed: `cmd_remove` now also drops matching entries from
the lockfile, and removes the file entirely when it ends up empty so
there's no header-only file polluting git diffs.

This applies to both bare-name remove (`pkg remove logger`) and the
versioned `pkg remove logger@0.1.0` path when the version being
removed is the active one.

### Audit notes (no code change)

Round-tripped a third-party path-based install end-to-end:
- `pkg install` → correct project-local install under `packages/` when
  `quirk.toml` is present; falls back to `~/.quirk/packages/` outside
  a project, as documented.
- `quirk-version = ">=X"` is enforced — packages requiring a newer
  compiler are rejected with a clear message.
- Resolver precedence is correct: project-local `./packages/X` wins
  over user-global `~/.quirk/packages/X`.

## [1.0.11] — 2026-06-03

### Cache key now walks the transitive import graph

The 1.0.10 bitcode cache hashed only the entry file. Changing a `use`d
file (a stdlib module, a project-local package) without also changing
the entry script produced a stale cache hit. Fixed: a mini-scanner
now walks `use` / `from … use` directives starting at the entry file,
following resolved imports recursively, and folds each loaded file's
bytes into the SHA-256. `typing` is seeded explicitly since it's
auto-imported by every program.

The scanner is deliberately simpler than the real parser — line-based
text scan, no AST, no Sema. Worst-case divergence (e.g. picking up a
`use ...` string inside a block comment) just adds a spurious file to
the hash, which downgrades to a cache miss on the next run. Never
produces a wrong result.

`--no-cache` still bypasses everything. Stale entries from old keys
linger in `~/.quirk/cache` until a future LRU eviction (TODO).

## [1.0.10] — 2026-06-03

### Per-invocation bitcode cache

- **`quirk <file>` now caches compiled bitcode under `~/.quirk/cache/`.**
  The cache key is `sha256(file content + compiler version + opt
  level)`. On a hit, parse + Sema + Codegen + LLVM passes are skipped
  entirely — the JIT consumes the cached IR directly. Saves ~15–25%
  per-run on real scripts that import stdlib; small wins on
  compute-heavy scripts where runtime dominates.
- **`--no-cache`** opts out, e.g. for benchmarking or when transitive
  imports have changed (see known limitation below).
- The cache is skipped automatically when `--debug` is on (the line
  stepper needs the source map that bitcode doesn't carry) and for the
  `-o`/`--check`/`--emit-*` paths.

**Known limitation:** the cache key hashes the entry file but not its
transitive `use` imports. If a `use`d file changes without the entry
also changing, the stale cache wins. Workaround: `--no-cache` or
`rm -rf ~/.quirk/cache`. A future release will walk imports during
key computation.

### Misc

- `quirk --help` now correctly documents `-O1` as the default optimization
  level (it always was — only the help text was wrong).

### Wart #2 status (Int 0 in nonlocal cells)

Investigated a fix using bit-48 tagging of nonlocal-cell pointers. The
tagged pointer survives the `ptrtoint→i32` unbox sites (truncation
discards the tag), but it breaks downstream consumers that treat the
cell value as a real `Any*` — `print(it())` crashed with a SEGV when
the tagged pointer was dereferenced. A correct fix needs coordinated
changes to both the boxing convention and every unbox site, which is
larger than this release. Tracked in `feedback_iter_lib_warts`.

## [1.0.9] — 2026-06-03

### Closures-with-state fixes

Two longstanding codegen warts that bit anyone writing stateful iterators
or filter pipelines through `Callable`. Both surfaced while building
`packages/itertools/` and have been worked around with annotations until
now.

- **Nonlocal Int values now flow through call sites correctly.** A
  `nonlocal i` cell stores `i8*`; passing it as the index to
  `List.get(i)` previously crashed with `Call parameter type does not
  match function signature` because the call-args codegen didn't insert
  a `ptrtoint`. The typed-local workaround (`idx: Int = i`) is no
  longer needed.
- **`pred(v) == false` no longer aborts the JIT.** Comparing the
  `i8*`-boxed return of a `Callable` against an `i1` literal used to
  hit `ICmpInst::AssertOK`. The BinaryOp codegen now routes the boxed
  side through `Core_Primitives_Any_to_int` (which reads `.ival` for
  `ANY_BOOL`/`ANY_INT`/`ANY_CHAR`) before the compare, so any
  `boxed-Any ==/!= primitive` pattern works.

Both fixes are pure compiler — no runtime changes — and existing tests
keep passing.

## [1.0.8] — 2026-06-02

### Stdlib lives under `packages/`

- The bundled standard library moved from `quirk-compiler/libs/` to
  `quirk-compiler/packages/`. Every stdlib module (`typing`, `console`,
  `net`, `crypto`, …) is now resolved through the same path the package
  manager uses for third-party packages.
- **Why:** a project that drops a `packages/<name>/` folder into its
  own tree can now shadow a stdlib module without reaching into the
  compiler install. Previously stdlib lived on a separate search path
  that user projects couldn't override.
- Install layout follows: tarballs now ship `<QUIRK_HOME>/packages/`
  (not `libs/`). `install.sh` wipes a stale `~/.quirk/libs/` on
  reinstall so the legacy fallback never shadows the fresh layout.
- The resolver still recognises legacy `libs/` directories — pre-1.0.8
  installs and any project that already had `./libs/` keep working
  until you reinstall.

## [1.0.7] — 2026-05-29

### Improved CLI surface
- **`quirk help` is now grouped** into RUN CODE / PROJECT / PACKAGES /
  PUBLISHING / COMPILER / MISC, with RUN FLAGS and ENVIRONMENT
  sections beneath. Replaces the alphabetical wall of 35 verbs.
- **`quirk bump-compiler`** is now also reachable as **`quirk compiler bump <part>`**
  (the old form still works; the new is documented).
- **`quirk stdlib`** is now also reachable as **`quirk compiler stdlib`**.
- **`quirk versions <pkg>`** is documented as **`quirk pkg versions <pkg>`**
  (the bare form remains an alias; the new is clearer that it lists
  *package* versions, not Quirk's).
- **`install.sh` flag renamed to `--with-extension`** (with
  `--install-extension` still working as a legacy alias, and a new
  explicit `--no-extension` opt-out). README + INSTALL.md updated.

The CLI is now consistently `quirk <command> [--flags] [args...]`,
matching standard Unix-tool conventions. No verbs were removed; only
the help / install-flag spellings changed.

## [1.0.6] — 2026-05-29

### New
- **`quirk` now notifies users when a newer release is available.**
  Cargo/npm-style: a one-line dim notice on stderr after any quirk
  subcommand finishes:

      ↑ A new Quirk release is available: 1.0.5 → 1.0.6
        run `quirk compiler update` to upgrade, or set
        QUIRK_NO_UPDATE_CHECK=1 to silence this.

  Behavior:
  - Checks GitHub at most **once per 24h** (cached at
    `~/.quirk/update-check.json`). Curl times out after 3s so a slow
    network never makes Quirk feel laggy.
  - Notice prints **at most once per 24h** even when the cache is
    refreshed sooner — tracked via `last_shown_at` in the cache file.
  - **Silent on non-TTY stdout** (pipes, redirects, CI logs).
  - **Silent on dev builds** — when the binary lives inside a source
    checkout with a `.git` directory above. Avoids nagging contributors
    working on the compiler.
  - **Silent when `QUIRK_NO_UPDATE_CHECK=1`** in the environment.

- **`quirk compiler check` — explicit on-demand version check.**
  Bypasses the 24h cache, hits GitHub right now, prints "Up to date"
  / "Update available" / "Newer than published (probably a dev build)".

### Setup note
- The first time a v1.0.5 user runs any quirk command after upgrading,
  the cache file is created. From then on the once-per-day check kicks
  in automatically. Users on older versions never see notices until they
  update once (manually or via `quirk compiler update`).

## [1.0.5] — 2026-05-29

### New
- **`quirk auth {login|status|logout}` — zero-setup publish auth.**
  Implements GitHub's OAuth Device Flow directly in the compiler so
  publishing a package doesn't need any of: SSH keys, deploy keys,
  PATs, or even the `gh` CLI. Flow:

      $ quirk auth login
      → Visit https://github.com/login/device
      → Code:  A4B7-C2D9
      [waiting for authorization...]
      ✓ Logged in as alexvachon

  Token is saved to `~/.quirk/auth.json` (chmod 600). One-time
  setup; subsequent `quirk release`/`quirk publish` Just Works.

- **`quirk release` push chain rewritten** to prefer the least-setup
  path that's actually available:
    1. Quirk's own stored token (from `quirk auth login`)
    2. `gh` CLI (from `gh auth login`)
    3. Plain `git push` against the configured remote
  When any of the first two are active, the push is automatically
  routed over HTTPS with the token applied via a scoped git
  credential helper — no manual remote-url switching, no global
  git config changes, no SSH identity routing.

### Improved
- **Updated deploy-key diagnostic** to lead with `quirk auth login`
  (zero external dependencies) as option 1, then `gh`, then the
  three SSH/PAT fallbacks.

### Setup note
- The first `quirk auth login` requires the Quirk OAuth App's
  `client_id` to be baked in (or supplied via `QUIRK_OAUTH_CLIENT_ID`).
  Build emits a clear error pointing at
  `https://github.com/settings/applications/new` if it's not
  configured. Public client IDs are safe to ship in the binary.

## [1.0.4] — 2026-05-29

### Improved
- **`quirk release` auto-routes through `gh` when available.** If the
  user has the GitHub CLI installed and is logged in (`gh auth login`,
  one-time, browser-based — no SSH keys, no PATs to manage), the
  release push automatically uses gh's stored credentials via a
  scoped git credential helper. SSH-only repos still work because
  we override the push URL to HTTPS for that single command. The
  user sees one extra line:

      using gh CLI auth (no SSH/PAT setup needed)

  …and the push just works.

  Goal: make "publish a Quirk package" cost one-time `gh auth login`
  and that's it. No deploy-key juggling, no PAT scoping, no SSH
  identity routing. The git-push fallback (with the diagnostic from
  1.0.3) still runs verbatim when `gh` isn't installed/authed.

- **Improved deploy-key diagnostic.** When `git push` fails with the
  "denied to deploy key" message, the diagnostic now leads with the
  `gh` setup (1-2 commands), then walks down through three SSH/PAT
  alternatives. Clarified that an empty deploy-keys page is fine
  (click "Add deploy key" and tick "Allow write access" on the form
  — there's no toggle on the empty list).

## [1.0.3] — 2026-05-29

### Improved
- **`quirk release` now diagnoses git-push failures** instead of just
  saying "git push failed". Pattern-matches the captured `git push`
  output for the most common reasons a publish blows up, then prints
  a targeted hint with concrete remediation steps and the relevant
  GitHub settings URL:
    - **Deploy key lacks write access** — links to
      `<repo>/settings/keys`, suggests the read-only key fix, an
      explicit `GIT_SSH_COMMAND` for a personal SSH identity, or
      switching to HTTPS + a fine-grained PAT.
    - **HTTPS auth missing / invalid** — explains PAT setup vs SSH switch.
    - **Repository not found** — flags missing repo or zero access.
    - **Network unreachable** — suggests retry.
    - **Branch is behind** — suggests `git pull --rebase && quirk release`.
  In all cases, when the commit was pushed but the tag wasn't, the
  diagnostic surfaces "the commit was pushed; only the tag is pending"
  so users know exactly which retry command to run.

  Reminder: Quirk publishes via plain `git push` — there's no central
  registry — so any auth issue that blocks `git push` blocks deploys.

## [1.0.2] — 2026-05-29

### Fixed
- **`quirk compiler` now shows up in tab-completion.** The new verb
  shipped in 1.0.1 wasn't in the hard-coded verb list inside the
  `quirk completion <shell>` output, so bash/zsh/fish tab-completion
  didn't suggest it. Added to the bash/zsh verb list plus a per-verb
  completion for the four subcommands (`version` / `list` / `install`
  / `update`), and the equivalent fish completions.

  After upgrading, refresh the completion in your shell:

  ```bash
  source <(quirk completion bash)   # or zsh / fish
  ```

  Or just open a new terminal — your rc file will source it.

## [1.0.1] — 2026-05-28

Four compiler correctness fixes, surfaced while building a real
third-party package (the in-tree `logger` lib), plus a new self-
management command so updating the compiler doesn't require touching
the install.sh URL.

### New
- **`quirk compiler <subcommand>`** — self-manage the compiler binary
  from inside the running compiler:
    - `quirk compiler version` — print the running version
    - `quirk compiler list` — list `vX.Y.Z` releases on GitHub
    - `quirk compiler install vA.B.C` — replace this binary with that version
    - `quirk compiler update` — replace this binary with the latest
  Internally delegates to `install.sh` on `main`, so there's a single
  source of truth for the install flow.

### Compiler fixes
- **Enum-typed parameters now compare to enum literals.** `define f(l: Level) -> Int { if l == Level.A {...} }` used to crash LLVM with an `ICmp` operand-type assertion because `TypeGen` fell through to `i8*` for any unrecognised name, while enum literals are codegen'd as `i32`. `TypeGen` now knows about user-declared enums and resolves them to `i32`.
- **Top-level `NAME := value` bindings are now importable across modules.** `from M use { NAME }` previously only matched struct / function / interface / enum names. A new `moduleConstRegistry` in `Sema` tracks module-level value bindings and the filter-list validator consults it.
- **Cross-module function-name collisions resolved.** Two files that both declared a function called `info` (or any common name) collided on the same LLVM symbol, and `module.info` calls could end up at the wrong implementation. Non-extern user functions now get module-prefixed linkage names (`MyMod$info`), and `moduleFunctionIndex` is keyed by both PascalCase and lowercase forms so `use console` matches the parser's `Console` prefix.
- **`list[i]` (`Any`) now passes correctly where `Callable*` (or any non-String struct pointer) is expected.** The argument-coercion site in `processCallArgs` was String-only for `i8* → struct*`; widened to all pointer-to-pointer casts, fixing the "Call parameter type does not match function signature" verifier failure that hit any code passing a `Callable` retrieved from a `List`.

### Known limitations
- Bare-name calls (`info(...)` without `module.` prefix) when two modules expose `info` still pick whichever was registered last. Module-qualified is the right pattern.
- `use .relative_path` in a sibling-file import still misses `moduleFunctionIndex`'s lookup keys; the surrounding tooling works around this for in-package imports.

## [1.0.0] — 2026-05-28

First stable release. The interactive debugger and VSCode integration are
the headline features; the rest is correctness and IDE-experience work
accumulated during that build-out.

### Interactive debugger
- **`debug.breakpoint(label)`** — tier-1 breakpoint helper that drops into an
  interactive `(qdb)` prompt with continue / quit / backtrace / skip-rest.
- **`--debug` line stepper** — tier-2 stepper that pauses at every statement.
  Commands: `c`/continue, `n`/next (step-over), `s`/step (step-into), `bt`,
  `where`, `b [file:]line` / `clear`, `skip`, `q`.
- **Locals inspection (`p <name>`, `locals`)** — Codegen registers each local
  with the runtime at its alloca site so the prompt can read live values
  for Int, Double, Bool, String, and any pointer type. Frame-scoped: stale
  entries get pruned when the owning function returns.
- **JSON event mode (`QUIRK_DBG_JSON=1`)** — qdb emits one machine-parseable
  event per stderr line (`stopped`, `stack`, `locals`, `breakpointSet`, …)
  so external tools can drive the debugger over stdio.

### VSCode integration
- **Debug Adapter Protocol bridge** — the extension ships a DAP adapter that
  spawns `quirk --debug` with `QUIRK_DBG_JSON=1` and translates DAP requests
  into qdb commands. Gutter breakpoints, F5 / F10 / F11 stepping, Call
  Stack panel, Variables panel, hover-evaluate for identifiers.
- **Inline values during debug** — Python-style `(value)` decorations next
  to variable references on paused lines, driven by a custom
  `InlineValuesProvider` that skips keywords, comments, string contents,
  and method/call chains.
- **Run/Debug dropdown** in the editor title bar with grouped sections —
  *Run File* + *Run File in Dedicated Terminal* and *Debug File* + *Debug
  using launch.json*.
- **Hover kind prefix** — every symbol hover leads with `(function)`,
  `(method)`, `(parameter)`, `(struct)`, `(enum)`, `(interface)`,
  `(type alias)`, `(constant)`, or `(variable)`, matching Pylance's
  shape. Lambda assignments (`c := fn(...)`) promote to `(function)`.
- **Ctrl+click navigation** now works on lambda parameters and variadic
  (`...args`) parameters.

### Codegen fixes
- **Variadic-lambda boxing** — passing a `Double` (or `Bool`) to a `fn(...args)`
  lambda no longer crashes with `Invalid bitcast: i8* bitcast (double … to
  i8*)`; values are now wrapped via `Core_Primitives_Any_box_*` before
  being placed into the variadic `List`. Same fix for the `is`/isinstance
  path.
- **Per-statement source line preservation** — the parser stamps `line`/`col`/
  `filePath` on every statement node so the stepper has something to show
  at each pause. The synthetic `main` shadow frame now carries the real
  source path instead of the string `"main"`.
- **Struct field inference from `__init`** — `self.X = paramName.method()`
  patterns where the receiver is a `String` parameter and the method is a
  known String-returning operation (`title`, `upper`, `lower`, `strip`, …)
  now infer to `String` instead of falling back to `Any` (which caused
  garbage memory reads on Any→String returns).

### Runtime
- **Locals registry overflow warning** — once-only `[debug] locals table
  full` message instead of silent truncation when more than 1024 locals
  are registered.
- **`strdup` routed through Boehm GC** — eliminates permanent leaks from
  libc calls that bypass the macro-no-op `free`.

### IDE extension polish
- **Variadic param syntax fixes** — `...args` no longer trips an `args is
  not defined` warning inside lambdas, and the spread operator wins the
  tokenization race against the range operator (`...` was being eaten as
  `..` + `.args`).
- **`QUIRK_HOME` auto-resolution** for the debug adapter — derived from the
  `quirk.quirkHome` setting when launching the debuggee, so the child can
  find runtime.so and the stdlib.

### Tooling
- **DAP adapter resilience** — pending `bt`/`locals`/`p` requests now reject
  when the child exits/errors, so VSCode panels stop hanging on a Promise
  that would never resolve.

### Breaking
- The synthetic main shadow frame's `file` field changed from the literal
  string `"main"` to the user's entry-file path. Tooling that parsed the
  old value should update.
- The hover provider's prefix format changed from no prefix to
  `(<kind>) name`. Documented for consistency with Python's Pylance.

## [0.3.0] — 2026-05-23

### New stdlib libraries
- **`random`** — xoshiro256**-backed RNG: `random()`, `randint(lo, hi)`, `uniform(lo, hi)`, `bool()`, `choice(list)`, `shuffle(list)`, `sample(list, k)`, `seed(n)`. Auto-seeded from time + pid at startup.
- **`uuid`** — RFC 4122 v4 UUIDs: `v4()`, `nil()`, `is_valid(s)`. Pure-Quirk, composes on `random`.
- **`itertools`** — eager combinators on Lists: `range_list`, `repeat`, `cycle`, `enumerate`, `zip`, `chain`, `take_while`, `drop_while`, `partition`, `groupby`, `unique`.
- **`datetime`** — now owns the high-level `DateTime` struct (moved from `time`), plus calendar helpers: `is_leap`, `days_in_month`, `is_weekend`, `day_name`, `month_name`, `start_of_day/week/month/year`, `diff_minutes/hours/days`, `humanize(dt, relative_to = now())`. The `time` lib retains only the low-level epoch + component externs.
- **`statistics`** — `mean`, `median` (+ `median_low`/`median_high`), `mode`, `variance`/`pvariance`, `stdev`/`pstdev`, `min_val`, `max_val`, `quantile(items, q)` with linear interpolation.

### Language features
- **Modulo operator** `%` and `%=` for Int/Double, including `__mod__` magic method on structs.
- **Hex / binary integer literals** — `0xFF`, `0b1010`, normalized to decimal at lex time.
- **Triple-quoted strings** `"""..."""` (and `'''...'''`) with multi-line content, interpolation, and automatic common-indent dedent.
- **Match guards** — `case x if cond =>`. A bare lowercase identifier `case x` now binds the scrutinee (instead of being a value-equality check against a free variable).
- **Pattern destructuring in `match`** — tuple form `case (a, b) =>` and list form `case [a, b, c] =>` with positional bindings.
- **Tuple `.N` field access** — `t.0`, `t.1` sugar for `t[0]`, `t[1]`. Works through `List.get` round-trips.
- **Optional chaining `?.`** — `obj?.field` and `obj?.method()` short-circuit on null without crashing.
- **List & map comprehensions** — `[expr for x in iter if cond]`, `{k: v for x in iter if cond}`. Both `where` and `if` accepted as the filter introducer.
- **f-string format specs** — `${x:.2f}`, `${n:04d}`, `${n:x}`. Legacy `%`/`|` separators still work.
- **Module-level mutable state** — top-level `counter := 0` materialises as an LLVM `GlobalVariable`, visible across all functions in the module.
- **Type narrowing** — inside `if x is T { … }` (and `and`-chained variants), `x` is treated as `T` for member/method lookup.
- **Property accessors** — `obj.name` (no parens) auto-calls a zero-arg method when no field by that name exists.
- **`finally` blocks** in `try`/`catch` (was already wired up; documented and covered with tests).
- **String comparison** — `<`, `<=`, `>`, `>=` on `String` now use proper `strcmp`. Was relying on raw pointer comparison and giving wrong answers like `"5" <= "9"` → false.
- **`ref` keyword removed** — was reserved but unused. `ref` is now a usable identifier.
- **Default argument values can be function calls** — `define foo(x = some_fn())` now parses correctly.

### Tooling
- **`quirk repl`** — interactive shell with persistent session state (preamble + bindings persist across turns; `:state`/`:reset`/`:quit` meta commands).
- **`quirk test`** — first-class test runner walking `tests/*_test.quirk`, exit-code-driven pass/fail.
- **`quirk fmt`** — canonical formatter with `--check` and `--stdout`. Handles inline docstrings, negative numbers, compound assigns, empty maps.
- **`quirk sync`** — install missing dependencies from `quirk.toml`.

### File extension
- **`.qk` → `.quirk`** across libs, tests, and the compiler's module-resolution paths. The VSCode extension associates `.quirk` as the language; old `.qk` files are no longer recognized.

### JSON improvements (`encoding.json`)
- **Typed `loads`** — numbers come back as real `Int`/`Double`, booleans as `Bool`, `null` as null (not as the strings `"42"`/`"true"`/`"null"`).
- **Pretty-print** — `dumps_pretty(val, indent=2)` for indented output that still round-trips through `loads`.

### HTTP (`net.http`)
- **Client** — `request`/`get`/`post`/`delete`/`post_json` accept custom `headers`, query `params` (auto-percent-encoded), `follow_redirects=true` (chases up to 5 hops, handles relative `Location`).
- **Server** — new `net.server` lib: `Server.listen(host, port, handler)` + `Request { method, path, query, headers, body }`. Hides the accept-loop / request-parsing / response-formatting boilerplate. Runtime now sets `SO_REUSEADDR` on `bind` so server restarts don't wait on TIME_WAIT.

### Compiler internals
- **Bigger codegen fix-list**: tuple `.N` access through `Tuple___get`; loops inside lambda bodies now capture outer-scope refs (free-var walker descends into `WhileNode`/`ForNode`/`MatchNode`/`TryCatchNode`); module globals materialise before non-main function bodies are emitted so cross-function reads resolve.
- **Runtime hygiene**: `strdup` now routed through a GC-backed copy (was leaking on every Map/Set key); Queue mutators all guard `if (!self)`; `cmd_release` now bails on git-push failure; regex `realloc` checks for NULL; `static` PackageManager helpers switched to `inline` (cleared two `-Wunused-function` warnings).

### VSCode extension
- Triple-quoted-string–aware masking in `maskLine`, with interpolation references preserved as code.
- Match-arm bindings (`case x`, `case (a, b)`) recognised so the body's diagnostics stay clean.
- Top-level `counter := 0` promoted to file-globals; `counter = …` inside a function no longer counted as a new local declaration (was producing "declared but never used" on real globals).
- Default-value params in the param regex (`(x: Int = -1, …)`) — every name after the first now lands in the local set.
- Format-spec characters inside `${…}` no longer leak as undefined identifiers.
- Semantic tokens skip docstrings + line comments so module names mentioned in commentary stay in the comment color.
- Logo refresh: alternate quark-themed icon at `icons/icon_quark.svg`.

### Known limitations carried into 0.3.0
- Enums with associated values (algebraic data types like `Result.Ok(v)`) — deferred; needs a runtime-representation design pass.
- BigInt / Decimal — deferred; each is a multi-day integration with libgmp / libmpdec.
- Storing raw `Double` primitives in a `List` doesn't auto-box; workaround is to keep values as `Any` and unbox at read.
- `Int 0` stored in a nonlocal cell reads back as null (boxed int 0 == NULL pointer); iterator-style code that uses `null` as an end-of-stream sentinel should avoid emitting raw `Int 0` through nonlocal state.

## [0.2.0]

Initial public version. See git history for details.
