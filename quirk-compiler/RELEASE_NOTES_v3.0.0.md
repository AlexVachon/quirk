# Quirk 3.0.0 — Type-system overhaul complete

**Release date:** 2026-06-10
**Previous release:** v2.4.4

v3.0.0 is the milestone bump for Quirk's type-system overhaul. The
two-year v2.x line started with operator/expression ergonomics and
finishes with a coherent, modern type system: nullable primitives,
tagged unions with payloads, generic types, exhaustive `match`,
variant methods, and a canonical `Option[T]` / `Result[T, E]` in
the standard library.

There are **no breaking changes** from v2.4.4 — code that compiled
on v2.4.4 compiles on v3.0.0. The major bump is the narrative
marker that the type-system milestone is complete; future v3.x
releases will land per-instantiation monomorphization (perf-only)
and continue with smaller features.

---

## What's new since v2.3.0 (the v3 type-system arc)

### Nullable primitives (v2.3.0)

```quirk
x: Int?     = null
y: Bool?    = true
z: Double?  = 1.5
```

Primitive scalars (`Int`, `Bool`, `Double`, `Char`) lower as `i8*`
when annotated nullable so they can hold `null` alongside their
value. Enum types are nullable for free (they lower to an ordinal
i32 plus an Any* sentinel for null).

### Tagged unions (v2.4.0)

```quirk
type Result = Ok(value: Int) | Err(msg: String)

match parse(input) {
    case Ok  as o => print("got ${o.value}")
    case Err as e => print("error: ${e.msg}")
}
```

Sum types with typed payloads. Each variant is a named constructor;
zero-payload variants use `()` (`Pending()`) to disambiguate from
the existing type-alias union (`type T = Int | String`).
`match` arms narrow `v` to the variant type, so `o.value` (where
`o: Ok`) GEPs against the correct layout.

### Exhaustiveness checking (v2.4.0)

```
[WARNING] non-exhaustive match on 'Result' — missing variant: Err.
          Add an arm or a `_` wildcard.
```

A `match` on a tagged-union scrutinee warns when one or more
variants are missing and no `_` wildcard is present. Warning, not
error — partial matches keep compiling, but the missing case
surfaces.

### Generic types (v2.4.1 – v2.4.3)

```quirk
type Option[T]    = Some(value: T) | None()
type Result[T, E] = Ok(value: T)   | Err(error: E)

struct Box[T] {
    value: T
    define __init(self, value: T) -> void { self.value = value }
    define triple(self) -> T { return self.value * 3 }
}

define double_it(b: Box[Int]) -> Int { return b.value * 2 }
```

`[T, U, ...]` parameter clauses on structs and tagged unions. Sema
substitutes the concrete type args at use sites (`b: Box[Int]`
narrows `b.value` from `T` to `Int`). Generic method bodies can do
arithmetic and equality on `T` directly. Codegen uses a uniform
representation (every generic field is `i8*` at the IR layer) with
shape-aware runtime dispatch via `quirk_opaque_to_int` and
`quirk_opaque_eq`.

### Variant methods + canonical `Option` / `Result` (v2.4.4)

```quirk
type Result = Ok(value: Int) | Err(msg: String)

extend Ok  { define describe(self) -> String { return "Ok(${self.value})" } }
extend Err { define describe(self) -> String { return "Err: ${self.msg}"  } }
```

Per-variant methods via the existing `extend` syntax. The canonical
`Option[T]` and `Result[T, E]` (with `is_some` / `is_none` / `is_ok`
/ `is_err` / `unwrap_or` helpers) ship in `quirk-typing` v1.1.0:

```quirk
from typing use { Option, Some, None, Result, Ok, Err }
```

---

## Stdlib

- **`typing` v1.1.0** — adds canonical `Option[T]` and `Result[T, E]`.
  Bootstrap pulls it via the version pin in the compiler's Makefile.
- Other stdlib packages keep their existing surfaces; no migrations
  required.

---

## Tooling

- **VSCode extension v0.2.11** — knows about tagged unions (incl.
  generic forms `type Option[T] = ...`), `case Variant as v`
  bindings, and the new `extend` shape on variants. Snippets
  added: `tunion`, `tmatch`.

---

## Upgrade

```
quirk compiler update --with-extension
```

No code changes required. The new type-system features are
opt-in syntax; existing structs / enums / functions continue to
work unchanged.

---

## What's deferred to future v3.x

- **Per-instantiation Codegen monomorphization** — currently
  `Box[Int]` and `Box[String]` share a single LLVM layout with
  `value: i8*`. A future v3.x release will emit distinct
  specialized structs per instantiation for the perf win on
  primitive payloads. Scoped, not currently demand-driven.

---

## Regression coverage

45 probes (`tests/probes/p01`–`p45`) lock the v3 surface against
crash / wrong-output regressions; 60 stdlib smoke tests cover the
broader compiler. All pass on both the linux-x86_64 and macos-arm64
release builds.
