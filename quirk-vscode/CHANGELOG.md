# Changelog — Quirk for VSCode

All notable changes to the extension land here. Versioning follows SemVer; minor bumps for new features, patches for fixes.

## [0.2.23] — 2026-08-18 — module-level constants export + no "unused" on public API

Two related fixes, both surfaced while dogfooding `qcm` against the
export-check.

**Module-level `NAME := value` constants are now exported.** The
export scanner recognised `define`/`struct`/`enum`/`type`/
`interface`/re-exports, but not top-level constant bindings. A file
with

```quirk
DIM   := "\x1b[2m"
BOLD  := "\x1b[1m"
```

read as "no exports for those names", so downstream
`from .format use { DIM, BOLD }` false-warned even when the
compiler compiled it fine.

**Module-level names are no longer flagged "unused".** Public API
by shape — other files import them via `from .this use { X }`,
which this pass can't see. Was firing on the same DIM/BOLD
constants that also had the missing-export false-warning above,
which combined to a very confusing "declared but never used, and
if you use it we'll say it doesn't exist" pair of squiggles.

Both fixes are in `DiagnosticsProvider.ts`:

- `collectModuleExports` — new depth-tracked loop that adds
  brace-depth-0 `NAME := ...` / `NAME: T = ...` bindings.
- Pass-3 (unused warnings) — skips when `fileGlobals.has(cleanKey)`.

## [0.2.22] — 2026-08-18 — (rolled into 0.2.23)

## [0.2.21] — 2026-08-18 — lint direct calls to magic methods

Info-level squiggle on `x.__get(i)` / `x.__set(i, v)` / `x.__add(y)`
and other dunder methods called directly, with a concrete
alternative:

```
avoid calling magic method '__get' directly — use bracket syntax: `x[i]`
avoid calling magic method '__add' directly — use `+`
avoid calling magic method '__iter' directly — use `for x in …` (loops call this automatically)
```

Dunder methods are the internal protocol behind bracket syntax and
operator overloads — user code should call the sugar, not the
implementation. `x[i]` reads better than `x.__get(i)` and produces
identical IR.

Exempted:

