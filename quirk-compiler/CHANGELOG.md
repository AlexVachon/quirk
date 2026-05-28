# Changelog

All notable changes to Quirk land here. The format is loosely
[Keep a Changelog](https://keepachangelog.com/) and the project follows
SemVer — minor bumps for new features, patches for fixes, major bumps
only for breaking changes.

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
