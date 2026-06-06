# Changelog — Quirk for VSCode

All notable changes to the extension land here. Versioning follows SemVer; minor bumps for new features, patches for fixes.

## [0.2.1] — 2026-06-06

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