- `self.__X(…)` — calling your own dunder from another method is the
  canonical stdlib pattern (e.g. `List.first()` calls `self.__get(0)`
  because bracket syntax on `self` isn't available inside the class).
- Method definitions (`define __get(self, i: Int) …`) — those are
  where dunders are supposed to appear.
- Docstring examples (`--- xs.__get(0) is equivalent to xs[0] ---`) —
  the fence-tracked skip keeps prose examples quiet.

## [0.2.20] — 2026-08-18 — warn when `from X use { name }` isn't exported

Previously the extension only validated the module path (`from .foo use {…}`)
and silently accepted every name in `{…}`. Typos like

```quirk
from .frontmatter use { split_frontmatter }
```

against a module that actually defines `split_formatter` compiled fine in
the editor and only surfaced at compile time with

```
[ERROR] module '.frontmatter' does not export symbol 'split_frontmatter'
```

Now: each name in the braces is validated against the target module's
real exports (top-level `define/struct/enum/type/interface`,
re-exported names, enum variants, tagged-union arms). A missing name
shows up as an inline warning in the editor:

```
'module '.frontmatter' does not export symbol 'split_frontmatter'
```

Cross-module lookups are cached per document, so a file that imports
`Int` five times from `typing` only pays the module-read cost once.

## [0.2.19] — 2026-08-17 — `=>` single-statement control-flow snippets

Companion to Quirk 5.3.0's new single-statement control-flow form.
Three new snippets:

  `if=>`     → `if ${1:condition} => $0`
  `while=>`  → `while ${1:condition} => $0`
  `for=>`    → `for ${1:item} in ${2:iterable} => $0`

The existing braced snippets (`if`, `while`, `for`) still work —
these are opt-in for the compact form.

## [0.2.18] — 2026-08-14 — track declarations in top-level control-flow blocks

`DiagnosticsProvider` was silently dropping every declaration inside
a top-level `if / for / while / match` body in script-mode files:

```quirk
if cmd == "build" {
    site := "."                    // declaration dropped
    if argv.length() >= 3 { site = argv.__get(2) }
    build(site)                     // → "'site' is not defined."
}
```

Root cause: the guard `if (isInsideFunc || isTopLevel)` was
gating declaration tracking on `braceDepth === 0 || currentFuncDepth
!= -1`. A script-mode `if` body sits at `braceDepth > 0` with no
enclosing `define`, so both were false and the whole tracking pass
was skipped. The comment above the guard even said `braceDepth > 0
outside a function means we're inside a struct body` — that
assumption breaks for script-style code.

Now the provider explicitly tracks struct/enum/interface body
regions via `structBodyDepth`, and declaration tracking is gated on
`isInsideFunc || !isInsideStructBody`. Script-mode top-level
control flow works; struct-body false positives (declarations
mis-identified from `field: Type` lines) stay suppressed.

## [0.2.17] — 2026-08-14 — 4-space indent in every snippet, not tabs

Every snippet body was inserting a literal `\t` for indentation.
In an editor configured for 4-space indent (Quirk's canonical
style), that showed up as a real tab character, mixing with the
surrounding spaces.

Fixed in two places:

- `snippets.json` — 34 `\t` occurrences → 4 spaces.
- `src/CompletionProvider.ts` — 39 `\n\t` occurrences in the
  runtime snippet strings (keyword completions like `define`,
  `struct`, `if`, `match`, and the __magic-method templates)
  → `\n    `.

The `out/` directory is rebuilt with the updated strings so the
fix ships in the packaged VSIX.

## [0.2.16] — 2026-06-20

### Stop flagging parameters after a function-call default value

`DiagnosticsProvider` split the parameter list with a single regex
whose default-value alternative `\([^)]*\)` doesn't handle nested
parens. For a signature like

```
define request(method: String, ..., params: Map = Map(),
               follow_redirects: Bool = true) -> Response
```

the matcher consumed `headers: Map = Map(`, then the fallback
alternative `[^,)]+` collapsed everything from there to the next
top-level `)`, swallowing `params` and `follow_redirects`. Both
got flagged `'params' is not defined.` and `'follow_redirects' is
not defined.` everywhere they were used inside the body, even
though the compiler accepted the code without complaint.

Replaced with a paren/bracket/brace depth-aware hand-coded
splitter that walks the parameter string and emits one segment
per top-level comma. Default values containing function calls,
nested lists, or tuples now split correctly:

  - `headers: Map = Map()`   ✓ now recognised
  - `pairs: List = [1, 2]`   ✓
  - `pos: Tuple = (1, 2)`    ✓

The regex stays for the per-segment name extraction (a single
identifier at the start), which is simple and bounded.

## [0.2.15] — 2026-06-19

### TextMate grammar: `extend`, type aliases, and relative imports

Three drift fixes against compiler v3.4.0+ syntax:

  - `extend Foo { ... }` (used by user code that bolts methods
    onto a struct from another module) is now highlighted as a
    declaration the same way `struct Foo { ... }` is.

  - `from ..element use { Element }` and similar parent-relative
    imports — the form added in compiler v3.4.0 to support
    multi-file packages like `html` — now syntax-highlight the
    dotted path correctly. Same fix applied to `from .x use`,
    `from x.y use`, and the bare `use ...` statement.

  - `data/stdlib-index.json` regenerated. The `html` package's
    488 symbols (Element, tag combinators, attribute helpers,
    escape/raw) are now indexed for completion and hover.

### LSP (quirk-lsp 0.19.0) sees `extend`, `type`, and `..` too

Mirrors the grammar changes on the LSP side so document-symbol
panels and Go-To-Definition pick up:

  - `extend Foo { ... }` blocks (DECL_PATTERNS + TOP_LEVEL_RE)
  - `type Name = T` type aliases (new `type_alias` symbol kind)
  - `from ..pkg use` / `use ..pkg` (relative-import regex)

Also adds the Option/Result combinator names (`Some`, `None`,
`Ok`, `Err`) to the builtin-identifier set so they stop being
reported as "unknown" in scope-resolution warnings.

## [0.2.14] — 2026-06-18

### Interpreter picker stops scanning /tmp and friends

"Select Quirk Interpreter" was walking up to 6 directories above
every open `.quirk` document and scanning each one for `.venv`
dirs. Files opened from `/tmp/<something>/src/file.quirk` ended
up triggering a scan of `/tmp` itself, which surfaced every
stale `.venv` left there by prior experiments — the picker grew
to dozens of entries and stalled on `stat()` calls.

Two guards:
- `SCAN_BLOCKLIST` — `scanForVenvs` refuses to descend into
  system roots (`/tmp`, `/var`, `/proc`, `/sys`, `/dev`, `/run`,
  `/snap`, `/usr`, `/opt`, and `/` itself), even when the
  upward walk lands there. Exception: if the user explicitly
  added one as a workspace folder, we honor it.
- Upward walk caps at `$HOME` — for docs inside the user's
  home, capping at home is the natural top of any project
  tree; for docs outside, the blocklist catches the system
  paths immediately.

The picker now opens in roughly the time of a single stat()
call regardless of how cluttered `/tmp` is.

## [0.2.13] — 2026-06-13

### Stdlib index now covers tagged unions and extend-block methods

The pre-built stdlib symbol index (`data/stdlib-index.json`) was
missing the canonical v2.4.4+ types — `Option[T]`, `Result[T,E]`,
their variants `Some` / `None` / `Ok` / `Err`, and every method
declared inside an `extend X { ... }` block (e.g. `Option.is_some`,
`Option.unwrap_or`, `Result.is_ok`). The generator only knew about
`struct X { ... }`-style declarations.

This release teaches `tools/gen_stdlib_docs.py` to recognize:

- `type Name[T] = Variant1(...) | Variant2(...)` — emits the
  union itself and each variant as a documented entry.
- `extend Name { ... }` — methods inside qualify as `Name.method`
  and end up in the index next to the struct they extend.
- Inline single-line docstrings `--- text ---` (in addition to
  the multi-line `--- ... ---` block form). Several stdlib files
  use the inline shape; previously they yielded zero index entries.

Index size jumped from 287 → 390 symbols, ~342 → ~467 documented
entries. Hover now resolves `Option`, `unwrap_or`, `is_ok`, and
the rest of the canonical sum-type API.

## [0.2.12] — 2026-06-12

### Hover for stdlib symbols, even when not explicitly `use`d

Previously, hovering over an stdlib name like `argv` in
`sys.argv()` produced nothing — `executeDefinitionProvider` only
resolves names reachable via an `from sys use { argv }` import.
Module-alias access dropped through to "no hover."

v0.2.12 ships a pre-built **stdlib symbol index** alongside the
extension and uses it as a hover fallback. The index is generated
by `tools/gen_stdlib_docs.py` in the compiler repo (run `make
docs` to refresh) and bundles 287 documented stdlib symbols —
signatures + docstrings extracted from in-source `---` blocks.

Resolution order in `HoverProvider`:
1. Keywords / known constants
2. Magic methods (`__init`, `__str`, …)
3. Built-in struct types (`String`, `List`, …) — live read from
   `packages/typing`
4. `executeDefinitionProvider` (existing path — works for any name
   reachable via imports in the current file)
5. **NEW: stdlib symbol index** (`data/stdlib-index.json`)

The index is loaded lazily on first hover and cached for the
session.

## [0.2.11] — 2026-06-10

### Catch up with compiler v2.4.1 — generic tagged unions

The type-alias regex now accepts an optional `[T, U]` clause between
the type name and the `=`:

```
type Option[T] = Some(value: T) | None()
```

Type params are themselves registered in `fileGlobals` so payload
annotations (`value: T`) don't false-flag, and the existing RHS
variant-walk still finds `Some` / `None`. Without this patch a
generic tagged-union file fired ~14 phantom "is not defined"
warnings (`Option`, `T`, `Some`, `None`, `E`, …).

## [0.2.10] — 2026-06-10

### Catch up with compiler v2.4.0 — tagged unions

The v2.4.0 compiler introduced tagged unions:

```quirk
type Result = Ok(value: Int) | Err(msg: String)
```

Without IDE catch-up, the extension fired 15 false-positive
"X is not defined" warnings against a basic tagged-union file
(variant decls, construction sites, `case Ok =>` arms). v0.2.10
closes those gaps.

### Diagnostics

* **Variant constructors collected.** The type-alias regex now
  walks the RHS for `Capitalized(` patterns and registers each
  variant identifier in `fileGlobals`. Covers the decl line itself
  (`Ok` / `Err`) and unblocks all downstream uses (`Ok(42)`,
  `case Err =>`, etc.).
* **`case Variant as v` registers `v` as a local.** The existing
  case-bind regexes only matched `case x` / `case (a, b)` shapes.
  Added a `case <CapitalizedType> as <bind>` regex so the body's
  `v.field` accesses don't false-flag as undefined.

### Grammar

* New `case <Variant>` rule in `keywords` — capitalized identifiers
  in case position color as `entity.name.type.variant.quirk` so the
  variant tag stands out the same way `case Int =>` does in
  primitive type-matches.

### Snippets

* `tunion` — `type Result = Ok(value: Int) | Err(msg: String)`
* `tmatch` — match block with `case Variant as v => ` narrow-bind
  shape pre-filled.

## [0.2.9] — 2026-06-08

### Fix: enum variant `Name = literal` no longer paints as keyword arg

The `keyword-arguments` rule (`ident = value` → `variable.parameter`
coloring, intended for `func(name = value)`) was unconstrained and
fired anywhere a bare identifier preceded `=`. Inside an enum body:

```quirk
enum Gender(String) {
    Male
    Female = "F"      // Female painted orange (parameter color)
    Other
}
```

`Female` got the parameter color while `Male` / `Other` (no `=`)
stayed identifier-colored. Inconsistent and visually misleading.

Tightened the lookbehind from `(?<![.\[])` to `(?<=[(,]\s*)` — the
ident must be immediately inside an argument list (after `(` or `,`,
possibly across a newline for multi-line calls). Multi-line keyword
args still work; bare top-level `Name = value` patterns no longer
mis-color.

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
