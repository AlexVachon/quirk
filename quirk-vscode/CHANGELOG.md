# Changelog — Quirk for VSCode

All notable changes to the extension land here. Versioning follows SemVer; minor bumps for new features, patches for fixes.

## [0.2.8] — 2026-06-08

### Catch up with compiler v2.3.1 — enum accessors are methods now

The six enum accessors switched from property to method shape in
the compiler (matches the rest of Quirk's API: `list.length()`,
`set.size()`, etc.). Extension updates to match:

**Completions** now insert with parens, kind=Method, cursor lands
between the parens:

| Insert | Was |
|---|---|
| `values()` | `values` |
| `names()` | `names` |
| `variants()` | `variants` |
| `value()` | `value` |
| `name()` | `name` |
| `ordinal()` | `ordinal` |

**Grammar** tightened: `enum-class-properties` now uses a negative
lookahead — `.values()` (correct, with parens) falls through to
`method-calls` and gets the method color; `.values` (no parens,
which is a compile error in v2.3.1) keeps the constant-property
color so the wrong shape is visually flagged.

`enum-instance-properties` already had the no-parens lookahead from
v0.2.7, so `.value` / `.ordinal` / `.name` without parens stay
property-colored (compile error), and the `()` form falls through
to method-calls.

## [0.2.7] — 2026-06-07

### Catch-up with the compiler's v2.2.16 and v2.3.0 enum surface

Adds IDE support for the enum-magic features that landed in the
compiler between vscode 0.2.6 and now.

**New completions on enum classes (`Gender.`):**

| Item | Kind | Description |
|---|---|---|
| `values` | property | `List` of backing values (v2.2.13) |
| `names` | property | `List<String>` of variant identifiers (v2.2.16) |
| `variants` | property | `List` of variant instances (v2.2.16) |
| `parse(...)` | method | Safe lookup, returns `EnumName?` (v2.2.16 / v2.3.0) |

**New completion on enum instances (`g.`):**

| Item | Kind | Description |
|---|---|---|
| `ordinal` | property | i32 declaration-order index (v2.2.16) |

Each completion ships rich Markdown docs with usage examples in
the suggestion popup.

### Grammar

* `Gender.names` and `Gender.variants` now color as constant
  properties (same shade as the v0.2.6 `Gender.values` rule). If
  themed properly, `Gender.names()` (incorrect — `.names` doesn't
  take parens) visually flags as a property rather than a method.
* New `enum-instance-properties` rule colors `.value`, `.ordinal`,
  `.name` distinctly from `.foo()` method calls. `.str` falls
  through to `method-calls` since it's a real method (takes parens).

### Diagnostics

Verified against the new compiler features:

* `enum Prices(Double) { Pi = 3.14, Half = 0.5, Neg = -1.5 }` — the
  v0.2.5 variant regex already accepts numeric values; no change
  needed.
* `for v in EnumName` — bare enum name in iterable position. The
  enum name is already registered in `fileGlobals` at pass 1, so
  the pass-2 identifier scanner doesn't false-flag it.
* `x: Int? = null` — nullable primitive declarations. `null` is in
  the keyword set and `Int` in builtins, so neither false-flags.

## [0.2.6] — 2026-06-06

### Syntax: `Gender.values` colors as a property, not a method

The TM grammar's `method-calls` rule matches any `.foo(` pattern and colors `foo` as a method name. That fires on `Gender.values()` too — but `.values` is a class-level *property accessor* (returns a `List`), not a method, so writing it with `()` is a usage error. The old highlight made the mistake look legitimate.

New `enum-class-properties` rule (placed before `method-calls`) matches `<Capitalized>.values` regardless of trailing parens and colors it as `variable.other.property.constant.quirk`. Themes that distinguish property vs method names now visually flag `Gender.values()` as wrong.

The completion item for `values` also pins `insertText: 'values'` explicitly — no editor setting can make it auto-paren now.

