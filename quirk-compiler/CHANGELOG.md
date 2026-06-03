# Changelog

All notable changes to Quirk land here. The format is loosely
[Keep a Changelog](https://keepachangelog.com/) and the project follows
SemVer — minor bumps for new features, patches for fixes, major bumps
only for breaking changes.

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
