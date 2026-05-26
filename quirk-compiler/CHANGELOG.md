# Changelog

All notable changes to Quirk land here. The format is loosely
[Keep a Changelog](https://keepachangelog.com/) and the project follows
SemVer — minor bumps for new features, patches for fixes, major bumps
only for breaking changes.

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