## [0.2.5] — 2026-06-06

### Backed-enum diagnostics actually fire now

0.2.4 added a variant regex that handled `Male = "male"` form, but
tested it against raw source — and the diagnostics provider runs
its regexes against the *masked* line where `maskLine()` has
replaced string contents with whitespace. So `Male = "male"`
arrived as `Male =        ` and the regex (which required a real
string literal or digit after `=`) never matched. Variants stayed
unrecognised; the warnings stayed up.

Widened to accept `^\s*<ident>\s*(?:=.*)?$` — the literal-shape
constraint dropped, since the post-mask `=.*` is whatever the
parser was going to scan anyway. Catches all three variant shapes
(unbacked, String-backed, Int-backed) consistently.

## [0.2.4] — 2026-06-06

### Backed-enum support (matches compiler v2.2.4+)

The diagnostics provider used to flag variant declarations inside a backed enum as `'Male' is not defined.`:

```quirk
enum Gender(String) {
    Male = "male"       // ← warning
    Female = "female"   // ← warning
    Other = "other"     // ← warning
}
```

Two parsing gaps caused this:

1. The variant regex required the line to end after the identifier, so `Male = "male"` didn't register as a declaration.
2. The enum decl regex didn't explicitly allow the `(BackingType)` clause; new inline-variant forms with `= literal` were missed.

Both regexes updated. Variant declarations with String, Int, and Double literal values now parse cleanly; commas between inline variants (`enum Small { A, B, C }`) are also accepted.

### `.value` and `.values` discoverability

New completions for enum class and instance dot-access:

- `Gender.` → lists every variant **plus** the class-level `values` accessor (v2.2.13's `EnumName.values → List`).
- `g.` where `g: Gender` → `value` (backing value), `str()` (variant name), `name` (alias for `str()`), each with rich documentation cards and code snippets.

The enum-instance branch fires when the inferred type matches an `enum Name` declaration in the file, so structurally-typed variables get the right completions even without an explicit annotation.

## [0.2.0] — 2026-05-28

First public release. Ships as a `.vsix` attached to a GitHub Release rather than via the VSCode Marketplace — see the README for the install command. Bundles the IDE work that landed alongside the Quirk 1.0.0 compiler.

### Debugger
- Full Debug Adapter Protocol bridge to qdb — gutter breakpoints, F5 / F10 / F11 stepping, Call Stack, Variables panel, hover-evaluate for identifiers.
- Inline value decorations on paused lines (Pylance-style) — driven by a custom `InlineValuesProvider` that skips keywords, comments, string contents, and method/call chains.
- Run/Debug combo button in the editor title with grouped sections: *Run File*, *Run File in Dedicated Terminal*, *Debug File*, *Debug using launch.json*.
- `launch.json` schema with snippets and the standard `program` / `args` / `cwd` / `env` / `stopOnEntry` / `compilerPath` / `quirkHome` fields.
- `QUIRK_HOME` auto-resolution: the adapter pulls the workspace setting and propagates it to the debuggee so `runtime.so` + the stdlib resolve correctly.

### Language services
- **Hover kind prefixes** matching Pylance shape: `(function)`, `(method)`, `(struct)`, `(enum)`, `(interface)`, `(type alias)`, `(parameter)`, `(constant)`, `(variable)`. Lambda-bound names (`c := fn(...)`) promote to `(function)`.
- **Ctrl+click navigation** now jumps to lambda parameters and variadic `...args` parameters (previously only `define foo(...)` params worked).
- **Variadic-param diagnostics fix** — `...args` no longer trips a phantom "args is not defined" warning inside the lambda body.

### Syntax
- Tokenization fix: the spread operator (`...`) is matched before the range operator (`..`), so `...args` no longer renders as `..` + `.args` in error-colored garbage.

## [0.1.3] and earlier

Initial preview releases — syntax highlighting, completions, diagnostics, hover, outline, rename, references, formatting.
